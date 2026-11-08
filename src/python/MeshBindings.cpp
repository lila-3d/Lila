#include "core/MainQueue.h"
#include "geo/Attribute.h"
#include "geo/Mesh.h"
#include "geo/Normals.h"
#include "ops/Command.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/Morphs.h"
#include "geo/Triangulate.h"

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/stl/vector.h>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <array>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <tuple>

namespace nb = nanobind;

namespace lila::python
{
namespace
{
static_assert(sizeof(glm::dvec3) == 3 * sizeof(double) && sizeof(glm::vec3) == 3 * sizeof(float) && sizeof(glm::vec2) == 2 * sizeof(float));

constexpr size_t kGrain = 1 << 14;

void requireMainThread(const char* what)
{
    if (!mainq::isMainThread())
        throw CommandError(std::string(what) + " uses the scene, so it runs on the main thread only: get the arrays there and hand them to "
                                               "worker threads, or call it through lila.tasks.call_on_main");
}

scene::Node& meshNode(const std::string& mesh)
{
    scene::Node* n = scene::Graph::current().find(mesh);
    if (!n || !n->type().isA("mesh"))
        throw nb::value_error((mesh + " is not a mesh").c_str());
    return *n;
}

/** The evaluated mesh (or its base under modifiers), with the GIL held: evaluating can run Python nodes. */
scene::MeshPtr meshOf(const std::string& mesh, bool base, const char* what)
{
    requireMainThread(what);
    scene::Graph& g = scene::Graph::current();
    scene::Node& n = meshNode(mesh);
    g.evaluate();
    scene::MeshPtr m = base ? scene::modifiers::cage(g, n.id()) : n.as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    if (!m)
        throw nb::value_error((mesh + " has no mesh").c_str());
    return m;
}

/** A capsule that keeps a shared buffer alive for as long as numpy views into it. */
template <class T> nb::capsule sharedOwner(std::shared_ptr<T> p)
{
    return nb::capsule(new std::shared_ptr<T>(std::move(p)), [](void* q) noexcept { delete static_cast<std::shared_ptr<T>*>(q); });
}

/** Element types the getters write. */
enum class Scalar : uint8_t
{
    F32,
    F64,
    I32,
    U32,
    I64,
    Bool
};
constexpr std::array kScalars = {Scalar::F32, Scalar::F64, Scalar::I32, Scalar::U32, Scalar::I64, Scalar::Bool};

nb::dlpack::dtype dtypeOf(Scalar s)
{
    switch (s)
    {
    case Scalar::F32: return nb::dtype<float>();
    case Scalar::F64: return nb::dtype<double>();
    case Scalar::I32: return nb::dtype<int32_t>();
    case Scalar::U32: return nb::dtype<uint32_t>();
    case Scalar::I64: return nb::dtype<int64_t>();
    case Scalar::Bool: return nb::dtype<bool>();
    }
    return nb::dtype<float>();
}

size_t scalarSize(Scalar s)
{
    return s == Scalar::F64 || s == Scalar::I64 ? 8 : s == Scalar::Bool ? 1 : 4;
}

std::optional<Scalar> scalarOf(nb::dlpack::dtype t)
{
    for (Scalar s : kScalars)
        if (dtypeOf(s) == t)
            return s;
    return std::nullopt;
}

using WritableArray = nb::ndarray<nb::c_contig, nb::device::cpu>;

/**
 * Where a getter writes its rows: a buffer of its own in the getter's native type, or the caller's out= array, which
 * must be writable, C-contiguous and either (rows, comps) or flat. comps 0 means a 1-D array of rows.
 */
class Dest
{
public:
    Dest(size_t rows, size_t comps, Scalar native) : mRows(rows), mComps(comps), mType(native) { allocate(); }

    Dest(const char* what, size_t rows, size_t comps, Scalar native, bool floatSource, nb::handle out) : mRows(rows), mComps(comps), mType(native)
    {
        if (out.is_none())
        {
            allocate();
            return;
        }
        auto fail = [&](const std::string& why) { throw nb::value_error((std::string(what) + ": out= " + why).c_str()); };
        WritableArray a;
        if (!nb::try_cast(out, a, false))
            fail("must be a C-contiguous numpy array on the CPU");
        if (nb::hasattr(out, "flags") && !nb::cast<bool>(out.attr("flags").attr("writeable")))
            fail("is read-only");
        const std::optional<Scalar> type = scalarOf(a.dtype());
        if (!type)
            fail("has an unsupported dtype: use float32, float64, int32, uint32, int64 or bool");
        if (floatSource && *type != Scalar::F32 && *type != Scalar::F64)
            fail("must be float32 or float64 for this data");
        const size_t elems = rows * width();
        const bool shaped = mComps ? a.ndim() == 2 && a.shape(0) == rows && a.shape(1) == mComps : a.ndim() == 1 && a.shape(0) == rows;
        const bool flat = a.ndim() == 1 && a.shape(0) == elems;
        if (!shaped && !flat)
            fail(mComps ? "must have shape (" + std::to_string(rows) + ", " + std::to_string(mComps) + ") or (" + std::to_string(elems) + ",)"
                        : "must have shape (" + std::to_string(rows) + ",)");
        mData = a.data();
        mType = *type;
        mOut = nb::borrow(out);
    }

    size_t rows() const { return mRows; }
    size_t width() const { return std::max<size_t>(mComps, 1); }

    /** Calls fn with the destination as a pointer of its element type. */
    template <class F> void visit(F&& fn)
    {
        switch (mType)
        {
        case Scalar::F32: fn(static_cast<float*>(mData)); break;
        case Scalar::F64: fn(static_cast<double*>(mData)); break;
        case Scalar::I32: fn(static_cast<int32_t*>(mData)); break;
        case Scalar::U32: fn(static_cast<uint32_t*>(mData)); break;
        case Scalar::I64: fn(static_cast<int64_t*>(mData)); break;
        case Scalar::Bool: fn(static_cast<bool*>(mData)); break;
        }
    }

    /** The caller's out= array, or a read-only numpy array that owns the buffer written. */
    nb::object result()
    {
        if (mOut.is_valid())
            return mOut;
        const std::array<size_t, 2> shape = {mRows, mComps};
        std::byte* data = mOwned.release();
        nb::capsule owner(data, [](void* p) noexcept { delete[] static_cast<std::byte*>(p); });
        return nb::ndarray<nb::numpy, nb::ro>(data, mComps ? 2 : 1, shape.data(), owner, nullptr, dtypeOf(mType)).cast();
    }

    /** The buffer written, for a getter that keeps it. Only for a Dest it owns. */
    std::shared_ptr<std::byte[]> take() { return std::shared_ptr<std::byte[]>(mOwned.release()); }

private:
    void allocate()
    {
        mOwned = std::make_unique_for_overwrite<std::byte[]>(std::max<size_t>(1, mRows * width() * scalarSize(mType)));
        mData = mOwned.get();
    }

    size_t mRows = 0, mComps = 0;
    Scalar mType = Scalar::F32;
    void* mData = nullptr;
    std::unique_ptr<std::byte[]> mOwned;
    nb::object mOut;
};

/** Valid ids below capacity in order, or empty when none is deleted (rows are ids). */
template <class Valid> std::vector<geo::Index> validIds(geo::Index capacity, geo::Index count, Valid&& valid)
{
    std::vector<geo::Index> ids;
    if (count == capacity)
        return ids;
    ids.reserve(count);
    for (geo::Index i = 0; i < capacity; ++i)
        if (valid(i))
            ids.push_back(i);
    return ids;
}

/** Valid faces in order (identity when none is deleted) and where each face's corners start in face-vertex order. */
struct FaceLayout
{
    std::vector<geo::Index> faces; // empty when every face is valid
    std::vector<uint64_t> offsets; // one per face, plus the total
    size_t count() const { return offsets.size() - 1; }
    geo::Index face(size_t i) const { return faces.empty() ? geo::Index(i) : faces[i]; }
};

FaceLayout faceLayout(const geo::Mesh& m)
{
    FaceLayout l;
    l.faces = validIds(m.faceCapacity(), m.numFaces(), [&](geo::Index f) { return m.faceValid(f); });
    const size_t n = l.faces.empty() ? m.faceCapacity() : l.faces.size();
    l.offsets.resize(n + 1);
    l.offsets[0] = 0;
    for (size_t i = 0; i < n; ++i)
        l.offsets[i + 1] = l.offsets[i] + m.faceSize(l.face(i));
    return l;
}

/** Point id to its row in the points array, or empty when no point is deleted (rows are ids). */
std::vector<geo::Index> pointRows(const geo::Mesh& m)
{
    std::vector<geo::Index> rows;
    if (m.numPoints() == m.pointCapacity())
        return rows;
    rows.assign(m.pointCapacity(), geo::kInvalid);
    for (geo::Index p = 0, k = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            rows[p] = k++;
    return rows;
}

/** Row i of d is row ids[i] of src (row i when ids is empty), d.width() scalars each. */
template <class S> void copyRows(Dest& d, const S* src, std::span<const geo::Index> ids)
{
    const size_t w = d.width();
    d.visit([&]<class O>(O* dst) {
        if constexpr (std::is_same_v<O, S>)
            if (ids.empty())
            {
                std::memcpy(dst, src, d.rows() * w * sizeof(S));
                return;
            }
        tbb::parallel_for(tbb::blocked_range<size_t>(0, d.rows(), kGrain), [&](const tbb::blocked_range<size_t>& r) {
            for (size_t i = r.begin(); i < r.end(); ++i)
            {
                const S* s = src + size_t(ids.empty() ? i : ids[i]) * w;
                O* o = dst + i * w;
                for (size_t k = 0; k < w; ++k)
                    o[k] = O(s[k]);
            }
        });
    });
}

/** Per-corner rows of src gathered into face-vertex order. */
template <class S> void gatherCorners(Dest& d, const geo::Mesh& m, const FaceLayout& l, const S* src)
{
    const size_t w = d.width();
    d.visit([&]<class O>(O* dst) {
        tbb::parallel_for(tbb::blocked_range<size_t>(0, l.count(), kGrain), [&](const tbb::blocked_range<size_t>& r) {
            for (size_t i = r.begin(); i < r.end(); ++i)
            {
                O* o = dst + l.offsets[i] * w;
                m.forEachFaceCorner(l.face(i), [&](geo::Index c) {
                    const S* s = src + size_t(c) * w;
                    for (size_t k = 0; k < w; ++k)
                        *o++ = O(s[k]);
                });
            }
        });
    });
}

nb::object pointsArray(const std::string& mesh, bool base, bool world, nb::handle out)
{
    scene::MeshPtr m = meshOf(mesh, base, "points_array");
    const size_t n = m->numPoints();
    if (out.is_none() && !world && n == m->pointCapacity())
        return nb::ndarray<nb::numpy, const double, nb::shape<-1, 3>>(reinterpret_cast<const double*>(m->positions().data()), {n, size_t(3)},
                                                                       sharedOwner(m))
            .cast();
    const glm::dmat4 w = world ? meshNode(mesh).as<glm::dmat4>(scene::Transform::kWorldMatrix) : glm::dmat4(1.0);
    Dest d("points_array", n, 3, Scalar::F64, true, out);
    {
        nb::gil_scoped_release nogil;
        const std::vector<geo::Index> ids = validIds(m->pointCapacity(), m->numPoints(), [&](geo::Index p) { return m->pointValid(p); });
        if (!world)
            copyRows(d, reinterpret_cast<const double*>(m->positions().data()), ids);
        else
            d.visit([&]<class O>(O* dst) {
                tbb::parallel_for(tbb::blocked_range<size_t>(0, n, kGrain), [&](const tbb::blocked_range<size_t>& r) {
                    for (size_t i = r.begin(); i < r.end(); ++i)
                    {
                        const glm::dvec3 p = glm::dvec3(w * glm::dvec4(m->position(ids.empty() ? geo::Index(i) : ids[i]), 1.0));
                        dst[i * 3] = O(p.x);
                        dst[i * 3 + 1] = O(p.y);
                        dst[i * 3 + 2] = O(p.z);
                    }
                });
            });
    }
    return d.result();
}

/** out as the pair a two-array getter writes, (None, None) when not given. */
std::pair<nb::handle, nb::handle> outPair(nb::handle out, const char* what)
{
    if (out.is_none())
        return {nb::none(), nb::none()};
    if (!nb::isinstance<nb::tuple>(out) || nb::len(out) != 2)
        throw nb::value_error((std::string(what) + ": out= must be a pair of arrays (or None in either place)").c_str());
    return {out[0], out[1]};
}

std::pair<nb::object, nb::object> faceArrays(const std::string& mesh, bool base, nb::handle out)
{
    scene::MeshPtr m = meshOf(mesh, base, "face_arrays");
    FaceLayout l;
    {
        nb::gil_scoped_release nogil;
        l = faceLayout(*m);
    }
    auto [countsOut, pointsOut] = outPair(out, "face_arrays");
    Dest counts("face_arrays", l.count(), 0, Scalar::U32, false, countsOut);
    Dest points("face_arrays", l.offsets.back(), 0, Scalar::U32, false, pointsOut);
    {
        nb::gil_scoped_release nogil;
        const std::vector<geo::Index> rows = pointRows(*m);
        counts.visit([&]<class C>(C* cdst) {
            points.visit([&]<class P>(P* pdst) {
                tbb::parallel_for(tbb::blocked_range<size_t>(0, l.count(), kGrain), [&](const tbb::blocked_range<size_t>& r) {
                    for (size_t i = r.begin(); i < r.end(); ++i)
                    {
                        cdst[i] = C(l.offsets[i + 1] - l.offsets[i]);
                        uint64_t k = l.offsets[i];
                        m->forEachFaceCorner(l.face(i), [&](geo::Index c) {
                            const geo::Index p = m->cornerPoint(c);
                            pdst[k++] = P(rows.empty() ? p : rows[p]);
                        });
                    }
                });
            });
        });
    }
    return {counts.result(), points.result()};
}

nb::object edgeArrays(const std::string& mesh, bool base, nb::handle out)
{
    scene::MeshPtr m = meshOf(mesh, base, "edge_arrays");
    Dest d("edge_arrays", m->numEdges(), 2, Scalar::U32, false, out);
    {
        nb::gil_scoped_release nogil;
        const std::vector<geo::Index> ids = validIds(m->edgeCapacity(), m->numEdges(), [&](geo::Index e) { return m->edgeValid(e); });
        const std::vector<geo::Index> rows = pointRows(*m);
        d.visit([&]<class O>(O* dst) {
            tbb::parallel_for(tbb::blocked_range<size_t>(0, d.rows(), kGrain), [&](const tbb::blocked_range<size_t>& r) {
                for (size_t i = r.begin(); i < r.end(); ++i)
                {
                    const geo::Index e = ids.empty() ? geo::Index(i) : ids[i];
                    for (int k = 0; k < 2; ++k)
                    {
                        const geo::Index p = m->edgePoint(e, k);
                        dst[i * 2 + k] = O(rows.empty() ? p : rows[p]);
                    }
                }
            });
        });
    }
    return d.result();
}

/** Derived face-vertex floats of one mesh snapshot (normals, tangents), (count, comps). */
struct FaceVertexFloats
{
    std::shared_ptr<std::byte[]> data;
    size_t count = 0, comps = 0;
    const float* floats() const { return reinterpret_cast<const float*>(data.get()); }
};
using FloatsPtr = std::shared_ptr<const FaceVertexFloats>;

/** Derived arrays per mesh snapshot and kind. Snapshots never change, so an entry holds while its mesh lives. Main thread only. */
struct CachedFloats
{
    std::weak_ptr<const geo::Mesh> mesh;
    FloatsPtr floats;
};
std::map<std::pair<const geo::Mesh*, std::string>, CachedFloats> gDerivedCache;

/** The cached array of this kind for m, or make() run without the GIL and cached. */
template <class F> FloatsPtr cachedOr(const scene::MeshPtr& m, const std::string& kind, F&& make)
{
    std::erase_if(gDerivedCache, [](const auto& e) { return e.second.mesh.expired(); });
    if (auto it = gDerivedCache.find({m.get(), kind}); it != gDerivedCache.end())
        return it->second.floats;
    std::shared_ptr<FaceVertexFloats> out;
    {
        nb::gil_scoped_release nogil;
        out = make();
    }
    gDerivedCache[{m.get(), kind}] = {m, out};
    return out;
}

/** The cached array of this kind for m, or compute(layout) gathered into face-vertex order (comps floats per corner) and cached. */
template <class F> FloatsPtr derived(const scene::MeshPtr& m, const std::string& kind, size_t comps, F&& compute)
{
    return cachedOr(m, kind, [&] {
        auto out = std::make_shared<FaceVertexFloats>();
        FaceLayout l = faceLayout(*m);
        Dest d(l.offsets.back(), comps, Scalar::F32);
        gatherCorners(d, *m, l, compute());
        out->data = d.take();
        out->count = l.offsets.back();
        out->comps = comps;
        return out;
    });
}

/** A cached array as a read-only view, or copied into out=. */
nb::object derivedResult(const FloatsPtr& f, const char* what, nb::handle out)
{
    if (out.is_none())
    {
        const std::array<size_t, 2> shape = {f->count, f->comps};
        return nb::ndarray<nb::numpy, const float>(f->floats(), 2, shape.data(), sharedOwner(f)).cast();
    }
    Dest d(what, f->count, f->comps, Scalar::F32, true, out);
    {
        nb::gil_scoped_release nogil;
        copyRows(d, f->floats(), {});
    }
    return d.result();
}

/** Corner normals (per corner capacity) as drawn: the stored split normals, else computed. */
std::span<const glm::vec3> cornerNormalsOf(const geo::Mesh& m, std::vector<glm::vec3>& storage)
{
    std::span<const glm::vec3> stored = m.cornerAttrs().get<glm::vec3>(geo::attr::kNormal);
    if (stored.size() == m.cornerCapacity())
        return stored;
    storage = geo::cornerNormals(m);
    return storage;
}

/** The last normals plan made: edits that only move points (a new mesh snapshot each time) keep using it. */
std::unique_ptr<geo::CornerNormalPlan> gNormalPlan;

nb::object normalsArray(const std::string& mesh, bool base, nb::handle out)
{
    scene::MeshPtr m = meshOf(mesh, base, "normals_array");
    FloatsPtr n;
    const std::span<const glm::vec3> stored = m->cornerAttrs().get<glm::vec3>(geo::attr::kNormal);
    if (stored.size() == m->cornerCapacity())
        n = derived(m, "normal", 3, [&] { return reinterpret_cast<const float*>(stored.data()); });
    else
        n = cachedOr(m, "normal", [&] {
            if (!gNormalPlan || !gNormalPlan->fits(*m))
                gNormalPlan = std::make_unique<geo::CornerNormalPlan>(*m);
            auto f = std::make_shared<FaceVertexFloats>();
            f->count = gNormalPlan->rows();
            f->comps = 3;
            f->data = std::make_shared_for_overwrite<std::byte[]>(std::max<size_t>(1, f->count * sizeof(glm::vec3)));
            gNormalPlan->compute(*m, {reinterpret_cast<glm::vec3*>(f->data.get()), f->count});
            return f;
        });
    return derivedResult(n, "normals_array", out);
}

nb::object tangentsArray(const std::string& mesh, const std::string& set, bool base, nb::handle out)
{
    scene::MeshPtr m = meshOf(mesh, base, "tangents_array");
    if (!m->cornerAttrs().has(set) && m->numCorners() > 0)
        throw nb::value_error((mesh + " has no uv set " + set + " (uv_sets lists what it has)").c_str());
    std::vector<glm::vec4> storage;
    FloatsPtr t = derived(m, "tangent:" + set, 4, [&] {
        // The stored tangents carry no uv set, so they only stand for uv0.
        std::span<const glm::vec4> stored = m->cornerAttrs().get<glm::vec4>(geo::attr::kTangent);
        if (set == "uv0" && stored.size() == m->cornerCapacity())
            return reinterpret_cast<const float*>(stored.data());
        std::vector<glm::vec3> normals;
        storage = geo::cornerTangents(*m, geo::triangulate(*m), cornerNormalsOf(*m, normals), set);
        storage.resize(m->cornerCapacity(), glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
        return reinterpret_cast<const float*>(storage.data());
    });
    return derivedResult(t, "tangents_array", out);
}

/** Every morph target of the mesh as exporters write it: in-betweens expanded, deltas in object space on points_array(base=True) rows. */
std::vector<nb::dict> morphTargets(const std::string& mesh)
{
    requireMainThread("morph_targets");
    scene::Graph& g = scene::Graph::current();
    scene::Node& n = meshNode(mesh);
    std::vector<nb::dict> out;
    const scene::NodeId morph = scene::morphs::find(g, n.id());
    if (morph == scene::kNoNode)
        return out;
    std::vector<scene::morphs::Target> targets = scene::morphs::targets(g, morph);
    scene::MeshPtr cage = scene::modifiers::cage(g, n.id());
    const std::vector<geo::Index> rows = cage ? pointRows(*cage) : std::vector<geo::Index>();
    for (scene::morphs::Target& t : targets)
    {
        auto keep = std::make_shared<scene::morphs::Target>(std::move(t));
        if (!rows.empty())
            for (uint32_t& i : keep->indices)
                i = i < rows.size() ? rows[i] : geo::kInvalid;
        const size_t k = keep->indices.size();
        nb::dict d;
        d["name"] = keep->name;
        d["weight"] = keep->weight;
        d["indices"] = nb::ndarray<nb::numpy, const uint32_t, nb::ndim<1>>(keep->indices.data(), {k}, sharedOwner(keep)).cast();
        d["deltas"] =
            nb::ndarray<nb::numpy, const double, nb::shape<-1, 3>>(reinterpret_cast<const double*>(keep->deltas.data()), {k, size_t(3)}, sharedOwner(keep))
                .cast();
        out.push_back(std::move(d));
    }
    return out;
}

nb::object uvArray(const std::string& mesh, const std::string& set, bool base, nb::handle out)
{
    scene::MeshPtr m = meshOf(mesh, base, "uv_array");
    std::span<const glm::vec2> uvs = m->cornerAttrs().get<glm::vec2>(set);
    if (uvs.empty() && m->numCorners() > 0)
    {
        std::string have;
        for (const std::string& name : m->cornerAttrs().names())
            if (const geo::Attribute* a = m->cornerAttrs().find(name); a && a->type() == geo::AttrType::Float2)
                have += (have.empty() ? "" : ", ") + name;
        throw nb::value_error((mesh + " has no uv set " + set + " (it has " + (have.empty() ? std::string("none") : have) + ")").c_str());
    }
    FaceLayout l;
    {
        nb::gil_scoped_release nogil;
        l = faceLayout(*m);
    }
    Dest d("uv_array", l.offsets.back(), 2, Scalar::F32, true, out);
    {
        nb::gil_scoped_release nogil;
        gatherCorners(d, *m, l, reinterpret_cast<const float*>(uvs.data()));
    }
    return d.result();
}

nb::object selectedUvs(const std::string& mesh)
{
    requireMainThread("selected_uvs");
    const scene::components::Ids rows = scene::components::uvRows(meshNode(mesh).id());
    Dest d(rows.size(), 0, Scalar::U32);
    d.visit([&]<class O>(O* dst) {
        for (uint32_t r : rows)
            *dst++ = O(r);
    });
    return d.result();
}

std::pair<nb::object, std::vector<std::string>> materialArray(const std::string& mesh, bool base, nb::handle out)
{
    scene::MeshPtr m = meshOf(mesh, base, "material_array");
    FaceLayout l;
    {
        nb::gil_scoped_release nogil;
        l = faceLayout(*m);
    }
    Dest d("material_array", l.count(), 0, Scalar::U32, false, out);
    int32_t top = std::max(0, int32_t(m->materialNames().size()) - 1);
    {
        nb::gil_scoped_release nogil;
        std::span<const int32_t> ids = m->faceAttrs().get<int32_t>(geo::attr::kMaterial);
        d.visit([&]<class O>(O* dst) {
            for (size_t i = 0; i < l.count(); ++i)
            {
                const int32_t id = ids.empty() ? 0 : std::max(0, ids[l.face(i)]);
                dst[i] = O(id);
                top = std::max(top, id);
            }
        });
    }
    std::vector<std::string> names;
    for (int32_t id = 0; id <= top; ++id)
        names.push_back(scene::materials::slotName(*m, id));
    return {d.result(), std::move(names)};
}

std::vector<std::string> uvSets(const std::string& mesh, bool base)
{
    scene::MeshPtr m = meshOf(mesh, base, "uv_sets");
    std::vector<std::string> out;
    for (const std::string& name : m->cornerAttrs().names())
        if (const geo::Attribute* a = m->cornerAttrs().find(name); a && a->type() == geo::AttrType::Float2)
            out.push_back(name);
    return out;
}

std::tuple<size_t, size_t, size_t, size_t> meshCounts(const std::string& mesh, bool base)
{
    scene::MeshPtr m = meshOf(mesh, base, "mesh_counts");
    return {m->numPoints(), m->numEdges(), m->numFaces(), m->numCorners()};
}

constexpr std::array kDomains = {geo::Domain::Point, geo::Domain::Edge, geo::Domain::Face, geo::Domain::Corner};

std::vector<std::tuple<std::string, std::string, std::string>> attributeNames(const std::string& mesh, bool base)
{
    scene::MeshPtr m = meshOf(mesh, base, "attribute_names");
    std::vector<std::tuple<std::string, std::string, std::string>> out;
    for (geo::Domain d : kDomains)
        for (const std::string& name : m->attributes(d).names())
            if (const geo::Attribute* a = m->attributes(d).find(name))
                out.emplace_back(name, geo::domainName(d), geo::attrTypeName(a->type()));
    return out;
}

geo::Index domainCapacity(const geo::Mesh& m, geo::Domain d)
{
    switch (d)
    {
    case geo::Domain::Point: return m.pointCapacity();
    case geo::Domain::Edge: return m.edgeCapacity();
    case geo::Domain::Face: return m.faceCapacity();
    default: return m.cornerCapacity();
    }
}

/** The attribute and its domain: the one named domain, or the only domain that has it. */
std::pair<const geo::Attribute*, geo::Domain> findAttribute(const geo::Mesh& m, const std::string& mesh, const std::string& name,
                                                            const std::string& domain)
{
    const geo::Attribute* found = nullptr;
    geo::Domain where = geo::Domain::Point;
    std::string in;
    for (geo::Domain d : kDomains)
    {
        if (!domain.empty() && domain != geo::domainName(d))
            continue;
        if (const geo::Attribute* a = m.attributes(d).find(name))
        {
            in += (in.empty() ? "" : ", ") + std::string(geo::domainName(d));
            if (found)
                throw nb::value_error((mesh + " has " + name + " on more than one domain (" + in + "): pass domain=").c_str());
            found = a;
            where = d;
        }
    }
    if (!domain.empty() && domain != "point" && domain != "edge" && domain != "face" && domain != "corner")
        throw nb::value_error(("domain must be point, edge, face or corner, not " + domain).c_str());
    if (!found)
        throw nb::value_error((mesh + " has no attribute " + name + (domain.empty() ? std::string() : " on the " + domain + " domain") +
                               " (attribute_names lists what it has)")
                                  .c_str());
    if (found->size() < domainCapacity(m, where))
        throw nb::value_error((name + " is shorter than its domain").c_str());
    return {found, where};
}

std::pair<nb::object, std::string> attributeArray(const std::string& mesh, const std::string& name, const std::string& domain, bool base,
                                                  nb::handle out)
{
    scene::MeshPtr m = meshOf(mesh, base, "attribute_array");
    auto [a, where] = findAttribute(*m, mesh, name, domain);
    const geo::AttrType type = a->type();
    const bool isFloat = geo::attrTypeIsFloat(type);
    const int comps = geo::attrTypeComponents(type);
    const Scalar native = isFloat ? Scalar::F32 : type == geo::AttrType::Int ? Scalar::I32 : Scalar::Bool;

    FaceLayout l;
    std::vector<geo::Index> ids;
    size_t rows = 0;
    {
        nb::gil_scoped_release nogil;
        switch (where)
        {
        case geo::Domain::Point:
            ids = validIds(m->pointCapacity(), m->numPoints(), [&](geo::Index p) { return m->pointValid(p); });
            rows = m->numPoints();
            break;
        case geo::Domain::Edge:
            ids = validIds(m->edgeCapacity(), m->numEdges(), [&](geo::Index e) { return m->edgeValid(e); });
            rows = m->numEdges();
            break;
        default:
            l = faceLayout(*m);
            rows = where == geo::Domain::Face ? l.count() : l.offsets.back();
            break;
        }
    }
    Dest d("attribute_array", rows, comps > 1 ? size_t(comps) : 0, native, isFloat, out);
    {
        nb::gil_scoped_release nogil;
        auto fill = [&]<class S>(const S* src) {
            if (where == geo::Domain::Corner)
                gatherCorners(d, *m, l, src);
            else
                copyRows(d, src, where == geo::Domain::Face ? std::span<const geo::Index>(l.faces) : std::span<const geo::Index>(ids));
        };
        const std::byte* raw = a->size() ? a->raw(0) : nullptr;
        if (isFloat)
            fill(reinterpret_cast<const float*>(raw));
        else if (type == geo::AttrType::Int)
            fill(reinterpret_cast<const int32_t*>(raw));
        else
            fill(reinterpret_cast<const bool*>(raw));
    }
    return {d.result(), geo::domainName(where)};
}

void setPoints(const std::string& mesh, nb::ndarray<const double, nb::shape<-1, 3>, nb::c_contig, nb::device::cpu> points)
{
    requireMainThread("set_points");
    std::vector<glm::dvec3> flat;
    {
        nb::gil_scoped_release nogil;
        flat.resize(points.shape(0));
        std::memcpy(flat.data(), points.data(), flat.size() * sizeof(glm::dvec3));
    }
    uint64_t token = scene::stashPoints(std::move(flat));
    ops::execute("setMeshPoints", {{"args", {mesh}}, {"buffer", int64_t(token)}});
}
} // namespace

/**
 * Meshes as numpy arrays for lila.nodes.Node: points, faces, edges, normals, uvs, any attribute, and a points write that
 * runs setMeshPoints. Arrays are read-only snapshots, or the caller's out= array; heavy copies run on TBB with the GIL
 * released.
 */
void bindMeshArrays(nb::module_& m)
{
    m.def("mesh_points_array", &pointsArray, nb::arg("mesh"), nb::arg("base") = false, nb::arg("world") = false, nb::arg("out") = nb::none(),
          "(N, 3) float64 points, one row per point in meshData's order; a view of the mesh itself when it can be.");
    m.def("mesh_face_arrays", &faceArrays, nb::arg("mesh"), nb::arg("base") = false, nb::arg("out") = nb::none(),
          "(counts, points): corners per face (F,) and every face's point indices in a row (sum of counts,), both uint32.");
    m.def("mesh_edge_arrays", &edgeArrays, nb::arg("mesh"), nb::arg("base") = false, nb::arg("out") = nb::none(),
          "(E, 2) uint32 point indices of each edge, in edge-domain attribute order.");
    m.def("mesh_normals_array", &normalsArray, nb::arg("mesh"), nb::arg("base") = false, nb::arg("out") = nb::none(),
          "(C, 3) float32 normals per face-vertex (faces in order, corners around each face).");
    m.def("mesh_tangents_array", &tangentsArray, nb::arg("mesh"), nb::arg("uv_set") = "uv0", nb::arg("base") = false,
          nb::arg("out") = nb::none(), "(C, 4) float32 MikkTSpace tangents per face-vertex, w the bitangent sign, in mesh_normals_array's order.");
    m.def("mesh_morph_targets", &morphTargets, nb::arg("mesh"),
          "[{name, weight, indices, deltas}] of the mesh's morph targets, in-betweens expanded; sparse object-space deltas.");
    m.def("mesh_uv_array", &uvArray, nb::arg("mesh"), nb::arg("uv_set") = "uv0", nb::arg("base") = false, nb::arg("out") = nb::none(),
          "(C, 2) float32 uvs per face-vertex, in mesh_normals_array's order.");
    m.def("mesh_selected_uvs", &selectedUvs, nb::arg("mesh"),
          "(K,) uint32 face-vertex rows of the uvs picked in the UV Editor, sorted; empty when the vertices were picked elsewhere.");
    m.def("mesh_material_array", &materialArray, nb::arg("mesh"), nb::arg("base") = false, nb::arg("out") = nb::none(),
          "(slots, names): each face's material slot (F,) uint32 in mesh_face_arrays' face order, and the slot names it indexes.");
    m.def("mesh_uv_sets", &uvSets, nb::arg("mesh"), nb::arg("base") = false, "The mesh's uv set names (uv0, uv1, ...).");
    m.def("mesh_counts", &meshCounts, nb::arg("mesh"), nb::arg("base") = false, "(points, edges, faces, corners) of the evaluated mesh.");
    m.def("mesh_attribute_names", &attributeNames, nb::arg("mesh"), nb::arg("base") = false,
          "[(name, domain, type)] of every attribute on the mesh.");
    m.def("mesh_attribute_array", &attributeArray, nb::arg("mesh"), nb::arg("name"), nb::arg("domain") = "", nb::arg("base") = false,
          nb::arg("out") = nb::none(), "(array, domain): an attribute in its domain's order, float32, int32 or bool.");
    m.def("set_mesh_points", &setPoints, nb::arg("mesh"), nb::arg("points"),
          "Moves every base point from an (N, 3) float64 array through setMeshPoints; one undo step.");
}
} // namespace lila::python
