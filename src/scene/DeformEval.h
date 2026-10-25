#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace lila::scene
{
class DeformerNode;
}

/**
 * Deformer evaluation beyond the CPU reference: GPU kernels for the topology-preserving deformers, and bind data
 * cached by content hash. DeformerNode::deform stays the reference and the fallback. The GPU runs a deformer only when
 * a backend is registered (the app registers the renderer's), the general pref /deformers/gpu allows it (default on),
 * the mesh has at least /deformers/gpuMinPoints points and no CpuOnly scope is open on the thread. lilabatch registers
 * no backend, and exports re-run any deformer that ran on the GPU under CpuOnly.
 */
namespace lila::scene::deformEval
{
/** The GPU kernels, one per deformer family (render/shaders/deform.slang). */
enum class Kernel : uint32_t
{
    Nonlinear,
    Ffd,
    Kelvinlet,
    CageLinear,     // mean value and Green coordinates: u = sum w * displacement
    CageSomigliana, // 3x3 coordinates over a rigid fit
    Wrap,
    DeltaMush,
};

/**
 * Bind data the backend keeps on the GPU, uploaded once per key: equal keys must mean equal contents. owner keeps the
 * bytes alive while the GPU copy is made.
 */
struct Blob
{
    uint64_t key = 0;
    std::shared_ptr<const void> owner;
    const void* data = nullptr;
    size_t bytes = 0;

    bool empty() const { return bytes == 0; }
};

/** Builds a blob over a vector some shared object owns. */
template <class T> Blob blob(uint64_t key, std::shared_ptr<const void> owner, const std::vector<T>& v)
{
    return {key, std::move(owner), v.data(), v.size() * sizeof(T)};
}

/**
 * One GPU deformer evaluation. The kernel reads the points (mesh space, float) and writes each point's displacement,
 * which is added to the double points, so a point the deformer leaves alone stays bit-identical. params is the
 * kernel's parameter block (32-bit words, laid out as deform.slang reads them), blobs its bind data.
 */
struct Program
{
    Kernel kernel = Kernel::Nonlinear;
    std::vector<uint32_t> params;
    std::array<Blob, 4> blobs;
    uint32_t iterations = 0; // delta mush smoothing passes

    void push(float v);
    void push(uint32_t v);
    void push(const glm::dvec3& v);
    /** Column major. */
    void push(const glm::dmat3& m);
    void push(const glm::dmat4& m);
    void push(std::span<const glm::dvec3> v);
};

class Backend
{
public:
    virtual ~Backend() = default;
    /** Adds the program's displacements to points. False when the GPU cannot run it now: points are left untouched. */
    virtual bool run(const Program& program, std::vector<glm::dvec3>& points) = 0;
    /** Drops the GPU copies of every blob. */
    virtual void clear() = 0;
};

/** Registers (or with null removes) the GPU backend. */
LILA_SCENE_API void setBackend(std::shared_ptr<Backend> backend);
LILA_SCENE_API bool hasBackend();

/** While alive, deformers on this thread evaluate on the CPU: exports, bakes and anything else that wants the reference. */
struct LILA_SCENE_API CpuOnly
{
    CpuOnly();
    ~CpuOnly();
    CpuOnly(const CpuOnly&) = delete;
    CpuOnly& operator=(const CpuOnly&) = delete;
};

/** Whether a deformer over this many points would run on the GPU now. */
LILA_SCENE_API bool useGpu(size_t points);

/**
 * Runs d on the GPU when useGpu allows and d has a GPU form: true with points deformed, false with them untouched (the
 * caller runs the CPU path). Throws what d's gpuProgram throws (an unbound deformer), as deform would.
 */
LILA_SCENE_API bool tryGpu(const DeformerNode& d, const geo::Mesh& in, std::vector<glm::dvec3>& points);

/** Counters since the last resetStats, for tests and the stack timing view. */
struct Stats
{
    uint64_t computes = 0;    // deformer evaluations that ran
    uint64_t cacheHits = 0;   // evaluations skipped: every input was what the last one saw
    uint64_t gpuRuns = 0;     // deformers that ran on the GPU
    uint64_t gpuDeclined = 0; // GPU-capable deformers that fell back to the CPU
    uint64_t bindHits = 0;
    uint64_t bindMisses = 0;
};
LILA_SCENE_API Stats stats();
LILA_SCENE_API void resetStats();
LILA_SCENE_API void countCompute(bool cacheHit);

// ------------------------------------------------------------------------------------------------ hashing and caches

/** 64-bit hash of bytes, the same on every run and thread count. */
LILA_SCENE_API uint64_t hashBytes(const void* data, size_t bytes, uint64_t seed = 0);
LILA_SCENE_API uint64_t hashCombine(uint64_t a, uint64_t b);
/** Hash of everything point adjacency and corner order depend on: point, edge and corner connectivity. Parallel. */
LILA_SCENE_API uint64_t topologyHash(const geo::Mesh& mesh);
/**
 * A key for a shared object's identity, distinct for every object alive at once and never reused for a later object
 * at the same address. For bind data too large to hash every evaluation (dense cage coordinates).
 */
LILA_SCENE_API uint64_t identityKey(const std::shared_ptr<const void>& object);

/**
 * Bind data shared by key across deformers and evaluations (two meshes with the same rest shape, a rebind to the same
 * shape, undo), kept under a byte budget, least recently used first out. make runs outside the lock; two threads
 * missing the same key at once may both build it.
 */
LILA_SCENE_API std::shared_ptr<const void> cachedBind(uint64_t key, const std::function<std::pair<std::shared_ptr<const void>, size_t>()>& make);
template <class T, class F> std::shared_ptr<const T> cachedBind(uint64_t key, F&& make)
{
    return std::static_pointer_cast<const T>(cachedBind(key, [&]() -> std::pair<std::shared_ptr<const void>, size_t> {
        std::shared_ptr<const T> v = make();
        const size_t bytes = v ? v->bytes() : 0;
        return {std::move(v), bytes};
    }));
}
/** Empties the bind cache (tests). */
LILA_SCENE_API void clearBindCache();
} // namespace lila::scene::deformEval
