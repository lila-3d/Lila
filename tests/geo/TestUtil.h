#pragma once

#include "geo/Mesh.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace lila::geo::test
{
inline bool isValid(const Mesh& m, uint32_t flags = kValidateDefault)
{
    const std::vector<std::string> issues = m.validate(flags);
    for (size_t i = 0; i < issues.size() && i < 10; ++i)
        MESSAGE(issues[i]);
    return issues.empty();
}

inline Index boundaryEdgeCount(const Mesh& m)
{
    Index n = 0;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.isBoundaryEdge(e))
            ++n;
    return n;
}

inline int euler(const Mesh& m)
{
    return int(m.numPoints()) - int(m.numEdges()) + int(m.numFaces());
}

/** Signed volume of a closed mesh; positive when faces point outward. */
inline double signedVolume(const Mesh& m)
{
    double v = 0.0;
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        const Index c0 = m.faceCorner(f);
        const glm::dvec3 a = m.position(m.cornerPoint(c0));
        for (Index c = m.cornerNext(c0); m.cornerNext(c) != c0; c = m.cornerNext(c))
            v += glm::dot(a, glm::cross(m.position(m.cornerPoint(c)), m.position(m.cornerNextPoint(c)))) / 6.0;
    }
    return v;
}

/** True when no two faces cover the same uv area and no face is mirrored (fan triangulation). */
inline bool uvsDontOverlap(const Mesh& m, const char* set = "uv0", int res = 256)
{
    std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>(set);
    if (uv.empty())
        return false;
    std::vector<uint8_t> cover(size_t(res) * res, 0);
    bool ok = true;
    for (Index f = 0; f < m.faceCapacity() && ok; ++f)
    {
        if (!m.faceValid(f))
            continue;
        const Index c0 = m.faceCorner(f);
        std::vector<uint8_t> mine(size_t(res) * res, 0);
        double area = 0.0;
        for (Index c = m.cornerNext(c0); m.cornerNext(c) != c0; c = m.cornerNext(c))
        {
            const glm::dvec2 a = uv[c0], b = uv[c], d = uv[m.cornerNext(c)];
            const double cr = (b.x - a.x) * (d.y - a.y) - (b.y - a.y) * (d.x - a.x);
            area += cr;
            if (std::abs(cr) < 1e-14)
                continue;
            const glm::dvec2 lo = glm::min(a, glm::min(b, d)) * double(res);
            const glm::dvec2 hi = glm::max(a, glm::max(b, d)) * double(res);
            for (int y = std::max(0, int(lo.y) - 1); y < std::min(res, int(hi.y) + 2); ++y)
                for (int x = std::max(0, int(lo.x) - 1); x < std::min(res, int(hi.x) + 2); ++x)
                {
                    const glm::dvec2 p((x + 0.5) / res, (y + 0.5) / res);
                    const double w0 = ((b.x - p.x) * (d.y - p.y) - (b.y - p.y) * (d.x - p.x)) / cr;
                    const double w1 = ((d.x - p.x) * (a.y - p.y) - (d.y - p.y) * (a.x - p.x)) / cr;
                    const double w2 = 1.0 - w0 - w1;
                    if (w0 > 1e-9 && w1 > 1e-9 && w2 > 1e-9)
                        mine[size_t(y) * res + x] = 1;
                }
        }
        if (area < 0.0)
        {
            MESSAGE("face " << f << " has mirrored uvs");
            ok = false;
        }
        for (size_t i = 0; i < mine.size(); ++i)
            if (mine[i])
            {
                if (cover[i])
                {
                    MESSAGE("face " << f << " overlaps in uv");
                    ok = false;
                    break;
                }
                cover[i] = 1;
            }
    }
    return ok;
}
} // namespace lila::geo::test
