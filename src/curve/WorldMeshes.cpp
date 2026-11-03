#include "curve/WorldMeshes.h"

#include "core/Profile.h"

#include <cmath>
#include <limits>

namespace lila::curve
{
using D3 = glm::dvec3;

void WorldMeshes::add(std::shared_ptr<const geo::Mesh> mesh, const glm::dmat4& world)
{
    if (!mesh || mesh->numFaces() == 0)
        return;
    Item it;
    it.mesh = std::move(mesh);
    it.bvh = std::make_shared<geo::MeshBvh>(*it.mesh);
    it.world = world;
    it.toLocal = glm::inverse(world);
    it.normalToWorld = glm::transpose(glm::inverse(glm::dmat3(world)));
    it.scale = std::max(std::cbrt(std::abs(glm::determinant(glm::dmat3(world)))), 1e-12);
    mItems.push_back(std::move(it));
}

std::optional<SurfacePoint> WorldMeshes::raycast(const D3& origin, const D3& dir, double maxDistance) const
{
    LILA_ZONE();

    const double len = glm::length(dir);
    if (len < 1e-12)
        return std::nullopt;
    const D3 d = dir / len;
    std::optional<SurfacePoint> best;
    double bestT = maxDistance;
    for (const Item& it : mItems)
    {
        // In local space the ray keeps its world parameter: t cm along d in the world is t along M d locally.
        const D3 lo(it.toLocal * glm::dvec4(origin, 1.0));
        const D3 ld = glm::dmat3(it.toLocal) * d;
        geo::BvhHit hit;
        if (!it.bvh->raycast(lo, ld, hit, bestT) || !it.mesh->faceValid(hit.face) || hit.t >= bestT)
            continue;
        bestT = hit.t;
        D3 n = it.normalToWorld * it.mesh->faceNormal(hit.face);
        n = glm::length(n) > 1e-12 ? glm::normalize(n) : -d;
        if (glm::dot(n, d) > 0.0)
            n = -n;
        best = SurfacePoint{origin + d * hit.t, n};
    }
    return best;
}

double WorldMeshes::signedDistance(const D3& p, double maxDistance) const
{
    // Unbounded, so a point deep inside a closed mesh still finds the surface it is inside of.
    double best = std::numeric_limits<double>::max();
    bool inside = false;
    for (const Item& it : mItems)
    {
        const D3 lp(it.toLocal * glm::dvec4(p, 1.0));
        geo::BvhClosest c;
        if (!it.bvh->closestPoint(lp, c, best < std::numeric_limits<double>::max() ? best / it.scale * 1.01 + 1e-9 : best) || !it.mesh->faceValid(c.face))
            continue;
        const D3 w(it.world * glm::dvec4(c.point, 1.0));
        const double dist = glm::distance(w, p);
        if (dist >= best)
            continue;
        best = dist;
        inside = glm::dot(p - w, it.normalToWorld * it.mesh->faceNormal(c.face)) < 0.0;
    }
    best = std::min(best, maxDistance);
    return inside ? -best : best;
}

geo::Bounds WorldMeshes::bounds() const
{
    geo::Bounds b;
    for (const Item& it : mItems)
    {
        geo::Bounds lb = it.mesh->bounds();
        for (int k = 0; k < 8; ++k)
            b.extend(D3(it.world * glm::dvec4(k & 1 ? lb.max.x : lb.min.x, k & 2 ? lb.max.y : lb.min.y, k & 4 ? lb.max.z : lb.min.z, 1.0)));
    }
    return b;
}
} // namespace lila::curve
