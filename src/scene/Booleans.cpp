#include "cmds/Command.h"
#include "cmds/Undo.h"
#include "core/Profile.h"
#include "geo/ColorSets.h"
#include "geo/Mesh.h"
#include "geo/Normals.h"
#include "geo/Ops.h"
#include "geo/Triangulate.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"

#include <manifold/manifold.h>

#include <unordered_map>

namespace lila::scene
{
namespace
{
using geo::Index;

constexpr int kProps = 5; // x y z u v, then rgba per color set

/**
 * World-space MeshGL with one vertex per corner (so uvs and the base's color sets survive), welded by position for
 * manifoldness. A mesh without one of the sets contributes white.
 */
manifold::MeshGL toManifold(const geo::Mesh& mesh, const glm::dmat4& world, uint32_t faceOffset, std::vector<Index>& faceOf,
                            const std::vector<std::string>& colorSets)
{
    geo::Triangulation tris = geo::triangulate(mesh);
    auto uv = mesh.cornerAttrs().get<glm::vec2>("uv0");
    std::vector<std::span<const glm::vec4>> colors;
    for (const std::string& set : colorSets)
        colors.push_back(geo::colors::values(mesh, set));
    manifold::MeshGL gl;
    gl.numProp = uint32_t(kProps + 4 * colorSets.size());
    std::unordered_map<Index, uint32_t> vertOfCorner;
    for (size_t t = 0; t < tris.faces.size(); ++t)
    {
        for (int k = 0; k < 3; ++k)
        {
            Index c = tris.corners[t * 3 + k];
            auto [it, fresh] = vertOfCorner.try_emplace(c, uint32_t(gl.vertProperties.size() / gl.numProp));
            if (fresh)
            {
                glm::dvec3 p = glm::dvec3(world * glm::dvec4(mesh.position(mesh.cornerPoint(c)), 1.0));
                glm::vec2 t2 = c < uv.size() ? uv[c] : glm::vec2(0.0f);
                gl.vertProperties.insert(gl.vertProperties.end(), {float(p.x), float(p.y), float(p.z), t2.x, t2.y});
                for (const auto& col : colors)
                {
                    glm::vec4 v = c < col.size() ? col[c] : glm::vec4(1.0f);
                    gl.vertProperties.insert(gl.vertProperties.end(), {v.r, v.g, v.b, v.a});
                }
            }
            gl.triVerts.push_back(it->second);
        }
        gl.faceID.push_back(faceOffset + uint32_t(faceOf.size()));
        faceOf.push_back(tris.faces[t]);
    }
    gl.Merge();
    return gl;
}

/** Rebuilds a polygon mesh in `toLocal` space: triangles sharing an input face merge back into n-gons. */
geo::Mesh fromManifold(const manifold::MeshGL& gl, const glm::dmat4& toLocal, const geo::Mesh& base)
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
            points.push_back(glm::dvec3(toLocal * glm::dvec4(p[0], p[1], p[2], 1.0)));
        }
        pointOf[v] = pointOf[c];
    }
    std::vector<Index> sizes(gl.NumTri(), 3), idx;
    idx.reserve(gl.triVerts.size());
    for (uint32_t v : gl.triVerts)
        idx.push_back(pointOf[v]);
    geo::Mesh m = geo::Mesh::fromPolygons(points, sizes, idx);

    const std::vector<std::string> sets = geo::colors::sets(base);
    for (const std::string& set : sets)
        geo::colors::add(m, set);
    auto uv = m.cornerAttrs().getOrAdd<glm::vec2>("uv0");
    std::vector<std::span<glm::vec4>> colors;
    for (const std::string& set : sets)
        colors.push_back(geo::colors::values(m, set));
    for (Index f = 0; f < m.faceCapacity() && f < gl.NumTri(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        int k = 0;
        m.forEachFaceCorner(f, [&](Index c) {
            const float* p = &gl.vertProperties[size_t(gl.triVerts[f * 3 + k]) * gl.numProp];
            uv[c] = gl.numProp >= 5 ? glm::vec2(p[3], p[4]) : glm::vec2(0.0f);
            for (size_t s = 0; s < colors.size() && kProps + 4 * s + 3 < gl.numProp; ++s)
                colors[s][c] = glm::vec4(p[kProps + 4 * s], p[kProps + 4 * s + 1], p[kProps + 4 * s + 2], p[kProps + 4 * s + 3]);
            ++k;
        });
    }
    m.colorSetInfo() = base.colorSetInfo();

    // Triangles from the same input face are one polygon again: dissolve the edges between them.
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

void registerBooleanCommands()
{
    using T = ArgType;
    cmds::registerCommand({
        .name = "polyBoolean",
        .doc = "Union, difference or intersection of meshes (Manifold: robust, watertight output). The first object keeps the "
               "result; the others are deleted unless keepCutters=True.",
        .positionalDoc = "base cutters...",
        .flags = {{"operation", "", T::String, "difference", "union, difference or intersection.", {"union", "difference", "intersection"}},
                  {"keepCutters", "", T::Bool, false, "Keep the cutter objects (hidden)."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            auto objs = a.objects();
            if (objs.size() < 2)
                throw CommandError("polyBoolean: give a base and at least one cutter");
            std::string op = a.get<std::string>("operation", "difference");
            if (op != "union" && op != "difference" && op != "intersection")
                throw CommandError("polyBoolean: operation must be union, difference or intersection");
            Graph& g = Graph::current();
            g.evaluate();
            std::vector<Node*> nodes;
            for (const std::string& name : objs)
            {
                Node* n = g.find(name);
                if (!n || !n->type().isA("mesh") || !n->as<MeshPtr>(MeshNode::kOutMesh))
                    throw CommandError("polyBoolean: " + name + " is not a mesh");
                nodes.push_back(n);
            }

            std::vector<Index> faceOf;
            const geo::Mesh& baseMesh = *nodes[0]->as<MeshPtr>(MeshNode::kOutMesh);
            const std::vector<std::string> colorSets = geo::colors::sets(baseMesh);
            auto toSolid = [&](Node* n) {
                manifold::Manifold solid(toManifold(*n->as<MeshPtr>(MeshNode::kOutMesh), n->as<glm::dmat4>(Transform::kWorldMatrix),
                                                    uint32_t(faceOf.size()), faceOf, colorSets));
                if (solid.Status() != manifold::Manifold::Error::NoError)
                    throw CommandError("polyBoolean: " + n->name() + " isn't a closed solid (" + std::to_string(int(solid.Status())) + ")");
                return solid;
            };
            manifold::Manifold result = toSolid(nodes[0]);
            for (size_t i = 1; i < nodes.size(); ++i)
            {
                manifold::Manifold cutter = toSolid(nodes[i]);
                result = op == "union" ? result + cutter : op == "difference" ? result - cutter : result ^ cutter;
            }
            glm::dmat4 toLocal = glm::inverse(nodes[0]->as<glm::dmat4>(Transform::kWorldMatrix));
            geo::Mesh out = fromManifold(result.GetMeshGL(), toLocal, baseMesh);

            UndoChunk chunk("Boolean");
            PlugRef base = modifiers::editPlug(g, nodes[0]->id());
            g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(out))));
            for (size_t i = 1; i < nodes.size(); ++i)
            {
                if (a.get<bool>("keepCutters"))
                    g.setValue({nodes[i]->id(), Transform::kVisibility, -1}, false);
                else
                    g.deleteNode(nodes[i]->id());
            }
            g.select({nodes[0]->id()});
            return nodes[0]->name();
        },
    });
    cmds::setAdjustable("polyBoolean");
}
} // namespace lila::scene
