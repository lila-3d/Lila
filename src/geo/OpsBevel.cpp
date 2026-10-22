#include "geo/Ops.h"
#include "geo/OpsCommon.h"

#include "geo/Normals.h"
#include "geo/Topology.h"

#include "core/Profile.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <tuple>
#include <unordered_map>

/*
 * Bevel in the manner of Blender's: every bevelled point is replaced by boundary points, one per fan face that
 * borders a bevelled edge (a miter where two bevelled edges share the face, otherwise a slide along the unbevelled
 * edge). Each bevelled edge becomes a strip of quads between the profiles at its two ends, and the hole left around
 * the point (its vertex mesh) is closed according to its shape: nothing when the profiles coincide, the profile
 * folded into the end face for a terminal edge, a fan around the kept point, or a grid of quads for real corners.
 */
namespace lila::geo
{
namespace
{
using namespace detail;

/** An unbevelled edge closer than this (sine of the angle) to the bevelled one cannot be slid along. */
constexpr double kMinSlide = 0.2;
/** Clamped offsets leave this share of every edge they shrink. */
constexpr double kClampMargin = 0.02;

/** Corner attribute recipe from corners of the original faces. */
struct Mix
{
    std::vector<Index> src;
    std::vector<float> w;

    static Mix one(Index c) { return {{c}, {1.0f}}; }
    static Mix two(Index a, Index b, double t) { return {{a, b}, {float(1.0 - t), float(t)}}; }
    static Mix lerp(const Mix& a, const Mix& b, double t)
    {
        Mix m;
        m.add(a, 1.0 - t);
        m.add(b, t);
        return m;
    }
    void add(const Mix& m, double s)
    {
        for (size_t i = 0; i < m.src.size(); ++i)
        {
            src.push_back(m.src[i]);
            w.push_back(float(m.w[i] * s));
        }
    }
    void apply(Mesh& mesh, Index c) const
    {
        if (!src.empty())
            mesh.cornerAttrs().mix(c, src, w);
    }
};

enum class Kind : uint8_t
{
    Point,  // the bevelled point itself
    Slide,  // along an unbevelled edge
    Offset, // inside the face
    Extra,  // added on a straight side of the vertex mesh
};

/** One of the points replacing the bevelled point in a fan face, in face order. */
struct ChainPoint
{
    Kind kind;
    Index edge = kInvalid;   // Slide: the edge it moves along
    glm::dvec3 dir{0.0};     // displacement per unit offset
    Index point = kInvalid;
    Mix mix;
    Index srcEdge = kInvalid; // attributes for the edge to the next point of the face
};

enum class FaceKind : uint8_t
{
    Keep,      // no bevelled edge in the face at this point
    Miter,     // both edges bevelled
    SlideOut,  // in-edge bevelled, slides along the out-edge
    SlideIn,   // out-edge bevelled, slides along the in-edge
    OffsetIn,  // in-edge bevelled but the out-edge cannot be slid along
    OffsetOut, // out-edge bevelled but the in-edge cannot be slid along
};

/** Fan of one bevelled point. Face i lies between edges[i] (coming in) and outEdge(i). */
struct Vert
{
    Index point;
    bool closed = false;
    std::vector<Index> corners;
    std::vector<Index> edges;
    std::vector<FaceKind> kind;
    std::vector<glm::dvec3> offsetDir;
    std::vector<std::vector<ChainPoint>> chain;
    std::vector<size_t> profileAt; // profile across edges[i] when it is bevelled

    size_t size() const { return corners.size(); }
    Index inEdge(size_t i) const { return edges[i]; }
    Index outEdge(size_t i) const { return edges[closed ? (i + 1) % corners.size() : i + 1]; }
};

double superR(double profile)
{
    const double pr = std::clamp(profile, 0.01, 0.999);
    return 2.0 * std::log(0.5) / std::log(pr);
}

/** Point of the unit superellipse x^r + y^r = 1 at t in [0, 1], a quarter turn. Symmetric in t and 1 - t. */
glm::dvec2 superXY(double t, double r)
{
    if (t <= 0.0)
        return {1.0, 0.0};
    if (t >= 1.0)
        return {0.0, 1.0};
    const double c = std::cos(t * glm::half_pi<double>()), s = std::cos((1.0 - t) * glm::half_pi<double>());
    return {std::pow(c, 2.0 / r), std::pow(s, 2.0 / r)};
}

/** Profile across a bevelled edge at one of its ends: the superellipse o + u x + w y from lo to hi. */
struct Profile
{
    Index lo = kInvalid, hi = kInvalid;
    glm::dvec3 o{0.0}, u{0.0}, w{0.0};
    bool linear = false;
    std::vector<glm::dvec4> planes; // tangency constraints dot(n, o) = d
    std::vector<Index> ids;        // interior points, lo to hi
    glm::dvec3 axis{0.0};          // direction of the bevelled edge it crosses
    Index through = kInvalid;      // the kept point, lying on the profile in segment throughSeg (lo to hi)
    size_t throughSeg = 0;

    glm::dvec3 at(double s, double r) const
    {
        if (linear)
            return o + u * (1.0 - s) + w * s;
        const glm::dvec2 xy = superXY(s, r);
        return o + u * xy.x + w * xy.y;
    }
};

glm::dvec3 safeNormalize(const glm::dvec3& v)
{
    const double l = glm::length(v);
    return l > 1e-300 ? v / l : glm::dvec3(0.0);
}

/**
 * The point nearest `near` that best satisfies the planes in the least-squares sense; directions the planes leave
 * free stay at `near`.
 */
glm::dvec3 nearestOnPlanes(std::span<const glm::dvec4> planes, const glm::dvec3& near)
{
    double a[3][3] = {}, y[3] = {};
    for (const glm::dvec4& pl : planes)
    {
        const glm::dvec3 n(pl);
        const double res = pl.w - glm::dot(n, near);
        for (int i = 0; i < 3; ++i)
        {
            y[i] += n[i] * res;
            for (int j = 0; j < 3; ++j)
                a[i][j] += n[i] * n[j];
        }
    }
    double v[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int sweep = 0; sweep < 32; ++sweep)
    {
        if (a[0][1] * a[0][1] + a[0][2] * a[0][2] + a[1][2] * a[1][2] < 1e-28)
            break;
        for (int p = 0; p < 2; ++p)
            for (int q = p + 1; q < 3; ++q)
            {
                if (std::abs(a[p][q]) < 1e-300)
                    continue;
                const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
                const double t = (theta >= 0.0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0), s = t * c;
                for (int k = 0; k < 3; ++k)
                {
                    const double akp = a[k][p], akq = a[k][q];
                    a[k][p] = c * akp - s * akq;
                    a[k][q] = s * akp + c * akq;
                }
                for (int k = 0; k < 3; ++k)
                {
                    const double apk = a[p][k], aqk = a[q][k];
                    a[p][k] = c * apk - s * aqk;
                    a[q][k] = s * apk + c * aqk;
                }
                for (int k = 0; k < 3; ++k)
                {
                    const double vkp = v[k][p], vkq = v[k][q];
                    v[k][p] = c * vkp - s * vkq;
                    v[k][q] = s * vkp + c * vkq;
                }
            }
    }
    const double top = std::max({a[0][0], a[1][1], a[2][2], 1e-300});
    glm::dvec3 out = near;
    for (int i = 0; i < 3; ++i)
    {
        if (a[i][i] <= 1e-7 * top)
            continue;
        const glm::dvec3 e(v[0][i], v[1][i], v[2][i]);
        out += e * (glm::dot(e, glm::dvec3(y[0], y[1], y[2])) / a[i][i]);
    }
    return out;
}

/** A face corner with its two neighbours, kept so corner recipes can be made after the face is rebuilt. */
struct CornerTri
{
    Index c = kInvalid, prev = kInvalid, next = kInvalid;
    glm::dvec3 o{0.0}, a{0.0}, b{0.0};

    static CornerTri of(const Mesh& mesh, Index c)
    {
        CornerTri t{c, mesh.cornerPrev(c), mesh.cornerNext(c)};
        t.o = mesh.position(mesh.cornerPoint(c));
        t.a = mesh.position(mesh.cornerPoint(t.prev)) - t.o;
        t.b = mesh.position(mesh.cornerPoint(t.next)) - t.o;
        return t;
    }

    /** Recipe for a point in the corner's face, extrapolated from the triangle. */
    Mix mix(const glm::dvec3& pos) const
    {
        const glm::dvec3 d = pos - o;
        const double aa = glm::dot(a, a), ab = glm::dot(a, b), bb = glm::dot(b, b);
        const double det = aa * bb - ab * ab;
        if (std::abs(det) <= 1e-12 * aa * bb)
            return Mix::one(c);
        const double al = (bb * glm::dot(a, d) - ab * glm::dot(b, d)) / det;
        const double be = (aa * glm::dot(b, d) - ab * glm::dot(a, d)) / det;
        return {{c, prev, next}, {float(1.0 - al - be), float(al), float(be)}};
    }
};

/** No two edges of the closed polygon cross or touch, apart from neighbours sharing their corner. */
bool polygonIsSimple(std::span<const glm::dvec2> p)
{
    const size_t n = p.size();
    auto cross = [](const glm::dvec2& a, const glm::dvec2& b) { return a.x * b.y - a.y * b.x; };
    for (size_t i = 0; i < n; ++i)
    {
        const glm::dvec2 a = p[i], b = p[(i + 1) % n];
        if (glm::distance(a, b) <= 0.0)
            return false;
        for (size_t j = i + 2; j < n; ++j)
        {
            if (i == 0 && j == n - 1)
                continue;
            const glm::dvec2 c = p[j], d = p[(j + 1) % n];
            const double d1 = cross(b - a, c - a), d2 = cross(b - a, d - a);
            const double d3 = cross(d - c, a - c), d4 = cross(d - c, b - c);
            if (d1 == 0 && d2 == 0)
            {
                // Collinear: only overlap counts.
                const glm::dvec2 dir = b - a;
                const double t0 = glm::dot(c - a, dir), t1 = glm::dot(d - a, dir), len = glm::dot(dir, dir);
                if (std::max(t0, t1) > 0.0 && std::min(t0, t1) < len)
                    return false;
                continue;
            }
            if (((d1 > 0) != (d2 > 0) || d1 == 0 || d2 == 0) && ((d3 > 0) != (d4 > 0) || d3 == 0 || d4 == 0))
                return false;
        }
    }
    return true;
}

/** Side of a vertex mesh between two of its boundary points. */
struct Side
{
    enum Type : uint8_t
    {
        Arc,      // a profile across a bevelled edge
        Straight, // an edge of a rebuilt fan face, chain[face][at + 1] to chain[face][at]
        Open,     // across the mesh border
    } type;
    size_t profile = 0;
    size_t face = 0;
    size_t at = 0;
};

/** The hole around a bevelled point: nodes[k] to nodes[k + 1] along sides[k], wound like the faces that will fill it. */
struct Hole
{
    std::vector<Index> nodes;
    std::vector<Side> sides;
};

enum class Fill : uint8_t
{
    None,
    Merge,   // terminal edge: the profile goes into the face beside it
    Polygon, // one face
    Fan,     // quads around the kept point
    Grid,    // Blender's quad grid corner
    Split,   // the boundary visits a point twice: one face per simple loop
};

struct VertMesh
{
    Hole hole;
    Fill fill = Fill::None;
    std::vector<std::vector<Index>> sideIds; // Grid: the n + 1 points of each side, node to node
};
} // namespace

std::vector<Index> bevelEdges(Mesh& mesh, std::span<const Index> edgesIn, const BevelOptions& opts)
{
    LILA_ZONE();

    const int segments = std::max(1, opts.segments);
    const double r = superR(opts.profile);
    std::vector<Index> sel = opts.symmetry ? opts.symmetry->expandEdges(mesh, edgesIn) : uniqueSorted(edgesIn);

    // Only manifold, consistently wound edges between manifold points.
    std::vector<uint8_t> bev(mesh.edgeCapacity(), 0);
    {
        std::vector<Index> fan, keep;
        for (Index e : sel)
        {
            if (!mesh.edgeValid(e) || !mesh.isManifoldEdge(e))
                continue;
            const Index c = mesh.edgeCorner(e);
            if (mesh.cornerPoint(c) == mesh.cornerPoint(mesh.cornerRadial(c)))
                continue;
            bool ok = true;
            for (int s = 0; s < 2 && ok; ++s)
            {
                bool closed = false;
                ok = pointFan(mesh, mesh.edgePoint(e, s), fan, closed);
            }
            if (ok)
            {
                bev[e] = 1;
                keep.push_back(e);
            }
        }
        sel = std::move(keep);
    }
    if (sel.empty())
        return {};

    const Index firstNewPoint = mesh.pointCapacity();
    const Index edgeCap0 = mesh.edgeCapacity();

    std::vector<Vert> verts;
    std::unordered_map<Index, size_t> vertOf;
    for (Index p : pointsOfEdges(mesh, sel))
    {
        Vert v;
        v.point = p;
        pointFan(mesh, p, v.corners, v.closed);
        for (Index c : v.corners)
            v.edges.push_back(mesh.cornerEdge(mesh.cornerPrev(c)));
        if (!v.closed)
            v.edges.push_back(mesh.cornerEdge(v.corners.back()));
        vertOf[p] = verts.size();
        verts.push_back(std::move(v));
    }

    // Boundary point directions per unit offset. Slides on an edge are shared by the faces on both sides.
    struct SlideAcc
    {
        double sum = 0.0;
        int count = 0;
        double dist = 0.0; // per unit offset, along the edge
        Index point = kInvalid;
        double t = 0.0; // fraction of the edge from the bevelled point
    };
    std::map<std::pair<size_t, Index>, SlideAcc> slides;
    for (size_t vi = 0; vi < verts.size(); ++vi)
    {
        Vert& v = verts[vi];
        const glm::dvec3 pv = mesh.position(v.point);
        v.kind.assign(v.size(), FaceKind::Keep);
        v.offsetDir.assign(v.size(), glm::dvec3(0.0));
        for (size_t i = 0; i < v.size(); ++i)
        {
            const bool bIn = bev[v.inEdge(i)], bOut = bev[v.outEdge(i)];
            if (!bIn && !bOut)
                continue;
            const Index c = v.corners[i];
            const glm::dvec3 n = mesh.faceNormal(mesh.cornerFace(c));
            const glm::dvec3 eIn = safeNormalize(mesh.position(mesh.cornerPoint(mesh.cornerPrev(c))) - pv);
            const glm::dvec3 eOut = safeNormalize(mesh.position(mesh.cornerNextPoint(c)) - pv);
            const glm::dvec3 perpIn = safeNormalize(glm::cross(eIn - n * glm::dot(eIn, n), n));
            const glm::dvec3 perpOut = safeNormalize(glm::cross(n, eOut - n * glm::dot(eOut, n)));
            if (bIn && bOut)
            {
                v.kind[i] = FaceKind::Miter;
                v.offsetDir[i] = (perpIn + perpOut) / std::max(1.0 + glm::dot(perpIn, perpOut), 0.05);
            }
            else if (bIn)
            {
                const double s = glm::dot(eOut, perpIn);
                if (s >= kMinSlide)
                {
                    v.kind[i] = FaceKind::SlideOut;
                    SlideAcc& acc = slides[{vi, v.outEdge(i)}];
                    acc.sum += 1.0 / s;
                    ++acc.count;
                }
                else
                {
                    v.kind[i] = FaceKind::OffsetIn;
                    v.offsetDir[i] = perpIn;
                }
            }
            else
            {
                const double s = glm::dot(eIn, perpOut);
                if (s >= kMinSlide)
                {
                    v.kind[i] = FaceKind::SlideIn;
                    SlideAcc& acc = slides[{vi, v.inEdge(i)}];
                    acc.sum += 1.0 / s;
                    ++acc.count;
                }
                else
                {
                    v.kind[i] = FaceKind::OffsetOut;
                    v.offsetDir[i] = perpOut;
                }
            }
        }
    }
    for (auto& [key, acc] : slides)
        acc.dist = acc.sum / acc.count;
    auto slideDir = [&](size_t vi, Index e) {
        const Index p = verts[vi].point;
        return safeNormalize(mesh.position(mesh.edgeOtherPoint(e, p)) - mesh.position(p)) * slides.at({vi, e}).dist;
    };

    // Chains: what replaces the point in each fan face, in face order.
    for (size_t vi = 0; vi < verts.size(); ++vi)
    {
        Vert& v = verts[vi];
        v.chain.assign(v.size(), {});
        for (size_t i = 0; i < v.size(); ++i)
        {
            const Index in = v.inEdge(i), out = v.outEdge(i);
            const bool sIn = slides.count({vi, in}) != 0, sOut = slides.count({vi, out}) != 0;
            std::vector<ChainPoint>& ch = v.chain[i];
            auto slide = [&](Index e, Index src) { ch.push_back({Kind::Slide, e, slideDir(vi, e), kInvalid, {}, src}); };
            auto offset = [&](Index src) { ch.push_back({Kind::Offset, kInvalid, v.offsetDir[i], kInvalid, {}, src}); };
            auto point = [&](Index src) { ch.push_back({Kind::Point, kInvalid, glm::dvec3(0.0), v.point, {}, src}); };
            switch (v.kind[i])
            {
            case FaceKind::Miter:
                offset(out);
                break;
            case FaceKind::SlideOut:
                slide(out, out);
                break;
            case FaceKind::SlideIn:
                slide(in, out);
                break;
            case FaceKind::OffsetIn:
                offset(kInvalid);
                point(out);
                if (sOut)
                    slide(out, out);
                break;
            case FaceKind::OffsetOut:
                if (sIn)
                    slide(in, in);
                point(kInvalid);
                offset(out);
                break;
            case FaceKind::Keep:
                if (sIn)
                    slide(in, sOut ? kInvalid : in);
                if (!(sIn && sOut))
                    point(out);
                if (sOut)
                    slide(out, out);
                break;
            }
        }
    }

    std::unordered_map<Index, std::pair<size_t, size_t>> fanSlot; // corner -> (vert, fan index)
    std::vector<Index> touchedFaces;
    for (size_t vi = 0; vi < verts.size(); ++vi)
        for (size_t i = 0; i < verts[vi].size(); ++i)
        {
            fanSlot[verts[vi].corners[i]] = {vi, i};
            touchedFaces.push_back(mesh.cornerFace(verts[vi].corners[i]));
        }
    touchedFaces = uniqueSorted(touchedFaces);

    // Clamp: along every edge of a rebuilt face, the points replacing its two ends must not pass each other.
    double offset = std::max(0.0, opts.offset);
    if (opts.clamp)
        for (Index f : touchedFaces)
            mesh.forEachFaceCorner(f, [&](Index c) {
                const Index cn = mesh.cornerNext(c);
                const glm::dvec3 a = mesh.position(mesh.cornerPoint(c)), b = mesh.position(mesh.cornerPoint(cn));
                const double len = glm::distance(a, b);
                if (len <= 0.0)
                    return;
                glm::dvec3 da(0.0), db(0.0);
                if (auto it = fanSlot.find(c); it != fanSlot.end())
                    da = verts[it->second.first].chain[it->second.second].back().dir;
                if (auto it = fanSlot.find(cn); it != fanSlot.end())
                    db = verts[it->second.first].chain[it->second.second].front().dir;
                const double k = glm::dot(da - db, (b - a) / len);
                if (k > 1e-12)
                    offset = std::min(offset, (1.0 - kClampMargin) * len / k);
            });
    // Points from different corners of one face can still cross inside it, as across a thin leg: shrink until none do.
    if (opts.clamp && offset > 0.0)
    {
        auto simpleAt = [&](Index f, double off) {
            const glm::dvec3 n = mesh.faceNormal(f);
            const glm::dvec3 ax = safeNormalize(glm::cross(n, std::abs(n.x) < 0.9 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 1, 0)));
            const glm::dvec3 ay = glm::cross(n, ax);
            std::vector<glm::dvec2> poly;
            mesh.forEachFaceCorner(f, [&](Index c) {
                const glm::dvec3 p = mesh.position(mesh.cornerPoint(c));
                auto it = fanSlot.find(c);
                if (it == fanSlot.end())
                    poly.push_back({glm::dot(p, ax), glm::dot(p, ay)});
                else
                    for (const ChainPoint& cp : verts[it->second.first].chain[it->second.second])
                    {
                        const glm::dvec3 q = p + cp.dir * off;
                        poly.push_back({glm::dot(q, ax), glm::dot(q, ay)});
                    }
            });
            return polygonIsSimple(poly);
        };
        std::vector<Index> crossing;
        for (Index f : touchedFaces)
            if (!simpleAt(f, offset) && simpleAt(f, offset * 1e-6))
                crossing.push_back(f);
        if (!crossing.empty())
        {
            double lo = 0.0, hi = offset;
            for (int it = 0; it < 40; ++it)
            {
                const double mid = 0.5 * (lo + hi);
                const bool ok = std::all_of(crossing.begin(), crossing.end(), [&](Index f) { return simpleAt(f, mid); });
                (ok ? lo : hi) = mid;
            }
            offset = lo * (1.0 - kClampMargin);
        }
    }
    if (offset <= 0.0)
        return {};

    // Points.
    for (auto& [key, acc] : slides)
    {
        const Index p = verts[key.first].point, w = mesh.edgeOtherPoint(key.second, p);
        acc.t = acc.dist * offset / mesh.edgeLength(key.second);
        acc.point = mesh.addPoint(glm::mix(mesh.position(p), mesh.position(w), acc.t));
        const Index src[2] = {p, w};
        const float wt[2] = {float(1.0 - acc.t), float(acc.t)};
        mesh.pointAttrs().mix(acc.point, src, wt);
    }
    for (size_t vi = 0; vi < verts.size(); ++vi)
    {
        Vert& v = verts[vi];
        const glm::dvec3 pv = mesh.position(v.point);
        for (size_t i = 0; i < v.size(); ++i)
        {
            const Index c = v.corners[i];
            for (ChainPoint& cp : v.chain[i])
                switch (cp.kind)
                {
                case Kind::Point:
                    cp.mix = Mix::one(c);
                    break;
                case Kind::Slide:
                {
                    const SlideAcc& acc = slides.at({vi, cp.edge});
                    cp.point = acc.point;
                    const bool isIn = cp.edge == v.inEdge(i);
                    cp.mix = Mix::two(c, isIn ? mesh.cornerPrev(c) : mesh.cornerNext(c), acc.t);
                    break;
                }
                case Kind::Offset:
                    cp.point = mesh.addPoint(pv + cp.dir * offset);
                    mesh.pointAttrs().copy(cp.point, v.point);
                    cp.mix = CornerTri::of(mesh, c).mix(mesh.position(cp.point));
                    break;
                case Kind::Extra:
                    break;
                }
        }
    }

    // Profiles at each end of each bevelled edge, shared by the strips whose ends meet the same two points.
    std::vector<Profile> profiles;
    std::map<std::tuple<size_t, Index, Index>, size_t> profileOf;
    for (size_t vi = 0; vi < verts.size(); ++vi)
    {
        Vert& v = verts[vi];
        v.profileAt.assign(v.size(), SIZE_MAX);
        for (size_t i = 0; i < v.size(); ++i)
        {
            if (!bev[v.inEdge(i)])
                continue;
            const size_t j = (i + v.size() - 1) % v.size();
            const Index a = v.chain[j].back().point, b = v.chain[i].front().point;
            auto key = std::make_tuple(vi, std::min(a, b), std::max(a, b));
            auto it = profileOf.find(key);
            if (it == profileOf.end())
            {
                Profile pr;
                pr.lo = std::get<1>(key);
                pr.hi = std::get<2>(key);
                it = profileOf.emplace(key, profiles.size()).first;
                profiles.push_back(std::move(pr));
            }
            v.profileAt[i] = it->second;
            // The profile leaves a tangent to the face of a and arrives tangent to the face of b.
            const glm::dvec3 na = mesh.faceNormal(mesh.cornerFace(v.corners[j]));
            const glm::dvec3 nb = mesh.faceNormal(mesh.cornerFace(v.corners[i]));
            Profile& pr = profiles[it->second];
            if (pr.planes.empty())
                pr.axis = safeNormalize(mesh.position(mesh.edgeOtherPoint(v.inEdge(i), v.point)) - mesh.position(v.point));
            pr.planes.push_back(glm::dvec4(na, glm::dot(na, mesh.position(b))));
            pr.planes.push_back(glm::dvec4(nb, glm::dot(nb, mesh.position(a))));
        }
    }
    std::vector<size_t> profileVert(profiles.size());
    for (const auto& [key, idx] : profileOf)
        profileVert[idx] = std::get<0>(key);
    for (size_t k = 0; k < profiles.size(); ++k)
    {
        Profile& pr = profiles[k];
        const glm::dvec3 lo = mesh.position(pr.lo), hi = mesh.position(pr.hi), mid = (lo + hi) * 0.5;
        const double chord = glm::distance(lo, hi);
        pr.o = nearestOnPlanes(pr.planes, mid);
        const double away = glm::distance(pr.o, mid);
        pr.linear = away <= 1e-9 * chord || away > 1e3 * chord;
        if (pr.linear)
        {
            pr.o = glm::dvec3(0.0);
            pr.u = lo;
            pr.w = hi;
        }
        else
        {
            pr.u = lo - pr.o;
            pr.w = hi - pr.o;
        }
        const Index src = verts[profileVert[k]].point;
        for (int s = 1; s < segments; ++s)
        {
            const Index np = mesh.addPoint(pr.at(double(s) / segments, r));
            mesh.pointAttrs().copy(np, src);
            pr.ids.push_back(np);
        }
    }
    auto arcIds = [&](size_t prof, Index from) {
        std::vector<Index> ids = profiles[prof].ids;
        if (from != profiles[prof].lo)
            std::reverse(ids.begin(), ids.end());
        return ids;
    };
    auto arcAt = [&](size_t prof, Index from, double s) {
        return profiles[prof].at(from == profiles[prof].lo ? s : 1.0 - s, r);
    };

    // Vertex meshes: walk the hole around each point against the fan so the fill winds like its neighbours.
    std::vector<VertMesh> vmesh(verts.size());
    struct Insert
    {
        size_t vert, face, at;
        std::vector<ChainPoint> pts;
    };
    std::vector<Insert> inserts;

    /**
     * A terminal edge on a point that stays for its other edges, where a fan from the point would have no area (a flat
     * surface) or fold back over its neighbours (the profile bulging toward a convex corner): the point moves onto the
     * profile instead and each face beside it takes the profile up to the point.
     */
    std::vector<Index> welded;
    auto zipTerminal = [&](size_t vi) {
        const Vert& v = verts[vi];
        const Hole& h = vmesh[vi].hole;
        if (h.nodes.size() != 3)
            return false;
        const size_t kv = size_t(std::find(h.nodes.begin(), h.nodes.end(), v.point) - h.nodes.begin());
        if (kv >= 3)
            return false;
        const Side& toA = h.sides[kv];
        const Side& arc = h.sides[(kv + 1) % 3];
        const Side& fromB = h.sides[(kv + 2) % 3];
        if (toA.type != Side::Straight || arc.type != Side::Arc || fromB.type != Side::Straight)
            return false;
        const Index a = h.nodes[(kv + 1) % 3], b = h.nodes[(kv + 2) % 3];
        std::vector<Index> full{a};
        const std::vector<Index> mid = arcIds(arc.profile, a);
        full.insert(full.end(), mid.begin(), mid.end());
        full.push_back(b);
        const glm::dvec3 pv = mesh.position(v.point);
        // Zip when the fan would be flat, face against the surface, or fold back over a face beside it.
        glm::dvec3 fanNormal(0.0), surface(0.0);
        for (size_t j = 0; j + 1 < full.size(); ++j)
            fanNormal += glm::cross(mesh.position(full[j]) - pv, mesh.position(full[j + 1]) - pv);
        for (Index c : v.corners)
            surface += mesh.faceNormal(mesh.cornerFace(c));
        const double chord = glm::distance(mesh.position(a), mesh.position(b));
        if (glm::length(fanNormal) > 1e-9 * chord * chord)
        {
            const glm::dvec3 nf = glm::normalize(fanNormal);
            const double fold = std::min(glm::dot(nf, mesh.faceNormal(mesh.cornerFace(v.corners[toA.face]))),
                                         glm::dot(nf, mesh.faceNormal(mesh.cornerFace(v.corners[fromB.face]))));
            if (fold > -0.9 && glm::dot(nf, safeNormalize(surface)) > -0.25)
                return false;
        }
        // The point moves onto the profile where it comes nearest, welding to a profile point close to there.
        const int n = segments;
        double sBest = 0.5, dBest = 1e300;
        for (int k = 0; k <= 64 * n; ++k)
        {
            const double sk = double(k) / (64 * n);
            const double d = glm::distance(arcAt(arc.profile, a, sk), pv);
            if (d < dBest)
            {
                dBest = d;
                sBest = sk;
            }
        }
        Profile& pr = profiles[arc.profile];
        const bool forward = a == pr.lo;
        const long jw = std::lround(sBest * n);
        size_t stop, cut; // full[1, stop) goes to the face on a's side, full[cut, end - 1) to b's
        if (jw > 0 && jw < n && std::abs(sBest * n - double(jw)) <= 0.25)
        {
            Index& slot = pr.ids[forward ? size_t(jw) - 1 : pr.ids.size() - size_t(jw)];
            mesh.setPosition(v.point, mesh.position(slot));
            welded.push_back(slot);
            slot = v.point;
            stop = size_t(jw);
            cut = size_t(jw) + 1;
        }
        else
        {
            const size_t seg = size_t(std::clamp(long(std::floor(sBest * n)), 0L, long(n) - 1));
            mesh.setPosition(v.point, arcAt(arc.profile, a, sBest));
            pr.through = v.point;
            pr.throughSeg = forward ? seg : size_t(n) - 1 - seg;
            stop = seg + 1;
            cut = seg + 1;
        }
        auto insert = [&](const Side& side, size_t from, size_t to) {
            Insert ins{vi, side.face, side.at, {}};
            const Index c = v.corners[side.face];
            const Index src = v.chain[side.face][side.at].srcEdge;
            for (size_t j = from; j < to; ++j)
                ins.pts.push_back({Kind::Extra, kInvalid, glm::dvec3(0.0), full[j], CornerTri::of(mesh, c).mix(mesh.position(full[j])), src});
            if (!ins.pts.empty())
                inserts.push_back(std::move(ins));
        };
        // Face order runs a to the kept point on one side and the kept point to b on the other.
        insert(toA, 1, stop);
        insert(fromB, cut, full.size() - 1);
        return true;
    };
    for (size_t vi = 0; vi < verts.size(); ++vi)
    {
        const Vert& v = verts[vi];
        Hole& h = vmesh[vi].hole;
        const size_t n = v.size();
        std::optional<Side> pending;
        for (size_t step = 0; step < n; ++step)
        {
            const size_t i = n - 1 - step;
            const std::vector<ChainPoint>& ch = v.chain[i];
            for (size_t k = ch.size(); k-- > 0;)
            {
                if (k + 1 < ch.size())
                    h.sides.push_back({Side::Straight, 0, i, k});
                else if (pending)
                {
                    h.sides.push_back(*pending);
                    pending.reset();
                }
                else if (!h.nodes.empty())
                    continue; // shared with the previous face across an unbevelled edge
                h.nodes.push_back(ch[k].point);
            }
            if (bev[v.inEdge(i)])
                pending = Side{Side::Arc, v.profileAt[i], 0, 0};
            else if (i == 0 && !v.closed)
                pending = Side{Side::Open};
        }
        if (pending)
            h.sides.push_back(*pending);
        else
            h.nodes.pop_back();

        // Drop profiles walked out and straight back: in-line points, and strips meeting along their miter.
        for (bool changed = true; changed && !h.sides.empty();)
        {
            changed = false;
            const size_t K = h.nodes.size();
            if (K == 2 && h.sides[0].type == Side::Arc && h.sides[1].type == Side::Arc && h.sides[0].profile == h.sides[1].profile)
            {
                h.nodes.clear();
                h.sides.clear();
                break;
            }
            for (size_t k = 0; k < K && K > 2; ++k)
            {
                const size_t k1 = (k + 1) % K, k2 = (k + 2) % K;
                if (h.sides[k].type != Side::Arc || h.sides[k1].type != Side::Arc || h.sides[k].profile != h.sides[k1].profile
                    || h.nodes[k] != h.nodes[k2])
                    continue;
                std::vector<Index> nodes;
                std::vector<Side> sides;
                for (size_t q = 0; q < K; ++q)
                {
                    if (q == k1 || q == k2)
                        continue;
                    nodes.push_back(h.nodes[q]);
                    sides.push_back(q == k ? h.sides[k2] : h.sides[q]);
                }
                h.nodes = std::move(nodes);
                h.sides = std::move(sides);
                changed = true;
                break;
            }
        }

        const size_t K = h.nodes.size();
        VertMesh& vm = vmesh[vi];
        if (K < 2)
            continue;
        size_t arcs = 0, opens = 0;
        for (const Side& s : h.sides)
        {
            arcs += s.type == Side::Arc;
            opens += s.type == Side::Open;
        }
        if (K == 2)
        {
            // Two profiles between the same points are one and the same, so the walk dropped them already.
            if (arcs == 1 && opens == 0)
            {
                // Terminal edge: the profile replaces the straight cut in the face beside it.
                const size_t s = h.sides[0].type == Side::Straight ? 0 : 1;
                const Side& st = h.sides[s];
                const Side& arc = h.sides[1 - s];
                const Index from = h.nodes[(1 - s)];
                Insert ins{vi, st.face, st.at, {}};
                const Index c = v.corners[st.face];
                const Index src = v.chain[st.face][st.at].srcEdge;
                for (Index p : arcIds(arc.profile, from))
                    ins.pts.push_back({Kind::Extra, kInvalid, glm::dvec3(0.0), p, CornerTri::of(mesh, c).mix(mesh.position(p)), src});
                inserts.push_back(std::move(ins));
                vm.fill = Fill::Merge;
            }
            continue;
        }
        std::vector<Index> sorted = h.nodes;
        std::sort(sorted.begin(), sorted.end());
        if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end())
            vm.fill = Fill::Split;
        else if (zipTerminal(vi))
            vm.fill = Fill::None;
        else if (segments == 1 || arcs == 0)
            vm.fill = Fill::Polygon;
        else if (std::find(h.nodes.begin(), h.nodes.end(), v.point) != h.nodes.end())
            vm.fill = Fill::Fan;
        else
        {
            // Every side gets the profile's segment count, so straight ones are split, in their face too.
            vm.fill = Fill::Grid;
            vm.sideIds.resize(K);
            for (size_t k = 0; k < K; ++k)
            {
                const Side& s = h.sides[k];
                const Index a = h.nodes[k], b = h.nodes[(k + 1) % K];
                std::vector<Index>& ids = vm.sideIds[k];
                ids.push_back(a);
                if (s.type == Side::Arc)
                {
                    const std::vector<Index> mid = arcIds(s.profile, a);
                    ids.insert(ids.end(), mid.begin(), mid.end());
                }
                else
                    for (int j = 1; j < segments; ++j)
                    {
                        const Index np = mesh.addPoint(glm::mix(mesh.position(a), mesh.position(b), double(j) / segments));
                        mesh.pointAttrs().copy(np, v.point);
                        ids.push_back(np);
                    }
                ids.push_back(b);
                if (s.type == Side::Straight)
                {
                    Insert ins{vi, s.face, s.at, {}};
                    const Index c = v.corners[s.face];
                    const Index src = v.chain[s.face][s.at].srcEdge;
                    for (size_t j = ids.size() - 1; j-- > 1;)
                        ins.pts.push_back({Kind::Extra, kInvalid, glm::dvec3(0.0), ids[j], CornerTri::of(mesh, c).mix(mesh.position(ids[j])), src});
                    inserts.push_back(std::move(ins));
                }
            }
        }
    }
    std::sort(inserts.begin(), inserts.end(), [](const Insert& a, const Insert& b) {
        return std::tie(a.vert, a.face, b.at) < std::tie(b.vert, b.face, a.at);
    });
    for (Insert& ins : inserts)
    {
        std::vector<ChainPoint>& ch = verts[ins.vert].chain[ins.face];
        ch.insert(ch.begin() + ptrdiff_t(ins.at) + 1, ins.pts.begin(), ins.pts.end());
    }

    // Rebuild every face around a bevelled point. Vertex meshes take their uvs from the first fan face.
    std::vector<CornerTri> repCorner;
    for (const Vert& v : verts)
        repCorner.push_back(CornerTri::of(mesh, v.corners[0]));
    const std::vector<Index> oldEdges = edgesOfFaces(mesh, touchedFaces);
    {
        std::vector<Index> pts, edgeSrc;
        std::vector<const Mix*> mixes;
        std::vector<Mix> own;
        for (Index f : touchedFaces)
        {
            pts.clear();
            edgeSrc.clear();
            mixes.clear();
            own.clear();
            own.reserve(mesh.faceSize(f));
            mesh.forEachFaceCorner(f, [&](Index c) {
                auto it = fanSlot.find(c);
                if (it == fanSlot.end())
                {
                    pts.push_back(mesh.cornerPoint(c));
                    own.push_back(Mix::one(c));
                    mixes.push_back(&own.back());
                    edgeSrc.push_back(mesh.cornerEdge(c));
                    return;
                }
                for (const ChainPoint& cp : verts[it->second.first].chain[it->second.second])
                {
                    pts.push_back(cp.point);
                    mixes.push_back(&cp.mix);
                    edgeSrc.push_back(cp.srcEdge);
                }
            });
            mesh.removeFace(f);
            const Index nf = addFaceFrom(mesh, pts, f, {}, edgeSrc);
            if (nf == kInvalid)
                continue;
            Index c = mesh.faceCorner(nf);
            for (size_t k = 0; k < pts.size(); ++k, c = mesh.cornerNext(c))
                mixes[k]->apply(mesh, c);
        }
    }

    std::vector<Index> out;
    auto emit = [&](std::span<const Index> pts, Index srcFace, std::span<const Mix> mix) {
        const Index f = mesh.addFace(pts, srcFace, {});
        if (f == kInvalid)
            return;
        out.push_back(f);
        Index c = mesh.faceCorner(f);
        for (size_t k = 0; k < pts.size(); ++k, c = mesh.cornerNext(c))
            mix[k].apply(mesh, c);
    };

    // Strips.
    for (Index e : sel)
    {
        std::vector<Index> side[2];
        std::vector<Mix> sideMix[2];
        struct Through
        {
            size_t seg = SIZE_MAX;
            Index point = kInvalid;
            Mix mix;
        } through[2];
        Index srcFace = kInvalid;
        for (int s = 0; s < 2; ++s)
        {
            const size_t vi = vertOf.at(mesh.edgePoint(e, s));
            const Vert& v = verts[vi];
            size_t i = 0;
            while (v.inEdge(i) != e)
                ++i;
            // The face where e goes out comes first.
            const size_t j = (i + v.size() - 1) % v.size();
            const ChainPoint& a = v.chain[j].back();
            const ChainPoint& b = v.chain[i].front();
            side[s].push_back(a.point);
            const std::vector<Index> mid = arcIds(v.profileAt[i], a.point);
            side[s].insert(side[s].end(), mid.begin(), mid.end());
            side[s].push_back(b.point);
            for (int k = 0; k <= segments; ++k)
                sideMix[s].push_back(Mix::lerp(a.mix, b.mix, double(k) / segments));
            const Profile& pr = profiles[v.profileAt[i]];
            if (pr.through != kInvalid)
            {
                const size_t seg = a.point == pr.lo ? pr.throughSeg : size_t(segments) - 1 - pr.throughSeg;
                through[s] = {seg, pr.through, Mix::lerp(a.mix, b.mix, (seg + 0.5) / segments)};
            }
            if (s == 0)
                srcFace = mesh.cornerFace(v.corners[j]);
        }
        const std::vector<Index>& pa = side[0];
        const std::vector<Index>& pb = side[1];
        const size_t k = size_t(segments);
        for (size_t j = 0; j < k; ++j)
        {
            std::vector<Index> pts{pb[k - j], pa[j]};
            std::vector<Mix> mix{sideMix[1][k - j], sideMix[0][j]};
            if (through[0].seg == j)
            {
                pts.push_back(through[0].point);
                mix.push_back(through[0].mix);
            }
            pts.insert(pts.end(), {pa[j + 1], pb[k - j - 1]});
            mix.insert(mix.end(), {sideMix[0][j + 1], sideMix[1][k - j - 1]});
            if (through[1].seg == k - j - 1)
            {
                pts.push_back(through[1].point);
                mix.push_back(through[1].mix);
            }
            emit(pts, srcFace, mix);
        }
    }

    // Vertex meshes.
    const double gCenter = [&] {
        const double num = std::pow(1.0 / 3.0, 1.0 / r) - 1.0 / 3.0;
        const double den = (2.0 / 3.0) * (std::pow(0.5, 1.0 / r) - 0.5);
        return std::abs(den) > 1e-9 ? num / den : 1.0;
    }();
    for (size_t vi = 0; vi < verts.size(); ++vi)
    {
        const Vert& v = verts[vi];
        const VertMesh& vm = vmesh[vi];
        const Hole& h = vm.hole;
        const size_t K = h.nodes.size();
        const Index srcFace = mesh.cornerFace(v.corners[0]);
        auto patchMix = [&](std::span<const Index> pts) {
            std::vector<Mix> m;
            for (Index p : pts)
                m.push_back(repCorner[vi].mix(mesh.position(p)));
            return m;
        };
        auto face = [&](std::span<const Index> pts) { emit(pts, srcFace, patchMix(pts)); };
        // The whole boundary, profile points included.
        auto ring = [&]() {
            std::vector<Index> all;
            for (size_t k = 0; k < K; ++k)
            {
                all.push_back(h.nodes[k]);
                if (h.sides[k].type == Side::Arc)
                {
                    const std::vector<Index> mid = arcIds(h.sides[k].profile, h.nodes[k]);
                    all.insert(all.end(), mid.begin(), mid.end());
                }
            }
            return all;
        };

        switch (vm.fill)
        {
        case Fill::None:
        case Fill::Merge:
            break;
        case Fill::Polygon:
            face(ring());
            break;
        case Fill::Split:
        {
            std::vector<Index> stack;
            for (Index p : ring())
            {
                auto it = std::find(stack.begin(), stack.end(), p);
                if (it != stack.end())
                {
                    std::vector<Index> sub(it, stack.end());
                    stack.erase(it + 1, stack.end());
                    if (sub.size() >= 3)
                        face(sub);
                    continue;
                }
                stack.push_back(p);
            }
            if (stack.size() >= 3)
                face(stack);
            break;
        }
        case Fill::Fan:
        {
            std::vector<Index> all = ring();
            std::rotate(all.begin(), std::find(all.begin(), all.end(), v.point), all.end());
            // Quads around the kept point, from both ends, with a triangle in the middle when the count is odd.
            size_t lo = 1, hi = all.size() - 1;
            const size_t quads = (hi - lo) / 2;
            for (size_t q = 0; q < (quads + 1) / 2; ++q, lo += 2)
            {
                const Index f[4] = {v.point, all[lo], all[lo + 1], all[lo + 2]};
                face(f);
            }
            for (size_t q = 0; q < quads / 2; ++q, hi -= 2)
            {
                const Index f[4] = {v.point, all[hi - 2], all[hi - 1], all[hi]};
                face(f);
            }
            if (hi - lo == 1)
            {
                const Index f[3] = {v.point, all[lo], all[hi]};
                face(f);
            }
            break;
        }
        case Fill::Grid:
        {
            const int n = segments;
            const int n2 = 2 * n;
            // Each side sampled at half steps: the grid's spokes run from side middles to the centre.
            std::vector<std::vector<glm::dvec3>> half(K, std::vector<glm::dvec3>(size_t(n2) + 1));
            for (size_t k = 0; k < K; ++k)
            {
                const Index a = h.nodes[k], b = h.nodes[(k + 1) % K];
                for (int j = 0; j <= n2; ++j)
                {
                    const double s = double(j) / n2;
                    half[k][size_t(j)] = h.sides[k].type == Side::Arc ? arcAt(h.sides[k].profile, a, s)
                                                                       : glm::mix(mesh.position(a), mesh.position(b), s);
                }
                half[k][0] = mesh.position(a);
                half[k][size_t(n2)] = mesh.position(b);
            }
            auto centre = [&](const std::vector<std::vector<glm::dvec3>>& H) {
                glm::dvec3 corners(0.0), bulge(0.0);
                for (const auto& s : H)
                {
                    corners += s[0];
                    bulge += s[size_t(n)] - (s[0] + s[size_t(n2)]) * 0.5;
                }
                return (corners + gCenter * bulge) / double(K);
            };
            auto coons = [&](const std::vector<std::vector<glm::dvec3>>& H, const glm::dvec3& Z, size_t i, int a, int b) {
                const std::vector<glm::dvec3>& S = H[i];
                const std::vector<glm::dvec3>& P = H[(i + K - 1) % K];
                const double s = double(a) / n, t = double(b) / n;
                const glm::dvec3 p0b = P[size_t(n2 - b)], pnb = glm::mix(S[size_t(n)], Z, t);
                const glm::dvec3 pa0 = S[size_t(a)], pan = glm::mix(P[size_t(n)], Z, s);
                return (1 - s) * p0b + s * pnb + (1 - t) * pa0 + t * pan
                    - ((1 - s) * (1 - t) * S[0] + s * (1 - t) * S[size_t(n)] + (1 - s) * t * P[size_t(n)] + s * t * Z);
            };
            const glm::dvec3 Z = centre(half);

            // A corner of three profiles sharing one centre is the affine image of a superellipsoid octant: snap to it.
            bool ideal = false;
            glm::dvec3 C(0.0);
            glm::dmat3 axes(1.0), inv(1.0);
            std::vector<std::vector<glm::dvec3>> idealHalf;
            glm::dvec3 idealZ(0.0);
            if (K == 3 && std::all_of(h.sides.begin(), h.sides.end(), [](const Side& s) { return s.type == Side::Arc; }))
            {
                for (const Side& s : h.sides)
                    C += profiles[s.profile].linear ? glm::dvec3(0.0) : profiles[s.profile].o;
                C /= 3.0;
                double shortest = 1e300;
                for (int k = 0; k < 3; ++k)
                {
                    axes[k] = mesh.position(h.nodes[size_t(k)]) - C;
                    shortest = std::min(shortest, glm::length(axes[k]));
                }
                double spread = 0.0;
                bool curved = true;
                for (const Side& s : h.sides)
                {
                    curved &= !profiles[s.profile].linear;
                    spread = std::max(spread, glm::distance(profiles[s.profile].o, C));
                }
                const double det = glm::determinant(axes);
                ideal = curved && spread <= 0.25 * shortest
                    && std::abs(det) > 1e-3 * glm::length(axes[0]) * glm::length(axes[1]) * glm::length(axes[2]);
                if (ideal)
                {
                    inv = glm::inverse(axes);
                    idealHalf.assign(3, std::vector<glm::dvec3>(size_t(n2) + 1));
                    for (int k = 0; k < 3; ++k)
                        for (int j = 0; j <= n2; ++j)
                        {
                            const glm::dvec2 xy = superXY(double(j) / n2, r);
                            idealHalf[size_t(k)][size_t(j)] = C + axes[k] * xy.x + axes[(k + 1) % 3] * xy.y;
                        }
                    idealZ = centre(idealHalf);
                }
            }
            auto snap = [&](const glm::dvec3& p) {
                glm::dvec3 q = inv * (p - C);
                const double top = std::max({std::abs(q.x), std::abs(q.y), std::abs(q.z)});
                if (top <= 0.0)
                    return p;
                const double norm = top
                    * std::pow(std::pow(std::abs(q.x) / top, r) + std::pow(std::abs(q.y) / top, r) + std::pow(std::abs(q.z) / top, r),
                               1.0 / r);
                return C + axes * (q / norm);
            };
            // Otherwise the strip surface nearest the whole boundary lends the grid its dome: exact for a junction
            // along one rounded edge, close for corners where nearly in-line edges meet.
            struct Surface
            {
                glm::dvec3 o, u, w, d;
                glm::dmat3 inv;

                /** Profile coordinates of p: x along u, y along w, z along the edge. */
                glm::dvec3 coords(const glm::dvec3& p) const { return inv * (p - o); }
                glm::dvec3 snap(const glm::dvec3& p, double r) const
                {
                    const glm::dvec3 q = coords(p);
                    const double x = std::max(q.x, 0.0), y = std::max(q.y, 0.0), top = std::max(x, y);
                    if (top <= 0.0)
                        return p;
                    const double norm = top * std::pow(std::pow(x / top, r) + std::pow(y / top, r), 1.0 / r);
                    return o + (u * x + w * y) / norm + d * q.z;
                }
            };
            std::optional<Surface> fit;
            std::vector<std::vector<glm::dvec3>> fitHalf;
            glm::dvec3 fitZ(0.0);
            if (!ideal)
            {
                double size = 0.0;
                for (const auto& sd : half)
                    size = std::max(size, glm::distance(sd.front(), sd.back()));
                double best = 0.25 * size;
                for (const Side& sd : h.sides)
                {
                    if (sd.type != Side::Arc || profiles[sd.profile].linear)
                        continue;
                    const Profile& pr = profiles[sd.profile];
                    // The section square to the edge: the same surface, but points move straight onto it.
                    const glm::dvec3 u = pr.u - pr.axis * glm::dot(pr.u, pr.axis), w = pr.w - pr.axis * glm::dot(pr.w, pr.axis);
                    const glm::dmat3 frame(u, w, pr.axis);
                    if (std::abs(glm::determinant(frame)) <= 1e-3 * glm::length(u) * glm::length(w))
                        continue;
                    const Surface surf{pr.o, u, w, pr.axis, glm::inverse(frame)};
                    // The boundary has to stay within the quarter turn the profile sweeps.
                    double err = 0.0;
                    for (const auto& sd2 : half)
                        for (const glm::dvec3& p : sd2)
                        {
                            const glm::dvec3 q = surf.coords(p);
                            err = std::max(err, q.x < -0.02 || q.y < -0.02 ? 1e300 : glm::distance(surf.snap(p, r), p));
                        }
                    if (err < best)
                    {
                        best = err;
                        fit = surf;
                    }
                }
                if (fit)
                {
                    fitHalf = half;
                    for (auto& sd : fitHalf)
                        for (glm::dvec3& p : sd)
                            p = fit->snap(p, r);
                    fitZ = centre(fitHalf);
                }
            }
            auto position = [&](size_t i, int a, int b) {
                glm::dvec3 p = coons(half, Z, i, a, b);
                if (ideal)
                {
                    const glm::dvec3 q = coons(idealHalf, idealZ, i, a, b);
                    p += snap(q) - q;
                }
                else if (fit)
                {
                    const glm::dvec3 q = coons(fitHalf, fitZ, i, a, b);
                    p += fit->snap(q, r) - q;
                }
                return p;
            };

            std::map<std::tuple<size_t, int, int>, Index> made;
            auto newPoint = [&](std::tuple<size_t, int, int> key, size_t i, int a, int b) {
                auto it = made.find(key);
                if (it != made.end())
                    return it->second;
                const Index p = mesh.addPoint(position(i, a, b));
                mesh.pointAttrs().copy(p, v.point);
                made.emplace(key, p);
                return p;
            };
            // Point (a, b) in half steps of sector i: a runs along side i from its node, b back along side i - 1.
            auto G = [&](size_t i, int a, int b) -> Index {
                const size_t prev = (i + K - 1) % K;
                if (b == 0)
                    return vm.sideIds[i][size_t(a / 2)];
                if (a == 0)
                    return vm.sideIds[prev][size_t(n - b / 2)];
                if (a == n && b == n)
                    return newPoint({SIZE_MAX, 0, 0}, i, a, b);
                if (a == n)
                    return newPoint({i, -1, b}, i, a, b);
                if (b == n)
                    return newPoint({prev, -1, a}, i, a, b);
                return newPoint({i, a, b}, i, a, b);
            };
            const int m = n / 2;
            for (size_t i = 0; i < K; ++i)
            {
                for (int q = 0; q < m; ++q)
                    for (int p = 0; p < m; ++p)
                    {
                        const Index f[4] = {G(i, 2 * p, 2 * q), G(i, 2 * p + 2, 2 * q), G(i, 2 * p + 2, 2 * q + 2), G(i, 2 * p, 2 * q + 2)};
                        face(f);
                    }
                if (n % 2 == 1)
                {
                    const size_t next = (i + 1) % K;
                    for (int q = 0; q < m; ++q)
                    {
                        const Index f[4] = {G(i, n - 1, 2 * q), G(next, 2 * q, n - 1), G(next, 2 * q + 2, n - 1), G(i, n - 1, 2 * q + 2)};
                        face(f);
                    }
                }
            }
            if (n % 2 == 1)
            {
                std::vector<Index> middle;
                for (size_t i = 0; i < K; ++i)
                    middle.push_back(G(i, n - 1, n - 1));
                face(middle);
            }
            break;
        }
        }
    }

    out.erase(std::remove_if(out.begin(), out.end(), [&](Index f) { return !mesh.faceValid(f); }), out.end());

    std::vector<Index> oldPoints;
    for (const Vert& v : verts)
        oldPoints.push_back(v.point);
    for (Index p : welded)
        mesh.removePoint(p);
    mesh.removeLoose(oldEdges, oldPoints);

    if (opts.smoothingAngle >= 0.0)
    {
        std::vector<Index> newEdges;
        for (Index e : edgesOfFaces(mesh, out))
            if (e >= edgeCap0)
                newEdges.push_back(e);
        setHardEdgesByAngle(mesh, opts.smoothingAngle, newEdges);
    }
    applySymmetrySnap(mesh, opts.symmetry, firstNewPoint);
    return out;
}
} // namespace lila::geo
