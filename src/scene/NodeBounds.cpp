#include "scene/NodeBounds.h"

#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

namespace lila::scene
{
geo::Bounds objectBounds(Graph& g, NodeId id)
{
    geo::Bounds b;
    Node* n = g.node(id);
    if (!n)
        return b;
    if (const components::PointProvider* pp = components::pointProvider(*n))
    {
        for (const auto& [cid, p] : pp->points(g, id))
            b.extend(p);
        return b;
    }
    if (n->type().isA("mesh"))
        if (const MeshPtr& m = n->as<MeshPtr>(MeshNode::kOutMesh); m && m->numPoints())
            b = m->bounds();
    return b;
}

geo::Bounds worldBounds(Graph& g, NodeId id)
{
    geo::Bounds local = objectBounds(g, id), out;
    if (!local.valid())
        return out;
    const glm::dmat4 w = g.node(id)->as<glm::dmat4>(Transform::kWorldMatrix);
    for (int c = 0; c < 8; ++c)
        out.extend(glm::dvec3(w * glm::dvec4(c & 1 ? local.max.x : local.min.x, c & 2 ? local.max.y : local.min.y, c & 4 ? local.max.z : local.min.z, 1.0)));
    return out;
}
} // namespace lila::scene
