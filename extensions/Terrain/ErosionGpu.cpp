// erode() on the GPU through lila::gpu: shaders/terrain_erosion.slang holds the kernels, this file drives them. The
// depression fill needs an upper bound to relax down from; a priority flood over 8x8 block maxima on the CPU gives one.

#include "ErosionGpu.h"

#include <lila/GpuCompute.h>
#include <lila/Profile.h>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <queue>

using namespace lila;

namespace terrain
{
namespace
{
// Same order as ErodeScene's pointers in the shader.
enum Buf
{
    BHeight,
    BKey,
    BFill,
    BCoarse,
    BFlag,
    BReceiver,
    BPtr0,
    BPtr1,
    BCount0,
    BCount1,
    BA0,
    BA1,
    BB0,
    BB1,
    BUplift,
    BHardness,
    BEdge,
    BSum,
    BMove,
    BDelta,
    BufCount,
    BScene = BufCount,
};

enum Kernel
{
    KKeys,
    KBlockMax,
    KInitFill,
    KRelax,
    KReceivers,
    KDonors,
    KAreaJump,
    KSolveInit,
    KSolveJump,
    KSolveStore,
    KShed,
    KGather,
    KApply,
    KHoldEdge,
    KernelCount,
};

constexpr const char* kEntries[KernelCount] = {"keysCS",       "blockMaxCS", "initFillCS", "relaxCS",        "receiversCS",   "donorsCS",    "areaJumpCS",
                                               "solveInitCS", "solveJumpCS", "solveStoreCS", "thermalShedCS", "thermalGatherCS", "thermalApplyCS", "holdEdgeCS"};

struct GpuScene
{
    uint64_t buffers[BufCount];
    uint32_t nx, nz, cnx, cnz;
    uint32_t block, margin, pad0, pad1;
    float cellM, strength, areaExponent, upliftRate;
    float talus, pad2, pad3, pad4;
};

struct Push
{
    uint64_t scene;
    uint32_t pass, flip, pad0, pad1;
};
static_assert(sizeof(Push) == 24);

constexpr uint32_t kBlock = 8;
constexpr uint32_t kRowGroups = 4096;
/** Jump passes: enough for any path through 2^26 samples, and even so the result lands in the *0 buffers. */
constexpr int kJumps = 26;
constexpr int kRelaxPerRun = 32;

uint32_t toKey(float f)
{
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return (b & 0x80000000u) ? ~b : (b | 0x80000000u);
}

float fromKey(uint32_t k)
{
    const uint32_t b = (k & 0x80000000u) ? (k & 0x7fffffffu) : ~k;
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}

/** Priority flood over block maxima, rising step keys per block: no sample's true fill is above its block's plus a margin. */
std::vector<uint32_t> coarseBound(const std::vector<uint32_t>& top, int cnx, int cnz, uint32_t step)
{
    LILA_ZONE();

    std::vector<uint32_t> w(top.size());
    std::vector<uint8_t> seen(top.size(), 0);
    using Entry = std::pair<uint32_t, uint32_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> open;
    for (int z = 0; z < cnz; ++z)
        for (int x = 0; x < cnx; ++x)
            if (x == 0 || z == 0 || x == cnx - 1 || z == cnz - 1)
            {
                const uint32_t i = uint32_t(z * cnx + x);
                w[i] = top[i];
                seen[i] = 1;
                open.push({w[i], i});
            }
    while (!open.empty())
    {
        const auto [k, i] = open.top();
        open.pop();
        const int x = int(i % uint32_t(cnx)), z = int(i / uint32_t(cnx));
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx)
            {
                const int nx = x + dx, nz = z + dz;
                if ((dx == 0 && dz == 0) || nx < 0 || nz < 0 || nx >= cnx || nz >= cnz)
                    continue;
                const uint32_t j = uint32_t(nz * cnx + nx);
                if (seen[j])
                    continue;
                seen[j] = 1;
                const uint64_t up = uint64_t(k) + step;
                w[j] = std::max(top[j], uint32_t(std::min<uint64_t>(up, 0xffffffffu)));
                open.push({w[j], j});
            }
    }
    return w;
}

std::vector<float> gpuMaskOr(const Heightfield& h, const std::string& name, float fallback)
{
    if (!name.empty())
        if (const std::vector<float>* m = h.findMask(name))
            return *m;
    return std::vector<float>(h.size(), fallback);
}

struct Runner
{
    gpu::Session session;
    std::mutex mutex;
    double ms = 0.0;
};

Runner& runner()
{
    // Never destroyed: process exit tears the renderer down first, and a session must not outlive it.
    static Runner* r = new Runner;
    return *r;
}

class Job
{
public:
    Job(Runner& run, const Heightfield& h, const ErosionParams& p) : mRun(run), mH(h), mP(p)
    {
        mN = h.size();
        mCnx = (uint32_t(h.nx) + kBlock - 1) / kBlock;
        mCnz = (uint32_t(h.nz) + kBlock - 1) / kBlock;
    }

    bool run(const std::function<void(gpu::Recorder&)>& fn, std::string* err)
    {
        const bool ok = mRun.session.run(
            [&](gpu::Recorder& r) {
                for (int k = 0; k < KernelCount; ++k)
                    r.kernel(k, "terrain_erosion.spv", kEntries[k], sizeof(Push));
                fn(r);
            },
            err);
        mRun.ms += mRun.session.lastMs();
        return ok;
    }

    void dispatch(gpu::Recorder& r, int kernel, size_t threads, uint32_t pass = 0, uint32_t flip = 0)
    {
        const Push push{r.address(BScene), pass, flip, 0, 0};
        const uint32_t groups = uint32_t((threads + 63) / 64);
        r.dispatch(kernel, &push, std::min(groups, kRowGroups), (groups + kRowGroups - 1) / kRowGroups);
        r.barrier();
    }

    void setup(gpu::Recorder& r, const std::vector<float>& uplift, const std::vector<float>& hardness)
    {
        const size_t f = mN * 4, c = size_t(mCnx) * mCnz * 4;
        const size_t sizes[BufCount] = {f, f, f, c, 16, f, f, f, f, f, f, f, f, f, f, f, f, f, f, f};
        GpuScene g{};
        for (int b = 0; b < BufCount; ++b)
            g.buffers[b] = r.buffer(b, sizes[b]);
        r.buffer(BScene, sizeof(GpuScene));
        g.nx = uint32_t(mH.nx);
        g.nz = uint32_t(mH.nz);
        g.cnx = mCnx;
        g.cnz = mCnz;
        g.block = kBlock;
        g.margin = 2 * kBlock;
        g.cellM = float(mH.cell * 0.01);
        g.strength = float(mP.strength);
        g.areaExponent = float(mP.areaExponent);
        g.upliftRate = float(mP.uplift);
        g.talus = float(std::tan(std::clamp(mP.talusAngle, 1.0, 89.0) * 0.017453292519943295) * mH.cell);
        r.upload(BScene, &g, sizeof g);
        r.upload(BHeight, mH.heights.data(), f);
        r.upload(BEdge, mH.heights.data(), f);
        r.upload(BUplift, uplift.data(), f);
        r.upload(BHardness, hardness.data(), f);
        r.barrier();
    }

    /** Only the buffers fill() uses: the heights, their keys, the fill, the coarse bound and the flag. */
    void setupFill(gpu::Recorder& r)
    {
        const size_t f = mN * 4, c = size_t(mCnx) * mCnz * 4;
        GpuScene g{};
        g.buffers[BHeight] = r.buffer(BHeight, f);
        g.buffers[BKey] = r.buffer(BKey, f);
        g.buffers[BFill] = r.buffer(BFill, f);
        g.buffers[BCoarse] = r.buffer(BCoarse, c);
        g.buffers[BFlag] = r.buffer(BFlag, 16);
        r.buffer(BScene, sizeof(GpuScene));
        g.nx = uint32_t(mH.nx);
        g.nz = uint32_t(mH.nz);
        g.cnx = mCnx;
        g.cnz = mCnz;
        g.block = kBlock;
        g.margin = 2 * kBlock;
        r.upload(BScene, &g, sizeof g);
        r.upload(BHeight, mH.heights.data(), f);
        r.barrier();
    }

    /** The filled surface into BFill: keys and block maxima, the CPU bound, then relaxation until nothing moves. */
    bool fill(std::string* err)
    {
        const size_t coarse = size_t(mCnx) * mCnz;
        if (!run(
                [&](gpu::Recorder& r) {
                    dispatch(r, KKeys, mN);
                    dispatch(r, KBlockMax, coarse);
                    r.readback(BCoarse, coarse * 4);
                },
                err))
            return false;
        std::vector<uint32_t> top(coarse);
        mRun.session.read(BCoarse, top.data(), coarse * 4);
        const std::vector<uint32_t> bound = coarseBound(top, int(mCnx), int(mCnz), 2 * kBlock);
        bool first = true;
        for (int guard = 0; guard < 100000; ++guard)
        {
            if (!run(
                    [&](gpu::Recorder& r) {
                        if (first)
                        {
                            r.upload(BCoarse, bound.data(), coarse * 4);
                            r.barrier();
                            dispatch(r, KInitFill, mN);
                        }
                        r.fill(BFlag, 0u);
                        r.barrier();
                        for (int k = 0; k < kRelaxPerRun; ++k)
                            dispatch(r, KRelax, mN);
                        r.readback(BFlag, 4);
                    },
                    err))
                return false;
            first = false;
            uint32_t changed = 1;
            mRun.session.read(BFlag, &changed, 4);
            if (!changed)
                return true;
        }
        if (err)
            *err = "the GPU depression fill did not settle";
        return false;
    }

    /** Receivers and drainage counts (descendants per sample) into BReceiver and BCount0. */
    void drainage(gpu::Recorder& r)
    {
        dispatch(r, KReceivers, mN);
        dispatch(r, KDonors, mN);
        for (int k = 0; k < kJumps; ++k)
        {
            const uint32_t flip = uint32_t(k & 1);
            r.copy(flip ? BCount1 : BCount0, flip ? BCount0 : BCount1, mN * 4);
            r.barrier();
            dispatch(r, KAreaJump, mN, uint32_t(k), flip);
        }
    }

    bool step(std::string* err)
    {
        if (!fill(err))
            return false;
        return run(
            [&](gpu::Recorder& r) {
                drainage(r);
                dispatch(r, KSolveInit, mN);
                for (int k = 0; k < kJumps; ++k)
                    dispatch(r, KSolveJump, mN, uint32_t(k), uint32_t(k & 1));
                dispatch(r, KSolveStore, mN, 0, 0);
                if (mP.thermalIterations > 0)
                {
                    for (int t = 0; t < mP.thermalIterations; ++t)
                    {
                        dispatch(r, KShed, mN);
                        dispatch(r, KGather, mN);
                        dispatch(r, KApply, mN);
                    }
                    dispatch(r, KHoldEdge, mN);
                }
            },
            err);
    }

    /** The filled heights, after fill(). */
    bool readFill(std::vector<float>& filled, std::string* err)
    {
        if (!run([&](gpu::Recorder& r) { r.readback(BFill, mN * 4); }, err))
            return false;
        std::vector<uint32_t> keys(mN);
        mRun.session.read(BFill, keys.data(), mN * 4);
        filled.resize(mN);
        tbb::parallel_for(size_t(0), mN, [&](size_t k) { filled[k] = fromKey(keys[k]); });
        return true;
    }

    /** The final drainage for the masks, read back with the heights. */
    bool finish(Heightfield& out, std::vector<float>& filled, std::vector<float>& area, std::string* err)
    {
        if (!fill(err))
            return false;
        if (!run(
                [&](gpu::Recorder& r) {
                    drainage(r);
                    r.readback(BHeight, mN * 4);
                    r.readback(BFill, mN * 4);
                    r.readback(BCount0, mN * 4);
                },
                err))
            return false;
        out.heights.resize(mN);
        mRun.session.read(BHeight, out.heights.data(), mN * 4);
        std::vector<uint32_t> keys(mN), counts(mN);
        mRun.session.read(BFill, keys.data(), mN * 4);
        mRun.session.read(BCount0, counts.data(), mN * 4);
        filled.resize(mN);
        area.resize(mN);
        tbb::parallel_for(size_t(0), mN, [&](size_t k) {
            filled[k] = fromKey(keys[k]);
            area[k] = float(1u + counts[k]);
        });
        return true;
    }

private:
    Runner& mRun;
    const Heightfield& mH;
    const ErosionParams& mP;
    size_t mN = 0;
    uint32_t mCnx = 0, mCnz = 0;
};
} // namespace

bool gpuErosionAvailable() { return gpu::available(); }

double lastGpuErosionMs() { return runner().ms; }

bool drainGpu(const Heightfield& h, Drainage& out, std::string* err)
{
    LILA_ZONE();

    if (!gpu::available())
    {
        if (err)
            *err = "no GPU (Lila's renderer isn't running)";
        return false;
    }
    if (h.nx < 3 || h.nz < 3)
    {
        if (err)
            *err = "the heightfield is too small";
        return false;
    }
    std::vector<float> filled;
    {
        Runner& run = runner();
        std::lock_guard lock(run.mutex);
        run.ms = 0.0;
        const ErosionParams p;
        Job job(run, h, p);
        std::string local;
        std::string* e = err ? err : &local;
        if (!job.run([&](gpu::Recorder& r) { job.setupFill(r); }, e) || !job.fill(e) || !job.readFill(filled, e))
            return false;
    }
    out = drainFilled(h, std::move(filled));
    return true;
}

bool erodeGpu(Heightfield& h, const ErosionParams& p, std::string* err)
{
    LILA_ZONE();

    if (!gpu::available())
    {
        if (err)
            *err = "no GPU (Lila's renderer isn't running)";
        return false;
    }
    if (h.nx < 3 || h.nz < 3)
    {
        if (err)
            *err = "the heightfield is too small";
        return false;
    }
    Runner& run = runner();
    std::lock_guard lock(run.mutex);
    run.ms = 0.0;
    const std::vector<float> uplift = gpuMaskOr(h, p.upliftMask, 1.0f);
    const std::vector<float> hardness = gpuMaskOr(h, p.hardnessMask, 0.0f);
    Job job(run, h, p);
    std::string local;
    std::string* e = err ? err : &local;
    if (!job.run([&](gpu::Recorder& r) { job.setup(r, uplift, hardness); }, e))
        return false;
    for (int it = 0; it < std::max(0, p.iterations); ++it)
        if (!job.step(e))
            return false;
    Heightfield out = h;
    Drainage d;
    if (!job.finish(out, d.filled, d.area, e))
        return false;
    const std::vector<float> start = std::move(h.heights);
    h.heights = std::move(out.heights);
    writeFlowMasks(h, d);
    std::vector<float> lost(h.size()), gained(h.size());
    tbb::parallel_for(size_t(0), h.size(), [&](size_t k) {
        const float change = h.heights[k] - start[k] - float(p.uplift * p.iterations * uplift[k]);
        lost[k] = std::max(0.0f, -change);
        gained[k] = std::max(0.0f, change);
    });
    auto normalised = [](std::vector<float>& dst, const std::vector<float>& src) {
        float hi = 0.0f;
        for (float v : src)
            hi = std::max(hi, v);
        const float inv = hi > 0.0f ? 1.0f / hi : 0.0f;
        for (size_t k = 0; k < src.size(); ++k)
            dst[k] = src[k] * inv;
    };
    normalised(h.mask("wear"), lost);
    normalised(h.mask("deposit"), gained);
    return true;
}
} // namespace terrain
