#include "retopo/Handles.h"

#include "core/Profile.h"

#include <glm/gtx/rotate_vector.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace lila::retopo
{
namespace
{
/** Distances along the mesh's edges from the seeds, limited to the points marked in `in`. */
std::vector<double> edgeDistances(const geo::Mesh& m, const std::vector<uint8_t>& in, std::span<const std::pair<Index, double>> seeds)
{
    std::vector<double> dist(m.pointCapacity(), std::numeric_limits<double>::infinity());
    using Item = std::pair<double, Index>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> queue;
    for (auto [p, d] : seeds)
        if (d < dist[p])
        {
            dist[p] = d;
            queue.push({d, p});
        }
    while (!queue.empty())
    {
        auto [d, p] = queue.top();
        queue.pop();
        if (d > dist[p])
            continue;
        m.forEachPointEdge(p, [&](Index e) {
            Index q = m.edgeOtherPoint(e, p);
            if (!in[q])
                return;
            double nd = d + m.edgeLength(e);
            if (nd < dist[q])
            {
                dist[q] = nd;
                queue.push({nd, q});
            }
        });
    }
    return dist;
}
} // namespace

glm::dvec3 HandleXform::apply(const glm::dvec3& p) const
{
    glm::dvec3 local = (p - pivot) * scale;
    if (rotate != 0.0 && glm::length(axis) > 0.0)
        local = glm::rotate(local, glm::radians(rotate), glm::normalize(axis));
    return pivot + translate + local;
}

HandleRig::HandleRig(const geo::Mesh& mesh, std::span<const Index> region, std::span<const glm::dvec3> handles, double falloff)
{
    LILA_ZONE();

    std::vector<uint8_t> in(mesh.pointCapacity(), 0);
    for (Index p : region)
        if (p < mesh.pointCapacity() && mesh.pointValid(p) && !in[p])
        {
            in[p] = 1;
            mPoints.push_back(p);
        }
    std::sort(mPoints.begin(), mPoints.end());
    mHandles = handles.size();
    for (Index p : mPoints)
        mRest.push_back(mesh.position(p));
    if (mPoints.empty() || mHandles == 0)
        return;

    // The region's own border: points with a neighbour outside it, or on the mesh border.
    std::vector<std::pair<Index, double>> border;
    for (Index p : mPoints)
    {
        bool edge = false;
        mesh.forEachPointEdge(p, [&](Index e) { edge |= !in[mesh.edgeOtherPoint(e, p)]; });
        if (edge)
            border.push_back({p, 0.0});
    }
    std::vector<double> toBorder = border.empty() ? std::vector<double>(mesh.pointCapacity(), 1.0) : edgeDistances(mesh, in, border);
    double depth = 0.0;
    for (Index p : mPoints)
        if (std::isfinite(toBorder[p]))
            depth = std::max(depth, toBorder[p]);
    double fadeWidth = std::max(1e-12, depth * std::clamp(falloff, 0.0, 1.0));

    std::vector<std::vector<double>> perHandle;
    for (const glm::dvec3& h : handles)
    {
        Index nearest = mPoints.front();
        double best = std::numeric_limits<double>::max();
        for (Index p : mPoints)
            if (double d = glm::length(mesh.position(p) - h); d < best)
            {
                best = d;
                nearest = p;
            }
        std::pair<Index, double> seed{nearest, best};
        perHandle.push_back(edgeDistances(mesh, in, std::span(&seed, 1)));
    }
    mWeights.assign(mPoints.size() * mHandles, 0.0);
    for (size_t i = 0; i < mPoints.size(); ++i)
    {
        Index p = mPoints[i];
        double sum = 0.0;
        for (size_t j = 0; j < mHandles; ++j)
        {
            double d = perHandle[j][p];
            double w = std::isfinite(d) ? 1.0 / (d * d + 1e-12 * (depth * depth + 1e-12)) : 0.0;
            mWeights[i * mHandles + j] = w;
            sum += w;
        }
        double fade = border.empty() || falloff <= 0.0 ? 1.0 : std::clamp(toBorder[p] / fadeWidth, 0.0, 1.0);
        fade = fade * fade * (3.0 - 2.0 * fade);
        for (size_t j = 0; j < mHandles; ++j)
            mWeights[i * mHandles + j] = sum > 0.0 ? mWeights[i * mHandles + j] / sum * fade : 0.0;
    }
}

std::vector<glm::dvec3> HandleRig::deform(std::span<const HandleXform> xforms) const
{
    std::vector<glm::dvec3> out = mRest;
    if (xforms.size() != mHandles)
        return out;
    for (size_t i = 0; i < mPoints.size(); ++i)
    {
        glm::dvec3 delta(0.0);
        for (size_t j = 0; j < mHandles; ++j)
            if (double w = mWeights[i * mHandles + j]; w > 0.0)
                delta += w * (xforms[j].apply(mRest[i]) - mRest[i]);
        out[i] += delta;
    }
    return out;
}

std::vector<Index> regionAlong(const geo::Mesh& mesh, std::span<const glm::dvec3> path, double radius)
{
    std::vector<uint8_t> in(mesh.pointCapacity(), 0);
    auto near = [&](const glm::dvec3& c) {
        for (size_t i = 0; i < path.size(); ++i)
        {
            glm::dvec3 a = path[i], b = i + 1 < path.size() ? path[i + 1] : path[i];
            glm::dvec3 ab = b - a;
            double t = std::clamp(glm::dot(c - a, ab) / std::max(glm::dot(ab, ab), 1e-300), 0.0, 1.0);
            if (glm::length(a + ab * t - c) <= radius)
                return true;
        }
        return false;
    };
    for (Index f = 0; f < mesh.faceCapacity(); ++f)
        if (mesh.faceValid(f) && near(mesh.faceCentroid(f)))
            for (Index p : mesh.facePoints(f))
                in[p] = 1;
    std::vector<Index> out;
    for (Index p = 0; p < mesh.pointCapacity(); ++p)
        if (in[p])
            out.push_back(p);
    return out;
}
} // namespace lila::retopo
