// The scene's meshes as one physics world: movers simulate, nearby meshes are hit, and results go back to transforms.

#include "Stagehand.h"

#include <lila/Mesh.h>
#include <lila/Profile.h>
#include <lila/Transform.h>

#include <algorithm>
#include <set>

using namespace lila;

namespace stagehand
{
namespace
{
/** World bounds of a mesh's evaluated output. */
bool worldBounds(Graph& g, NodeId id, glm::dvec3& lo, glm::dvec3& hi)
{
    Node* n = g.node(id);
    const int out = n ? n->type().attrIndex("outMesh") : -1;
    if (out < 0)
        return false;
    const auto mesh = n->as<scene::MeshPtr>(out);
    if (!mesh)
        return false;
    const glm::dmat4 w = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
    lo = glm::dvec3(1e300);
    hi = glm::dvec3(-1e300);
    bool any = false;
    for (geo::Index p = 0; p < mesh->pointCapacity(); ++p)
        if (mesh->pointValid(p))
        {
            const glm::dvec3 q(w * glm::dvec4(mesh->position(p), 1.0));
            lo = glm::min(lo, q);
            hi = glm::max(hi, q);
            any = true;
        }
    return any;
}

bool overlaps(const glm::dvec3& alo, const glm::dvec3& ahi, const glm::dvec3& blo, const glm::dvec3& bhi)
{
    return alo.x <= bhi.x && blo.x <= ahi.x && alo.y <= bhi.y && blo.y <= ahi.y && alo.z <= bhi.z && blo.z <= ahi.z;
}

physics::Role roleOf(RoleChoice r)
{
    switch (r)
    {
    case RoleChoice::Dynamic:
        return physics::Role::Dynamic;
    case RoleChoice::Kinematic:
        return physics::Role::Kinematic;
    default:
        return physics::Role::Static;
    }
}
} // namespace

World::World(Graph& g, const WorldOptions& o)
{
    LILA_ZONE();

    g.evaluate();
    physics::Settings settings;
    settings.gravity = o.gravity;
    settings.ground = o.ground;
    settings.groundHeight = o.groundHeight;
    settings.collisionSteps = o.collisionSteps;
    mScene = std::make_unique<physics::RigidScene>(settings);

    // Movers in the order given, then dynamic bodies in scene order, so the same scene always builds the same world.
    std::vector<NodeId> movers;
    std::set<NodeId> taken;
    for (NodeId id : o.movers)
        if (physical(g, id) && taken.insert(id).second)
            movers.push_back(id);
    if (o.includeDynamicBodies)
        for (NodeId id : g.nodes())
            if (!taken.count(id) && physical(g, id) && bodySettings(g, id).role == RoleChoice::Dynamic)
            {
                movers.push_back(id);
                taken.insert(id);
            }

    glm::dvec3 lo(1e300), hi(-1e300);
    for (NodeId id : movers)
    {
        glm::dvec3 a, b;
        if (worldBounds(g, id, a, b))
        {
            lo = glm::min(lo, a);
            hi = glm::max(hi, b);
        }
    }
    lo -= glm::dvec3(o.reach);
    hi += glm::dvec3(o.reach);

    // Colliders named are always in; without collidersGiven, everything nearby joins them.
    std::vector<NodeId> colliders;
    for (NodeId id : o.colliders)
        if (!taken.count(id) && physical(g, id) && taken.insert(id).second)
            colliders.push_back(id);
    if (!o.collidersGiven)
        for (NodeId id : g.nodes())
        {
            glm::dvec3 a, b;
            if (taken.count(id) || !physical(g, id) || !worldBounds(g, id, a, b) || !overlaps(a, b, lo, hi))
                continue;
            colliders.push_back(id);
            taken.insert(id);
        }

    auto add = [&](NodeId id, bool mover) {
        Node* n = g.node(id);
        const BodySettings s = bodySettings(g, id);
        const glm::dmat4 w = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        const Frame f = frameOf(w);
        physics::BodyDesc b;
        b.role = mover ? (s.role == RoleChoice::Kinematic ? physics::Role::Kinematic : physics::Role::Dynamic) : roleOf(s.role);
        if (!mover && b.role == physics::Role::Dynamic)
            b.role = physics::Role::Static;
        b.shape = shapeOf(g, id, s.shape, s.rounding, b.role == physics::Role::Static);
        b.position = f.position;
        b.rotation = f.rotation;
        b.density = s.density;
        b.friction = s.friction;
        b.bounce = s.bounce;
        b.user = int64_t(id);
        mScene->add(b);
        mNodes.push_back(id);
        mStart.push_back(w);
        mScale.push_back(f.scale);
    };
    for (NodeId id : movers)
        add(id, true);
    mMoverCount = movers.size();
    for (NodeId id : colliders)
        add(id, false);
}

int World::bodyOf(NodeId n) const
{
    const auto it = std::find(mNodes.begin(), mNodes.end(), n);
    return it == mNodes.end() ? -1 : int(it - mNodes.begin());
}

glm::dmat4 World::worldMatrix(int body) const
{
    const physics::Pose p = mScene->pose(body);
    return matrixOf({p.position, p.rotation, mScale[size_t(body)]});
}

void World::write(Graph& g) const
{
    LILA_ZONE();

    for (size_t i = 0; i < mMoverCount; ++i)
        setWorld(g, mNodes[i], worldMatrix(int(i)));
}

void World::revert(Graph& g) const
{
    for (size_t i = 0; i < mMoverCount; ++i)
        setWorld(g, mNodes[i], mStart[i]);
}
} // namespace stagehand
