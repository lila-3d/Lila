#include "gn/NodesCommon.h"

#include "core/Profile.h"
#include "geo/Normals.h"
#include "geo/Ops.h"
#include "geo/Triangulate.h"

#include <manifold/manifold.h>

#include <unordered_map>

namespace lila::gn::nodes
{
namespace
{
constexpr int kProps = 5; // x y z u v

/** One vertex per corner so uvs survive; Merge() welds by position so the solid is closed. */
manifold::MeshGL toManifold(const geo::Mesh& mesh, uint32_t faceOffset)
{
    geo::Triangulation tris = geo::triangulate(mesh);
    auto uv = mesh.cornerAttrs().get<glm::vec2>("uv0");
    manifold::MeshGL gl;
    gl.numProp = kProps;
    std::unordered_map<Index, uint32_t> vertOfCorner;
    for (size_t t = 0; t < tris.faces.size(); ++t)
    {
        for (int k = 0; k < 3; ++k)
        {
            Index c = tris.corners[t * 3 + k];
            auto [it, fresh] = vertOfCorner.try_emplace(c, uint32_t(gl.vertProperties.size() / kProps));
            if (fresh)
            {
                glm::dvec3 p = mesh.position(mesh.cornerPoint(c));
                glm::vec2 t2 = c < uv.size() ? uv[c] : glm::vec2(0.0f);
                gl.vertProperties.insert(gl.vertProperties.end(), {float(p.x), float(p.y), float(p.z), t2.x, t2.y});
            }
            gl.triVerts.push_back(it->second);
        }
        gl.faceID.push_back(faceOffset + tris.faces[t]);
    }
    gl.Merge();
    return gl;
}

/** Triangles that came from the same input face merge back into one polygon. */
geo::Mesh fromManifold(const manifold::MeshGL& gl)
{
    LILA_ZONE();

    size_t verts = gl.NumVert();
    std::vector<uint32_t> canon(verts);
    for (size_t v = 0; v < verts; ++v)
        canon[v] = uint32_t(v);
    for (size_t i = 0; i < gl.mergeFromVert.size(); ++i)
        canon[gl.mergeFromVert[i]] = gl.mergeToVert[i];
    std::vector<Index> pointOf(verts, geo::kInvalid);
    std::vector<glm::dvec3> points;
    for (size_t v = 0; v < verts; ++v)
    {
        uint32_t c = canon[v];
        if (pointOf[c] == geo::kInvalid)
        {
            const float* p = &gl.vertProperties[size_t(c) * gl.numProp];
            pointOf[c] = Index(points.size());
            points.push_back({p[0], p[1], p[2]});
        }
        pointOf[v] = pointOf[c];
    }
    std::vector<Index> sizes(gl.NumTri(), 3), idx;
    for (uint32_t v : gl.triVerts)
        idx.push_back(pointOf[v]);
    geo::Mesh m = geo::Mesh::fromPolygons(points, sizes, idx);
    auto uv = m.cornerAttrs().getOrAdd<glm::vec2>("uv0");
    for (Index f = 0; f < m.faceCapacity() && f < gl.NumTri(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        int k = 0;
        m.forEachFaceCorner(f, [&](Index c) {
            const float* p = &gl.vertProperties[size_t(gl.triVerts[f * 3 + k]) * gl.numProp];
            uv[c] = gl.numProp >= 5 ? glm::vec2(p[3], p[4]) : glm::vec2(0.0f);
            ++k;
        });
    }
    std::vector<Index> inner;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
    {
        if (!m.edgeValid(e) || m.edgeFaceCount(e) != 2)
            continue;
        Index c = m.edgeCorner(e);
        Index f0 = m.cornerFace(c), f1 = m.cornerFace(m.cornerRadial(c));
        if (f0 < gl.faceID.size() && f1 < gl.faceID.size() && gl.faceID[f0] == gl.faceID[f1])
            inner.push_back(e);
    }
    geo::dissolveEdges(m, inner, true);
    m.compact();
    geo::setHardEdgesByAngle(m, 30.0);
    return m;
}
} // namespace

void registerBooleanNode()
{
    registerDecl({
        .type = "MeshBoolean",
        .label = "Mesh Boolean",
        .category = "Mesh",
        .doc = "Union, difference or intersection of closed meshes (Manifold). Difference subtracts every mesh B from mesh A.",
        .props = {prop("operation", {"difference", "union", "intersect"}, "difference")},
        .sockets = {.inputs = {geoIn("meshA"), multiIn("meshB")}, .outputs = {geoOut("mesh")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                std::string op = c.prop("operation");
                std::vector<GeometryPtr> parts;
                if (op == "difference")
                    parts.push_back(c.geometry("meshA"));
                else
                {
                    GeometryPtr a = c.geometry("meshA");
                    if (!a->empty())
                        parts.push_back(a);
                }
                for (const GeometryPtr& b : c.geometries("meshB"))
                    parts.push_back(b);
                uint32_t offset = 0;
                std::optional<manifold::Manifold> result;
                for (size_t i = 0; i < parts.size(); ++i)
                {
                    Geometry r = realize(*parts[i]);
                    if (!r.mesh || r.mesh->numFaces() == 0)
                    {
                        // Nothing to subtract from.
                        if (op == "difference" && i == 0)
                            break;
                        continue;
                    }
                    manifold::Manifold solid(toManifold(*r.mesh, offset));
                    offset += r.mesh->faceCapacity();
                    if (solid.Status() != manifold::Manifold::Error::NoError)
                        throw ExecError("an input isn't a closed solid");
                    if (!result)
                        result = solid;
                    else
                        result = op == "union" ? *result + solid : op == "difference" ? *result - solid : *result ^ solid;
                }
                c.out("mesh", result ? meshGeometry(fromManifold(result->GetMeshGL())) : makeGeometry({}));
            },
    });
}
} // namespace lila::gn::nodes
