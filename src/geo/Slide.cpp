#include "geo/Slide.h"

#include "core/Profile.h"
#include "geo/Topology.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace lila::geo
{
namespace
{
glm::dvec3 unitOr0(const glm::dvec3& v)
{
    const double l = glm::length(v);
    return l > 1e-12 ? v / l : glm::dvec3(0.0);
}

glm::dvec3 railDir(const Mesh& m, Index from, Index to, const glm::dmat3& toWorld)
{
    return unitOr0(toWorld * (m.position(to) - m.position(from)));
}

/** The face beside edge e on side s of its a->b direction: side 0 is the face whose winding runs a->b. */
Index faceOnSide(const Mesh& m, Index e, Index a, int s)
{
    Index out = kInvalid;
    m.forEachEdgeCorner(e, [&](Index c) {
        if (out == kInvalid && (m.cornerPoint(c) == a) == (s == 0))
            out = m.cornerFace(c);
    });
    return out;
}

/** Unselected edges at v crossed turning around it from edge `from` through face f, until a selected edge or a border. */
std::vector<Index> fanEdges(const Mesh& m, Index v, Index f, Index from, const std::vector<uint8_t>& selected)
{
    std::vector<Index> crossed;
    for (int guard = 0; guard < 64 && f != kInvalid; ++guard)
    {
        const Index c = m.findCorner(f, v);
        if (c == kInvalid)
            break;
        const Index a = m.cornerEdge(c), b = m.cornerEdge(m.cornerPrev(c));
        const Index other = a == from ? b : a;
        if (other == kInvalid || other == from || selected[other])
            break;
        crossed.push_back(other);
        f = m.oppositeFace(other, f);
        from = other;
    }
    return crossed;
}
} // namespace

SlideRails edgeSlideRails(const Mesh& m, std::span<const Index> edges, const glm::dvec3* direction, const glm::dmat3& toWorld)
{
    LILA_ZONE();

    SlideRails out;
    std::vector<Index> valid;
    for (Index e : edges)
        if (m.edgeValid(e))
            valid.push_back(e);
    const std::vector<uint8_t> selected = makeMask(m.edgeCapacity(), valid);
    std::vector<uint8_t> done(m.pointCapacity(), 0);
    for (const EdgePath& path : chainEdges(m, valid))
    {
        const size_t first = out.points.size();
        const size_t n = path.points.size();
        for (size_t i = 0; i < n; ++i)
        {
            const Index v = path.points[i];
            if (done[v])
                continue;
            // The path's edges at v, each with the point it runs from.
            struct Along
            {
                Index edge, from;
            };
            std::vector<Along> at;
            if (i < path.edges.size())
                at.push_back({path.edges[i], v});
            if (i > 0)
                at.push_back({path.edges[i - 1], path.points[i - 1]});
            else if (path.closed)
                at.push_back({path.edges.back(), path.points.back()});
            std::array<Index, 2> ends{kInvalid, kInvalid};
            for (int s = 0; s < 2; ++s)
                for (const Along& a : at)
                {
                    const Index f = faceOnSide(m, a.edge, a.from, s);
                    if (f == kInvalid)
                        continue;
                    const std::vector<Index> fan = fanEdges(m, v, f, a.edge, selected);
                    if (fan.empty())
                        continue;
                    Index pick = fan.front();
                    // Between two selected edges, an uneven fan (a pole beside the loop) takes the edge down its middle.
                    if (at.size() > 1 && fan.size() > 1)
                    {
                        glm::dvec3 mean(0.0);
                        for (Index r : fan)
                            mean += railDir(m, v, m.edgeOtherPoint(r, v), toWorld);
                        double best = -std::numeric_limits<double>::infinity();
                        for (Index r : fan)
                            if (const double d = glm::dot(railDir(m, v, m.edgeOtherPoint(r, v), toWorld), mean); d > best)
                            {
                                best = d;
                                pick = r;
                            }
                    }
                    ends[size_t(s)] = m.edgeOtherPoint(pick, v);
                    break;
                }
            done[v] = 1;
            out.points.push_back(v);
            out.ends.push_back(ends);
        }
        if (!direction)
            continue;
        glm::dvec3 lean(0.0);
        for (size_t k = first; k < out.points.size(); ++k)
        {
            const Index v = out.points[k];
            if (out.ends[k][0] != kInvalid)
                lean += railDir(m, v, out.ends[k][0], toWorld);
            if (out.ends[k][1] != kInvalid)
                lean -= railDir(m, v, out.ends[k][1], toWorld);
        }
        if (glm::dot(lean, *direction) < 0.0)
            for (size_t k = first; k < out.points.size(); ++k)
                std::swap(out.ends[k][0], out.ends[k][1]);
    }
    return out;
}

SlideRails vertexSlideRails(const Mesh& m, std::span<const Index> points, const glm::dvec3& direction, const glm::dmat3& toWorld)
{
    LILA_ZONE();

    SlideRails out;
    const glm::dvec3 dir = unitOr0(direction);
    for (Index v : points)
    {
        if (!m.pointValid(v))
            continue;
        std::array<Index, 2> ends{kInvalid, kInvalid};
        double most = -2.0, least = 2.0;
        m.forEachPointEdge(v, [&](Index e) {
            const Index o = m.edgeOtherPoint(e, v);
            const double d = glm::dot(railDir(m, v, o, toWorld), dir);
            if (d > most)
            {
                most = d;
                ends[0] = o;
            }
            if (d < least)
            {
                least = d;
                ends[1] = o;
            }
        });
        // A point with one edge can only go back along it.
        if (ends[0] == ends[1])
            (most >= -least ? ends[1] : ends[0]) = kInvalid;
        out.points.push_back(v);
        out.ends.push_back(ends);
    }
    return out;
}

std::vector<glm::dvec3> slidePositions(const Mesh& m, const SlideRails& rails, double amount, bool even, const glm::dmat3& toWorld)
{
    amount = std::clamp(amount, -1.0, 1.0);
    const int s = amount < 0.0 ? 1 : 0;
    const double t = std::abs(amount);
    double shortest = std::numeric_limits<double>::infinity();
    if (even)
        for (size_t k = 0; k < rails.points.size(); ++k)
            if (const Index to = rails.ends[k][size_t(s)]; to != kInvalid)
                shortest = std::min(shortest, glm::length(toWorld * (m.position(to) - m.position(rails.points[k]))));
    std::vector<glm::dvec3> out(rails.points.size());
    for (size_t k = 0; k < rails.points.size(); ++k)
    {
        const glm::dvec3 p = m.position(rails.points[k]);
        const Index to = rails.ends[k][size_t(s)];
        if (to == kInvalid)
        {
            out[k] = p;
            continue;
        }
        double f = t;
        if (even)
        {
            const double len = glm::length(toWorld * (m.position(to) - p));
            f = len > 1e-12 ? std::min(1.0, t * shortest / len) : 0.0;
        }
        out[k] = glm::mix(p, m.position(to), f);
    }
    return out;
}
} // namespace lila::geo
