#include "geo/FoldEdges.h"

#include "core/Profile.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace lila::geo
{
namespace
{
double pointSegmentDistance(const glm::dvec3& p, const glm::dvec3& a, const glm::dvec3& b)
{
    const glm::dvec3 ab = b - a;
    const double len2 = glm::dot(ab, ab);
    const double t = len2 > 0.0 ? std::clamp(glm::dot(p - a, ab) / len2, 0.0, 1.0) : 0.0;
    return glm::length(p - (a + ab * t));
}

/** The edge's two faces, false unless it has exactly two. */
bool twoFaces(const Mesh& m, Index e, Index& f0, Index& f1)
{
    if (!m.edgeValid(e) || !m.isManifoldEdge(e))
        return false;
    const Index c = m.edgeCorner(e);
    f0 = m.cornerFace(c);
    f1 = m.cornerFace(m.cornerRadial(c));
    return f0 != f1;
}

double foldAngle(const glm::dvec3& n0, const glm::dvec3& n1)
{
    return glm::degrees(std::acos(std::clamp(glm::dot(n0, n1), -1.0, 1.0)));
}

/** s < 0 on a convex edge: each face's centroid lies behind the other face's plane. */
double convexity(const glm::dvec3& m, const glm::dvec3& c0, const glm::dvec3& n0, const glm::dvec3& c1, const glm::dvec3& n1)
{
    return glm::dot(c1 - m, n0) + glm::dot(c0 - m, n1);
}

double widthAt(const Mesh& m, Index f, Index e)
{
    const Index a = m.edgePoint(e, 0), b = m.edgePoint(e, 1);
    const glm::dvec3 mid = (m.position(a) + m.position(b)) * 0.5;
    double best = std::numeric_limits<double>::max();
    if (m.faceSize(f) == 3)
    {
        m.forEachFaceCorner(f, [&](Index c) {
            const Index p = m.cornerPoint(c);
            if (p != a && p != b)
                best = std::min(best, glm::length(m.position(p) - mid));
        });
        return best;
    }
    m.forEachFaceCorner(f, [&](Index c) {
        const Index p = m.cornerPoint(c), q = m.cornerNextPoint(c);
        if (p != a && p != b && q != a && q != b)
            best = std::min(best, pointSegmentDistance(mid, m.position(p), m.position(q)));
    });
    return best;
}

/** At a valence-4 interior point, the edge across from e (sharing no face with it). */
Index oppositeEdge(const Mesh& m, Index e, Index p)
{
    Index f0, f1;
    if (!twoFaces(m, e, f0, f1))
        return kInvalid;
    Index found = kInvalid;
    int count = 0;
    m.forEachPointEdge(p, [&](Index o) {
        if (o == e)
            return;
        bool touches = false;
        m.forEachEdgeCorner(o, [&](Index c) { touches |= m.cornerFace(c) == f0 || m.cornerFace(c) == f1; });
        if (!touches)
        {
            found = o;
            ++count;
        }
    });
    return count == 1 ? found : kInvalid;
}

std::vector<Index> maskToList(const std::vector<uint8_t>& mask)
{
    std::vector<Index> out;
    for (Index e = 0; e < Index(mask.size()); ++e)
        if (mask[e])
            out.push_back(e);
    return out;
}

void completeCorners(const Mesh& m, std::vector<uint8_t>& sel, const std::vector<uint8_t>& usable)
{
    LILA_ZONE();

    const std::vector<uint8_t> base = sel;
    auto selectedAt = [&](Index p, std::vector<Index>* out) {
        int n = 0;
        m.forEachPointEdge(p, [&](Index e) {
            if (base[e])
            {
                ++n;
                if (out)
                    out->push_back(e);
            }
        });
        return n;
    };

    std::vector<uint8_t> seen(m.pointCapacity(), 0);
    std::vector<Index> here, path;
    for (Index e0 = 0; e0 < m.edgeCapacity(); ++e0)
    {
        if (!base[e0])
            continue;
        for (int s = 0; s < 2; ++s)
        {
            const Index v = m.edgePoint(e0, s);
            if (seen[v])
                continue;
            seen[v] = 1;
            here.clear();
            const int n = selectedAt(v, &here);
            bool corner = n >= 3;
            if (n == 2)
            {
                const glm::dvec3 d0 = glm::normalize(m.position(m.edgeOtherPoint(here[0], v)) - m.position(v));
                const glm::dvec3 d1 = glm::normalize(m.position(m.edgeOtherPoint(here[1], v)) - m.position(v));
                corner = glm::dot(d0, d1) > -0.94;
            }
            if (!corner)
                continue;
            m.forEachPointEdge(v, [&](Index start) {
                if (base[start] || !usable[start])
                    return;
                path.assign(1, start);
                Index prev = start, at = m.edgeOtherPoint(start, v);
                while (true)
                {
                    if (selectedAt(at, nullptr) > 0)
                    {
                        for (Index pe : path)
                            sel[pe] = 1;
                        return;
                    }
                    if (m.isBoundaryPoint(at) || m.pointValence(at) != 4)
                        return;
                    const Index next = oppositeEdge(m, prev, at);
                    if (next == kInvalid || !usable[next] || base[next] || std::find(path.begin(), path.end(), next) != path.end())
                        return;
                    path.push_back(next);
                    at = m.edgeOtherPoint(next, at);
                    prev = next;
                }
            });
        }
    }
}

uint64_t cellKey(int64_t x, int64_t y, int64_t z)
{
    return uint64_t(x) * 0x9E3779B97F4A7C15ull ^ uint64_t(y) * 0xC2B2AE3D27D4EB4Full ^ uint64_t(z) * 0x165667B19E3779F9ull;
}

/** Sorted (cell, element) pairs; a cell's elements are an equal_range. */
struct CellList
{
    std::vector<std::pair<uint64_t, Index>> items;

    void sort() { std::sort(items.begin(), items.end()); }
    template <class F> void each(uint64_t key, F&& fn) const
    {
        auto it = std::lower_bound(items.begin(), items.end(), std::pair<uint64_t, Index>{key, 0});
        for (; it != items.end() && it->first == key; ++it)
            fn(it->second);
    }
};
} // namespace

double edgeFoldAngle(const Mesh& m, Index e)
{
    Index f0, f1;
    if (!twoFaces(m, e, f0, f1))
        return -1.0;
    return foldAngle(m.faceNormal(f0), m.faceNormal(f1));
}

bool edgeIsConvex(const Mesh& m, Index e)
{
    Index f0, f1;
    if (!twoFaces(m, e, f0, f1))
        return false;
    const glm::dvec3 mid = (m.position(m.edgePoint(e, 0)) + m.position(m.edgePoint(e, 1))) * 0.5;
    return convexity(mid, m.faceCentroid(f0), m.faceNormal(f0), m.faceCentroid(f1), m.faceNormal(f1)) < 0.0;
}

double faceWidthAt(const Mesh& m, Index f, Index e) { return widthAt(m, f, e); }

std::vector<Index> foldEdges(const Mesh& m, const FoldEdgeOptions& o)
{
    LILA_ZONE();

    const Index nf = m.faceCapacity(), ne = m.edgeCapacity();
    std::vector<glm::dvec3> normal(nf), centroid(nf);
    tbb::parallel_for(Index(0), nf, [&](Index f) {
        if (m.faceValid(f))
        {
            normal[f] = m.faceNormal(f);
            centroid[f] = m.faceCentroid(f);
        }
    });

    // 1 = fold, 2 = usable for corner loops but not a fold.
    std::vector<uint8_t> state(ne, 0);
    tbb::parallel_for(Index(0), ne, [&](Index e) {
        Index f0, f1;
        if (!twoFaces(m, e, f0, f1))
            return;
        const glm::dvec3 a = m.position(m.edgePoint(e, 0)), b = m.position(m.edgePoint(e, 1));
        const double len = glm::length(b - a);
        if (len < o.minLength || len <= 0.0)
            return;
        if (o.thin > 0.0 && (len < o.thin || widthAt(m, f0, e) < o.thin || widthAt(m, f1, e) < o.thin))
            return;
        state[e] = 2;
        const double angle = foldAngle(normal[f0], normal[f1]);
        if (angle < o.minAngle || angle > o.maxAngle)
            return;
        if (o.side != FoldSide::Both)
        {
            const bool convex = convexity((a + b) * 0.5, centroid[f0], normal[f0], centroid[f1], normal[f1]) < 0.0;
            if (convex != (o.side == FoldSide::Outside))
                return;
        }
        state[e] = 1;
    });

    std::vector<uint8_t> sel(ne, 0), usable(ne, 0);
    for (Index e = 0; e < ne; ++e)
    {
        sel[e] = state[e] == 1;
        usable[e] = state[e] != 0;
    }
    if (o.completeCorners)
        completeCorners(m, sel, usable);
    return maskToList(sel);
}

std::vector<Index> angleEdges(const Mesh& m, double degrees)
{
    LILA_ZONE();

    std::vector<uint8_t> sel(m.edgeCapacity(), 0);
    const double cosLimit = std::cos(glm::radians(degrees));
    tbb::parallel_for(Index(0), m.edgeCapacity(), [&](Index e) {
        Index f0, f1;
        if (twoFaces(m, e, f0, f1) && glm::dot(m.faceNormal(f0), m.faceNormal(f1)) < cosLimit)
            sel[e] = 1;
    });
    return maskToList(sel);
}

std::vector<Index> weightedEdges(const Mesh& m, double threshold)
{
    std::vector<Index> out;
    auto w = m.edgeAttrs().get<float>(attr::kBevelWeight);
    for (Index e = 0; e < Index(w.size()) && e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && w[e] > threshold)
            out.push_back(e);
    return out;
}

EdgeRef edgeRef(const Mesh& m, Index e)
{
    EdgeRef r;
    r.edge = e;
    r.p0 = m.edgePoint(e, 0);
    r.p1 = m.edgePoint(e, 1);
    r.a = m.position(r.p0);
    r.b = m.position(r.p1);
    return r;
}

struct EdgeMatcher::Grids
{
    double pointCell = 1.0;
    CellList points;
    double edgeCell = 1.0;
    CellList edges;
    std::vector<Index> big; // edges spanning too many cells to list in each

    int64_t cellOf(double v, double cell) const { return int64_t(std::floor(v / cell)); }
};

EdgeMatcher::EdgeMatcher(const Mesh& mesh) : mMesh(mesh)
{
    const Bounds b = mesh.bounds();
    mTol = std::max(1e-9, glm::length(b.size()) * 1e-7);
}

EdgeMatcher::~EdgeMatcher() { delete mGrids; }

EdgeMatcher::Grids& EdgeMatcher::grids() const
{
    if (mGrids)
        return *mGrids;
    LILA_ZONE();

    auto* g = new Grids;
    const Mesh& m = mMesh;
    g->pointCell = mTol * 4.0;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
        {
            const glm::dvec3& q = m.position(p);
            g->points.items.push_back({cellKey(g->cellOf(q.x, g->pointCell), g->cellOf(q.y, g->pointCell), g->cellOf(q.z, g->pointCell)), p});
        }
    g->points.sort();

    double total = 0.0;
    size_t count = 0;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e))
        {
            total += m.edgeLength(e);
            ++count;
        }
    g->edgeCell = std::max(mTol * 16.0, count ? 2.0 * total / double(count) : 1.0);
    for (Index e = 0; e < m.edgeCapacity(); ++e)
    {
        if (!m.edgeValid(e))
            continue;
        const glm::dvec3 a = m.position(m.edgePoint(e, 0)), b = m.position(m.edgePoint(e, 1));
        const glm::dvec3 lo = glm::min(a, b) - mTol, hi = glm::max(a, b) + mTol;
        int64_t c0[3], c1[3];
        int64_t cells = 1;
        for (int k = 0; k < 3; ++k)
        {
            c0[k] = g->cellOf(lo[k], g->edgeCell);
            c1[k] = g->cellOf(hi[k], g->edgeCell);
            cells *= c1[k] - c0[k] + 1;
        }
        if (cells > 64)
        {
            g->big.push_back(e);
            continue;
        }
        for (int64_t x = c0[0]; x <= c1[0]; ++x)
            for (int64_t y = c0[1]; y <= c1[1]; ++y)
                for (int64_t z = c0[2]; z <= c1[2]; ++z)
                    g->edges.items.push_back({cellKey(x, y, z), e});
    }
    g->edges.sort();
    mGrids = g;
    return *g;
}

Index EdgeMatcher::byEndpoints(const glm::dvec3& a, const glm::dvec3& b) const
{
    const Grids& g = grids();
    const Mesh& m = mMesh;
    Index found = kInvalid;
    const int64_t cx = g.cellOf(a.x, g.pointCell), cy = g.cellOf(a.y, g.pointCell), cz = g.cellOf(a.z, g.pointCell);
    for (int64_t x = cx - 1; x <= cx + 1 && found == kInvalid; ++x)
        for (int64_t y = cy - 1; y <= cy + 1 && found == kInvalid; ++y)
            for (int64_t z = cz - 1; z <= cz + 1 && found == kInvalid; ++z)
                g.points.each(cellKey(x, y, z), [&](Index p) {
                    if (found != kInvalid || glm::length(m.position(p) - a) > mTol)
                        return;
                    m.forEachPointEdge(p, [&](Index e) {
                        if (found == kInvalid && glm::length(m.position(m.edgeOtherPoint(e, p)) - b) <= mTol)
                            found = e;
                    });
                });
    return found;
}

Index EdgeMatcher::byOverlap(const glm::dvec3& a, const glm::dvec3& b) const
{
    const Grids& g = grids();
    const Mesh& m = mMesh;
    const glm::dvec3 ab = b - a;
    const double len = glm::length(ab);
    if (len <= mTol)
        return kInvalid;
    const glm::dvec3 dir = ab / len, mid = (a + b) * 0.5;
    const double lineTol = std::max(mTol * 10.0, len * 1e-5);

    Index best = kInvalid;
    double bestDist = std::numeric_limits<double>::max();
    auto consider = [&](Index e) {
        const glm::dvec3 c = m.position(m.edgePoint(e, 0)), d = m.position(m.edgePoint(e, 1));
        auto offLine = [&](const glm::dvec3& p) { return glm::length(glm::cross(p - a, dir)); };
        if (offLine(c) > lineTol || offLine(d) > lineTol)
            return;
        const double tc = glm::dot(c - a, dir), td = glm::dot(d - a, dir);
        const double lo = std::max(0.0, std::min(tc, td)), hi = std::min(len, std::max(tc, td));
        if (hi - lo <= lineTol)
            return;
        const double dist = glm::length((c + d) * 0.5 - mid);
        if (dist < bestDist)
        {
            bestDist = dist;
            best = e;
        }
    };

    const glm::dvec3 lo = glm::min(a, b) - lineTol, hi = glm::max(a, b) + lineTol;
    int64_t c0[3], c1[3];
    int64_t cells = 1;
    for (int k = 0; k < 3; ++k)
    {
        c0[k] = g.cellOf(lo[k], g.edgeCell);
        c1[k] = g.cellOf(hi[k], g.edgeCell);
        cells *= c1[k] - c0[k] + 1;
    }
    if (cells > 4096)
    {
        for (Index e = 0; e < m.edgeCapacity(); ++e)
            if (m.edgeValid(e))
                consider(e);
        return best;
    }
    for (int64_t x = c0[0]; x <= c1[0]; ++x)
        for (int64_t y = c0[1]; y <= c1[1]; ++y)
            for (int64_t z = c0[2]; z <= c1[2]; ++z)
                g.edges.each(cellKey(x, y, z), consider);
    for (Index e : g.big)
        consider(e);
    return best;
}

Index EdgeMatcher::find(const EdgeRef& r, bool sameTopology, EdgeMatch* how) const
{
    const Mesh& m = mMesh;
    auto result = [&](Index e, EdgeMatch kind) {
        if (how)
            *how = e == kInvalid ? EdgeMatch::Lost : kind;
        return e;
    };
    if (r.edge != kInvalid && m.edgeValid(r.edge))
    {
        const Index q0 = m.edgePoint(r.edge, 0), q1 = m.edgePoint(r.edge, 1);
        if (sameTopology && ((q0 == r.p0 && q1 == r.p1) || (q0 == r.p1 && q1 == r.p0)))
            return result(r.edge, EdgeMatch::Id);
        const glm::dvec3 qa = m.position(q0), qb = m.position(q1);
        if ((glm::length(qa - r.a) <= mTol && glm::length(qb - r.b) <= mTol) || (glm::length(qa - r.b) <= mTol && glm::length(qb - r.a) <= mTol))
            return result(r.edge, EdgeMatch::Position);
    }
    if (Index e = byEndpoints(r.a, r.b); e != kInvalid)
        return result(e, EdgeMatch::Position);
    return result(byOverlap(r.a, r.b), EdgeMatch::Overlap);
}
} // namespace lila::geo
