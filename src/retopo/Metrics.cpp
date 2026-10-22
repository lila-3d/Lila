#include "retopo/Retopo.h"

#include "core/Profile.h"
#include "geo/Normals.h"

#include <tbb/blocked_range.h>
#include <tbb/parallel_reduce.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace lila::retopo
{
QualityMetrics measureQuality(const geo::Mesh& m, const geo::MeshBvh* source)
{
    LILA_ZONE();

    QualityMetrics q;
    double angleSum = 0.0;
    Index angleCount = 0;
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        ++q.faces;
        Index n = m.faceSize(f);
        if (n == 3)
            ++q.triangles;
        else if (n == 4)
        {
            ++q.quads;
            m.forEachFaceCorner(f, [&](Index c) {
                double dev = std::abs(glm::degrees(geo::cornerAngle(m, c)) - 90.0);
                q.maxAngleDeviation = std::max(q.maxAngleDeviation, dev);
                angleSum += dev;
                ++angleCount;
            });
        }
        else
            ++q.ngons;
    }
    q.quadPercent = q.faces ? 100.0 * double(q.quads) / double(q.faces) : 0.0;
    q.meanAngleDeviation = angleCount ? angleSum / double(angleCount) : 0.0;
    for (Index p = 0; p < m.pointCapacity(); ++p)
    {
        if (!m.pointValid(p))
            continue;
        if (m.isBoundaryPoint(p))
        {
            ++q.boundaryPoints;
            continue;
        }
        Index v = m.pointValence(p);
        if (q.valence.size() <= v)
            q.valence.resize(v + 1, 0);
        ++q.valence[v];
        if (v != 4)
            ++q.irregular;
    }
    if (source && !source->empty())
    {
        // Points and face centres both, so a face bulging off the surface counts too.
        std::vector<glm::dvec3> samples;
        for (Index p = 0; p < m.pointCapacity(); ++p)
            if (m.pointValid(p))
                samples.push_back(m.position(p));
        for (Index f = 0; f < m.faceCapacity(); ++f)
            if (m.faceValid(f))
                samples.push_back(m.faceCentroid(f));
        using Pair = std::pair<double, double>;
        Pair r = tbb::parallel_reduce(
            tbb::blocked_range<size_t>(0, samples.size()), Pair{0.0, 0.0},
            [&](const tbb::blocked_range<size_t>& range, Pair acc) {
                for (size_t i = range.begin(); i != range.end(); ++i)
                {
                    geo::BvhClosest c;
                    if (source->closestPoint(samples[i], c))
                    {
                        acc.first = std::max(acc.first, c.distance);
                        acc.second += c.distance;
                    }
                }
                return acc;
            },
            [](Pair a, Pair b) { return Pair{std::max(a.first, b.first), a.second + b.second}; });
        q.maxDistance = r.first;
        q.meanDistance = samples.empty() ? 0.0 : r.second / double(samples.size());
    }
    return q;
}

std::string QualityMetrics::summary() const
{
    std::string val;
    for (size_t k = 0; k < valence.size(); ++k)
        if (valence[k])
            val += std::format("{}{}:{}", val.empty() ? "" : " ", k, valence[k]);
    return std::format("{} faces, {:.1f}% quads ({} tris, {} ngons), valence [{}], {} irregular, angle dev max {:.1f} mean {:.1f}, "
                       "distance max {:.4g} mean {:.4g}",
                       faces, quadPercent, triangles, ngons, val, irregular, maxAngleDeviation, meanAngleDeviation, maxDistance,
                       meanDistance);
}
} // namespace lila::retopo
