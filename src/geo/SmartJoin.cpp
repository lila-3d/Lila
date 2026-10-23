#include "geo/SmartJoin.h"

#include "geo/Bvh.h"
#include "geo/Normals.h"
#include "geo/Ops.h"
#include "geo/Topology.h"

#include "core/Profile.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <numeric>
#include <queue>
#include <set>

namespace lila::geo
{
namespace
{
using V = glm::dvec3;
using V2 = glm::dvec2;

/** Plane frame with u x v = n. */
struct Frame
{
    V o{0.0}, u{1, 0, 0}, v{0, 1, 0}, n{0, 0, 1};

    V2 to2(const V& p) const
    {
        const V d = p - o;
        return {glm::dot(d, u), glm::dot(d, v)};
    }
};

Frame frameAround(const V& o, const V& normal)
{
    Frame f;
    f.o = o;
    f.n = glm::normalize(normal);
    const V a = std::abs(f.n.x) < 0.9 ? V(1, 0, 0) : V(0, 1, 0);
    f.u = glm::normalize(glm::cross(a, f.n));
    f.v = glm::cross(f.n, f.u);
    return f;
}

double signedArea(const std::vector<V2>& p)
{
    double a = 0.0;
    for (size_t i = 0; i < p.size(); ++i)
    {
        const V2& x = p[i];
        const V2& y = p[(i + 1) % p.size()];
        a += x.x * y.y - y.x * x.y;
    }
    return 0.5 * a;
}

int sense(const Frame& f, const Mesh& m, const std::vector<Index>& pts)
{
    std::vector<V2> p;
    for (Index i : pts)
        p.push_back(f.to2(m.position(i)));
    return signedArea(p) >= 0.0 ? 1 : -1;
}

int senseOf(const Frame& f, const std::vector<V>& pts)
{
    std::vector<V2> p;
    for (const V& x : pts)
        p.push_back(f.to2(x));
    return signedArea(p) >= 0.0 ? 1 : -1;
}

bool insidePolygon(const std::vector<V2>& poly, const V2& q)
{
    bool in = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
        if ((poly[i].y > q.y) != (poly[j].y > q.y) &&
            q.x < (poly[j].x - poly[i].x) * (q.y - poly[i].y) / (poly[j].y - poly[i].y) + poly[i].x)
            in = !in;
    return in;
}

double distanceToPolygon(const std::vector<V2>& poly, const V2& q)
{
    double best = std::numeric_limits<double>::max();
    for (size_t i = 0; i < poly.size(); ++i)
    {
        const V2 a = poly[i], b = poly[(i + 1) % poly.size()];
        const V2 ab = b - a;
        const double len2 = glm::dot(ab, ab);
        const double t = len2 > 0.0 ? std::clamp(glm::dot(q - a, ab) / len2, 0.0, 1.0) : 0.0;
        best = std::min(best, glm::length(q - (a + ab * t)));
    }
    return best;
}

/** Distance from the origin along unit dir to the polygon, 0 if the ray misses it. */
double borderDistance(const std::vector<V2>& poly, const V2& dir)
{
    double best = 0.0;
    for (size_t i = 0; i < poly.size(); ++i)
    {
        const V2 a = poly[i], b = poly[(i + 1) % poly.size()];
        const V2 e = b - a;
        const double den = dir.x * e.y - dir.y * e.x;
        if (std::abs(den) < 1e-15)
            continue;
        const double t = (a.x * e.y - a.y * e.x) / den;
        const double u = (a.x * dir.y - a.y * dir.x) / den;
        if (t > 0.0 && u >= 0.0 && u <= 1.0 && (best == 0.0 || t < best))
            best = t;
    }
    return best;
}

/** Ray parity, majority of three skewed directions, so a ray grazing an edge cannot decide alone. */
bool insideSolid(const MeshBvh& bvh, const V& p, double eps)
{
    static const V dirs[3] = {glm::normalize(V(0.577, 0.613, 0.539)), glm::normalize(V(-0.61, 0.37, -0.70)),
                              glm::normalize(V(0.29, -0.83, 0.47))};
    int votes = 0;
    for (const V& d : dirs)
    {
        int hits = 0;
        V o = p;
        for (int k = 0; k < 256; ++k)
        {
            BvhHit h;
            if (!bvh.raycast(o, d, h))
                break;
            ++hits;
            o = o + d * (h.t + eps);
        }
        votes += hits & 1;
    }
    return votes >= 2;
}

struct Candidate
{
    std::vector<Index> points;
    std::vector<Index> edges;
    std::vector<Index> removeFaces;
    bool faceBorder = false;
    V centroid{0.0}, axis{0.0};
    double radius = 0.0, gap = 0.0, planarity = 0.0;
};

struct LimbProbe
{
    const Mesh& limb;
    const Mesh& body;
    const MeshBvh& bvh;
    std::vector<uint8_t> inside; // per limb point
    double eps;

    bool hitBody(const V& o, const V& d, BvhHit& h) const
    {
        if (!bvh.raycast(o, d, h))
            return false;
        // From outside the ray must arrive at the body's front.
        return glm::dot(body.faceNormal(h.face), d) < 0.0;
    }
};

std::vector<Candidate> gatherCandidates(const Mesh& limb, const Mesh& body, const MeshBvh& bvh, double eps, bool faceBorders)
{
    LILA_ZONE();

    std::vector<uint8_t> inside(limb.pointCapacity(), 0);
    for (Index p = 0; p < limb.pointCapacity(); ++p)
        if (limb.pointValid(p))
            inside[p] = insideSolid(bvh, limb.position(p), eps);
    LimbProbe probe{limb, body, bvh, inside, eps};

    std::vector<Candidate> out;
    std::set<std::vector<Index>> seen;
    auto add = [&](const EdgePath& path, bool faceBorder) {
        if (!path.closed || path.points.size() < 3)
            return;
        std::vector<Index> key = path.edges;
        std::sort(key.begin(), key.end());
        if (!seen.insert(key).second)
            return;
        Candidate c;
        c.points = path.points;
        c.edges = path.edges;
        c.faceBorder = faceBorder;
        V nl(0.0);
        for (size_t i = 0; i < c.points.size(); ++i)
        {
            const V& a = limb.position(c.points[i]);
            const V& b = limb.position(c.points[(i + 1) % c.points.size()]);
            c.centroid += a;
            nl += glm::cross(a, b);
            if (inside[c.points[i]])
                return;
        }
        c.centroid /= double(c.points.size());
        if (glm::length(nl) < 1e-12)
            return;
        nl = glm::normalize(nl);
        for (Index p : c.points)
            c.radius += glm::length(limb.position(p) - c.centroid);
        c.radius /= double(c.points.size());
        for (Index p : c.points)
            c.planarity = std::max(c.planarity, std::abs(glm::dot(limb.position(p) - c.centroid, nl)));
        c.planarity /= std::max(c.radius, 1e-12);

        const bool boundary = std::all_of(c.edges.begin(), c.edges.end(), [&](Index e) { return limb.isBoundaryEdge(e); });
        auto meanOf = [&](std::span<const Index> faces) {
            V m(0.0);
            int n = 0;
            for (Index p : pointsOfFaces(limb, faces))
            {
                m += limb.position(p);
                ++n;
            }
            return n ? m / double(n) : c.centroid;
        };
        if (boundary)
        {
            const V k = meanOf(facesOfEdges(limb, c.edges));
            c.axis = glm::dot(nl, c.centroid - k) >= 0.0 ? nl : -nl;
            BvhHit h;
            if (!probe.hitBody(c.centroid, c.axis, h))
                return;
            c.gap = h.t;
        }
        else
        {
            std::vector<uint8_t> block = makeMask(limb.edgeCapacity(), c.edges);
            std::vector<Index> comp;
            faceComponents(limb, comp, block);
            std::set<Index> sides;
            for (Index f : facesOfEdges(limb, c.edges))
                sides.insert(comp[f]);
            if (sides.size() != 2)
                return;
            double best = std::numeric_limits<double>::max();
            Index bestSide = kInvalid;
            for (Index s : sides)
            {
                std::vector<Index> adj;
                for (Index f : facesOfEdges(limb, c.edges))
                    if (comp[f] == s)
                        adj.push_back(f);
                const V d = glm::dot(meanOf(adj) - c.centroid, nl) >= 0.0 ? nl : -nl;
                BvhHit h;
                if (probe.hitBody(c.centroid, d, h) && h.t < best)
                {
                    best = h.t;
                    bestSide = s;
                    c.axis = d;
                }
            }
            if (bestSide == kInvalid)
                return;
            c.gap = best;
            for (Index f = 0; f < limb.faceCapacity(); ++f)
                if (limb.faceValid(f) && comp[f] == bestSide)
                    c.removeFaces.push_back(f);
        }
        out.push_back(std::move(c));
    };

    std::vector<uint8_t> loopSeen(limb.edgeCapacity(), 0);
    for (const EdgePath& p : boundaryLoops(limb))
        add(p, false);
    for (Index e = 0; e < limb.edgeCapacity(); ++e)
    {
        if (!limb.edgeValid(e) || loopSeen[e])
            continue;
        std::vector<Index> loop = edgeLoop(limb, e);
        for (Index x : loop)
            loopSeen[x] = 1;
        std::vector<EdgePath> paths = chainEdges(limb, loop);
        if (paths.size() == 1)
            add(paths[0], false);
    }
    for (Index f = 0; f < limb.faceCapacity(); ++f)
    {
        if (!limb.faceValid(f) || (!faceBorders && limb.faceSize(f) < 5))
            continue;
        EdgePath path;
        path.closed = true;
        limb.forEachFaceCorner(f, [&](Index c) {
            path.points.push_back(limb.cornerPoint(c));
            path.edges.push_back(limb.cornerEdge(c));
        });
        add(path, limb.faceSize(f) < 5);
    }
    std::stable_sort(out.begin(), out.end(), [](const Candidate& a, const Candidate& b) { return a.gap < b.gap; });
    return out;
}

/**
 * Inserts an edge loop across the ring of limb edges that pierce the body, just outside the surface. Returns the new
 * loop's point count, 0 when the limb does not cross the body along one ring.
 */
int trimAtBody(Mesh& limb, const MeshBvh& bvh, double eps, double marginFrac)
{
    LILA_ZONE();

    std::vector<uint8_t> inside(limb.pointCapacity(), 0);
    for (Index p = 0; p < limb.pointCapacity(); ++p)
        if (limb.pointValid(p))
            inside[p] = insideSolid(bvh, limb.position(p), eps);
    std::vector<Index> crossing;
    for (Index e = 0; e < limb.edgeCapacity(); ++e)
        if (limb.edgeValid(e) && inside[limb.edgePoint(e, 0)] != inside[limb.edgePoint(e, 1)])
            crossing.push_back(e);
    if (crossing.empty())
        return 0;

    V mid(0.0);
    for (Index e : crossing)
        mid += 0.5 * (limb.position(limb.edgePoint(e, 0)) + limb.position(limb.edgePoint(e, 1)));
    mid /= double(crossing.size());
    double radius = 0.0;
    for (Index e : crossing)
        radius += glm::length(0.5 * (limb.position(limb.edgePoint(e, 0)) + limb.position(limb.edgePoint(e, 1))) - mid);
    radius /= double(crossing.size());

    // The crossing edge reaching furthest out decides, so every ring edge's cut lands outside.
    for (Index e0 : crossing)
    {
        const bool firstOutside = !inside[limb.edgePoint(e0, 0)];
        const V out = limb.position(limb.edgePoint(e0, firstOutside ? 0 : 1));
        const V in = limb.position(limb.edgePoint(e0, firstOutside ? 1 : 0));
        const double len = glm::length(in - out);
        if (len <= 0.0)
            continue;
        BvhHit h;
        double d = bvh.raycast(out, in - out, h, 1.0) ? h.t * len : 0.5 * len;
        double s = std::max(d - marginFrac * radius, 0.05 * d) / len;
        for (int attempt = 0; attempt < 6; ++attempt, s *= 0.6)
        {
            Mesh copy = limb;
            std::vector<Index> edges = insertEdgeLoop(copy, e0, {.fraction = firstOutside ? s : 1.0 - s});
            if (edges.empty())
                break;
            std::vector<Index> pts = pointsOfEdges(copy, edges);
            const bool clear = std::none_of(pts.begin(), pts.end(), [&](Index p) { return insideSolid(bvh, copy.position(p), eps); });
            if (clear)
            {
                limb = std::move(copy);
                return int(pts.size());
            }
        }
    }
    return 0;
}

struct Region
{
    std::vector<Index> faces;
    std::vector<Index> border; // ordered points of its outer border
    double maxR = 0.0;
    bool enclosed = false;
    int poles = 0;
    int nonQuads = 0;
};

bool evalRegion(const Mesh& m, std::vector<Index> faces, const Frame& fr, const std::vector<V2>& hole, double holeR, Region& out)
{
    std::vector<Index> border = regionBoundaryEdges(m, faces);
    for (Index e : border)
        if (m.edgeFaceCount(e) != 2)
            return false;
    std::vector<EdgePath> paths = chainEdges(m, border);
    if (paths.size() != 1 || !paths[0].closed)
        return false;
    const std::vector<Index> pts = pointsOfFaces(m, faces);
    const std::vector<Index> edges = edgesOfFaces(m, faces);
    if (int(pts.size()) - int(edges.size()) + int(faces.size()) != 1)
        return false;
    out.faces = std::move(faces);
    out.border = paths[0].points;
    std::vector<V2> poly;
    out.maxR = 0.0;
    for (Index p : out.border)
    {
        poly.push_back(fr.to2(m.position(p)));
        out.maxR = std::max(out.maxR, glm::length(poly.back()));
    }
    out.enclosed = true;
    for (const V2& q : hole)
        if (!insidePolygon(poly, q) || distanceToPolygon(poly, q) < 0.03 * holeR)
            out.enclosed = false;
    out.poles = 0;
    for (Index p : pts)
        if (m.pointValence(p) != 4)
            ++out.poles;
    out.nonQuads = 0;
    for (Index f : out.faces)
        if (m.faceSize(f) != 4)
            ++out.nonQuads;
    return true;
}

/** Faces grown from f0 nearest first; returns the first disc whose border count is n and that holds the hole. */
bool growRegion(const Mesh& m, Index f0, const Frame& fr, const std::vector<V2>& hole, double holeR, int n, Region& exact,
                Region& fallback, bool& haveFallback)
{
    LILA_ZONE();

    auto key = [&](Index f) { return glm::length(fr.to2(m.faceCentroid(f))); };
    using Item = std::pair<double, Index>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> frontier;
    std::vector<uint8_t> queued(m.faceCapacity(), 0);
    std::vector<Index> faces;
    frontier.push({key(f0), f0});
    queued[f0] = 1;
    int fallbackDiff = std::numeric_limits<int>::max();
    while (!frontier.empty() && faces.size() < 80)
    {
        auto [k, f] = frontier.top();
        frontier.pop();
        if (k > 3.0 * holeR && !faces.empty())
            break;
        faces.push_back(f);
        m.forEachFaceCorner(f, [&](Index c) {
            m.forEachEdgeCorner(m.cornerEdge(c), [&](Index r) {
                const Index g = m.cornerFace(r);
                if (!queued[g])
                {
                    queued[g] = 1;
                    frontier.push({key(g), g});
                }
            });
        });
        Region r;
        if (!evalRegion(m, faces, fr, hole, holeR, r) || !r.enclosed)
            continue;
        const int count = int(r.border.size());
        if (count == n && r.maxR <= 2.2 * holeR)
        {
            exact = std::move(r);
            return true;
        }
        const int diff = std::abs(count - n);
        if (r.maxR <= 3.0 * holeR && diff < fallbackDiff)
        {
            fallbackDiff = diff;
            fallback = std::move(r);
            haveFallback = true;
        }
    }
    return false;
}

/** Closest face to p on m. */
Index faceAt(const MeshBvh& bvh, const V& p)
{
    BvhClosest c;
    return bvh.closestPoint(p, c) ? c.face : kInvalid;
}

/** Edge positions along dir of body edges running along other near the centre, sorted. */
std::vector<double> existingLines(const Mesh& m, const V& center, const V& dir, const V& other, double reach, double otherReach)
{
    std::vector<double> out;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
    {
        if (!m.edgeValid(e))
            continue;
        const V p0 = m.position(m.edgePoint(e, 0)), p1 = m.position(m.edgePoint(e, 1));
        const V d = p1 - p0;
        if (glm::length(d) <= 0.0 || std::abs(glm::dot(glm::normalize(d), other)) < 0.8)
            continue;
        const V mid = 0.5 * (p0 + p1) - center;
        if (std::abs(glm::dot(mid, dir)) < reach && std::abs(glm::dot(mid, other)) < otherReach + 0.5 * glm::length(d))
            out.push_back(glm::dot(mid, dir));
    }
    std::sort(out.begin(), out.end());
    return out;
}

/**
 * Line positions for one grid axis: extents just outside the hole (an existing edge there is reused), existing edges
 * between them, and new lines splitting the widest gaps until there are segs + 1.
 */
std::vector<double> planLines(const std::vector<double>& existing, double hole, int segs)
{
    double lo = -1.3 * hole, hi = 1.3 * hole;
    for (double c : existing)
        if (c >= 0.75 * hole && c <= 1.7 * hole)
        {
            hi = c;
            break;
        }
    for (auto it = existing.rbegin(); it != existing.rend(); ++it)
        if (*it <= -0.75 * hole && *it >= -1.7 * hole)
        {
            lo = *it;
            break;
        }
    const double tol = 0.08 * (hi - lo);
    std::vector<double> lines{lo};
    for (double c : existing)
        if (c > lo + tol && c < hi - tol && (lines.size() == 1 || c - lines.back() > tol))
            lines.push_back(c);
    lines.push_back(hi);
    while (int(lines.size()) < segs + 1)
    {
        size_t widest = 0;
        for (size_t i = 1; i + 1 < lines.size(); ++i)
            if (lines[i + 1] - lines[i] > lines[widest + 1] - lines[widest])
                widest = i;
        lines.insert(lines.begin() + std::ptrdiff_t(widest) + 1, 0.5 * (lines[widest] + lines[widest + 1]));
    }
    return lines;
}

/** Inserts an edge loop through the body at center + dir * x, cutting the edges that run along dir. False if not needed. */
bool insertLine(Mesh& m, const V& center, const V& dir, const V& normal, double x)
{
    MeshBvh bvh(m);
    BvhClosest c;
    // Straight down onto the surface, so a curved body is cut where the hole's frame says, not at the nearest facet.
    const V p = center + dir * x;
    const double lift = 4.0 * std::abs(x) + 1.0;
    BvhHit h;
    if (bvh.raycast(p + normal * lift, -normal, h, 2.0 * lift) || bvh.raycast(p - normal * lift, normal, h, 2.0 * lift))
    {
        c.point = h.point;
        c.face = h.face;
    }
    else if (!bvh.closestPoint(p, c))
        return false;
    if (m.faceSize(c.face) != 4)
        return false;
    Index best = kInvalid;
    double bestDot = 0.0;
    m.forEachFaceCorner(c.face, [&](Index k) {
        const Index e = m.cornerEdge(k);
        const V d = m.position(m.edgePoint(e, 1)) - m.position(m.edgePoint(e, 0));
        const double dd = std::abs(glm::dot(glm::normalize(d), dir));
        if (dd > bestDot)
        {
            bestDot = dd;
            best = e;
        }
    });
    if (best == kInvalid)
        return false;
    const V p0 = m.position(m.edgePoint(best, 0));
    const V e = m.position(m.edgePoint(best, 1)) - p0;
    const double t = glm::dot(c.point - p0, e) / glm::dot(e, e);
    // An existing edge this close already serves as the line.
    if (t < 0.08 || t > 0.92)
        return false;
    return !insertEdgeLoop(m, best, {.fraction = t}).empty();
}

/**
 * Support loops through the body so the faces around the hole form an a x b quad block (2(a+b) border points).
 * Returns the loops inserted; the block spans linesA and linesB.
 */
int insertSupportGrid(Mesh& m, const V& center, const V& da, const V& db, double holeA, double holeB, int a, int b,
                      std::vector<double>& linesA, std::vector<double>& linesB)
{
    LILA_ZONE();

    const std::vector<double> haveA = existingLines(m, center, da, db, 2.0 * holeA, 1.7 * holeB);
    const std::vector<double> haveB = existingLines(m, center, db, da, 2.0 * holeB, 1.7 * holeA);
    linesA = planLines(haveA, holeA, a);
    linesB = planLines(haveB, holeB, b);
    auto existing = [](const std::vector<double>& have, double x) { return std::find(have.begin(), have.end(), x) != have.end(); };
    const V normal = glm::normalize(glm::cross(da, db));
    int inserted = 0;
    for (double x : linesA)
        if (!existing(haveA, x))
            inserted += insertLine(m, center, da, normal, x);
    for (double y : linesB)
        if (!existing(haveB, y))
            inserted += insertLine(m, center, db, normal, y);
    return inserted;
}

/** Quads between a hole border (fill order) and an inner loop running the same way, triangles where counts differ. */
std::vector<Index> stitch(Mesh& m, const std::vector<Index>& outer, const std::vector<Index>& inner)
{
    std::vector<Index> faces;
    const size_t M = outer.size(), N = inner.size();
    auto add = [&](std::initializer_list<Index> pts) {
        if (Index f = m.addFace(pts); f != kInvalid)
            faces.push_back(f);
    };
    // The longer side advances every step; the shorter one evenly often, so the extra steps become spread triangles.
    if (M >= N)
        for (size_t k = 0; k < M; ++k)
        {
            const size_t j = k * N / M, next = (k + 1) * N / M;
            if (next > j)
                add({outer[k], outer[(k + 1) % M], inner[next % N], inner[j % N]});
            else
                add({outer[k], outer[(k + 1) % M], inner[j % N]});
        }
    else
        for (size_t k = 0; k < N; ++k)
        {
            const size_t i = k * M / N, next = (k + 1) * M / N;
            if (next > i)
                add({outer[i % M], outer[next % M], inner[(k + 1) % N], inner[k]});
            else
                add({outer[i % M], inner[(k + 1) % N], inner[k]});
        }
    return faces;
}

V hermite(const V& p0, const V& m0, const V& p1, const V& m1, double t)
{
    const double t2 = t * t, t3 = t2 * t;
    return (2 * t3 - 3 * t2 + 1) * p0 + (t3 - 2 * t2 + t) * m0 + (-2 * t3 + 3 * t2) * p1 + (t3 - t2) * m1;
}

SmartJoinResult fail(std::string why)
{
    SmartJoinResult r;
    r.reasons.push_back(std::move(why));
    r.summary = r.reasons.front();
    return r;
}
} // namespace

SmartJoinResult smartJoin(const Mesh& limbIn, const Mesh& bodyIn, const SmartJoinOptions& opts)
{
    LILA_ZONE();

    Mesh limb = limbIn;
    Mesh body = bodyIn;
    limb.compact();
    body.compact();
    if (limb.numFaces() == 0 || body.numFaces() == 0)
        return fail("both meshes need faces");

    SmartJoinResult res;
    res.originalPoints = int(limb.numPoints() + body.numPoints());
    const Index limbCap = limb.pointCapacity();
    struct Penalty
    {
        double points;
        std::string why;
    };
    std::vector<Penalty> penalties;
    std::vector<std::string> notes;

    const MeshBvh bodyBvh(body);
    const double scale = glm::length(body.bounds().size()) + glm::length(limb.bounds().size());
    const double eps = 1e-7 * std::max(scale, 1.0);

    // Limb: choose the loop, trimming the limb at the body first when it pierces it and no loop sits close outside.
    std::vector<Candidate> cands = gatherCandidates(limb, body, bodyBvh, eps, false);
    {
        const bool pierces = [&] {
            for (Index p = 0; p < limb.pointCapacity(); ++p)
                if (limb.pointValid(p) && insideSolid(bodyBvh, limb.position(p), eps))
                    return true;
            return false;
        }();
        if (pierces && opts.limbLoop < 0 && (cands.empty() || cands.front().gap > 0.6 * cands.front().radius))
        {
            if (trimAtBody(limb, bodyBvh, eps, opts.trimMargin) > 0)
            {
                res.trimmed = true;
                cands = gatherCandidates(limb, body, bodyBvh, eps, false);
            }
        }
    }
    bool faceBorderLoop = false;
    if (cands.empty())
    {
        cands = gatherCandidates(limb, body, bodyBvh, eps, true);
        faceBorderLoop = !cands.empty();
    }
    if (cands.empty())
        return fail("no loop on the limb faces the body (is the limb pointing at it?)");
    res.loopCandidates = int(cands.size());
    const int pick = opts.limbLoop < 0 ? 0 : std::min(opts.limbLoop, int(cands.size()) - 1);
    res.loopIndex = pick;
    const Candidate cand = cands[size_t(pick)];
    if (opts.limbLoop >= int(cands.size()))
        notes.push_back(std::format("only {} limb loops face the body; used the last", cands.size()));

    // Remove (or detach) the limb beyond the loop, then read the loop back as a border in fill order.
    if (!cand.removeFaces.empty())
    {
        if (opts.deleteBuried)
            deleteFaces(limb, cand.removeFaces);
        else
        {
            std::vector<Index> dup(limb.pointCapacity(), kInvalid);
            for (Index p : cand.points)
                dup[p] = limb.addPoint(limb.position(p));
            for (Index f : cand.removeFaces)
            {
                std::vector<Index> pts = limb.facePoints(f);
                for (Index& p : pts)
                    if (p < dup.size() && dup[p] != kInvalid)
                        p = dup[p];
                limb.setFacePoints(f, pts);
            }
            // Edges that joined the loop to the detached side are left without faces.
            limb.removeAllLoose();
        }
    }
    std::vector<Index> A;
    {
        const std::set<Index> want(cand.points.begin(), cand.points.end());
        for (const EdgePath& p : boundaryLoops(limb))
            if (p.closed && p.points.size() == want.size() &&
                std::all_of(p.points.begin(), p.points.end(), [&](Index x) { return want.count(x) != 0; }))
                A = p.points;
    }
    if (A.empty())
        return fail("the limb loop does not become an open border (non-manifold limb?)");
    int limbPreserved = 0;
    for (Index p = 0; p < limbCap; ++p)
        if (limb.pointValid(p))
            ++limbPreserved;
    {
        MeshRemap remap;
        limb.compact(&remap);
        for (Index& p : A)
            p = remap.points[p];
    }
    const int N = int(A.size());
    res.limbLoopSize = N;
    const V axis = cand.axis;

    // Rails: the limb edge leaving each loop point, carried on toward the body.
    std::vector<V> rails(size_t(N), axis);
    {
        const std::set<Index> loopPts(A.begin(), A.end());
        for (int k = 0; k < N; ++k)
        {
            double best = -2.0;
            limb.forEachPointEdge(A[size_t(k)], [&](Index e) {
                const Index q = limb.edgeOtherPoint(e, A[size_t(k)]);
                if (loopPts.count(q))
                    return;
                const V d = limb.position(A[size_t(k)]) - limb.position(q);
                if (glm::length(d) <= 0.0)
                    return;
                const V dn = glm::normalize(d);
                if (glm::dot(dn, axis) > best)
                {
                    best = glm::dot(dn, axis);
                    rails[size_t(k)] = dn;
                }
            });
            if (best < 0.0)
                rails[size_t(k)] = axis;
        }
    }

    // Project the loop onto the body along the axis, then scale and slide it about its centre.
    BvhHit centreHit;
    V loopCentre(0.0);
    for (Index p : A)
        loopCentre += limb.position(p);
    loopCentre /= double(N);
    if (!bodyBvh.raycast(loopCentre, axis, centreHit))
    {
        BvhClosest c;
        bodyBvh.closestPoint(loopCentre, c);
        centreHit.point = c.point;
        centreHit.face = c.face;
    }
    const V bodyNormal = body.faceNormal(centreHit.face);
    const double cosAngle = std::clamp(glm::dot(axis, -bodyNormal), -1.0, 1.0);
    Frame tangent = frameAround(centreHit.point, bodyNormal);

    std::vector<V> Q(static_cast<size_t>(N));
    int misses = 0;
    for (int k = 0; k < N; ++k)
    {
        const V p = limb.position(A[size_t(k)]);
        BvhHit h;
        if (bodyBvh.raycast(p, axis, h) && h.t < cand.gap + 4.0 * cand.radius)
            Q[size_t(k)] = h.point;
        else
        {
            BvhClosest c;
            bodyBvh.closestPoint(p, c);
            Q[size_t(k)] = c.point;
            ++misses;
        }
    }
    V holeCentre = centreHit.point + tangent.u * opts.offsetU + tangent.v * opts.offsetV;
    {
        BvhClosest c;
        if (bodyBvh.closestPoint(holeCentre, c))
            holeCentre = c.point;
    }
    std::vector<V> bodyNormals(size_t(N), bodyNormal);
    for (int k = 0; k < N; ++k)
    {
        V q = holeCentre + (Q[size_t(k)] - centreHit.point) * std::max(opts.holeScale, 0.01);
        BvhClosest c;
        if (bodyBvh.closestPoint(q, c))
        {
            q = c.point;
            bodyNormals[size_t(k)] = body.faceNormal(c.face);
        }
        Q[size_t(k)] = q;
    }
    {
        // Averaged over the hole so a hit on an edge or a ridge between facets does not tilt the frame.
        V avg(0.0);
        const double reach = 1.5 * cand.radius * std::max(opts.holeScale, 0.01);
        for (Index f = 0; f < body.faceCapacity(); ++f)
            if (body.faceValid(f) && glm::length(body.faceCentroid(f) - holeCentre) < reach)
                avg += body.faceAreaVector(f);
        const V n = glm::length(avg) > 0.0 ? glm::normalize(avg) : body.faceNormal(faceAt(bodyBvh, holeCentre));
        tangent = frameAround(holeCentre, n);
    }
    std::vector<V2> hole2;
    double holeR = 0.0;
    for (const V& q : Q)
    {
        hole2.push_back(tangent.to2(q));
        holeR = std::max(holeR, glm::length(hole2.back()));
    }
    if (holeR <= eps)
        return fail("the loop projects to a point on the body");

    // Body: find or make a disc of faces with as many border points as the loop.
    const Index bodyCap = body.pointCapacity();
    Region region;
    bool exact = false;
    {
        MeshBvh bvh(body);
        Region fallback;
        bool haveFallback = false;
        const Index f0 = faceAt(bvh, holeCentre);
        exact = growRegion(body, f0, tangent, hole2, holeR, N, region, fallback, haveFallback);
        if (!exact && body.faceSize(f0) == 4)
        {
            // Grid axes follow the hit face's own edge flow.
            const Index c0 = body.faceCorner(f0);
            auto edgeDir = [&](Index c) {
                V d = body.position(body.cornerNextPoint(c)) - body.position(body.cornerPoint(c));
                d -= tangent.n * glm::dot(d, tangent.n);
                return glm::normalize(d);
            };
            const V da = edgeDir(c0), db = edgeDir(body.cornerNext(c0));
            double ea = 0.0, eb = 0.0;
            for (const V& q : Q)
            {
                ea = std::max(ea, std::abs(glm::dot(q - holeCentre, da)));
                eb = std::max(eb, std::abs(glm::dot(q - holeCentre, db)));
            }
            const int half = (N + (N & 1)) / 2;
            const int a = std::clamp(int(std::lround(half * ea / std::max(ea + eb, 1e-12))), 1, std::max(1, half - 1));
            const int b = std::max(1, half - a);
            Mesh trial = body;
            std::vector<double> linesA, linesB;
            const int loops = insertSupportGrid(trial, holeCentre, da, db, ea, eb, a, b, linesA, linesB);
            MeshBvh tb(trial);
            const Index g0 = faceAt(tb, holeCentre);
            std::vector<Index> block;
            std::vector<uint8_t> seen(trial.faceCapacity(), 0);
            auto inBlock = [&](Index f) {
                const V d = trial.faceCentroid(f) - holeCentre;
                const double x = glm::dot(d, da), y = glm::dot(d, db);
                return x > linesA.front() && x < linesA.back() && y > linesB.front() && y < linesB.back();
            };
            std::vector<Index> stack{g0};
            seen[g0] = 1;
            while (!stack.empty())
            {
                const Index f = stack.back();
                stack.pop_back();
                block.push_back(f);
                trial.forEachFaceCorner(f, [&](Index c) {
                    trial.forEachEdgeCorner(trial.cornerEdge(c), [&](Index r) {
                        const Index g = trial.cornerFace(r);
                        if (!seen[g] && inBlock(g))
                        {
                            seen[g] = 1;
                            stack.push_back(g);
                        }
                    });
                });
            }
            Region r;
            if (evalRegion(trial, block, tangent, hole2, holeR, r) &&
                std::abs(int(r.border.size()) - N) < (haveFallback ? std::abs(int(fallback.border.size()) - N) : 1 << 20))
            {
                body = std::move(trial);
                region = std::move(r);
                res.supportLoops = loops;
                exact = int(region.border.size()) == N;
            }
            else if (haveFallback)
                region = std::move(fallback);
        }
        else if (!exact && haveFallback)
            region = std::move(fallback);
        if (region.faces.empty())
        {
            Region r;
            if (!evalRegion(body, {f0}, tangent, hole2, holeR, r))
                return fail("the body around the hole is not a simple surface");
            region = std::move(r);
        }
    }
    const int M = int(region.border.size());

    // A hole wider than the faces around it is pulled in to sit inside their border.
    bool shrunk = false;
    if (!region.enclosed)
    {
        std::vector<V2> poly;
        for (Index p : region.border)
            poly.push_back(tangent.to2(body.position(p)));
        for (int k = 0; k < N; ++k)
        {
            const V2 q = hole2[size_t(k)];
            const double len = glm::length(q);
            if (len <= 0.0)
                continue;
            const double reach = borderDistance(poly, q / len);
            if (reach > 0.0 && len > 0.85 * reach)
            {
                BvhClosest c;
                const V p = holeCentre + (Q[size_t(k)] - holeCentre) * (0.85 * reach / len);
                Q[size_t(k)] = bodyBvh.closestPoint(p, c) ? c.point : p;
                hole2[size_t(k)] = tangent.to2(Q[size_t(k)]);
                shrunk = true;
            }
        }
        region.enclosed = std::all_of(hole2.begin(), hole2.end(), [&](const V2& q) { return insidePolygon(poly, q); });
    }
    res.holeSize = M;

    // Carve the region and read its border back in fill order.
    const Index borderStart = region.border.front();
    deleteFaces(body, region.faces);
    std::vector<Index> O;
    for (const EdgePath& p : boundaryLoops(body))
        if (p.closed && std::find(p.points.begin(), p.points.end(), borderStart) != p.points.end())
            O = p.points;
    if (int(O.size()) != M)
        return fail("carving the hole left an unexpected border");

    // Orientation: the limb border and the hole border must circle the axis in opposite senses.
    const Frame axial = frameAround(holeCentre, axis);
    const int sigmaO = sense(axial, body, O);
    if (sense(axial, limb, A) == sigmaO)
    {
        for (Index f = 0; f < limb.faceCapacity(); ++f)
            if (limb.faceValid(f))
                limb.flipFace(f);
        std::reverse(A.begin(), A.end());
        std::reverse(rails.begin(), rails.end());
        std::reverse(Q.begin(), Q.end());
        std::reverse(bodyNormals.begin(), bodyNormals.end());
        res.flippedLimb = true;
    }
    const int sigmaA = -sigmaO;
    const int sigmaI = senseOf(axial, Q);

    std::vector<Index> I(static_cast<size_t>(N));
    for (int k = 0; k < N; ++k)
        I[size_t(k)] = body.addPoint(Q[size_t(k)]);
    std::vector<Index> J = I;
    if (sigmaI != sigmaO)
        std::reverse(J.begin(), J.end());
    {
        // Start the inner loop at the point matching the border's first point.
        size_t best = 0;
        double bestCost = std::numeric_limits<double>::max();
        for (size_t s = 0; s < J.size(); ++s)
        {
            double cost = 0.0;
            if (M == N)
                for (size_t i = 0; i < size_t(M); ++i)
                    cost += glm::length(body.position(O[i]) - body.position(J[(i + s) % J.size()]));
            else
                cost = glm::length(body.position(O[0]) - body.position(J[s]));
            if (cost < bestCost)
            {
                bestCost = cost;
                best = s;
            }
        }
        std::rotate(J.begin(), J.begin() + std::ptrdiff_t(best), J.end());
    }
    std::vector<Index> ringFaces = stitch(body, O, J);

    // Inner ring relax happens on the surface, before the body is merged with the limb.
    for (int it = 0; it < opts.relax; ++it)
    {
        std::vector<V> next(J.size());
        for (size_t j = 0; j < J.size(); ++j)
        {
            const V avg = 0.5 * (body.position(J[(j + J.size() - 1) % J.size()]) + body.position(J[(j + 1) % J.size()]));
            V p = glm::mix(body.position(J[j]), avg, 0.5);
            BvhClosest c;
            if (bodyBvh.closestPoint(p, c))
                p = c.point;
            next[j] = p;
        }
        for (size_t j = 0; j < J.size(); ++j)
            body.setPosition(J[j], next[j]);
    }

    int preserved = 0;
    for (Index p = 0; p < bodyCap && p < body.pointCapacity(); ++p)
        if (body.pointValid(p))
            ++preserved;
    // Support loops add points with ids past the original capacity, and carving removes some originals.
    {
        MeshRemap remap;
        body.compact(&remap);
        for (Index& p : I)
            p = remap.points[p];
        for (Index& f : ringFaces)
            f = remap.faces[f];
    }
    preserved += limbPreserved;
    res.preservedPoints = preserved;

    // Merge and bridge.
    Mesh& W = res.mesh;
    W = std::move(body);
    const Index offset = W.append(limb);
    for (Index& p : A)
        p += offset;

    double meanGap = 0.0, meanEdge = 0.0;
    std::vector<int> partner(static_cast<size_t>(N));
    for (int i = 0; i < N; ++i)
    {
        const int m = sigmaI == sigmaA ? i + opts.twist : opts.twist - i;
        partner[size_t(i)] = ((m % N) + N) % N;
        meanGap += glm::length(W.position(I[size_t(partner[size_t(i)])]) - W.position(A[size_t(i)]));
        meanEdge += glm::length(W.position(A[size_t((i + 1) % N)]) - W.position(A[size_t(i)]));
    }
    meanGap /= N;
    meanEdge /= N;
    const int S = opts.segments > 0 ? std::min(opts.segments, 64)
                                    : std::clamp(int(std::lround(meanGap / std::max(meanEdge, 1e-12))), 1, 8);
    res.segments = S;

    std::vector<std::vector<Index>> rows(size_t(S + 1), std::vector<Index>(size_t(N)));
    rows[0] = A;
    for (int i = 0; i < N; ++i)
    {
        const int k = partner[size_t(i)];
        rows[size_t(S)][size_t(i)] = I[size_t(k)];
        const V p0 = W.position(A[size_t(i)]);
        const V p1 = W.position(I[size_t(k)]);
        const double L = glm::length(p1 - p0) * opts.tension;
        const V m0 = rails[size_t(i)] * L;
        const V m1 = -bodyNormals[size_t(k)] * L;
        for (int r = 1; r < S; ++r)
            rows[size_t(r)][size_t(i)] = W.addPoint(hermite(p0, m0, p1, m1, double(r) / S));
    }
    for (int it = 0; it < opts.relax && S > 1; ++it)
    {
        std::vector<std::vector<V>> next(size_t(S + 1));
        for (int r = 1; r < S; ++r)
            for (int i = 0; i < N; ++i)
            {
                const V avg = 0.25 * (W.position(rows[size_t(r)][size_t((i + 1) % N)]) + W.position(rows[size_t(r)][size_t((i + N - 1) % N)]) +
                                      W.position(rows[size_t(r - 1)][size_t(i)]) + W.position(rows[size_t(r + 1)][size_t(i)]));
                next[size_t(r)].push_back(glm::mix(W.position(rows[size_t(r)][size_t(i)]), avg, 0.5));
            }
        for (int r = 1; r < S; ++r)
            for (int i = 0; i < N; ++i)
                W.setPosition(rows[size_t(r)][size_t(i)], next[size_t(r)][size_t(i)]);
    }
    std::vector<Index> newFaces = ringFaces;
    for (int r = 0; r < S; ++r)
        for (int i = 0; i < N; ++i)
        {
            const Index f = W.addFace({rows[size_t(r)][size_t(i)], rows[size_t(r)][size_t((i + 1) % N)], rows[size_t(r + 1)][size_t((i + 1) % N)],
                                       rows[size_t(r + 1)][size_t(i)]});
            if (f != kInvalid)
                newFaces.push_back(f);
        }
    setSoftEdges(W, edgesOfFaces(W, newFaces));
    for (Index f : newFaces)
    {
        if (!W.faceValid(f))
            continue;
        if (W.faceSize(f) == 3)
            ++res.triangles;
        else if (W.faceSize(f) > 4)
            ++res.ngons;
    }
    const std::vector<std::string> issues = W.validate();
    W.compact();

    // Confidence.
    if (M != N)
        penalties.push_back({10.0 + 6.0 * std::abs(M - N),
                             std::format("hole border has {} points for a {}-point loop: {} triangle{}", M, N, res.triangles,
                                         res.triangles == 1 ? "" : "s")});
    const double angle = glm::degrees(std::acos(std::abs(cosAngle)));
    if (angle > 30.0)
        penalties.push_back({std::min(45.0, (angle - 30.0) * 0.9), std::format("limb meets the body at a grazing angle ({:.0f} deg off its normal)", angle)});
    if (region.poles > 0)
        penalties.push_back({std::min(20.0, 4.0 * region.poles), std::format("hole region touches {} poles", region.poles)});
    if (region.nonQuads > 0)
        penalties.push_back({std::min(15.0, 5.0 * region.nonQuads), std::format("hole region replaced {} non-quad faces", region.nonQuads)});
    if (shrunk)
        penalties.push_back({3.0, "hole pulled in to fit inside the faces around it"});
    if (!region.enclosed)
        penalties.push_back({25.0, "the hole is larger than the faces around it; the ring may overlap"});
    if (misses > 0)
        penalties.push_back({std::min(20.0, 3.0 * misses), std::format("{} loop points missed the body along the axis", misses)});
    if (cand.planarity > 0.25)
        penalties.push_back({10.0, "the limb loop is far from planar"});
    if (cand.gap > 4.0 * cand.radius)
        penalties.push_back({10.0, "long bridge: the limb is far from the body"});
    if (faceBorderLoop)
        penalties.push_back({10.0, "no edge loop on the limb faces the body; used a face border"});
    if (N < 4)
        penalties.push_back({10.0, std::format("only {} points in the loop", N)});
    if (res.supportLoops > 0)
        penalties.push_back({std::min(6.0, 1.0 * res.supportLoops), std::format("added {} support loops to the body", res.supportLoops)});
    if (res.flippedLimb)
        penalties.push_back({5.0, "limb faces pointed inward; flipped them"});
    if (sigmaI != sigmaA)
        penalties.push_back({20.0, "the projected loop folds over itself"});
    if (!issues.empty())
        penalties.push_back({30.0, "result is not clean: " + issues.front()});
    std::stable_sort(penalties.begin(), penalties.end(), [](const Penalty& a, const Penalty& b) { return a.points > b.points; });

    double score = 100.0;
    for (const Penalty& p : penalties)
    {
        score -= p.points;
        res.reasons.push_back(p.why);
    }
    res.confidence = std::clamp(int(std::lround(score)), 0, 100);

    std::vector<std::string> good;
    if (M == N)
        good.push_back(std::format("matching {}-point loops", N));
    if (angle <= 15.0)
        good.push_back("square-on");
    if (res.trimmed)
        good.push_back("trimmed at the intersection");
    if (res.triangles == 0 && res.ngons == 0 && issues.empty())
        good.push_back("clean quads");
    for (const std::string& n : notes)
        res.reasons.push_back(n);
    for (const std::string& g : good)
        res.reasons.push_back(g);

    // A confident join leads with what went right; a doubtful one with what went wrong.
    std::vector<std::string> lead;
    if (res.confidence >= 85)
        lead = good;
    for (const Penalty& p : penalties)
        lead.push_back(p.why);
    if (res.confidence < 85)
        lead.insert(lead.end(), good.begin(), good.end());
    std::string head;
    for (size_t i = 0; i < lead.size() && i < 3; ++i)
        head += (i ? ", " : "") + lead[i];
    res.summary = std::format("{}: {}%", head.empty() ? std::string("joined") : head, res.confidence);
    res.ok = true;
    return res;
}
} // namespace lila::geo
