#include "core/MainQueue.h"
#include "geo/Attribute.h"
#include "geo/Mesh.h"
#include "geo/Normals.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <cstring>
#include <memory>
#include <unordered_map>

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

/** A capsule that keeps the snapshot alive for as long as numpy views into it. */
nb::capsule meshOwner(scene::MeshPtr m)
{
    return nb::capsule(new scene::MeshPtr(std::move(m)), [](void* p) noexcept { delete static_cast<scene::MeshPtr*>(p); });
}

template <class T> nb::capsule bufferOwner(std::unique_ptr<T[]>& data)
{
    return nb::capsule(data.release(), [](void* p) noexcept { delete[] static_cast<T*>(p); });
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
    if (m.numFaces() != m.faceCapacity())
    {
        l.faces.reserve(m.numFaces());
        for (geo::Index f = 0; f < m.faceCapacity(); ++f)
            if (m.faceValid(f))
                l.faces.push_back(f);
    }
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

/** Per-corner values gathered into face-vertex order. */
template <class T> std::unique_ptr<T[]> gatherCorners(const geo::Mesh& m, const FaceLayout& l, std::span<const T> src)
{
    auto out = std::make_unique_for_overwrite<T[]>(l.offsets.back());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, l.count(), kGrain), [&](const tbb::blocked_range<size_t>& r) {
        for (size_t i = r.begin(); i < r.end(); ++i)
        {
            uint64_t k = l.offsets[i];
            m.forEachFaceCorner(l.face(i), [&](geo::Index c) { out[k++] = src[c]; });
        }
    });
    return out;
}

nb::ndarray<nb::numpy, const double, nb::shape<-1, 3>> pointsArray(const std::string& mesh, bool base, bool world)
{
    scene::MeshPtr m = meshOf(mesh, base, "points_array");
    const size_t n = m->numPoints();
    if (!world && n == m->pointCapacity())
        return {reinterpret_cast<const double*>(m->positions().data()), {n, size_t(3)}, meshOwner(m)};
    const glm::dmat4 w = world ? meshNode(mesh).as<glm::dmat4>(scene::Transform::kWorldMatrix) : glm::dmat4(1.0);
    auto out = std::make_unique_for_overwrite<glm::dvec3[]>(n);
    {
        nb::gil_scoped_release nogil;
        std::vector<geo::Index> rows = pointRows(*m);
        tbb::parallel_for(tbb::blocked_range<geo::Index>(0, m->pointCapacity(), kGrain), [&](const tbb::blocked_range<geo::Index>& r) {
            for (geo::Index p = r.begin(); p < r.end(); ++p)
                if (rows.empty() || rows[p] != geo::kInvalid)
                    out[rows.empty() ? p : rows[p]] = glm::dvec3(w * glm::dvec4(m->position(p), 1.0));
        });
    }
    double* data = reinterpret_cast<double*>(out.get());
    return {data, {n, size_t(3)}, bufferOwner(out)};
}

using IndexArray = nb::ndarray<nb::numpy, const uint32_t, nb::ndim<1>>;

std::pair<IndexArray, IndexArray> faceArrays(const std::string& mesh, bool base)
{
    scene::MeshPtr m = meshOf(mesh, base, "face_arrays");
    std::unique_ptr<uint32_t[]> counts, points;
    size_t faces = 0, corners = 0;
    {
        nb::gil_scoped_release nogil;
        FaceLayout l = faceLayout(*m);
        std::vector<geo::Index> rows = pointRows(*m);
        faces = l.count();
        corners = l.offsets.back();
        counts = std::make_unique_for_overwrite<uint32_t[]>(faces);
        points = std::make_unique_for_overwrite<uint32_t[]>(corners);
        tbb::parallel_for(tbb::blocked_range<size_t>(0, faces, kGrain), [&](const tbb::blocked_range<size_t>& r) {
            for (size_t i = r.begin(); i < r.end(); ++i)
            {
                counts[i] = uint32_t(l.offsets[i + 1] - l.offsets[i]);
                uint64_t k = l.offsets[i];
                m->forEachFaceCorner(l.face(i), [&](geo::Index c) {
                    const geo::Index p = m->cornerPoint(c);
                    points[k++] = rows.empty() ? p : rows[p];
                });
            }
        });
    }
    const uint32_t* countData = counts.get();
    const uint32_t* pointData = points.get();
    return {IndexArray(countData, {faces}, bufferOwner(counts)), IndexArray(pointData, {corners}, bufferOwner(points))};
}

struct FaceVertexNormals
{
    std::unique_ptr<glm::vec3[]> data;
    size_t count = 0;
};
using NormalsPtr = std::shared_ptr<const FaceVertexNormals>;

/** Face-vertex normals per mesh snapshot. Snapshots never change, so an entry holds while its mesh lives. Main thread only. */
struct CachedNormals
{
    std::weak_ptr<const geo::Mesh> mesh;
    NormalsPtr normals;
};
std::unordered_map<const geo::Mesh*, CachedNormals> gNormalsCache;

NormalsPtr faceVertexNormals(const scene::MeshPtr& m)
{
    std::erase_if(gNormalsCache, [](const auto& e) { return e.second.mesh.expired(); });
    if (auto it = gNormalsCache.find(m.get()); it != gNormalsCache.end())
        return it->second.normals;
    auto out = std::make_shared<FaceVertexNormals>();
    {
        nb::gil_scoped_release nogil;
        FaceLayout l = faceLayout(*m);
        std::span<const glm::vec3> stored = m->cornerAttrs().get<glm::vec3>(geo::attr::kNormal);
        std::vector<glm::vec3> computed;
        if (stored.size() != m->cornerCapacity())
        {
            computed = geo::cornerNormals(*m);
            stored = computed;
        }
        out->data = gatherCorners(*m, l, stored);
        out->count = l.offsets.back();
    }
    gNormalsCache[m.get()] = {m, out};
    return out;
}

nb::ndarray<nb::numpy, const float, nb::shape<-1, 3>> normalsArray(const std::string& mesh, bool base)
{
    NormalsPtr n = faceVertexNormals(meshOf(mesh, base, "normals_array"));
    const float* data = reinterpret_cast<const float*>(n->data.get());
    const size_t corners = n->count;
    return {data, {corners, size_t(3)}, nb::capsule(new NormalsPtr(std::move(n)), [](void* p) noexcept { delete static_cast<NormalsPtr*>(p); })};
}

nb::ndarray<nb::numpy, const float, nb::shape<-1, 2>> uvArray(const std::string& mesh, const std::string& set, bool base)
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
    std::unique_ptr<glm::vec2[]> out;
    size_t corners = 0;
    {
        nb::gil_scoped_release nogil;
        FaceLayout l = faceLayout(*m);
        corners = l.offsets.back();
        out = gatherCorners(*m, l, uvs);
    }
    const float* data = reinterpret_cast<const float*>(out.get());
    return {data, {corners, size_t(2)}, bufferOwner(out)};
}

std::pair<IndexArray, std::vector<std::string>> materialArray(const std::string& mesh, bool base)
{
    scene::MeshPtr m = meshOf(mesh, base, "material_array");
    std::unique_ptr<uint32_t[]> slots;
    size_t faces = 0;
    int32_t top = std::max(0, int32_t(m->materialNames().size()) - 1);
    {
        nb::gil_scoped_release nogil;
        FaceLayout l = faceLayout(*m);
        faces = l.count();
        slots = std::make_unique<uint32_t[]>(faces);
        std::span<const int32_t> ids = m->faceAttrs().get<int32_t>(geo::attr::kMaterial);
        if (!ids.empty())
            for (size_t i = 0; i < faces; ++i)
            {
                const int32_t id = std::max(0, ids[l.face(i)]);
                slots[i] = uint32_t(id);
                top = std::max(top, id);
            }
    }
    std::vector<std::string> names;
    for (int32_t id = 0; id <= top; ++id)
        names.push_back(scene::materials::slotName(*m, id));
    const uint32_t* data = slots.get();
    return {IndexArray(data, {faces}, bufferOwner(slots)), std::move(names)};
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
 * Meshes as numpy arrays for lila.nodes.Node: points, faces, normals, uvs, and a points write that runs setMeshPoints.
 * Arrays are read-only snapshots; heavy copies run on TBB with the GIL released.
 */
void bindMeshArrays(nb::module_& m)
{
    m.def("mesh_points_array", &pointsArray, nb::arg("mesh"), nb::arg("base") = false, nb::arg("world") = false,
          "(N, 3) float64 points, one row per point in meshData's order; a view of the mesh itself when it can be.");
    m.def("mesh_face_arrays", &faceArrays, nb::arg("mesh"), nb::arg("base") = false,
          "(counts, points): corners per face (F,) and every face's point indices in a row (sum of counts,), both uint32.");
    m.def("mesh_normals_array", &normalsArray, nb::arg("mesh"), nb::arg("base") = false,
          "(C, 3) float32 normals per face-vertex (faces in order, corners around each face).");
    m.def("mesh_uv_array", &uvArray, nb::arg("mesh"), nb::arg("uv_set") = "uv0", nb::arg("base") = false,
          "(C, 2) float32 uvs per face-vertex, in mesh_normals_array's order.");
    m.def("mesh_material_array", &materialArray, nb::arg("mesh"), nb::arg("base") = false,
          "(slots, names): each face's material slot (F,) uint32 in mesh_face_arrays' face order, and the slot names it indexes.");
    m.def("mesh_uv_sets", &uvSets, nb::arg("mesh"), nb::arg("base") = false, "The mesh's uv set names (uv0, uv1, ...).");
    m.def("set_mesh_points", &setPoints, nb::arg("mesh"), nb::arg("points"),
          "Moves every base point from an (N, 3) float64 array through setMeshPoints; one undo step.");
}
} // namespace lila::python
