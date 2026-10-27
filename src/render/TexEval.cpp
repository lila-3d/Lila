#include "render/TexEval.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "render/Compute.h"
#include "render/Renderer.h"
#include "tex/Natives.h"
#include "tex/NodeDef.h"
#include "tex/PaintStrokes.h"
#include "tex/Prelude.h"
#include "tex/SlangRt.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <numeric>
#include <set>
#include <stdexcept>

namespace lila::render::texgpu
{
namespace fs = std::filesystem;
using tex::BufferDesc;
using tex::BufferKind;
using tex::Pass;
using tex::PassKind;
using tex::Plan;

namespace
{
constexpr VkDeviceSize kTexel = 16;
/** Texels around a page region that its region externals also hold, for bilinear reads at its edges. */
constexpr VkDeviceSize kPageMargin = 2;
/** Per slice: descriptor sets and pass headers. */
constexpr uint32_t kMaxDispatches = 512;
constexpr VkDeviceSize kHeaderStride = 256;
constexpr VkShaderStageFlags kCompute = VK_SHADER_STAGE_COMPUTE_BIT;

struct PassHeader
{
    float res[4];
    float uv[4];
    /** x: the output is half floats. */
    uint32_t info[4];
};

struct StorePush
{
    VkDeviceAddress arena;
    VkDeviceAddress dst;
    uint32_t src[4][4];
    float constants[4];
    uint32_t width, height, format, srgb, words, groupsX;
    /** The rectangle written: x | y << 16 and width | height << 16. */
    uint32_t origin, extent;
};
static_assert(sizeof(StorePush) == 128);

struct DecodePush
{
    VkDeviceAddress src;
    VkDeviceAddress dst;
    uint32_t width, height, format, srgb, groupsX, dstStride, srcWidth, srcHeight;
    float scale[2];
    float origin[2];
    uint32_t dstHalf, pad0;
};
static_assert(sizeof(DecodePush) == 72);

// Vectors first: in push constants they align to 16 bytes.
struct ComposePush
{
    uint32_t srcSize[4];
    uint32_t srcMode[4];
    float constants[4];
    VkDeviceAddress dst;
    VkDeviceAddress src[4];
    uint32_t width, height, format, srgb, groupsX, words;
    float x0, y0, levelW, levelH;
};
static_assert(sizeof(ComposePush) == 128);

struct UvPush
{
    VkDeviceAddress dst;
    uint32_t width, height;
    float u0, v0;
    uint32_t half, pad;
};

struct BlurPush
{
    VkDeviceAddress src;
    VkDeviceAddress dst;
    uint32_t srcWidth, srcHeight, width, height;
    float sigma;
    int32_t radius;
    uint32_t vertical, pad0;
};

struct Kernel
{
    VkPipelineLayout layout = VK_NULL_HANDLE;
    std::vector<uint32_t> code;
    uint64_t hash = 0;
    uint32_t push = 0;
};

using Code = std::shared_ptr<const std::vector<uint32_t>>;

struct Shared
{
    std::mutex mutex;
    std::map<std::string, NativeGpuFn> natives;
    MeshMapSource meshMaps;
    std::map<int, std::function<void(const std::string&)>> listeners;
    int nextListener = 1;
    std::map<uint64_t, Code> spirv;
    size_t compiles = 0, hits = 0;
    std::vector<std::weak_ptr<Evaluator::State>> live;

    // Render thread.
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout passLayout = VK_NULL_HANDLE;
    std::map<std::string, Kernel> kernels;
};

// Leaked on purpose: evaluators may be dropped during static destruction.
Shared& shared()
{
    static Shared* s = new Shared;
    return *s;
}

VkPipeline kernel(VulkanContext& vk, const char* file, const char* entry, uint32_t push, VkPipelineLayout& layout)
{
    Kernel& k = shared().kernels[entry];
    if (!k.layout)
    {
        std::string err;
        k.code = compute::loadSpirv(file, &err);
        if (k.code.empty())
            throw std::runtime_error("texture graph GPU: " + err);
        k.hash = compute::spirvHash(k.code);
        k.push = push;
        k.layout = compute::pipelineLayout(vk, push, kCompute);
    }
    layout = k.layout;
    return compute::sharedPipelines().get(vk, k.layout, compute::layoutKey(push, kCompute), k.code, k.hash, entry);
}

void ensurePassLayout(VulkanContext& vk)
{
    Shared& s = shared();
    if (s.passLayout)
        return;
    VkDescriptorSetLayoutBinding b[4]{};
    for (uint32_t i = 0; i < 4; ++i)
    {
        b[i].binding = i;
        b[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = kCompute;
    }
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = 4;
    ci.pBindings = b;
    if (vkCreateDescriptorSetLayout(vk.device, &ci, nullptr, &s.setLayout) != VK_SUCCESS)
        throw std::runtime_error("texture graph GPU: cannot create the pass descriptor layout");
    s.passLayout = compute::pipelineLayout(vk, 0, kCompute, std::span(&s.setLayout, 1));
}

uint32_t formatCode(TextureFormat f) { return uint32_t(f); }

void dispatchLinear(VkCommandBuffer cmd, uint64_t items)
{
    const uint64_t groups = std::max<uint64_t>((items + 63) / 64, 1);
    const uint32_t gx = uint32_t(std::min<uint64_t>(groups, 65535));
    vkCmdDispatch(cmd, gx, uint32_t((groups + gx - 1) / gx), 1);
}

uint32_t groupsX(uint64_t items)
{
    return uint32_t(std::min<uint64_t>(std::max<uint64_t>((items + 63) / 64, 1), 65535));
}

/** Separable Gaussian, as the CPU's blurCpu: radius in uv (fraction of the width), wrapping. */
void blurGpu(VulkanContext& vk, VkCommandBuffer cmd, const NativeCall& c)
{
    auto it = c.inputs.find("input");
    if (it == c.inputs.end())
        return;
    const NativeBuffer& in = it->second;
    const int w = c.output.width, h = c.output.height;
    const float sigma = std::max(1e-4f, c.param("radius") * float(w) / 3.0f);
    const int r = std::min(std::max(w, h), int(std::ceil(sigma * 3.0f)));
    VkPipelineLayout layout;
    VkPipeline p = kernel(vk, "tex_blur.spv", "texBlurCS", sizeof(BlurPush), layout);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    BlurPush push{in.address, c.scratch, uint32_t(in.width), uint32_t(in.height), uint32_t(w), uint32_t(h), sigma, r, 0, 0};
    vkCmdPushConstants(cmd, layout, kCompute, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, uint32_t((w + 7) / 8), uint32_t((h + 7) / 8), 1);
    compute::memoryBarrier(cmd);
    push = {c.scratch, c.output.address, uint32_t(w), uint32_t(h), uint32_t(w), uint32_t(h), sigma, r, 1, 0};
    vkCmdPushConstants(cmd, layout, kCompute, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, uint32_t((w + 7) / 8), uint32_t((h + 7) / 8), 1);
}

NativeGpuFn findNative(const std::string& type)
{
    Shared& s = shared();
    std::scoped_lock lock(s.mutex);
    auto it = s.natives.find(type);
    return it == s.natives.end() ? NativeGpuFn() : it->second;
}

/**
 * How a native pass runs: a registered GPU function, the node's Slang kernels (tex/Natives.h), the built-in blur, or its
 * CPU function on read-back buffers (slow, for natives with no GPU form).
 */
enum class Route : uint8_t
{
    None,
    Registered,
    Kernels,
    Builtin,
    Cpu,
};

Route nativeRoute(const std::string& type, bool kernels = true)
{
    if (findNative(type))
        return Route::Registered;
    if (kernels && tex::hasNativeKernels(type) && tex::findNode(type))
        return Route::Kernels;
    if (type == "blur")
        return Route::Builtin;
    if (tex::nativeCpu(type))
        return Route::Cpu;
    return Route::None;
}

/**
 * Rewrites every OpSRem as a - (a / b) * b. Some drivers (NVIDIA 5080, seen in tests) evaluate OpSRem with a negative
 * dividend as an unsigned remainder, which breaks the wrapped cell lookups of every cellular node at the tile's edges.
 */
void rewriteSignedRemainder(std::vector<uint32_t>& code)
{
    constexpr uint32_t kSRem = 138, kSDiv = 135, kIMul = 132, kISub = 130;
    if (code.size() < 5)
        return;
    std::vector<uint32_t> out(code.begin(), code.begin() + 5);
    out.reserve(code.size() + 64);
    uint32_t bound = code[3];
    for (size_t i = 5; i < code.size();)
    {
        const uint32_t op = code[i] & 0xffffu, n = std::max<uint32_t>(code[i] >> 16, 1);
        if (op == kSRem && n == 5 && i + 5 <= code.size())
        {
            const uint32_t type = code[i + 1], result = code[i + 2], a = code[i + 3], b = code[i + 4];
            const uint32_t q = bound++, m = bound++;
            out.insert(out.end(), {(5u << 16) | kSDiv, type, q, a, b, (5u << 16) | kIMul, type, m, q, b, (5u << 16) | kISub, type, result, a, m});
        }
        else
            out.insert(out.end(), code.begin() + std::ptrdiff_t(i), code.begin() + std::ptrdiff_t(std::min(i + n, code.size())));
        i += n;
    }
    out[3] = bound;
    code = std::move(out);
}

/**
 * A fused pass as the GPU runs it: the arena and the output as 32-bit pairs, so each buffer can hold float4 texels (two
 * pairs) or half4 texels (one, descriptor flag bit 0), and the output written through texOut, which packs halves when
 * the header says so. Empty when the source isn't in the shape Plan.cpp emits; the pass then runs as written.
 */
std::string gpuSource(const std::string& source)
{
    static const std::string prelude = tex::kPrelude;
    static const std::string gpuPrelude = [] {
        std::string p = tex::kPrelude;
        auto swap = [&p](const std::string& from, const std::string& to) {
            const size_t at = p.find(from);
            if (at == std::string::npos)
                return false;
            p.replace(at, from.size(), to);
            return true;
        };
        bool ok = swap("    float4 uv;\n};", "    float4 uv;\n    uint4 info;\n};");
        ok = ok && swap("StructuredBuffer<float4> gArena;\nRWStructuredBuffer<float4> gOut;", R"(StructuredBuffer<uint2> gArena;
RWStructuredBuffer<uint2> gOut;

float4 texWords(uint2 a, uint2 b) { return float4(asfloat(a.x), asfloat(a.y), asfloat(b.x), asfloat(b.y)); }
float4 texHalves(uint2 q) { return float4(f16tof32(q.x), f16tof32(q.x >> 16), f16tof32(q.y), f16tof32(q.y >> 16)); }
void texOut(uint i, float4 v)
{
    if (gPass.info.x != 0)
        gOut[i] = uint2(f32tof16(v.x) | (f32tof16(v.y) << 16), f32tof16(v.z) | (f32tof16(v.w) << 16));
    else
    {
        gOut[2 * i] = asuint(v.xy);
        gOut[2 * i + 1] = asuint(v.zw);
    }
})");
        ok = ok && swap("float2 texel() { return gPass.res.zw; }\nfloat2 texResolution() { return gPass.res.xy; }",
                        "float2 texel() { return gPass.res.zw * gPass.uv.zw; }\nfloat2 texResolution() { return gPass.res.xy / gPass.uv.zw; }");
        // Buffer reads: half texels (descriptor flag 1), and regions (flag 2) holding part of a tile for a page, placed by
        // the gU slot in the flags' upper bits (its first texel x, y and the level's width, height), clamped at their edges.
        const size_t from = p.find("float4 texFetchBuf(uint slot, int2 p)");
        const std::string tail = "    return lerp(lerp(a, b, t.x), lerp(c, e, t.x), t.y);\n}\n";
        const size_t to = from == std::string::npos ? std::string::npos : p.find(tail, from);
        if (!ok || to == std::string::npos)
            return std::string();
        p.replace(from, to + tail.size() - from, R"(float4 texFetchAt(float4 d, uint i)
{
    if ((asuint(d.w) & 1u) != 0)
        return texHalves(gArena[asuint(d.x) + i]);
    return texWords(gArena[asuint(d.x) + 2 * i], gArena[asuint(d.x) + 2 * i + 1]);
}

float4 texFetchBuf(uint slot, int2 p)
{
    float4 d = gU[slot];
    int w = int(asuint(d.y));
    int h = int(asuint(d.z));
    uint flags = asuint(d.w);
    int x, y;
    if ((flags & 2u) != 0)
    {
        float4 t = gU[flags >> 8];
        x = clamp(p.x - int(t.x), 0, w - 1);
        y = clamp(p.y - int(t.y), 0, h - 1);
    }
    else
    {
        x = ((p.x % w) + w) % w;
        y = ((p.y % h) + h) % h;
    }
    return texFetchAt(d, uint(y * w + x));
}

float4 texReadBuf(uint slot, float2 uv)
{
    float4 d = gU[slot];
    float2 size = float2(float(asuint(d.y)), float(asuint(d.z)));
    uint flags = asuint(d.w);
    if ((flags & 2u) != 0)
        size = gU[flags >> 8].zw;
    float2 p = float2(uv.x, 1.0 - uv.y) * size - 0.5;
    float2 f = floor(p);
    float2 t = p - f;
    int2 i = int2(f);
    float4 a = texFetchBuf(slot, i);
    float4 b = texFetchBuf(slot, i + int2(1, 0));
    float4 c = texFetchBuf(slot, i + int2(0, 1));
    float4 e = texFetchBuf(slot, i + int2(1, 1));
    return lerp(lerp(a, b, t.x), lerp(c, e, t.x), t.y);
}
)");
        return p;
    }();
    if (gpuPrelude.empty())
        return {};
    const size_t at = source.find(prelude);
    const std::string store = "    gOut[id.y * w + id.x] = ";
    const size_t out = source.find(store);
    if (at == std::string::npos || out == std::string::npos)
        return {};
    const size_t end = source.find(";\n", out);
    if (end == std::string::npos)
        return {};
    std::string s = source;
    s.replace(end, 1, ");");
    s.replace(out, store.size(), "    texOut(id.y * w + id.x, ");
    s.replace(at, prelude.size(), gpuPrelude);
    return s;
}

/** SPIR-V for Slang source, compiled once per hash for the life of the process. */
Code spirvFor(const std::string& source, uint64_t hash, bool& compiled, std::string& err)
{
    Shared& s = shared();
    {
        std::scoped_lock lock(s.mutex);
        if (auto it = s.spirv.find(hash); it != s.spirv.end())
        {
            ++s.hits;
            compiled = false;
            return it->second;
        }
    }
    std::vector<uint32_t> code = tex::slangrt::spirv(source, &err);
    if (code.empty())
        return nullptr;
    rewriteSignedRemainder(code);
    auto ptr = std::make_shared<const std::vector<uint32_t>>(std::move(code));
    std::scoped_lock lock(s.mutex);
    ++s.compiles;
    compiled = true;
    return s.spirv.emplace(hash, ptr).first->second;
}

void releaseShared(VulkanContext& vk);
void installStopHook()
{
    static std::once_flag once;
    std::call_once(once, [] { Renderer::get().addStopHook([](VulkanContext& vk) { releaseShared(vk); }); });
}

/** Image parameters by path, kept while the file is unchanged so value edits don't read them again. */
struct LoadedImage
{
    fs::file_time_type time;
    std::shared_ptr<const image::Image> image;
};

/** A region of a buffer or image as fractions of its size: x right, y down (rows), [x0, x1) x [y0, y1). */
struct Rect
{
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;

    static Rect full() { return {0.0f, 0.0f, 1.0f, 1.0f}; }
    static Rect texels(const std::array<int, 4>& r, int w, int h)
    {
        return {float(r[0]) / float(w), float(r[1]) / float(h), float(r[2]) / float(w), float(r[3]) / float(h)};
    }
    bool empty() const { return x1 <= x0 || y1 <= y0; }
    Rect unite(const Rect& o) const
    {
        if (empty())
            return o;
        if (o.empty())
            return *this;
        return {std::min(x0, o.x0), std::min(y0, o.y0), std::max(x1, o.x1), std::max(y1, o.y1)};
    }
    Rect grow(float dx, float dy) const
    {
        if (empty())
            return *this;
        return {std::max(0.0f, x0 - dx), std::max(0.0f, y0 - dy), std::min(1.0f, x1 + dx), std::min(1.0f, y1 + dy)};
    }
    /** Texels [x0, x1) x [y0, y1) of a w x h grid covering it, the ends rounded out to multiples of align (and clamped). */
    std::array<int, 4> cover(int w, int h, int align = 1) const
    {
        auto lo = [&](float f, int n) { return std::clamp(int(std::floor(f * float(n))) / align * align, 0, n); };
        auto hi = [&](float f, int n) { return std::clamp((int(std::ceil(f * float(n))) + align - 1) / align * align, 0, n); };
        return {lo(x0, w), lo(y0, h), hi(x1, w), hi(y1, h)};
    }
};

/**
 * Places buffers so that those never live at once share memory: each lives from the pass that writes it (loaded ones
 * from the start) to the last pass that reads it, outputs to the end. spans are in texels; returns the texels used.
 */
VkDeviceSize packBuffers(const Plan& plan, const std::vector<VkDeviceSize>& spans, std::vector<VkDeviceSize>& offset)
{
    LILA_ZONE();
    const size_t n = plan.buffers.size();
    std::vector<int> first(n, -1), last(n, -1);
    for (size_t p = 0; p < plan.passes.size(); ++p)
    {
        const Pass& pass = plan.passes[p];
        first[size_t(pass.output)] = int(p);
        last[size_t(pass.output)] = std::max(last[size_t(pass.output)], int(p));
        for (int r : pass.reads)
            last[size_t(r)] = std::max(last[size_t(r)], int(p));
        for (const auto& [id, r] : pass.inputBuffers)
            last[size_t(r)] = std::max(last[size_t(r)], int(p));
    }
    for (const BufferDesc& b : plan.buffers)
        if (b.kind == BufferKind::Output)
            last[size_t(b.id)] = std::numeric_limits<int>::max();
    for (const auto& [name, id] : plan.outputs)
        last[size_t(id)] = std::numeric_limits<int>::max();

    std::vector<size_t> order(n);
    std::iota(order.begin(), order.end(), size_t(0));
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return spans[a] > spans[b]; });
    std::vector<size_t> placed;
    std::vector<std::pair<VkDeviceSize, VkDeviceSize>> taken;
    VkDeviceSize total = 0;
    for (size_t b : order)
    {
        taken.clear();
        for (size_t o : placed)
            if (first[o] <= last[b] && first[b] <= last[o])
                taken.emplace_back(offset[o], offset[o] + spans[o]);
        std::sort(taken.begin(), taken.end());
        VkDeviceSize at = 0;
        for (const auto& [lo, hi] : taken)
        {
            if (lo >= at + spans[b])
                break;
            at = std::max(at, hi);
        }
        offset[b] = at;
        placed.push_back(b);
        total = std::max(total, at + spans[b]);
    }
    return total;
}
} // namespace

// -------------------------------------------------------------------------------------------------------- State

/** What setPlan hands the render thread. */
struct Prepared
{
    Plan plan;
    Inputs inputs;
    std::vector<StoreTarget> stores;
    /** Per pass; null for natives. And the hash the code is cached by. */
    std::vector<Code> code;
    std::vector<uint64_t> codeHash;
    /** Per buffer: stored as half4 texels (8 bytes) instead of float4. */
    std::vector<uint8_t> half;
    /** Fused passes run as gpuSource rewrote them (else as the plan wrote them, every buffer float4). */
    bool gpuForm = false;
    /** Pages plans: per pass, run only per page; per buffer, an External that comes in per page. */
    std::vector<uint8_t> pagePass, regionExternal;
    /** Per pass: how a native runs, and for kernel natives their schedule and each step's SPIR-V and hash. */
    std::vector<Route> routes;
    std::vector<tex::NativeSchedule> schedules;
    std::vector<std::vector<std::pair<Code, uint64_t>>> stepCode;
    /** Texels the largest kernel native's scratch buffers take (at the end of the arena). */
    VkDeviceSize scratchTexels = 0;
    /** Image buffers by id. */
    std::map<int, std::shared_ptr<const image::Image>> images;
    /** Per buffer: nothing reads or writes it. */
    std::vector<uint8_t> unused;
    /** Every buffer's size, Image and External ones from their sources. */
    std::vector<std::pair<int, int>> sizes;
    /** Per store, the output buffer each channel reads (-1 for a constant). */
    std::vector<std::array<int, 4>> storeSources;
    size_t compiled = 0, cached = 0;
    std::vector<std::string> warnings;
};

struct Evaluator::State
{
    mutable std::mutex mutex;
    std::unique_ptr<Prepared> pending; // guarded
    Stats stats;                       // guarded
    std::map<std::string, LoadedImage> imageCache; // guarded
    std::atomic<bool> done{true};
    /** refreshExternals asked: compare the GPU externals' versions on the next step. */
    std::atomic<bool> refresh{false};

    // Render thread.
    std::unique_ptr<Prepared> cur;
    compute::Buffer arena, uniforms, uniformStaging, headers, scratch, words, loadRaw;
    std::vector<VkDeviceSize> offset; // texels
    std::vector<float> uniformValues;
    bool uniformsDirty = true;
    std::vector<uint8_t> dirty;      // per pass
    std::vector<uint8_t> load;       // per buffer
    std::vector<uint8_t> storeDirty; // per store
    /** What of each dirty pass, pending load and dirty store has to be redone. */
    std::vector<Rect> passRect, loadRect, storeRect;
    /** Per buffer with a pending partial load: the image version it brings the buffer up to (0: whatever is current). */
    std::vector<uint64_t> loadTarget;
    std::vector<uint64_t> loadedVersion; // per buffer: the GPU external's key and version last loaded
    std::vector<std::pair<uint64_t, uint64_t>> loadedImage; // per buffer: that image's key and version, for partial reloads
    /** The arena was made this step: nothing in it can be kept. */
    bool arenaFresh = true;
    /** Buffers share memory over their lifetimes (Inputs::compact): nothing outlives a run, so any change reruns it all. */
    bool packed = false;
    /** The most bytes the arena may take: one storage buffer binding, or Inputs::maxArenaBytes. */
    VkDeviceSize arenaLimit = 0;
    VkDeviceSize arenaTexels = 0;
    bool reallocate = true;
    VkDeviceSize scratchBase = 0;
    VkDeviceSize scratchCapacity = 0;
    /** Per-slice native frames (gU of kernel natives), bump allocated. */
    compute::Buffer frames;
    VkDeviceSize framesUsed = 0;
    /** Pages plans: where the page area starts in the arena and what one page takes (16-byte texels), and the places in it. */
    VkDeviceSize pageBase = 0, pageTexels = 0;
    std::map<int, VkDeviceSize> pageExternalAt, pageOutputAt;
    VkDescriptorPool pagePool = VK_NULL_HANDLE;
    compute::Buffer pageHeaders, pageUniforms;
    /** Texels this slice loaded, computed and stored, for Stats. */
    uint64_t sliceLoaded = 0, sliceComputed = 0, sliceStored = 0;
    /** The next pass is a CPU-route native, run between slices. */
    bool cpuNext = false;
    size_t passIndex = 0;
    /** The next row of the current pass, and the texels [passX0, passX1) x [.., passY1) it computes; set when it starts. */
    int passRow = 0;
    int passX0 = 0, passX1 = 0, passY1 = 0;
    bool passStarted = false, passBands = false;
    double rate = 4e6; // texel * cost units per ms
    VkDescriptorPool pool = VK_NULL_HANDLE;
    compute::Submitter sub;

    void release(VulkanContext& vk)
    {
        for (compute::Buffer* b : {&arena, &uniforms, &uniformStaging, &headers, &scratch, &words, &frames, &loadRaw})
            compute::destroy(vk, *b);
        if (pool)
            vkDestroyDescriptorPool(vk.device, pool, nullptr);
        pool = VK_NULL_HANDLE;
        if (pagePool)
            vkDestroyDescriptorPool(vk.device, pagePool, nullptr);
        pagePool = VK_NULL_HANDLE;
        compute::destroy(vk, pageHeaders);
        compute::destroy(vk, pageUniforms);
        sub.destroy(vk);
        // Whatever ran is gone with the arena: the next step runs everything again.
        if (cur)
        {
            dirtyAll();
            done = false;
        }
    }

    /** Marks every load, pass and store to be redone in full, from the first pass. */
    void dirtyAll()
    {
        std::fill(dirty.begin(), dirty.end(), uint8_t(1));
        std::fill(load.begin(), load.end(), uint8_t(1));
        std::fill(storeDirty.begin(), storeDirty.end(), uint8_t(1));
        std::fill(passRect.begin(), passRect.end(), Rect::full());
        std::fill(loadRect.begin(), loadRect.end(), Rect::full());
        std::fill(storeRect.begin(), storeRect.end(), Rect::full());
        std::fill(loadTarget.begin(), loadTarget.end(), uint64_t(0));
        uniformsDirty = true;
        passIndex = 0;
        passRow = 0;
        passStarted = false;
    }

    /** Pages plans: page passes never run in step, and externals that come in per page never load whole. */
    void maskPages()
    {
        if (!cur || !cur->inputs.pages)
            return;
        for (size_t p = 0; p < dirty.size(); ++p)
            if (cur->pagePass[p])
                dirty[p] = 0;
        for (size_t b = 0; b < load.size(); ++b)
            if (cur->regionExternal[b])
                load[b] = 0;
    }
    /** Bytes per texel of a buffer in the arena. */
    VkDeviceSize elemBytes(size_t b) const { return cur->half[b] ? 8 : kTexel; }
    /** Takes a pending plan and places its buffers, packing them when asked or when they'd pass maxBytes side by side. */
    void adopt(VkDeviceSize maxBytes);
    /** Loads the GPU externals whose version moved and dirties what reads them (render thread). */
    void markExternals();
    /**
     * Marks a GPU external for loading: just what changed since its last load when that is known, else all of it.
     * Returns false when it is up to date.
     */
    bool markExternal(size_t b, const GpuImageRef& img);
    /**
     * Dirties the passes reading pending loads, value slots in changed or (moved) the uv offset, and everything
     * downstream, each over the region its changed inputs reach (all of it unless it reads them at its own uv), then the
     * stores those passes feed. Unions with what was already dirty.
     */
    void propagate(const std::set<int>& changed, bool moved);
    /** Starts a pass: the rows and columns it computes, aligned for row bands. */
    void startPass(VulkanContext& vk, const Pass& p);
    void allocate(VulkanContext& vk);
    void recordLoads(VulkanContext& vk, VkCommandBuffer cmd, std::vector<compute::Buffer>& temps);
    /** Records passes until the budget is used; returns the predicted ms. */
    double recordPasses(VulkanContext& vk, VkCommandBuffer cmd, double budgetMs, Stats& delta);
    /** Records a kernel native's steps from passRow on; false when the slice filled first. */
    bool recordKernelSteps(VulkanContext& vk, VkCommandBuffer cmd, const Pass& p, double budgetMs, double& used, uint32_t& dispatches, Stats& delta);
    /** Runs a CPU-route native: reads its inputs back, runs it, uploads its output. Returns the ms spent. */
    double runCpuNative(VulkanContext& vk);
    void recordStores(VulkanContext& vk, VkCommandBuffer cmd, std::vector<compute::Buffer>& temps, Stats& delta);
    bool anyWork() const;
};

namespace
{
void releaseShared(VulkanContext& vk)
{
    Shared& s = shared();
    std::vector<std::shared_ptr<Evaluator::State>> live;
    {
        std::scoped_lock lock(s.mutex);
        for (auto& w : s.live)
            if (auto p = w.lock())
                live.push_back(std::move(p));
        s.live.clear();
        for (auto& p : live)
            s.live.push_back(p);
    }
    for (auto& st : live)
        st->release(vk);
    for (auto& [name, k] : s.kernels)
        compute::destroy(vk, k.layout);
    s.kernels.clear();
    compute::destroy(vk, s.passLayout);
    s.passLayout = VK_NULL_HANDLE;
    if (s.setLayout)
        vkDestroyDescriptorSetLayout(vk.device, s.setLayout, nullptr);
    s.setLayout = VK_NULL_HANDLE;
}

bool sameBits(const float* a, const float* b) { return std::memcmp(a, b, sizeof(float) * 4) == 0; }

bool sameImage(const image::Image& a, const image::Image& b) { return a.width == b.width && a.height == b.height && a.rgba == b.rgba; }

bool sameStores(const std::vector<StoreTarget>& a, const std::vector<StoreTarget>& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
    {
        if (a[i].image != b[i].image)
            return false;
        for (int c = 0; c < 4; ++c)
        {
            const auto &x = a[i].channels[size_t(c)], &y = b[i].channels[size_t(c)];
            if (x.output != y.output || x.component != y.component || x.constant != y.constant)
                return false;
        }
    }
    return true;
}

uint64_t versionOf(const GpuImageRef& img) { return img ? img->key() * 1000003ull ^ img->version() : 0; }
} // namespace

void Evaluator::State::adopt(VkDeviceSize maxBytes)
{
    std::unique_ptr<Prepared> next;
    {
        std::scoped_lock lock(mutex);
        next = std::move(pending);
    }
    if (!next)
        return;
    const Plan& np = next->plan;
    const size_t nPasses = np.passes.size(), nBuffers = np.buffers.size();
    const bool keep = cur && arena.buffer && cur->plan.structureHash == np.structureHash && cur->plan.width == np.width &&
                      cur->plan.height == np.height && cur->sizes == next->sizes && cur->unused == next->unused && cur->half == next->half &&
                      cur->pagePass == next->pagePass && cur->regionExternal == next->regionExternal && cur->plan.passes.size() == nPasses &&
                      next->scratchTexels <= scratchCapacity;
    std::set<int> changed;
    bool moved = false, storesChanged = false;
    std::vector<std::pair<size_t, GpuImageRef>> gpuLoads;
    if (!keep)
    {
        reallocate = true;
        load.assign(nBuffers, 1);
        dirty.assign(nPasses, 1);
        storeDirty.assign(next->stores.size(), 1);
        loadRect.assign(nBuffers, Rect::full());
        passRect.assign(nPasses, Rect::full());
        storeRect.assign(next->stores.size(), Rect::full());
        loadTarget.assign(nBuffers, 0);
        loadedVersion.assign(nBuffers, 0);
        uniformsDirty = true;
    }
    else
    {
        // Value slots that changed; descriptor slots are the evaluator's own.
        std::set<int> descriptors;
        for (const BufferDesc& b : np.buffers)
            descriptors.insert(b.slot);
        for (int s = 0; s < np.uniformSlots; ++s)
            if (!descriptors.contains(s) && !sameBits(&np.uniforms[size_t(s) * 4], &cur->plan.uniforms[size_t(s) * 4]))
                changed.insert(s);
        const Inputs &ni = next->inputs, &ci = cur->inputs;
        moved = ni.uvOffset[0] != ci.uvOffset[0] || ni.uvOffset[1] != ci.uvOffset[1];
        for (const BufferDesc& b : np.buffers)
        {
            const size_t id = size_t(b.id);
            bool whole = false;
            if (b.kind == BufferKind::Image)
                whole = b.name != cur->plan.buffers[id].name || next->images[b.id] != cur->images[b.id];
            else if (b.kind == BufferKind::External)
            {
                auto g = ni.gpuExternals.find(b.name);
                auto c = ni.cpuExternals.find(b.name);
                auto oc = ci.cpuExternals.find(b.name);
                if (g != ni.gpuExternals.end() && g->second)
                    gpuLoads.emplace_back(id, g->second);
                else if (c != ni.cpuExternals.end())
                    whole = oc == ci.cpuExternals.end() || !sameImage(c->second, oc->second);
                else
                    whole = moved || loadedVersion[id] != 0 || oc != ci.cpuExternals.end();
            }
            if (whole)
            {
                load[id] = 1;
                loadRect[id] = Rect::full();
                loadTarget[id] = 0;
            }
        }
        if (!changed.empty())
            uniformsDirty = true;
        storesChanged = !sameStores(next->stores, cur->stores);
    }
    passIndex = 0;
    passRow = 0;
    passStarted = false;

    Stats fresh;
    fresh.compiled = next->compiled;
    fresh.cached = next->cached;
    fresh.warnings = next->warnings;
    cur = std::move(next);
    if (keep)
    {
        for (auto& [id, img] : gpuLoads)
            markExternal(id, img);
        if (storesChanged)
        {
            storeDirty.assign(cur->stores.size(), 1);
            storeRect.assign(cur->stores.size(), Rect::full());
        }
        propagate(changed, moved);
    }
    maskPages();
    fresh.passesDirty = size_t(std::count(dirty.begin(), dirty.end(), uint8_t(1)));
    {
        std::scoped_lock lock(mutex);
        stats = fresh;
    }

    // Place the buffers: each starts 256-byte aligned, so row bands of a pass bind at aligned offsets.
    offset.assign(nBuffers, 0);
    std::vector<VkDeviceSize> spans(nBuffers);
    VkDeviceSize total = 0;
    for (size_t b = 0; b < nBuffers; ++b)
    {
        offset[b] = total;
        const auto [w, h] = cur->sizes[b];
        const VkDeviceSize texels = VkDeviceSize(w) * VkDeviceSize(h);
        spans[b] = ((cur->half[b] ? (texels + 1) / 2 : texels) + 15) & ~VkDeviceSize(15);
        total += spans[b];
    }
    const bool wasPacked = packed;
    arenaLimit = cur->inputs.maxArenaBytes ? std::min<VkDeviceSize>(maxBytes, cur->inputs.maxArenaBytes) : maxBytes;
    packed = !cur->inputs.pages && (cur->inputs.compact || (total + cur->scratchTexels) * kTexel > arenaLimit);
    if (packed)
        total = packBuffers(cur->plan, spans, offset);
    if (keep && packed != wasPacked)
        reallocate = true;
    if (keep && (packed || wasPacked))
    {
        dirtyAll();
        std::scoped_lock lock(mutex);
        stats.passesDirty = nPasses;
    }
    // Pages plans: room for a batch of pages after the buffers, each page its region externals (with a margin for
    // bilinear reads) and its page passes' outputs.
    pageBase = total;
    pageTexels = 0;
    pageExternalAt.clear();
    pageOutputAt.clear();
    if (cur->inputs.pages)
    {
        const VkDeviceSize side = VkDeviceSize(std::max(cur->inputs.pageSize, 8));
        for (const BufferDesc& b : cur->plan.buffers)
            if (cur->regionExternal[size_t(b.id)])
            {
                pageExternalAt[b.id] = pageTexels;
                pageTexels += ((side + 2 * kPageMargin) * (side + 2 * kPageMargin) + 15) & ~VkDeviceSize(15);
            }
        for (size_t p = 0; p < nPasses; ++p)
            if (cur->pagePass[p])
            {
                pageOutputAt[cur->plan.passes[p].output] = pageTexels;
                pageTexels += (side * side + 15) & ~VkDeviceSize(15);
            }
        total += pageTexels * Evaluator::kPagesPerBatch;
    }
    if (!keep)
        scratchCapacity = cur->scratchTexels;
    scratchBase = total;
    arenaTexels = std::max<VkDeviceSize>(total + scratchCapacity, 16);
    {
        std::scoped_lock lock(mutex);
        stats.arenaBytes = arenaTexels * kTexel;
    }
    uniformValues = cur->plan.uniforms;
    if (uniformValues.empty())
        uniformValues.assign(4, 0.0f);
    for (const BufferDesc& b : cur->plan.buffers)
    {
        // The GPU form indexes the arena in 32-bit pairs.
        const size_t id = size_t(b.id);
        const uint32_t d[4] = {uint32_t(cur->gpuForm ? offset[id] * 2 : offset[id]), uint32_t(cur->sizes[id].first), uint32_t(cur->sizes[id].second),
                               cur->half[id] ? 1u : 0u};
        std::memcpy(&uniformValues[size_t(b.slot) * 4], d, sizeof(d));
    }
    if (!keep)
        uniformsDirty = true;
}

bool Evaluator::State::markExternal(size_t b, const GpuImageRef& img)
{
    if (versionOf(img) == loadedVersion[b])
        return false;
    Rect r = Rect::full();
    uint64_t target = 0;
    if (loadedImage.size() == loadedVersion.size() && loadedImage[b].first == img->key() && !arenaFresh)
    {
        const uint64_t now = img->version();
        if (std::optional<std::array<int, 4>> t = img->rectSince(loadedImage[b].second))
        {
            r = Rect::texels(*t, img->width(), img->height());
            target = now;
        }
    }
    // A whole load already pending covers this; otherwise r holds every change since the last load.
    if (load[b] && loadTarget[b] == 0)
        return true;
    load[b] = 1;
    loadRect[b] = target ? r : Rect::full();
    loadTarget[b] = target;
    return true;
}

void Evaluator::State::markExternals()
{
    bool any = false;
    for (const BufferDesc& b : cur->plan.buffers)
        if (b.kind == BufferKind::External)
            if (auto g = cur->inputs.gpuExternals.find(b.name); g != cur->inputs.gpuExternals.end() && g->second)
                any = markExternal(size_t(b.id), g->second) || any;
    if (!any)
        return;
    if (packed)
        dirtyAll();
    else
        propagate({}, false);
    maskPages();
    passIndex = 0;
    passRow = 0;
    passStarted = false;
    std::scoped_lock lock(mutex);
    stats.passesDirty = size_t(std::count(dirty.begin(), dirty.end(), uint8_t(1)));
}

void Evaluator::State::propagate(const std::set<int>& changed, bool moved)
{
    const Plan& plan = cur->plan;
    std::vector<int> writer(plan.buffers.size(), -1);
    for (size_t p = 0; p < plan.passes.size(); ++p)
        writer[size_t(plan.passes[p].output)] = int(p);
    for (size_t p = 0; p < plan.passes.size(); ++p)
    {
        const Pass& pass = plan.passes[p];
        const auto [ow, oh] = cur->sizes[size_t(pass.output)];
        bool whole = moved && pass.kind == PassKind::Fused;
        for (int s : pass.slots)
            whole = whole || changed.contains(s);
        bool any = whole;
        Rect r;
        for (int b : pass.reads)
        {
            Rect src;
            bool srcDirty = false;
            if (load[size_t(b)])
            {
                src = loadRect[size_t(b)];
                srcDirty = true;
            }
            if (const int w = writer[size_t(b)]; w >= 0 && dirty[size_t(w)])
            {
                src = srcDirty ? src.unite(passRect[size_t(w)]) : passRect[size_t(w)];
                srcDirty = true;
            }
            if (!srcDirty)
                continue;
            any = true;
            if (pass.kind == PassKind::Fused && std::find(pass.local.begin(), pass.local.end(), b) != pass.local.end())
            {
                // Bilinear reads reach a texel of the source either side, plus the output texel's own extent.
                const auto [bw, bh] = cur->sizes[size_t(b)];
                r = r.unite(src.grow(1.5f / float(bw) + 1.0f / float(ow), 1.5f / float(bh) + 1.0f / float(oh)));
            }
            else
                whole = true;
        }
        if (!any)
            continue;
        const Rect add = whole ? Rect::full() : r;
        passRect[p] = dirty[p] ? passRect[p].unite(add) : add;
        dirty[p] = 1;
    }
    for (size_t t = 0; t < cur->stores.size(); ++t)
    {
        const StoreTarget& target = cur->stores[t];
        Rect r;
        bool any = false;
        for (int b : cur->storeSources[t])
            if (b >= 0 && writer[size_t(b)] >= 0 && dirty[size_t(writer[size_t(b)])])
            {
                r = r.unite(passRect[size_t(writer[size_t(b)])]);
                any = true;
            }
        if (!any)
            continue;
        if (target.image)
            r = r.grow(1.0f / float(target.image->width()), 1.0f / float(target.image->height()));
        storeRect[t] = storeDirty[t] ? storeRect[t].unite(r) : r;
        storeDirty[t] = 1;
    }
}

bool Evaluator::State::anyWork() const
{
    auto any = [](const std::vector<uint8_t>& v) { return std::find(v.begin(), v.end(), uint8_t(1)) != v.end(); };
    return uniformsDirty || any(load) || any(dirty) || any(storeDirty);
}

void Evaluator::State::allocate(VulkanContext& vk)
{
    ensurePassLayout(vk);
    sub.init(vk);
    if (!pool)
    {
        VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kMaxDispatches}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * kMaxDispatches}};
        VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        ci.maxSets = kMaxDispatches;
        ci.poolSizeCount = 2;
        ci.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(vk.device, &ci, nullptr, &pool) != VK_SUCCESS)
            throw std::runtime_error("texture graph GPU: cannot create a descriptor pool");
    }
    if (!frames.buffer)
        frames = compute::makeBuffer(vk, VkDeviceSize(256) << 10, 0, compute::Host::Write);
    if (!headers.buffer)
        headers = compute::makeBuffer(vk, kHeaderStride * kMaxDispatches, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, compute::Host::Write);
    if (reallocate || !arena.buffer)
    {
        const VkDeviceSize bytes = arenaTexels * kTexel;
        if (bytes > arenaLimit)
            throw ArenaTooLarge("texture graph GPU: the graph's buffers need " + std::to_string(bytes >> 20) + " MB, more than one GPU buffer can hold (" +
                                std::to_string(arenaLimit >> 20) + " MB)");
        compute::destroy(vk, arena);
        arena = compute::makeBuffer(vk, bytes, 0, compute::Host::None);
        reallocate = false;
        arenaFresh = true;
    }
    const VkDeviceSize uniformBytes = uniformValues.size() * sizeof(float);
    if (uniforms.size < uniformBytes)
    {
        compute::destroy(vk, uniforms);
        compute::destroy(vk, uniformStaging);
        uniforms = compute::makeBuffer(vk, uniformBytes, 0, compute::Host::None);
        uniformStaging = compute::makeBuffer(vk, uniformBytes, 0, compute::Host::Write);
        uniformsDirty = true;
    }
    VkDeviceSize scratchBytes = 0, wordBytes = 0;
    for (const Pass& p : cur->plan.passes)
        if (p.kind == PassKind::Native)
        {
            const auto [w, h] = cur->sizes[size_t(p.output)];
            scratchBytes = std::max(scratchBytes, VkDeviceSize(w) * VkDeviceSize(h) * kTexel);
        }
    for (const StoreTarget& t : cur->stores)
        if (t.image)
            wordBytes = std::max(wordBytes, (VkDeviceSize(t.image->width()) * VkDeviceSize(t.image->height()) * VkDeviceSize(texelBytes(t.image->format())) + 3) &
                                                ~VkDeviceSize(3));
    if (scratch.size < scratchBytes)
    {
        compute::destroy(vk, scratch);
        scratch = compute::makeBuffer(vk, scratchBytes, 0, compute::Host::None);
    }
    if (words.size < wordBytes)
    {
        compute::destroy(vk, words);
        words = compute::makeBuffer(vk, wordBytes, 0, compute::Host::None);
    }
}

void Evaluator::State::recordLoads(VulkanContext& vk, VkCommandBuffer cmd, std::vector<compute::Buffer>& temps)
{
    bool any = false;
    auto upload = [&](const image::Image& img, size_t b) {
        std::vector<uint16_t> halves;
        if (cur->half[b])
        {
            halves.resize(img.rgba.size());
            for (size_t i = 0; i < halves.size(); ++i)
                halves[i] = tex::paint::floatToHalf(img.rgba[i]);
        }
        const size_t bytes = cur->half[b] ? halves.size() * sizeof(uint16_t) : img.rgba.size() * sizeof(float);
        compute::Buffer staging = compute::hostBuffer(vk, cur->half[b] ? static_cast<const void*>(halves.data()) : img.rgba.data(), bytes);
        temps.push_back(staging);
        VkBufferCopy region{0, offset[b] * kTexel, bytes};
        vkCmdCopyBuffer(cmd, staging.buffer, arena.buffer, 1, &region);
    };
    if (loadedImage.size() != loadedVersion.size())
        loadedImage.assign(loadedVersion.size(), {0, 0});
    auto uvFill = [&](size_t b) {
        VkPipelineLayout layout;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, kernel(vk, "tex_uvfill.spv", "texUvFillCS", sizeof(UvPush), layout));
        const auto [w, h] = cur->sizes[b];
        UvPush push{arena.address + offset[b] * kTexel, uint32_t(w), uint32_t(h), cur->inputs.uvOffset[0], cur->inputs.uvOffset[1], cur->half[b], 0};
        vkCmdPushConstants(cmd, layout, kCompute, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, uint32_t((w + 7) / 8), uint32_t((h + 7) / 8), 1);
        loadedVersion[b] = 0;
        loadedImage[b] = {0, 0};
    };

    /** A GPU external's rectangle to bring in: dst in buffer texels, src in image texels, at in the raw buffer. */
    struct GpuLoad
    {
        size_t buffer = 0;
        GpuImageRef image;
        std::array<int, 4> dst{}, src{};
        VkDeviceSize at = 0;
    };
    std::vector<GpuLoad> gpuLoads;
    VkDeviceSize rawBytes = 0;
    for (const BufferDesc& d : cur->plan.buffers)
    {
        const size_t b = size_t(d.id);
        if (!load[b])
            continue;
        load[b] = 0;
        if (cur->unused[b])
            continue;
        any = true;
        if (d.kind == BufferKind::Image)
        {
            upload(*cur->images.at(d.id), b);
            continue;
        }
        if (d.kind != BufferKind::External)
            continue;
        const Inputs& in = cur->inputs;
        if (auto g = in.gpuExternals.find(d.name); g != in.gpuExternals.end() && g->second)
        {
            const GpuImageRef& img = g->second;
            // Before prepare: an update queued after it must not count as loaded.
            const uint64_t now = img->version();
            std::vector<compute::Buffer> staging;
            const bool ok = img->prepare(vk, cmd, staging);
            temps.insert(temps.end(), staging.begin(), staging.end());
            if (!ok || !img->realized())
            {
                uvFill(b);
                continue;
            }
            const auto [bw, bh] = cur->sizes[b];
            const bool partial = !arenaFresh && loadedImage[b].first == img->key() && loadTarget[b] != 0;
            const uint64_t target = partial ? loadTarget[b] : now;
            loadedImage[b] = {img->key(), target};
            loadedVersion[b] = img->key() * 1000003ull ^ target;
            GpuLoad l;
            l.buffer = b;
            l.image = img;
            l.dst = (partial ? loadRect[b] : Rect::full()).cover(bw, bh);
            if (l.dst[2] <= l.dst[0] || l.dst[3] <= l.dst[1])
                continue;
            l.src = Rect::texels(l.dst, bw, bh).cover(img->width(), img->height());
            l.at = rawBytes;
            const VkDeviceSize bytes = VkDeviceSize(l.src[2] - l.src[0]) * VkDeviceSize(l.src[3] - l.src[1]) * VkDeviceSize(texelBytes(img->format()));
            rawBytes += (bytes + 255) & ~VkDeviceSize(255);
            gpuLoads.push_back(std::move(l));
        }
        else if (auto c = in.cpuExternals.find(d.name); c != in.cpuExternals.end() && c->second.width > 0)
        {
            upload(c->second, b);
            loadedVersion[b] = 0;
            loadedImage[b] = {0, 0};
        }
        else
            uvFill(b);
    }
    if (!gpuLoads.empty())
    {
        // Earlier slices have finished (each waits), so the previous raw buffer is free to go.
        if (loadRaw.size < rawBytes)
        {
            compute::destroy(vk, loadRaw);
            loadRaw = compute::makeBuffer(vk, std::max<VkDeviceSize>(rawBytes, VkDeviceSize(4) << 20), 0, compute::Host::None);
        }
        for (const GpuLoad& l : gpuLoads)
            l.image->recordReadRect(cmd, loadRaw, l.at, l.src[0], l.src[1], l.src[2] - l.src[0], l.src[3] - l.src[1]);
        compute::memoryBarrier(cmd);
        VkPipelineLayout layout;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, kernel(vk, "tex_decode.spv", "texDecodeCS", sizeof(DecodePush), layout));
        for (const GpuLoad& l : gpuLoads)
        {
            const auto [bw, bh] = cur->sizes[l.buffer];
            const int w = l.dst[2] - l.dst[0], h = l.dst[3] - l.dst[1];
            const float sx = float(l.image->width()) / float(bw), sy = float(l.image->height()) / float(bh);
            const uint64_t texels = uint64_t(w) * uint64_t(h);
            DecodePush push{loadRaw.address + l.at,
                            arena.address + offset[l.buffer] * kTexel +
                                (VkDeviceSize(l.dst[1]) * VkDeviceSize(bw) + VkDeviceSize(l.dst[0])) * elemBytes(l.buffer),
                            uint32_t(w),
                            uint32_t(h),
                            formatCode(l.image->format()),
                            usesSrgb(l.image->format(), l.image->desc().srgb) ? 1u : 0u,
                            groupsX(texels),
                            uint32_t(bw),
                            uint32_t(l.src[2] - l.src[0]),
                            uint32_t(l.src[3] - l.src[1]),
                            {sx, sy},
                            {float(l.dst[0]) * sx - float(l.src[0]), float(l.dst[1]) * sy - float(l.src[1])},
                            cur->half[l.buffer],
                            0};
            vkCmdPushConstants(cmd, layout, kCompute, 0, sizeof(push), &push);
            dispatchLinear(cmd, texels);
            sliceLoaded += texels;
        }
    }
    if (any)
        compute::memoryBarrier(cmd);
}

void Evaluator::State::startPass(VulkanContext& vk, const Pass& p)
{
    passStarted = true;
    const size_t out = size_t(p.output);
    const auto [w, h] = cur->sizes[out];
    passX0 = 0;
    passX1 = w;
    passRow = 0;
    passY1 = h;
    passBands = false;
    const VkDeviceSize align = std::max<VkDeviceSize>(vk.properties.limits.minStorageBufferOffsetAlignment, 1);
    const VkDeviceSize rowBytes = VkDeviceSize(w) * elemBytes(out);
    // A band binds gOut at its first texel: rows of 8 keep that aligned, and columns of 8 too when the width allows.
    passBands = h % 8 == 0 && (offset[out] * kTexel) % align == 0 && (rowBytes * 8) % align == 0;
    if (!passBands)
        return;
    const std::array<int, 4> c = passRect[passIndex].cover(w, h, 8);
    passRow = c[1];
    passY1 = c[3];
    if (w % 8 == 0 && (8 * elemBytes(out)) % align == 0)
    {
        passX0 = c[0];
        passX1 = c[2];
    }
}

double Evaluator::State::recordPasses(VulkanContext& vk, VkCommandBuffer cmd, double budgetMs, Stats& delta)
{
    double used = 0.0;
    uint32_t dispatches = 0;
    const VkDeviceSize uniformBytes = uniformValues.size() * sizeof(float);
    const uint64_t passKey = compute::layoutKey(0, kCompute, "texgraph");
    const auto& passes = cur->plan.passes;
    auto finishPass = [&] {
        dirty[passIndex] = 0;
        ++delta.passesRun;
        ++passIndex;
        passRow = 0;
        passStarted = false;
        compute::memoryBarrier(cmd);
    };
    while (passIndex < passes.size())
    {
        if (!dirty[passIndex])
        {
            ++passIndex;
            passRow = 0;
            passStarted = false;
            continue;
        }
        const Pass& p = passes[passIndex];
        const size_t out = size_t(p.output);
        const auto [w, h] = cur->sizes[out];
        const double unit = std::max(p.cost, 1.0);
        if (p.kind == PassKind::Native)
        {
            const double rowMs = double(w) * unit / rate;
            const Route route = cur->routes[passIndex];
            if (route == Route::Cpu)
            {
                cpuNext = true;
                break;
            }
            if (route == Route::Kernels)
            {
                if (!recordKernelSteps(vk, cmd, p, budgetMs, used, dispatches, delta))
                    break;
                sliceComputed += uint64_t(w) * uint64_t(h);
                finishPass();
                continue;
            }
            const double ms = rowMs * double(h);
            if (used > 0.0 && used + ms > budgetMs)
                break;
            NativeGpuFn fn = route == Route::Registered ? findNative(p.nativeType) : NativeGpuFn(blurGpu);
            if (!fn)
                throw std::runtime_error("no GPU implementation of native node " + p.nativeType);
            NativeCall call{cur->plan, p, std::span<const float>(uniformValues), {}, {arena.address + offset[out] * kTexel, w, h}, scratch.address};
            for (const auto& [id, b] : p.inputBuffers)
                call.inputs[id] = {arena.address + offset[size_t(b)] * kTexel, cur->sizes[size_t(b)].first, cur->sizes[size_t(b)].second};
            compute::memoryBarrier(cmd);
            fn(vk, cmd, call);
            used += ms;
            ++delta.dispatches;
            sliceComputed += uint64_t(w) * uint64_t(h);
            finishPass();
            continue;
        }
        if (dispatches >= kMaxDispatches)
            break;
        if (!passStarted)
            startPass(vk, p);
        const int cols = passX1 - passX0;
        if (cols <= 0 || passRow >= passY1)
        {
            finishPass();
            continue;
        }
        const double rowMs = double(cols) * unit / rate;
        int rows = passY1 - passRow;
        if (passBands)
        {
            const size_t groups = compute::sliceFit(size_t(rows / 8), rowMs * 8.0, used, budgetMs, 1);
            if (groups == 0)
                break;
            rows = int(groups) * 8;
        }
        else if (used > 0.0 && used + rowMs * double(rows) > budgetMs)
            break;

        const size_t misses = compute::sharedPipelines().misses();
        VkPipeline pipe = compute::sharedPipelines().get(vk, shared().passLayout, passKey, *cur->code[passIndex], p.hash, p.entry.c_str());
        delta.pipelinesCreated += compute::sharedPipelines().misses() - misses;

        // A band covers rows [passRow, passRow + rows) of columns [passX0, passX1): gOut starts at its first texel and
        // uv shifts by as much.
        PassHeader header{{float(w), float(h), 1.0f / float(w), 1.0f / float(h)},
                          {float(double(cur->inputs.uvOffset[0]) + double(passX0) / double(w)),
                           float(double(cur->inputs.uvOffset[1]) - double(passRow) / double(h)), 1.0f, 1.0f},
                          {cur->half[out] ? 1u : 0u, 0u, 0u, 0u}};
        std::memcpy(static_cast<uint8_t*>(headers.mapped) + dispatches * kHeaderStride, &header, sizeof(header));

        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &shared().setLayout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(vk.device, &ai, &set) != VK_SUCCESS)
            throw std::runtime_error("texture graph GPU: out of descriptor sets");
        const VkDescriptorBufferInfo infos[4] = {
            {headers.buffer, dispatches * kHeaderStride, sizeof(PassHeader)},
            {uniforms.buffer, 0, uniformBytes},
            {arena.buffer, 0, VK_WHOLE_SIZE},
            {arena.buffer, offset[out] * kTexel + (VkDeviceSize(passRow) * VkDeviceSize(w) + VkDeviceSize(passX0)) * elemBytes(out),
             (VkDeviceSize(rows - 1) * VkDeviceSize(w) + VkDeviceSize(cols)) * elemBytes(out)}};
        VkWriteDescriptorSet writes[4]{};
        for (uint32_t i = 0; i < 4; ++i)
        {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(vk.device, 4, writes, 0, nullptr);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, shared().passLayout, 0, 1, &set, 0, nullptr);
        vkCmdDispatch(cmd, uint32_t((cols + 7) / 8), uint32_t((rows + 7) / 8), 1);
        ++dispatches;
        ++delta.dispatches;
        used += rowMs * double(rows);
        sliceComputed += uint64_t(cols) * uint64_t(rows);
        passRow += rows;
        if (passRow >= passY1)
            finishPass();
    }
    if (dispatches)
        vmaFlushAllocation(vk.allocator, headers.alloc, 0, VK_WHOLE_SIZE);
    return used;
}

bool Evaluator::State::recordKernelSteps(VulkanContext& vk, VkCommandBuffer cmd, const Pass& p, double budgetMs, double& used, uint32_t& dispatches,
                                         Stats& delta)
{
    const tex::NativeSchedule& sched = cur->schedules[passIndex];
    const tex::NodeDef* def = tex::findNode(p.nativeType);
    if (!def)
        throw std::runtime_error("no node type " + p.nativeType);
    std::vector<std::array<uint32_t, 3>> descs;
    for (const tex::PortDecl& in : def->inputs)
    {
        auto it = p.inputBuffers.find(in.id);
        if (it == p.inputBuffers.end())
            descs.push_back({uint32_t(scratchBase), 1u, 1u});
        else
            descs.push_back({uint32_t(offset[size_t(it->second)]), uint32_t(cur->sizes[size_t(it->second)].first),
                             uint32_t(cur->sizes[size_t(it->second)].second)});
    }
    const size_t out = size_t(p.output);
    descs.push_back({uint32_t(offset[out]), uint32_t(cur->sizes[out].first), uint32_t(cur->sizes[out].second)});
    VkDeviceSize at = scratchBase;
    for (const auto& sc : sched.scratch)
    {
        descs.push_back({uint32_t(at), uint32_t(sc[0]), uint32_t(sc[1])});
        at += (VkDeviceSize(sc[0]) * VkDeviceSize(sc[1]) + 15) & ~VkDeviceSize(15);
    }
    const std::vector<float> frame = tex::nativeFrame(*def, p, uniformValues, descs);
    const VkDeviceSize align = std::max<VkDeviceSize>(vk.properties.limits.minStorageBufferOffsetAlignment, 16);
    const VkDeviceSize frameAt = (framesUsed + align - 1) / align * align, frameBytes = frame.size() * sizeof(float);
    if (frameAt + frameBytes > frames.size)
    {
        if (framesUsed == 0)
            throw std::runtime_error("texture graph GPU: the frame of native node " + p.nativeType + " is too large");
        return false;
    }
    std::memcpy(static_cast<uint8_t*>(frames.mapped) + frameAt, frame.data(), frameBytes);
    vmaFlushAllocation(vk.allocator, frames.alloc, frameAt, frameBytes);
    framesUsed = frameAt + frameBytes;
    const uint64_t passKey = compute::layoutKey(0, kCompute, "texgraph");
    for (; passRow < int(sched.steps.size()); ++passRow)
    {
        const tex::NativeStep& st = sched.steps[size_t(passRow)];
        const double ms = double(st.threadsX) * double(st.threadsY) / rate;
        if ((used > 0.0 && used + ms > budgetMs) || dispatches >= kMaxDispatches)
            return false;
        const auto& [code, hash] = cur->stepCode[passIndex][size_t(passRow)];
        const size_t misses = compute::sharedPipelines().misses();
        VkPipeline pipe = compute::sharedPipelines().get(vk, shared().passLayout, passKey, *code, hash, "texMain");
        delta.pipelinesCreated += compute::sharedPipelines().misses() - misses;
        const auto& od = descs.at(size_t(st.output));
        PassHeader header{{float(od[1]), float(od[2]), 1.0f / float(od[1]), 1.0f / float(od[2])},
                          {st.constants[0], st.constants[1], st.constants[2], st.constants[3]},
                          {0u, 0u, 0u, 0u}};
        std::memcpy(static_cast<uint8_t*>(headers.mapped) + dispatches * kHeaderStride, &header, sizeof(header));
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &shared().setLayout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(vk.device, &ai, &set) != VK_SUCCESS)
            throw std::runtime_error("texture graph GPU: out of descriptor sets");
        const VkDescriptorBufferInfo infos[4] = {{headers.buffer, dispatches * kHeaderStride, sizeof(PassHeader)},
                                                 {frames.buffer, frameAt, frameBytes},
                                                 {arena.buffer, 0, VK_WHOLE_SIZE},
                                                 {arena.buffer, VkDeviceSize(od[0]) * kTexel, VkDeviceSize(od[1]) * VkDeviceSize(od[2]) * kTexel}};
        VkWriteDescriptorSet writes[4]{};
        for (uint32_t i = 0; i < 4; ++i)
        {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(vk.device, 4, writes, 0, nullptr);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, shared().passLayout, 0, 1, &set, 0, nullptr);
        vkCmdDispatch(cmd, uint32_t((st.threadsX + 7) / 8), uint32_t((st.threadsY + 7) / 8), 1);
        compute::memoryBarrier(cmd);
        used += ms;
        ++dispatches;
        ++delta.dispatches;
    }
    return true;
}

double Evaluator::State::runCpuNative(VulkanContext& vk)
{
    LILA_ZONE();
    const Pass& p = cur->plan.passes[passIndex];
    tex::NativeCpuFn fn = tex::nativeCpu(p.nativeType);
    if (!fn)
        throw std::runtime_error("no implementation of native node " + p.nativeType);
    const size_t out = size_t(p.output);
    const auto [w, h] = cur->sizes[out];
    VkDeviceSize total = 0;
    std::vector<VkDeviceSize> places;
    for (const auto& [id, b] : p.inputBuffers)
    {
        places.push_back(total);
        total += VkDeviceSize(cur->sizes[size_t(b)].first) * VkDeviceSize(cur->sizes[size_t(b)].second) * kTexel;
    }
    compute::Buffer host = compute::makeBuffer(vk, std::max<VkDeviceSize>(total, 16), 0, compute::Host::Read);
    compute::Buffer staging;
    double ms = 0.0;
    try
    {
        ms += sub.run(vk, [&](VkCommandBuffer cmd) {
            size_t k = 0;
            for (const auto& [id, b] : p.inputBuffers)
            {
                VkBufferCopy region{offset[size_t(b)] * kTexel, places[k++],
                                    VkDeviceSize(cur->sizes[size_t(b)].first) * VkDeviceSize(cur->sizes[size_t(b)].second) * kTexel};
                vkCmdCopyBuffer(cmd, arena.buffer, host.buffer, 1, &region);
            }
        });
        vmaInvalidateAllocation(vk.allocator, host.alloc, 0, VK_WHOLE_SIZE);
        std::vector<float> result(size_t(w) * size_t(h) * 4, 0.0f);
        tex::NativeCall call{cur->plan, p, std::span<const float>(uniformValues), {}, {result.data(), w, h}};
        size_t k = 0;
        for (const auto& [id, b] : p.inputBuffers)
        {
            float* data = reinterpret_cast<float*>(static_cast<uint8_t*>(host.mapped) + places[k++]);
            call.inputs[id] = {data, cur->sizes[size_t(b)].first, cur->sizes[size_t(b)].second};
        }
        const auto t0 = std::chrono::steady_clock::now();
        fn(call);
        ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        staging = compute::hostBuffer(vk, result.data(), result.size() * sizeof(float));
        ms += sub.run(vk, [&](VkCommandBuffer cmd) {
            VkBufferCopy region{0, offset[out] * kTexel, result.size() * sizeof(float)};
            vkCmdCopyBuffer(cmd, staging.buffer, arena.buffer, 1, &region);
        });
    }
    catch (...)
    {
        compute::destroy(vk, host);
        compute::destroy(vk, staging);
        throw;
    }
    compute::destroy(vk, host);
    compute::destroy(vk, staging);
    dirty[passIndex] = 0;
    ++passIndex;
    passRow = 0;
    cpuNext = false;
    return ms;
}

namespace
{
/** Tells the user, once per node type, that a GPU evaluation runs this native on the CPU, which stalls the viewport. */
void warnCpuRoute(const std::string& type, const std::string& why)
{
    static std::mutex mutex;
    static std::set<std::string> warned;
    {
        std::scoped_lock lock(mutex);
        if (!warned.insert(type).second)
            return;
    }
    log::warn("texture graph: {} runs on the CPU, not the GPU, so texture sets using it are slow and stall the viewport ({})", type, why);
}

/** Picks a native pass's route and, for kernels, compiles its steps. Empty or why it can't run. */
std::string prepareNative(Prepared& prep, const Plan& plan, size_t i)
{
    const Pass& p = plan.passes[i];
    Route route = nativeRoute(p.nativeType);
    std::string cpuWhy = "it has no GPU form";
    if (route == Route::Kernels)
    {
        const BufferDesc& out = plan.buffers.at(size_t(p.output));
        auto param = [&](const std::string& id, int c) {
            auto it = p.paramSlots.find(id);
            return it == p.paramSlots.end() ? 0.0f : plan.uniforms[size_t(it->second) * 4 + size_t(c)];
        };
        tex::NativeSchedule sched = tex::nativeSchedule(p.nativeType, out.width > 0 ? out.width : plan.width, out.height > 0 ? out.height : plan.height, param);
        std::vector<std::pair<Code, uint64_t>> steps;
        for (const tex::NativeStep& st : sched.steps)
        {
            std::string err;
            const std::string source = tex::nativeKernelSource(sched.file, st.entry, &err);
            if (source.empty())
            {
                cpuWhy = err;
                steps.clear();
                break;
            }
            const uint64_t hash = tex::hash64(source);
            bool compiled = false;
            Code code = spirvFor(source, hash, compiled, err);
            if (!code)
                return "native kernel " + sched.file + "." + st.entry + " failed to compile:\n" + err;
            ++(compiled ? prep.compiled : prep.cached);
            steps.emplace_back(std::move(code), hash);
        }
        if (steps.empty())
            route = nativeRoute(p.nativeType, false);
        else
        {
            VkDeviceSize texels = 0;
            for (const auto& sc : sched.scratch)
                texels += (VkDeviceSize(sc[0]) * VkDeviceSize(sc[1]) + 15) & ~VkDeviceSize(15);
            prep.scratchTexels = std::max(prep.scratchTexels, texels);
            prep.schedules[i] = std::move(sched);
            prep.stepCode[i] = std::move(steps);
        }
    }
    if (route == Route::None)
        return "no implementation of native node " + p.nativeType;
    if (route == Route::Cpu)
        warnCpuRoute(p.nativeType, cpuWhy);
    prep.routes[i] = route;
    return {};
}
} // namespace

void Evaluator::State::recordStores(VulkanContext& vk, VkCommandBuffer cmd, std::vector<compute::Buffer>& temps, Stats& delta)
{
    for (size_t t = 0; t < cur->stores.size(); ++t)
    {
        if (!storeDirty[t])
            continue;
        storeDirty[t] = 0;
        const StoreTarget& target = cur->stores[t];
        const GpuImageRef& img = target.image;
        if (!img)
            continue;
        std::vector<compute::Buffer> staging;
        const bool ok = img->prepare(vk, cmd, staging);
        temps.insert(temps.end(), staging.begin(), staging.end());
        if (!ok)
            continue;
        const std::array<int, 4> r = storeRect[t].cover(img->width(), img->height());
        const int rw = r[2] - r[0], rh = r[3] - r[1];
        if (rw <= 0 || rh <= 0)
            continue;
        const uint64_t texels = uint64_t(rw) * uint64_t(rh);
        const uint64_t nWords = (texels * uint64_t(texelBytes(img->format())) + 3) / 4;
        StorePush push{};
        push.arena = arena.address;
        push.dst = words.address;
        for (size_t c = 0; c < 4; ++c)
        {
            const int b = cur->storeSources[t][c];
            if (b < 0)
            {
                push.src[c][0] = 0;
                push.src[c][1] = push.src[c][2] = 1;
                push.src[c][3] = 4;
                push.constants[c] = target.channels[c].constant;
                continue;
            }
            push.src[c][0] = uint32_t(offset[size_t(b)] * 2);
            push.src[c][1] = uint32_t(cur->sizes[size_t(b)].first);
            push.src[c][2] = uint32_t(cur->sizes[size_t(b)].second);
            push.src[c][3] = uint32_t(std::clamp(target.channels[c].component, 0, 3)) | (cur->half[size_t(b)] ? 0x100u : 0u);
        }
        push.width = uint32_t(img->width());
        push.height = uint32_t(img->height());
        push.format = formatCode(img->format());
        push.srgb = usesSrgb(img->format(), img->desc().srgb) ? 1u : 0u;
        push.words = uint32_t(nWords);
        push.groupsX = groupsX(nWords);
        push.origin = uint32_t(r[0]) | (uint32_t(r[1]) << 16);
        push.extent = uint32_t(rw) | (uint32_t(rh) << 16);
        VkPipelineLayout layout;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, kernel(vk, "tex_store.spv", "texStoreCS", sizeof(StorePush), layout));
        vkCmdPushConstants(cmd, layout, kCompute, 0, sizeof(push), &push);
        dispatchLinear(cmd, nWords);
        compute::memoryBarrier(cmd);
        img->beginWrite(cmd);
        compute::memoryBarrier(cmd);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageOffset = {r[0], r[1], 0};
        region.imageExtent = {uint32_t(rw), uint32_t(rh), 1};
        vkCmdCopyBufferToImage(cmd, words.buffer, img->image(), VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        compute::memoryBarrier(cmd);
        img->noteWriteRect(r[0], r[1], r[2], r[3]);
        img->endWrite(cmd);
        sliceStored += texels;
        ++delta.stores;
    }
}

// -------------------------------------------------------------------------------------------------------- pages

bool Evaluator::pagesReady() const
{
    const State& st = *m;
    std::scoped_lock lock(st.mutex);
    return st.cur && st.cur->inputs.pages && !st.pending && st.arena.buffer && !st.anyWork() && !st.arenaFresh;
}

size_t Evaluator::recordPages(VulkanContext& vk, VkCommandBuffer cmd, std::span<const PageJob> jobs, std::span<const PageLayer> layers,
                              const RegionLoader& load, std::vector<compute::Buffer>& temps)
{
    LILA_ZONE();
    State& st = *m;
    if (jobs.empty() || !pagesReady())
        return 0;
    const Prepared& c = *st.cur;
    const Plan& plan = c.plan;
    const size_t n = std::min(jobs.size(), kPagesPerBatch);
    const int side = std::max(c.inputs.pageSize, 8);
    for (size_t j = 0; j < n; ++j)
        if (jobs[j].width > side || jobs[j].height > side || jobs[j].width < 1 || jobs[j].height < 1 || jobs[j].layers.size() != layers.size())
            throw std::runtime_error("texture graph pages: a page doesn't fit the plan's page size or layers");

    // Every layer channel's source: a page pass's output (per page) or a buffer the base passes made (sampled whole).
    struct Source
    {
        int buffer = -1;
        bool page = false;
    };
    std::vector<std::array<Source, 4>> sources(layers.size());
    for (size_t l = 0; l < layers.size(); ++l)
        for (size_t ch = 0; ch < 4; ++ch)
        {
            const std::string& name = layers[l].channels[ch].output;
            if (name.empty())
                continue;
            auto it = plan.outputs.find(name);
            if (it == plan.outputs.end())
                throw std::runtime_error("the plan has no output " + name);
            sources[l][ch] = {it->second, st.pageOutputAt.contains(it->second)};
        }

    ensurePassLayout(vk);
    if (!st.pagePool)
    {
        VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kMaxDispatches}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * kMaxDispatches}};
        VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        ci.maxSets = kMaxDispatches;
        ci.poolSizeCount = 2;
        ci.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(vk.device, &ci, nullptr, &st.pagePool) != VK_SUCCESS)
            throw std::runtime_error("texture graph GPU: cannot create a descriptor pool");
    }
    vkResetDescriptorPool(vk.device, st.pagePool, 0);
    if (!st.pageHeaders.buffer)
        st.pageHeaders = compute::makeBuffer(vk, kHeaderStride * kMaxDispatches, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, compute::Host::Write);

    // Per page uniforms: the plan's, with the region externals' descriptors pointing at the page's regions, each
    // placed by a slot appended after the plan's.
    std::vector<int> regions;
    for (const BufferDesc& b : plan.buffers)
        if (c.regionExternal[size_t(b.id)])
            regions.push_back(b.id);
    const VkDeviceSize align = std::max<VkDeviceSize>(vk.properties.limits.minStorageBufferOffsetAlignment, 16);
    const VkDeviceSize uniformBytes = (st.uniformValues.size() + regions.size() * 4) * sizeof(float);
    const VkDeviceSize uniformStride = (uniformBytes + align - 1) / align * align;
    if (st.pageUniforms.size < uniformStride * n)
    {
        compute::destroy(vk, st.pageUniforms);
        st.pageUniforms = compute::makeBuffer(vk, uniformStride * kPagesPerBatch, 0, compute::Host::Write);
    }
    const int slots = int(st.uniformValues.size() / 4);
    for (size_t j = 0; j < n; ++j)
    {
        const PageJob& job = jobs[j];
        std::vector<float> u = st.uniformValues;
        u.resize(u.size() + regions.size() * 4, 0.0f);
        const VkDeviceSize base = st.pageBase + VkDeviceSize(j) * st.pageTexels;
        for (size_t k = 0; k < regions.size(); ++k)
        {
            const BufferDesc& b = plan.buffers[size_t(regions[k])];
            const uint32_t rw = uint32_t(job.width + 2 * int(kPageMargin)), rh = uint32_t(job.height + 2 * int(kPageMargin));
            const uint32_t d[4] = {uint32_t((base + st.pageExternalAt.at(b.id)) * 2), rw, rh, 2u | (uint32_t(slots + int(k)) << 8)};
            std::memcpy(&u[size_t(b.slot) * 4], d, sizeof(d));
            // The region's first texel in the tile's uv space as texReadBuf sees it (UDIM offset included).
            float* t = &u[size_t(slots + int(k)) * 4];
            t[0] = float(double(c.inputs.uvOffset[0]) * job.levelWidth + job.x0 - int(kPageMargin));
            t[1] = float(-double(c.inputs.uvOffset[1]) * job.levelHeight + job.y0 - int(kPageMargin));
            t[2] = float(job.levelWidth);
            t[3] = float(job.levelHeight);
        }
        std::memcpy(static_cast<uint8_t*>(st.pageUniforms.mapped) + uniformStride * j, u.data(), u.size() * sizeof(float));
    }
    vmaFlushAllocation(vk.allocator, st.pageUniforms.alloc, 0, VK_WHOLE_SIZE);

    // Regions of the externals, then the page passes, then the layers.
    for (size_t j = 0; j < n; ++j)
    {
        const PageJob& job = jobs[j];
        const VkDeviceSize base = st.pageBase + VkDeviceSize(j) * st.pageTexels;
        for (int b : regions)
        {
            const int m = int(kPageMargin);
            const std::array<int, 4> rect{job.x0 - m, job.y0 - m, job.x0 + job.width + m, job.y0 + job.height + m};
            const VkDeviceSize at = (base + st.pageExternalAt.at(b)) * kTexel;
            const uint32_t stride = uint32_t(job.width + 2 * m);
            if (!load || !load(vk, cmd, plan.buffers[size_t(b)].name, job.levelWidth, job.levelHeight, rect, st.arena.address + at, stride))
                vkCmdFillBuffer(cmd, st.arena.buffer, at, VkDeviceSize(stride) * VkDeviceSize(job.height + 2 * m) * kTexel, 0);
            st.sliceLoaded += uint64_t(stride) * uint64_t(job.height + 2 * m);
        }
    }
    compute::memoryBarrier(cmd);
    const uint64_t passKey = compute::layoutKey(0, kCompute, "texgraph");
    uint32_t dispatches = 0;
    for (size_t j = 0; j < n; ++j)
    {
        const PageJob& job = jobs[j];
        const VkDeviceSize base = st.pageBase + VkDeviceSize(j) * st.pageTexels;
        for (size_t p = 0; p < plan.passes.size(); ++p)
        {
            if (!c.pagePass[p])
                continue;
            if (dispatches >= kMaxDispatches)
                throw std::runtime_error("texture graph pages: too many passes in one batch");
            const Pass& pass = plan.passes[p];
            VkPipeline pipe = compute::sharedPipelines().get(vk, shared().passLayout, passKey, *c.code[p], c.codeHash[p], pass.entry.c_str());
            const double lw = double(job.levelWidth), lh = double(job.levelHeight);
            PassHeader header{{float(job.width), float(job.height), 1.0f / float(job.width), 1.0f / float(job.height)},
                              {float(double(c.inputs.uvOffset[0]) + double(job.x0) / lw),
                               float(double(c.inputs.uvOffset[1]) + 1.0 - double(job.y0 + job.height) / lh), float(double(job.width) / lw),
                               float(double(job.height) / lh)},
                              {0u, 0u, 0u, 0u}};
            std::memcpy(static_cast<uint8_t*>(st.pageHeaders.mapped) + dispatches * kHeaderStride, &header, sizeof(header));
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = st.pagePool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &shared().setLayout;
            VkDescriptorSet set = VK_NULL_HANDLE;
            if (vkAllocateDescriptorSets(vk.device, &ai, &set) != VK_SUCCESS)
                throw std::runtime_error("texture graph GPU: out of descriptor sets");
            const VkDescriptorBufferInfo infos[4] = {
                {st.pageHeaders.buffer, dispatches * kHeaderStride, sizeof(PassHeader)},
                {st.pageUniforms.buffer, uniformStride * j, uniformBytes},
                {st.arena.buffer, 0, VK_WHOLE_SIZE},
                {st.arena.buffer, (base + st.pageOutputAt.at(pass.output)) * kTexel, VkDeviceSize(job.width) * VkDeviceSize(job.height) * kTexel}};
            VkWriteDescriptorSet writes[4]{};
            for (uint32_t i = 0; i < 4; ++i)
            {
                writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet = set;
                writes[i].dstBinding = i;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[i].pBufferInfo = &infos[i];
            }
            vkUpdateDescriptorSets(vk.device, 4, writes, 0, nullptr);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, shared().passLayout, 0, 1, &set, 0, nullptr);
            vkCmdDispatch(cmd, uint32_t((job.width + 7) / 8), uint32_t((job.height + 7) / 8), 1);
            ++dispatches;
            st.sliceComputed += uint64_t(job.width) * uint64_t(job.height);
        }
    }
    if (dispatches)
        vmaFlushAllocation(vk.allocator, st.pageHeaders.alloc, 0, VK_WHOLE_SIZE);
    compute::memoryBarrier(cmd);

    VkPipelineLayout layout;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, kernel(vk, "tex_page.spv", "texComposeCS", sizeof(ComposePush), layout));
    for (size_t j = 0; j < n; ++j)
    {
        const PageJob& job = jobs[j];
        const VkDeviceSize base = st.pageBase + VkDeviceSize(j) * st.pageTexels;
        for (size_t l = 0; l < layers.size(); ++l)
        {
            ComposePush push{};
            push.dst = job.layers[l];
            for (size_t ch = 0; ch < 4; ++ch)
            {
                const Source& src = sources[l][ch];
                push.constants[ch] = layers[l].channels[ch].constant;
                if (src.buffer < 0)
                    continue;
                const uint32_t comp = uint32_t(std::clamp(layers[l].channels[ch].component, 0, 3));
                if (src.page)
                {
                    push.src[ch] = st.arena.address + (base + st.pageOutputAt.at(src.buffer)) * kTexel;
                    push.srcSize[ch] = uint32_t(job.width) | (uint32_t(job.height) << 16);
                    push.srcMode[ch] = comp;
                }
                else
                {
                    const auto [bw, bh] = c.sizes[size_t(src.buffer)];
                    push.src[ch] = st.arena.address + st.offset[size_t(src.buffer)] * kTexel;
                    push.srcSize[ch] = uint32_t(bw) | (uint32_t(bh) << 16);
                    push.srcMode[ch] = comp | 0x200u | (c.half[size_t(src.buffer)] ? 0x100u : 0u);
                }
            }
            const uint64_t texels = uint64_t(job.width) * uint64_t(job.height);
            const uint64_t words = (texels * uint64_t(texelBytes(layers[l].format)) + 3) / 4;
            push.width = uint32_t(job.width);
            push.height = uint32_t(job.height);
            push.format = formatCode(layers[l].format);
            push.srgb = usesSrgb(layers[l].format, layers[l].srgb) ? 1u : 0u;
            push.groupsX = groupsX(words);
            push.words = uint32_t(words);
            push.x0 = float(job.x0);
            push.y0 = float(job.y0);
            push.levelW = float(job.levelWidth);
            push.levelH = float(job.levelHeight);
            vkCmdPushConstants(cmd, layout, kCompute, 0, sizeof(push), &push);
            dispatchLinear(cmd, words);
            st.sliceStored += texels;
        }
    }
    compute::memoryBarrier(cmd);
    {
        std::scoped_lock lock(st.mutex);
        st.stats.texelsLoaded += st.sliceLoaded;
        st.stats.texelsComputed += st.sliceComputed;
        st.stats.texelsStored += st.sliceStored;
    }
    st.sliceLoaded = st.sliceComputed = st.sliceStored = 0;
    (void)temps;
    return n;
}

// -------------------------------------------------------------------------------------------------------- Evaluator

Evaluator::Evaluator() : m(std::make_shared<State>())
{
    installStopHook();
    Shared& s = shared();
    std::scoped_lock lock(s.mutex);
    std::erase_if(s.live, [](const std::weak_ptr<State>& w) { return w.expired(); });
    s.live.push_back(m);
}

Evaluator::~Evaluator()
{
    Renderer& r = Renderer::get();
    if (r.running())
        r.runTask([st = m](VulkanContext& vk) {
            st->release(vk);
            return true;
        });
}

namespace
{
std::shared_ptr<const image::Image> blackImage()
{
    static auto black = std::make_shared<const image::Image>(image::Image{1, 1, {0.0f, 0.0f, 0.0f, 1.0f}});
    return black;
}
} // namespace

std::string Evaluator::setPlan(Plan plan, Inputs inputs, std::vector<StoreTarget> stores)
{
    LILA_ZONE();
    if (!plan.error.empty())
        return plan.error;
    if (plan.passes.empty())
        return "the plan has no passes";
    auto prep = std::make_unique<Prepared>();
    prep->code.resize(plan.passes.size());
    prep->codeHash.resize(plan.passes.size());
    std::vector<std::string> gpuForms(plan.passes.size());
    prep->gpuForm = true;
    for (size_t i = 0; i < plan.passes.size() && prep->gpuForm; ++i)
        if (plan.passes[i].kind == PassKind::Fused)
        {
            gpuForms[i] = gpuSource(plan.passes[i].source);
            prep->gpuForm = !gpuForms[i].empty();
        }
    prep->routes.assign(plan.passes.size(), Route::None);
    prep->schedules.resize(plan.passes.size());
    prep->stepCode.resize(plan.passes.size());
    for (size_t i = 0; i < plan.passes.size(); ++i)
    {
        const Pass& p = plan.passes[i];
        if (p.kind == PassKind::Native)
        {
            if (std::string err = prepareNative(*prep, plan, i); !err.empty())
                return err;
            continue;
        }
        bool compiled = false;
        std::string err;
        prep->codeHash[i] = prep->gpuForm ? tex::hash64(gpuForms[i]) : p.hash;
        prep->code[i] = spirvFor(prep->gpuForm ? gpuForms[i] : p.source, prep->codeHash[i], compiled, err);
        if (!prep->code[i])
            return "pass for " + p.node + " failed to compile:\n" + err;
        ++(compiled ? prep->compiled : prep->cached);
    }

    // Buffers no pass touches and no output names (those of outputs another evaluation makes) take no room and load nothing.
    prep->unused.assign(plan.buffers.size(), 1);
    for (const Pass& p : plan.passes)
    {
        prep->unused[size_t(p.output)] = 0;
        for (int r : p.reads)
            prep->unused[size_t(r)] = 0;
        for (const auto& [id, r] : p.inputBuffers)
            prep->unused[size_t(r)] = 0;
    }
    for (const auto& [name, id] : plan.outputs)
        prep->unused[size_t(id)] = 0;

    std::vector<std::string> warnings;
    prep->sizes.resize(plan.buffers.size());
    for (const BufferDesc& b : plan.buffers)
    {
        if (prep->unused[size_t(b.id)])
        {
            prep->sizes[size_t(b.id)] = {1, 1};
            continue;
        }
        std::pair<int, int> size{b.width, b.height};
        if (b.kind == BufferKind::Image)
        {
            std::shared_ptr<const image::Image> img = blackImage();
            if (!b.name.empty())
            {
                const fs::path path = tex::resolveImagePath(b.name, inputs.imageRoot);
                std::error_code ec;
                const fs::file_time_type time = fs::last_write_time(path, ec);
                const std::string key = path.string();
                std::shared_ptr<const image::Image> cached;
                {
                    std::scoped_lock lock(m->mutex);
                    if (auto it = m->imageCache.find(key); it != m->imageCache.end() && !ec && it->second.time == time)
                        cached = it->second.image;
                }
                if (cached)
                    img = cached;
                else
                {
                    std::string err;
                    image::Image read = image::read(path, &err);
                    if (read.width > 0)
                    {
                        img = std::make_shared<const image::Image>(std::move(read));
                        std::scoped_lock lock(m->mutex);
                        m->imageCache[key] = {time, img};
                    }
                    else
                        warnings.push_back("image " + b.name + ": " + (err.empty() ? "can't read" : err));
                }
            }
            prep->images[b.id] = img;
            size = {img->width, img->height};
        }
        else if (b.kind == BufferKind::External)
        {
            size = {plan.width, plan.height};
            // A GPU image larger than the plan (a preview of a big tile) loads at the plan's size. CPU texels upload
            // as they are, so they keep their own size, as on the CPU.
            if (auto g = inputs.gpuExternals.find(b.name); g != inputs.gpuExternals.end() && g->second)
            {
                if (g->second->width() < plan.width || g->second->height() < plan.height)
                    size = {g->second->width(), g->second->height()};
            }
            else if (auto c = inputs.cpuExternals.find(b.name); c != inputs.cpuExternals.end() && c->second.width > 0)
                size = {c->second.width, c->second.height};
        }
        if (size.first < 1 || size.second < 1)
            size = {plan.width, plan.height};
        prep->sizes[size_t(b.id)] = size;
    }

    // Half texels for buffers that only fused passes touch and that don't need float precision: CPU externals and uv
    // fills (missing mesh maps) hold UDIM coordinates and 32-bit maps, so they stay float.
    prep->half.assign(plan.buffers.size(), 0);
    if (prep->gpuForm && inputs.halfBuffers)
    {
        std::vector<uint8_t> native(plan.buffers.size(), 0);
        for (const Pass& p : plan.passes)
            if (p.kind == PassKind::Native)
            {
                native[size_t(p.output)] = 1;
                for (int r : p.reads)
                    native[size_t(r)] = 1;
            }
        for (const BufferDesc& b : plan.buffers)
        {
            bool h = b.format != "rgba32f" && !native[size_t(b.id)];
            if (b.kind == BufferKind::External)
            {
                auto g = inputs.gpuExternals.find(b.name);
                h = h && g != inputs.gpuExternals.end() && g->second && g->second->format() != TextureFormat::R32F;
            }
            prep->half[size_t(b.id)] = h ? 1 : 0;
        }
    }

    prep->pagePass.assign(plan.passes.size(), 0);
    prep->regionExternal.assign(plan.buffers.size(), 0);
    if (inputs.pages)
    {
        if (!prep->gpuForm)
            return "the plan's passes can't run per page";
        if (!stores.empty())
            return "a pages plan has no stores";
        std::vector<int> readers(plan.buffers.size(), 0);
        for (const Pass& p : plan.passes)
            for (int r : p.reads)
                ++readers[size_t(r)];
        for (size_t i = 0; i < plan.passes.size(); ++i)
        {
            const Pass& p = plan.passes[i];
            prep->pagePass[i] = p.kind == PassKind::Fused && plan.buffers[size_t(p.output)].kind == BufferKind::Output && readers[size_t(p.output)] == 0;
        }
        // An External every reader of is a page pass reading it at its own uv.
        for (const BufferDesc& b : plan.buffers)
        {
            if (b.kind != BufferKind::External)
                continue;
            bool region = readers[size_t(b.id)] > 0;
            for (size_t i = 0; i < plan.passes.size() && region; ++i)
            {
                const Pass& p = plan.passes[i];
                if (std::find(p.reads.begin(), p.reads.end(), b.id) == p.reads.end())
                    continue;
                region = prep->pagePass[i] && std::find(p.local.begin(), p.local.end(), b.id) != p.local.end();
            }
            prep->regionExternal[size_t(b.id)] = region;
        }
        for (size_t b = 0; b < plan.buffers.size(); ++b)
            if (prep->regionExternal[b])
            {
                prep->sizes[b] = {1, 1};
                prep->half[b] = 0;
            }
        for (size_t i = 0; i < plan.passes.size(); ++i)
            if (prep->pagePass[i])
            {
                prep->sizes[size_t(plan.passes[i].output)] = {1, 1};
                prep->half[size_t(plan.passes[i].output)] = 0;
            }
    }

    prep->storeSources.resize(stores.size());
    for (size_t t = 0; t < stores.size(); ++t)
        for (size_t c = 0; c < 4; ++c)
        {
            const std::string& name = stores[t].channels[c].output;
            if (name.empty())
            {
                prep->storeSources[t][c] = -1;
                continue;
            }
            auto it = plan.outputs.find(name);
            if (it == plan.outputs.end())
                return "the plan has no output " + name;
            prep->storeSources[t][c] = it->second;
        }

    prep->plan = std::move(plan);
    prep->inputs = std::move(inputs);
    prep->stores = std::move(stores);
    prep->warnings = std::move(warnings);
    std::scoped_lock lock(m->mutex);
    m->pending = std::move(prep);
    m->done = false;
    return {};
}

bool Evaluator::step(VulkanContext& vk, double budgetMs)
{
    LILA_ZONE();
    State& st = *m;
    st.adopt(vk.properties.limits.maxStorageBufferRange);
    if (st.cur && st.refresh.exchange(false))
        st.markExternals();
    auto pendingNow = [&] {
        std::scoped_lock lock(st.mutex);
        return st.pending != nullptr;
    };
    if (!st.cur || !st.anyWork())
    {
        st.done = !pendingNow();
        return st.done;
    }
    st.allocate(vk);
    vkResetDescriptorPool(vk.device, st.pool, 0);
    st.framesUsed = 0;
    st.cpuNext = false;
    st.sliceLoaded = st.sliceComputed = st.sliceStored = 0;
    std::vector<compute::Buffer> temps;
    Stats delta;
    double predicted = 0.0, ms = 0.0;
    try
    {
        ms = st.sub.run(vk, [&](VkCommandBuffer cmd) {
            st.recordLoads(vk, cmd, temps);
            st.arenaFresh = false;
            if (st.uniformsDirty)
            {
                std::memcpy(st.uniformStaging.mapped, st.uniformValues.data(), st.uniformValues.size() * sizeof(float));
                vmaFlushAllocation(vk.allocator, st.uniformStaging.alloc, 0, VK_WHOLE_SIZE);
                compute::copy(cmd, st.uniformStaging, st.uniforms, st.uniformValues.size() * sizeof(float));
                st.uniformsDirty = false;
            }
            compute::memoryBarrier(cmd);
            predicted = st.recordPasses(vk, cmd, budgetMs, delta);
            if (st.passIndex >= st.cur->plan.passes.size())
                st.recordStores(vk, cmd, temps, delta);
        });
    }
    catch (...)
    {
        for (compute::Buffer& b : temps)
            compute::destroy(vk, b);
        throw;
    }
    for (compute::Buffer& b : temps)
        compute::destroy(vk, b);
    if (predicted > 1.0)
        st.rate = compute::adaptRate(st.rate, predicted, ms);
    if (st.cpuNext)
    {
        ms += st.runCpuNative(vk);
        delta.passesRun += 1;
    }
    {
        std::scoped_lock lock(st.mutex);
        st.stats.passesRun += delta.passesRun;
        st.stats.dispatches += delta.dispatches;
        st.stats.stores += delta.stores;
        st.stats.pipelinesCreated += delta.pipelinesCreated;
        st.stats.slices += 1;
        st.stats.gpuMs += ms;
        st.stats.texelsLoaded += st.sliceLoaded;
        st.stats.texelsComputed += st.sliceComputed;
        st.stats.texelsStored += st.sliceStored;
    }
    st.done = !st.anyWork() && !pendingNow();
    return st.done;
}

void Evaluator::refreshExternals()
{
    m->refresh = true;
    m->done = false;
}

bool Evaluator::idle() const
{
    std::scoped_lock lock(m->mutex);
    return !m->pending && m->done;
}

Stats Evaluator::stats() const
{
    std::scoped_lock lock(m->mutex);
    return m->stats;
}

std::map<std::string, image::Image> Evaluator::readOutputs()
{
    Renderer& r = Renderer::get();
    if (!r.running())
        throw std::runtime_error("texture graph GPU: the renderer is not running");
    std::map<std::string, image::Image> out;
    std::shared_ptr<State> st = m;
    r.runTask([&out, st](VulkanContext& vk) {
         if (!st->cur || !st->arena.buffer)
             return true;
         VkDeviceSize total = 0;
         std::vector<std::pair<std::string, VkDeviceSize>> places;
         for (const auto& [name, id] : st->cur->plan.outputs)
         {
             places.emplace_back(name, total);
             const auto [w, h] = st->cur->sizes[size_t(id)];
             total += VkDeviceSize(w) * VkDeviceSize(h) * st->elemBytes(size_t(id));
         }
         compute::Buffer host = compute::makeBuffer(vk, total, 0, compute::Host::Read);
         try
         {
             st->sub.run(vk, [&](VkCommandBuffer cmd) {
                 for (const auto& [name, at] : places)
                 {
                     const int id = st->cur->plan.outputs.at(name);
                     const auto [w, h] = st->cur->sizes[size_t(id)];
                     VkBufferCopy region{st->offset[size_t(id)] * kTexel, at, VkDeviceSize(w) * VkDeviceSize(h) * st->elemBytes(size_t(id))};
                     vkCmdCopyBuffer(cmd, st->arena.buffer, host.buffer, 1, &region);
                 }
             });
             vmaInvalidateAllocation(vk.allocator, host.alloc, 0, VK_WHOLE_SIZE);
             for (const auto& [name, at] : places)
             {
                 const size_t id = size_t(st->cur->plan.outputs.at(name));
                 const auto [w, h] = st->cur->sizes[id];
                 image::Image img{w, h, std::vector<float>(size_t(w) * size_t(h) * 4)};
                 const uint8_t* src = static_cast<const uint8_t*>(host.mapped) + at;
                 if (st->cur->half[id])
                     for (size_t i = 0; i < img.rgba.size(); ++i)
                     {
                         uint16_t h16;
                         std::memcpy(&h16, src + i * 2, 2);
                         img.rgba[i] = tex::paint::halfToFloat(h16);
                     }
                 else
                     std::memcpy(img.rgba.data(), src, img.rgba.size() * sizeof(float));
                 out[name] = std::move(img);
             }
         }
         catch (...)
         {
             compute::destroy(vk, host);
             throw;
         }
         compute::destroy(vk, host);
         return true;
     }).get();
    return out;
}

// -------------------------------------------------------------------------------------------------------- free functions

void registerNativeGpu(const std::string& type, NativeGpuFn fn)
{
    Shared& s = shared();
    std::scoped_lock lock(s.mutex);
    s.natives[type] = std::move(fn);
}

bool hasNativeGpu(const std::string& type)
{
    const Route r = nativeRoute(type);
    return r == Route::Registered || r == Route::Kernels || r == Route::Builtin;
}

void setMeshMapSource(MeshMapSource fn)
{
    Shared& s = shared();
    std::scoped_lock lock(s.mutex);
    s.meshMaps = std::move(fn);
}

GpuImageRef meshMap(const std::string& bakeProject, const std::string& map, int udim)
{
    MeshMapSource fn;
    {
        Shared& s = shared();
        std::scoped_lock lock(s.mutex);
        fn = s.meshMaps;
    }
    return fn && !bakeProject.empty() ? fn(bakeProject, map, udim) : nullptr;
}

void meshMapsChanged(const std::string& bakeProject)
{
    std::vector<std::function<void(const std::string&)>> fns;
    {
        Shared& s = shared();
        std::scoped_lock lock(s.mutex);
        for (auto& [id, fn] : s.listeners)
            fns.push_back(fn);
    }
    for (auto& fn : fns)
        fn(bakeProject);
}

int addMeshMapListener(std::function<void(const std::string& bakeProject)> fn)
{
    Shared& s = shared();
    std::scoped_lock lock(s.mutex);
    s.listeners[s.nextListener] = std::move(fn);
    return s.nextListener++;
}

void removeMeshMapListener(int id)
{
    Shared& s = shared();
    std::scoped_lock lock(s.mutex);
    s.listeners.erase(id);
}

namespace
{
/** The plan cut down to the passes the named outputs need. Outputs it drops that a kept pass reads become intermediates. */
Plan outputsOnly(const Plan& plan, std::span<const std::string> names)
{
    Plan p = plan;
    p.outputs.clear();
    std::vector<uint8_t> need(plan.buffers.size(), 0);
    for (const std::string& n : names)
    {
        const int id = plan.outputs.at(n);
        p.outputs[n] = id;
        need[size_t(id)] = 1;
    }
    std::vector<size_t> kept;
    for (size_t i = plan.passes.size(); i-- > 0;)
    {
        const Pass& pass = plan.passes[i];
        if (!need[size_t(pass.output)])
            continue;
        kept.push_back(i);
        for (int r : pass.reads)
            need[size_t(r)] = 1;
        for (const auto& [id, r] : pass.inputBuffers)
            need[size_t(r)] = 1;
    }
    p.passes.clear();
    for (auto i = kept.rbegin(); i != kept.rend(); ++i)
        p.passes.push_back(plan.passes[*i]);
    std::set<int> outs;
    for (const auto& [n, id] : p.outputs)
        outs.insert(id);
    for (BufferDesc& b : p.buffers)
        if (b.kind == BufferKind::Output && !outs.contains(b.id))
            b.kind = BufferKind::Intermediate;
    return p;
}

/** Runs the plan for the named outputs into result, halving the group while its buffers don't fit one GPU buffer. */
void evaluateGroup(const Plan& plan, const tex::EvalOptions& options, std::span<const std::string> names, uint64_t maxArenaBytes, tex::EvalResult& result)
{
    LILA_ZONE();
    const Plan part = names.size() == plan.outputs.size() ? plan : outputsOnly(plan, names);
    Inputs in;
    auto external = [&](int b) {
        const BufferDesc& d = part.buffers[size_t(b)];
        if (d.kind == BufferKind::External && !in.cpuExternals.contains(d.name))
            if (auto it = options.externals.find(d.name); it != options.externals.end())
                in.cpuExternals.emplace(d.name, it->second);
    };
    for (const Pass& p : part.passes)
    {
        for (int b : p.reads)
            external(b);
        for (const auto& [id, b] : p.inputBuffers)
            external(b);
    }
    in.compact = true;
    in.maxArenaBytes = maxArenaBytes;
    in.imageRoot = options.imageRoot;
    in.uvOffset[0] = options.uvOffset[0];
    in.uvOffset[1] = options.uvOffset[1];
    Evaluator ev;
    if (std::string err = ev.setPlan(part, std::move(in)); !err.empty())
        throw std::runtime_error(err);
    try
    {
        Renderer::get().runTask([&ev](VulkanContext& vk) { return ev.step(vk); }).get();
    }
    catch (const ArenaTooLarge&)
    {
        if (names.size() < 2)
            throw;
        const size_t half = names.size() / 2;
        evaluateGroup(plan, options, names.first(half), maxArenaBytes, result);
        evaluateGroup(plan, options, names.subspan(half), maxArenaBytes, result);
        return;
    }
    result.outputs.merge(ev.readOutputs());
    const Stats s = ev.stats();
    result.compiled += s.compiled;
    result.cached += s.cached;
    for (const std::string& w : s.warnings)
        if (std::find(result.warnings.begin(), result.warnings.end(), w) == result.warnings.end())
            result.warnings.push_back(w);
}
} // namespace

tex::EvalResult evaluate(const Plan& plan, const tex::EvalOptions& options, uint64_t maxArenaBytes)
{
    LILA_ZONE();
    if (!plan.error.empty())
        throw std::runtime_error(plan.error);
    Renderer& r = Renderer::get();
    if (!r.running() && !r.start())
        throw std::runtime_error("no GPU: the renderer could not start");
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::string> names;
    for (const auto& [name, id] : plan.outputs)
        names.push_back(name);
    tex::EvalResult result;
    evaluateGroup(plan, options, names, maxArenaBytes, result);
    result.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return result;
}

std::set<std::string> wholeExternals(const Plan& plan)
{
    std::vector<int> readers(plan.buffers.size(), 0);
    for (const Pass& p : plan.passes)
        for (int r : p.reads)
            ++readers[size_t(r)];
    std::set<std::string> out;
    for (const BufferDesc& b : plan.buffers)
    {
        if (b.kind != BufferKind::External)
            continue;
        for (const Pass& p : plan.passes)
        {
            if (std::find(p.reads.begin(), p.reads.end(), b.id) == p.reads.end())
                continue;
            const bool page = p.kind == PassKind::Fused && plan.buffers[size_t(p.output)].kind == BufferKind::Output && readers[size_t(p.output)] == 0;
            if (!page || std::find(p.local.begin(), p.local.end(), b.id) == p.local.end())
                out.insert(b.name);
        }
    }
    return out;
}

bool recordImageRegion(VulkanContext& vk, VkCommandBuffer cmd, const GpuImageRef& image, int levelWidth, int levelHeight, const std::array<int, 4>& rect,
                       VkDeviceAddress dst, uint32_t dstStride, std::vector<compute::Buffer>& temps)
{
    const int w = rect[2] - rect[0], h = rect[3] - rect[1];
    if (!image || w <= 0 || h <= 0)
        return false;
    std::vector<compute::Buffer> staging;
    const bool ok = image->prepare(vk, cmd, staging);
    temps.insert(temps.end(), staging.begin(), staging.end());
    if (!ok || !image->realized())
        return false;
    levelWidth = std::max(levelWidth, 1);
    levelHeight = std::max(levelHeight, 1);
    // The part of the level the rectangle covers, in image texels; texels outside the level clamp to it.
    const std::array<int, 4> inside{std::clamp(rect[0], 0, levelWidth - 1), std::clamp(rect[1], 0, levelHeight - 1),
                                    std::clamp(rect[2], 1, levelWidth), std::clamp(rect[3], 1, levelHeight)};
    const std::array<int, 4> src = Rect::texels(inside, levelWidth, levelHeight).cover(image->width(), image->height());
    const int sw = std::max(src[2] - src[0], 1), sh = std::max(src[3] - src[1], 1);
    compute::Buffer raw =
        compute::makeBuffer(vk, (VkDeviceSize(sw) * VkDeviceSize(sh) * VkDeviceSize(texelBytes(image->format())) + 3) & ~VkDeviceSize(3), 0, compute::Host::None);
    temps.push_back(raw);
    image->recordReadRect(cmd, raw, 0, src[0], src[1], sw, sh);
    compute::memoryBarrier(cmd);
    const float sx = float(image->width()) / float(levelWidth), sy = float(image->height()) / float(levelHeight);
    const uint64_t texels = uint64_t(w) * uint64_t(h);
    DecodePush push{raw.address,
                    dst,
                    uint32_t(w),
                    uint32_t(h),
                    formatCode(image->format()),
                    usesSrgb(image->format(), image->desc().srgb) ? 1u : 0u,
                    groupsX(texels),
                    dstStride,
                    uint32_t(sw),
                    uint32_t(sh),
                    {sx, sy},
                    {float(rect[0]) * sx - float(src[0]), float(rect[1]) * sy - float(src[1])},
                    0,
                    0};
    VkPipelineLayout layout;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, kernel(vk, "tex_decode.spv", "texDecodeCS", sizeof(DecodePush), layout));
    vkCmdPushConstants(cmd, layout, kCompute, 0, sizeof(push), &push);
    dispatchLinear(cmd, texels);
    return true;
}

std::pair<size_t, size_t> spirvCacheStats()
{
    Shared& s = shared();
    std::scoped_lock lock(s.mutex);
    return {s.compiles, s.hits};
}

void registerTexGpu()
{
    tex::setGpuEvaluator({[](const Plan& p, const tex::EvalOptions& o) { return evaluate(p, o); },
                          [] {
                              Renderer& r = Renderer::get();
                              return r.running() || r.start();
                          }});
}
} // namespace lila::render::texgpu
