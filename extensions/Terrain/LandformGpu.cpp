// The landform kernels in shaders/terrain_landform.slang, driven through lila::gpu like ErosionGpu.cpp.

#include "LandformGpu.h"

#include "Landform.h"

#include <lila/GpuCompute.h>
#include <lila/Profile.h>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <mutex>

using namespace lila;

namespace terrain
{
namespace landgpu_
{
enum Buf
{
    BHeight,
    BOffset,
    BTalus,
    BSum,
    BMove,
    BDelta,
    BufCount,
    BScene = BufCount,
};

enum Kernel
{
    KFilter,
    KShed,
    KGather,
    KApply,
    KernelCount,
};

constexpr const char* kEntries[KernelCount] = {"filterCS", "talusShedCS", "talusGatherCS", "talusApplyCS"};

struct LandScene
{
    uint64_t buffers[BufCount];
    uint32_t nx, nz;
    float cell, originX;
    float originZ, scale, strength, lacunarity;
    float gain, slopeFull;
    int32_t octaves;
    uint32_t seed;
};
static_assert(sizeof(LandScene) == 96);

struct Push
{
    uint64_t scene;
    uint32_t pass, pad0;
};
static_assert(sizeof(Push) == 16);

constexpr uint32_t kRowGroups = 4096;

struct Runner
{
    gpu::Session session;
    std::mutex mutex;
};

Runner& runner()
{
    // Never destroyed: process exit tears the renderer down first, and a session must not outlive it.
    static Runner* r = new Runner;
    return *r;
}

void dispatch(gpu::Recorder& r, int kernel, size_t threads)
{
    const Push push{r.address(BScene), 0, 0};
    const uint32_t groups = uint32_t((threads + 63) / 64);
    r.dispatch(kernel, &push, std::min(groups, kRowGroups), (groups + kRowGroups - 1) / kRowGroups);
    r.barrier();
}

bool ready(const Heightfield& h, std::string* err)
{
    if (!gpu::available())
    {
        if (err)
            *err = "no GPU (Lila's renderer isn't running)";
        return false;
    }
    if (h.empty())
    {
        if (err)
            *err = "the heightfield is empty";
        return false;
    }
    return true;
}

/** Records the kernels, the buffers and the scene, then fn; one submission. */
bool runJob(const Heightfield& h, const LandScene& base, const std::function<void(gpu::Recorder&)>& fn, std::string* err)
{
    Runner& run = runner();
    std::lock_guard lock(run.mutex);
    return run.session.run(
        [&](gpu::Recorder& r) {
            for (int k = 0; k < KernelCount; ++k)
                r.kernel(k, "terrain_landform.spv", kEntries[k], sizeof(Push));
            const size_t bytes = h.size() * 4;
            LandScene s = base;
            for (int b = 0; b < BufCount; ++b)
                s.buffers[b] = r.buffer(b, bytes);
            r.buffer(BScene, sizeof(LandScene));
            r.upload(BScene, &s, sizeof s);
            r.upload(BHeight, h.heights.data(), bytes);
            r.barrier();
            fn(r);
        },
        err);
}

LandScene sceneOf(const Heightfield& h)
{
    LandScene s{};
    s.nx = uint32_t(h.nx);
    s.nz = uint32_t(h.nz);
    s.cell = float(h.cell);
    s.originX = float(h.origin.x);
    s.originZ = float(h.origin.y);
    return s;
}
} // namespace landgpu_

bool erosionFilterGpu(Heightfield& h, const ErosionFilterParams& p, std::string* err)
{
    LILA_ZONE();
    using namespace landgpu_;

    if (!ready(h, err))
        return false;
    LandScene s = sceneOf(h);
    s.scale = float(p.scale);
    s.strength = float(p.strength);
    s.lacunarity = float(p.lacunarity);
    s.gain = float(p.gain);
    s.slopeFull = float(p.slopeFull);
    s.octaves = std::max(0, p.octaves);
    s.seed = p.seed;
    const size_t n = h.size();
    if (!runJob(h, s,
                [&](gpu::Recorder& r) {
                    dispatch(r, KFilter, n);
                    r.readback(BOffset, n * 4);
                },
                err))
        return false;
    std::vector<float> offset(n);
    runner().session.read(BOffset, offset.data(), n * 4);
    std::vector<float> floors(n);
    for (size_t k = 0; k < n; ++k)
    {
        h.heights[k] += offset[k];
        floors[k] = std::max(0.0f, -offset[k]);
    }
    accumulateMask(h, "wear", floors);
    return true;
}

bool thermalMaterialGpu(Heightfield& h, const std::vector<float>& angles, int iterations, std::string* err)
{
    LILA_ZONE();
    using namespace landgpu_;

    if (!ready(h, err))
        return false;
    if (angles.size() != h.size())
    {
        if (err)
            *err = "one talus angle per sample is needed";
        return false;
    }
    const size_t n = h.size();
    std::vector<float> talus(n);
    tbb::parallel_for(size_t(0), n, [&](size_t k) { talus[k] = float(std::tan(std::clamp(double(angles[k]), 1.0, 89.0) * 0.017453292519943295) * h.cell); });
    if (!runJob(h, sceneOf(h),
                [&](gpu::Recorder& r) {
                    r.upload(BTalus, talus.data(), n * 4);
                    r.barrier();
                    for (int it = 0; it < iterations; ++it)
                    {
                        dispatch(r, KShed, n);
                        dispatch(r, KGather, n);
                        dispatch(r, KApply, n);
                    }
                    r.readback(BHeight, n * 4);
                },
                err))
        return false;
    const std::vector<float> start = h.heights;
    runner().session.read(BHeight, h.heights.data(), n * 4);
    std::vector<float> gained(n);
    tbb::parallel_for(size_t(0), n, [&](size_t k) { gained[k] = std::max(0.0f, h.heights[k] - start[k]); });
    accumulateMask(h, "talus", gained);
    return true;
}
} // namespace terrain
