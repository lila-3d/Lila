// The scene's meshes as one physics world: movers simulate, nearby meshes are hit, and results go back to transforms.

#include "Stagehand.h"

#include <lila/Command.h>
#include <lila/Mesh.h>
#include <lila/Profile.h>
#include <lila/Transform.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <set>
#include <unordered_map>

using namespace lila;

namespace stagehand
{
namespace
{
bool overlaps(const glm::dvec3& alo, const glm::dvec3& ahi, const glm::dvec3& blo, const glm::dvec3& bhi)
{
    return alo.x <= bhi.x && blo.x <= ahi.x && alo.y <= bhi.y && blo.y <= ahi.y && alo.z <= bhi.z && blo.z <= ahi.z;
}

/**
 * The movers' reach boxes on a hash grid, so finding the colliders near any of thousands of movers spread over a city
 * doesn't test every pair, and a collider counts only when it is near some mover, not merely inside their joint bounds.
 */
class ReachGrid
{
public:
    ReachGrid(std::vector<std::pair<glm::dvec3, glm::dvec3>> boxes) : mBoxes(std::move(boxes))
    {
        double biggest = 1.0;
        for (const auto& [lo, hi] : mBoxes)
            biggest = std::max(biggest, std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z}));
        mCell = biggest;
        for (size_t k = 0; k < mBoxes.size(); ++k)
            forCells(mBoxes[k].first, mBoxes[k].second, [&](uint64_t c) { mCells[c].push_back(uint32_t(k)); });
    }

    bool near(const glm::dvec3& lo, const glm::dvec3& hi) const
    {
        // A collider much bigger than the cells (a terrain, a city block) is tested against each box instead.
        const glm::dvec3 span = (hi - lo) / mCell;
        if (span.x * span.y * span.z > 4096.0)
        {
            for (const auto& [blo, bhi] : mBoxes)
                if (overlaps(lo, hi, blo, bhi))
                    return true;
            return false;
        }
        bool found = false;
        forCells(lo, hi, [&](uint64_t c) {
            if (found)
                return;
            const auto it = mCells.find(c);
            if (it == mCells.end())
                return;
            for (uint32_t k : it->second)
                if (overlaps(lo, hi, mBoxes[k].first, mBoxes[k].second))
                {
                    found = true;
                    return;
                }
        });
        return found;
    }

private:
    template <class F>
    void forCells(const glm::dvec3& lo, const glm::dvec3& hi, F&& f) const
    {
        const glm::dvec3 a = glm::floor(lo / mCell), b = glm::floor(hi / mCell);
        for (double z = a.z; z <= b.z; ++z)
            for (double y = a.y; y <= b.y; ++y)
                for (double x = a.x; x <= b.x; ++x)
                    f((uint64_t(int64_t(x) & 0x1FFFFF) << 42) | (uint64_t(int64_t(y) & 0x1FFFFF) << 21) | uint64_t(int64_t(z) & 0x1FFFFF));
    }

    std::vector<std::pair<glm::dvec3, glm::dvec3>> mBoxes;
    std::unordered_map<uint64_t, std::vector<uint32_t>> mCells;
    double mCell = 1.0;
};

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

    // Each mover reaches `reach` all round, and further down the way it falls, so a drop finds what it lands on.
    const glm::dvec3 down = glm::length(o.gravity) > 1e-9 ? glm::normalize(o.gravity) : glm::dvec3(0, -1, 0);
    std::vector<std::pair<glm::dvec3, glm::dvec3>> reachBoxes;
    glm::dvec3 lo(1e300), hi(-1e300);
    for (NodeId id : movers)
    {
        glm::dvec3 a, b;
        if (!worldBoundsOf(g, id, a, b))
            continue;
        lo = glm::min(lo, a);
        hi = glm::max(hi, b);
        a -= glm::dvec3(o.reach);
        b += glm::dvec3(o.reach);
        const glm::dvec3 fall = down * o.reach * 4.0;
        reachBoxes.emplace_back(glm::min(a, a + fall), glm::max(b, b + fall));
    }

    physics::Settings settings;
    settings.gravity = o.gravity;
    settings.ground = o.ground;
    settings.groundHeight = o.groundHeight;
    settings.collisionSteps = o.collisionSteps;
    settings.velocitySteps = o.velocitySteps;
    settings.maxMassRatio = o.maxMassRatio;
    // Centred on the movers, metre aligned, so a set kilometres out simulates as precisely as one at the origin.
    if (lo.x <= hi.x)
        settings.origin = glm::round((lo + hi) * 0.005) * 100.0;

    // Colliders named are always in; without collidersGiven, everything near a mover joins them.
    std::vector<NodeId> colliders;
    for (NodeId id : o.colliders)
        if (!taken.count(id) && physical(g, id) && taken.insert(id).second)
            colliders.push_back(id);
    if (!o.collidersGiven && !reachBoxes.empty())
    {
        const ReachGrid grid(std::move(reachBoxes));
        for (NodeId id : g.nodes())
        {
            glm::dvec3 a, b;
            if (taken.count(id) || !physical(g, id) || !worldBoundsOf(g, id, a, b) || !grid.near(a, b))
                continue;
            colliders.push_back(id);
            taken.insert(id);
        }
    }

    const size_t total = movers.size() + colliders.size();
    if (total > size_t(physics::maxBodiesLimit()))
        throw CommandError(std::format("{} bodies ({} moving, {} colliders within reach) is more than the {} physics takes at once: settle fewer at "
                                       "a time, lower reach=, or name colliders=",
                                       total, movers.size(), colliders.size(), physics::maxBodiesLimit()));
    settings.maxBodies = int(std::max<size_t>(65536, total + 16));
    mScene = std::make_unique<physics::RigidScene>(settings);

    auto add = [&](NodeId id, bool mover) {
        Node* n = g.node(id);
        const BodySettings s = bodySettings(g, id);
        const glm::dmat4 w = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        const Frame f = frameOf(w);
        physics::BodyDesc b;
        b.role = mover ? (s.role == RoleChoice::Kinematic ? physics::Role::Kinematic : physics::Role::Dynamic) : roleOf(s.role);
        if (!mover && b.role == physics::Role::Dynamic)
            b.role = physics::Role::Static;
        b.shape = shapeOf(g, id, s.shape, s.rounding, b.role == physics::Role::Static, s.maxParts);
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

physics::SettleResult World::settle(double cap)
{
    physics::SettleOptions so;
    so.cap = cap;
    return mScene->settleUntilRest(so);
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
