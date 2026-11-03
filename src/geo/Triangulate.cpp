#include "geo/Triangulate.h"

#include "core/Profile.h"

#include <tbb/blocked_range.h>
#include <tbb/enumerable_thread_specific.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>

namespace lila::geo
{
namespace
{
double orient2d(const glm::dvec2& a, const glm::dvec2& b, const glm::dvec2& c)
{
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

/** Orthonormal u, v with u x v along n. */
void planeAxes(const glm::dvec3& n, glm::dvec3& u, glm::dvec3& v)
{
    const glm::dvec3 a = std::abs(n.x) < 0.6 ? glm::dvec3(1, 0, 0) : std::abs(n.y) < 0.6 ? glm::dvec3(0, 1, 0)
                                                                                          : glm::dvec3(0, 0, 1);
    u = glm::normalize(glm::cross(a, n));
    v = glm::cross(n, u);
}

/** Largest coordinate extent, so tolerances scale with the polygon. */
double polygonSize(std::span<const glm::dvec2> poly)
{
    glm::dvec2 lo(1e300), hi(-1e300);
    for (const glm::dvec2& p : poly)
    {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    const double e = std::max(hi.x - lo.x, hi.y - lo.y);
    return e > 0.0 ? e : 1.0;
}

/** p inside or on the closed triangle abc (counter-clockwise), with a tolerance that blocks touching. */
bool inClosedTriangle(const glm::dvec2& p, const glm::dvec2& a, const glm::dvec2& b, const glm::dvec2& c, double eps)
{
    return orient2d(a, b, p) >= -eps && orient2d(b, c, p) >= -eps && orient2d(c, a, p) >= -eps;
}

/** > 0 when d lies inside the circle through the counter-clockwise triangle abc. */
double inCircle(const glm::dvec2& a, const glm::dvec2& b, const glm::dvec2& c, const glm::dvec2& d)
{
    const glm::dvec2 ad = a - d, bd = b - d, cd = c - d;
    const double a2 = glm::dot(ad, ad), b2 = glm::dot(bd, bd), c2 = glm::dot(cd, cd);
    return ad.x * (bd.y * c2 - b2 * cd.y) - ad.y * (bd.x * c2 - b2 * cd.x) + a2 * (bd.x * cd.y - bd.y * cd.x);
}

/**
 * Lawson flips toward the constrained Delaunay triangulation of the polygon: an interior diagonal
 * is swapped when the quad around it is strictly convex and the far point is inside the circumcircle.
 */
bool isForbidden(std::span<const uint64_t> forbidden, Index n, Index a, Index b)
{
    return !forbidden.empty() && std::binary_search(forbidden.begin(), forbidden.end(), uint64_t(std::min(a, b)) * n + std::max(a, b));
}

void delaunayFlips(std::span<const glm::dvec2> poly, std::span<Index> tris, std::span<const uint64_t> forbidden)
{
    const Index n = Index(poly.size());
    const size_t nt = tris.size() / 3;
    if (nt < 2)
        return;
    const double ext = polygonSize(poly);
    const double areaEps = 1e-12 * ext * ext;
    const double circleEps = 1e-10 * ext * ext * ext * ext;
    const auto key = [n](Index a, Index b) { return uint64_t(a) * n + b; };

    // Directed edge a->b to the triangle holding it.
    std::unordered_map<uint64_t, Index> owner;
    owner.reserve(nt * 3);
    for (size_t t = 0; t < nt; ++t)
        for (int k = 0; k < 3; ++k)
            owner[key(tris[t * 3 + k], tris[t * 3 + (k + 1) % 3])] = Index(t);

    std::vector<std::pair<Index, Index>> stack;
    for (size_t t = nt; t-- > 0;)
        for (int k = 3; k-- > 0;)
        {
            const Index a = tris[t * 3 + k], b = tris[t * 3 + (k + 1) % 3];
            if (a < b && owner.count(key(b, a)))
                stack.push_back({a, b});
        }

    const auto third = [&](Index t, Index a, Index b) {
        for (int k = 0; k < 3; ++k)
        {
            const Index v = tris[t * 3 + k];
            if (v != a && v != b)
                return v;
        }
        return kInvalid;
    };
    const auto setTri = [&](Index t, Index a, Index b, Index c) {
        tris[t * 3] = a;
        tris[t * 3 + 1] = b;
        tris[t * 3 + 2] = c;
        owner[key(a, b)] = t;
        owner[key(b, c)] = t;
        owner[key(c, a)] = t;
    };

    // Each flip strictly improves the triangulation, the cap only guards against float noise.
    size_t budget = nt * nt * 4 + 64;
    while (!stack.empty() && budget-- > 0)
    {
        auto [a, b] = stack.back();
        stack.pop_back();
        auto ia = owner.find(key(a, b)), ib = owner.find(key(b, a));
        if (ia == owner.end() || ib == owner.end())
            continue;
        const Index t1 = ia->second, t2 = ib->second;
        const Index x = third(t1, a, b), y = third(t2, a, b);
        if (x == kInvalid || y == kInvalid || x == y || owner.count(key(x, y)) || owner.count(key(y, x)) || isForbidden(forbidden, n, x, y))
            continue;
        // t1 = (a, b, x), t2 = (b, a, y): the quad is a, y, b, x.
        if (orient2d(poly[a], poly[y], poly[x]) <= areaEps || orient2d(poly[y], poly[b], poly[x]) <= areaEps)
            continue;
        if (inCircle(poly[a], poly[b], poly[x], poly[y]) <= circleEps)
            continue;
        owner.erase(key(a, b));
        owner.erase(key(b, a));
        setTri(t1, a, y, x);
        setTri(t2, y, b, x);
        for (auto [p, q] : {std::pair{a, y}, std::pair{y, b}, std::pair{b, x}, std::pair{x, a}})
            stack.push_back({std::min(p, q), std::max(p, q)});
    }
}

/**
 * Orders a diagonal by its endpoints' absolute coordinates. Mirroring across any axis plane keeps it, so equal-length
 * diagonals of mirrored quads (squares on a symmetric mesh) split the mirrored way.
 */
std::array<double, 6> mirrorKey(const glm::dvec3& a, const glm::dvec3& b)
{
    std::array<double, 3> x = {std::abs(a.x), std::abs(a.y), std::abs(a.z)}, y = {std::abs(b.x), std::abs(b.y), std::abs(b.z)};
    if (x < y)
        std::swap(x, y);
    return {x[0], x[1], x[2], y[0], y[1], y[2]};
}

struct FaceScratch
{
    std::vector<Index> corners;
    std::vector<glm::dvec3> pos;
    std::vector<glm::dvec2> poly;
    std::vector<Index> local;
    std::vector<Index> tris;
    std::vector<std::pair<Index, Index>> touch;
    std::vector<uint64_t> forbidden;
};

/** Some other face, or a loose edge, already joins points u and v. */
bool joinedElsewhere(const Mesh& mesh, Index f, Index u, Index v)
{
    bool found = mesh.findEdge(u, v) != kInvalid;
    if (!found)
        mesh.forEachPointCorner(u, [&](Index c) {
            const Index g = mesh.cornerFace(c);
            found = found || (g != f && mesh.findCorner(g, v) != kInvalid);
        });
    return found;
}

/**
 * Diagonals of face f (local a * n + b, a < b, sorted) whose two points another face also spans, as at a valence-2
 * point between two faces. Taking one would build the same triangle or edge twice.
 */
void sharedDiagonals(const Mesh& mesh, Index f, FaceScratch& s)
{
    s.forbidden.clear();
    s.touch.clear();
    const Index n = Index(s.corners.size());
    for (Index i = 0; i < n; ++i)
        mesh.forEachPointCorner(mesh.cornerPoint(s.corners[i]), [&](Index c) {
            if (mesh.cornerFace(c) != f)
                s.touch.push_back({mesh.cornerFace(c), i});
        });
    for (Index i = 0; i < n; ++i)
        for (Index j = i + 2; j < n; ++j)
            if (!(i == 0 && j == n - 1) && mesh.findEdge(mesh.cornerPoint(s.corners[i]), mesh.cornerPoint(s.corners[j])) != kInvalid)
                s.forbidden.push_back(uint64_t(i) * n + j);
    std::sort(s.touch.begin(), s.touch.end());
    for (size_t a = 0; a < s.touch.size();)
    {
        size_t b = a;
        while (b < s.touch.size() && s.touch[b].first == s.touch[a].first)
            ++b;
        for (size_t x = a; x < b; ++x)
            for (size_t y = x + 1; y < b; ++y)
            {
                const Index i = std::min(s.touch[x].second, s.touch[y].second), j = std::max(s.touch[x].second, s.touch[y].second);
                if (j - i >= 2 && !(i == 0 && j == n - 1))
                    s.forbidden.push_back(uint64_t(i) * n + j);
            }
        a = b;
    }
    std::sort(s.forbidden.begin(), s.forbidden.end());
    s.forbidden.erase(std::unique(s.forbidden.begin(), s.forbidden.end()), s.forbidden.end());
}

void earClipImpl(std::span<const glm::dvec2> poly, std::vector<Index>& out, std::span<const uint64_t> forbidden);

void triangulateFaceImpl(const Mesh& mesh, Index f, std::vector<Index>& out, const TriangulateOptions& opts,
                         FaceScratch& s)
{
    const Index n = mesh.faceSize(f);
    s.corners.clear();
    mesh.forEachFaceCorner(f, [&](Index c) { s.corners.push_back(c); });
    const std::vector<Index>& c = s.corners;
    if (n == 3)
    {
        out.insert(out.end(), {c[0], c[1], c[2]});
        return;
    }

    s.pos.resize(n);
    for (Index i = 0; i < n; ++i)
        s.pos[i] = mesh.position(mesh.cornerPoint(c[i]));
    glm::dvec3 newell(0.0);
    for (Index i = 0; i < n; ++i)
        newell += glm::cross(s.pos[i] - s.pos[0], s.pos[(i + 1) % n] - s.pos[0]);
    const double newellLen = glm::length(newell);
    if (!(newellLen > 0.0))
    {
        for (Index i = 1; i + 1 < n; ++i)
            out.insert(out.end(), {c[0], c[i], c[i + 1]});
        return;
    }
    glm::dvec3 u, v;
    planeAxes(newell / newellLen, u, v);
    s.poly.resize(n);
    for (Index i = 0; i < n; ++i)
    {
        const glm::dvec3 d = s.pos[i] - s.pos[0];
        s.poly[i] = {glm::dot(d, u), glm::dot(d, v)};
    }

    if (n == 4)
    {
        const double ext = polygonSize(s.poly);
        const double eps = 1e-12 * ext * ext;
        const auto& q = s.poly;
        // A diagonal folds when one of its triangles faces against the polygon.
        const bool ok02 = orient2d(q[0], q[1], q[2]) > eps && orient2d(q[0], q[2], q[3]) > eps;
        const bool ok13 = orient2d(q[1], q[2], q[3]) > eps && orient2d(q[1], q[3], q[0]) > eps;
        bool alt = false;
        if (opts.quads == QuadSplit::ShortestDiagonal)
        {
            const double d13 = glm::distance(s.pos[1], s.pos[3]), d02 = glm::distance(s.pos[0], s.pos[2]);
            alt = std::abs(d13 - d02) > 1e-9 * (d13 + d02) ? d13 < d02 : mirrorKey(s.pos[1], s.pos[3]) > mirrorKey(s.pos[0], s.pos[2]);
        }
        if (alt ? !ok13 && ok02 : !ok02 && ok13)
            alt = !alt;
        const Index p0 = mesh.cornerPoint(c[0]), p1 = mesh.cornerPoint(c[1]), p2 = mesh.cornerPoint(c[2]), p3 = mesh.cornerPoint(c[3]);
        if (alt ? joinedElsewhere(mesh, f, p1, p3) && (ok02 || !ok13) && !joinedElsewhere(mesh, f, p0, p2)
                : joinedElsewhere(mesh, f, p0, p2) && (ok13 || !ok02) && !joinedElsewhere(mesh, f, p1, p3))
            alt = !alt;
        if (alt)
            out.insert(out.end(), {c[1], c[2], c[3], c[1], c[3], c[0]});
        else
            out.insert(out.end(), {c[0], c[1], c[2], c[0], c[2], c[3]});
        return;
    }

    s.local.clear();
    sharedDiagonals(mesh, f, s);
    earClipImpl(s.poly, s.local, s.forbidden);
    if (opts.beauty)
        delaunayFlips(s.poly, s.local, s.forbidden);
    for (Index i : s.local)
        out.push_back(c[i]);
}
} // namespace

namespace
{
void earClipImpl(std::span<const glm::dvec2> poly, std::vector<Index>& out, std::span<const uint64_t> forbidden)
{
    const Index n = Index(poly.size());
    if (n < 3)
        return;
    const double ext = polygonSize(poly);
    const double areaEps = 1e-12 * ext * ext;
    std::vector<Index> next(n), prev(n);
    for (Index i = 0; i < n; ++i)
    {
        next[i] = (i + 1) % n;
        prev[i] = (i + n - 1) % n;
    }
    Index remaining = n;
    // Flat (collinear) tips never make ears, so the extra points bevels and loops leave on straight
    // edges get joined across the polygon instead of into zero-area slivers.
    auto isEar = [&](Index i) {
        const Index ia = prev[i], ic = next[i];
        const glm::dvec2 &a = poly[ia], &b = poly[i], &c = poly[ic];
        if (orient2d(a, b, c) <= areaEps || (remaining > 3 && isForbidden(forbidden, n, ia, ic)))
            return false;
        const glm::dvec2 lo = glm::min(a, glm::min(b, c)), hi = glm::max(a, glm::max(b, c));
        for (Index j = next[ic]; j != ia; j = next[j])
        {
            const glm::dvec2& p = poly[j];
            if (p.x < lo.x || p.y < lo.y || p.x > hi.x || p.y > hi.y)
                continue;
            // A point on top of a corner is the polygon touching itself there, not blocking.
            if (p == a || p == b || p == c)
                continue;
            if (inClosedTriangle(p, a, b, c, areaEps))
                return false;
        }
        return true;
    };

    Index i = 0;
    Index sinceClip = 0;
    while (remaining > 3)
    {
        const bool stuck = sinceClip >= remaining;
        if (stuck)
        {
            // No clean ear (self-overlapping projection or float noise): clip the most convex tip.
            Index best = i;
            double bestCross = -1e300;
            Index j = i;
            for (Index k = 0; k < remaining; ++k, j = next[j])
            {
                const double cr = orient2d(poly[prev[j]], poly[j], poly[next[j]]);
                if (cr > bestCross)
                {
                    bestCross = cr;
                    best = j;
                }
            }
            i = best;
        }
        if (stuck || isEar(i))
        {
            out.insert(out.end(), {prev[i], i, next[i]});
            next[prev[i]] = next[i];
            prev[next[i]] = prev[i];
            --remaining;
            i = prev[i];
            sinceClip = 0;
            continue;
        }
        i = next[i];
        ++sinceClip;
    }
    out.insert(out.end(), {prev[i], i, next[i]});
}
} // namespace

void earClip(std::span<const glm::dvec2> poly, std::vector<Index>& out) { earClipImpl(poly, out, {}); }

void triangulateFace(const Mesh& mesh, Index f, std::vector<Index>& out, const TriangulateOptions& opts)
{
    FaceScratch s;
    triangulateFaceImpl(mesh, f, out, opts, s);
}

Triangulation triangulate(const Mesh& mesh, const TriangulateOptions& opts)
{
    LILA_ZONE();

    Triangulation t;
    const Index fc = mesh.faceCapacity();
    t.faceFirst.resize(fc);
    Index total = 0;
    for (Index f = 0; f < fc; ++f)
    {
        if (!mesh.faceValid(f))
        {
            t.faceFirst[f] = kInvalid;
            continue;
        }
        t.faceFirst[f] = total;
        total += mesh.faceSize(f) - 2;
    }
    t.corners.resize(size_t(total) * 3);
    t.faces.resize(total);

    tbb::enumerable_thread_specific<FaceScratch> scratch;
    tbb::parallel_for(tbb::blocked_range<Index>(0, fc, 4096), [&](const tbb::blocked_range<Index>& r) {
        FaceScratch& s = scratch.local();
        for (Index f = r.begin(); f < r.end(); ++f)
        {
            if (t.faceFirst[f] == kInvalid)
                continue;
            s.tris.clear();
            triangulateFaceImpl(mesh, f, s.tris, opts, s);
            const Index first = t.faceFirst[f];
            std::copy(s.tris.begin(), s.tris.end(), t.corners.begin() + size_t(first) * 3);
            for (Index k = 0; k < mesh.faceSize(f) - 2; ++k)
                t.faces[first + k] = f;
        }
    });
    return t;
}
} // namespace lila::geo
