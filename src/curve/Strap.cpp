#include "curve/Strap.h"

#include "core/Profile.h"
#include "curve/CurveBoolean.h"
#include "curve/Fit.h"
#include "curve/Tessellate.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtx/norm.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace lila::curve
{
namespace strapimpl
{
namespace
{
using D3 = glm::dvec3;
using D2 = glm::dvec2;

double newellAlong(const std::vector<D3>& pts, const D3& n)
{
    D3 a(0.0);
    for (size_t i = 0; i < pts.size(); ++i)
        a += glm::cross(pts[i], pts[(i + 1) % pts.size()]);
    return glm::dot(a, n);
}

void dropRepeats(std::vector<D3>& pts, bool closed)
{
    std::vector<D3> out;
    for (const D3& p : pts)
        if (out.empty() || glm::distance(out.back(), p) > 1e-7)
            out.push_back(p);
    while (closed && out.size() > 1 && glm::distance(out.front(), out.back()) <= 1e-7)
        out.pop_back();
    pts = std::move(out);
}

Spline polySpline(std::vector<D3> pts, bool closed)
{
    Spline s;
    s.kind = Kind::Poly;
    s.closed = closed;
    s.points = std::move(pts);
    s.normalize();
    return s;
}

struct PlaneBasis
{
    D3 origin, u, v, n;
    D2 to2(const D3& p) const { return {glm::dot(p - origin, u), glm::dot(p - origin, v)}; }
    D3 to3(const D2& p) const { return origin + u * p.x + v * p.y; }
};

PlaneBasis basisOf(const D3& origin, const D3& n)
{
    D3 ref = std::abs(n.x) < 0.9 ? D3(1, 0, 0) : D3(0, 1, 0);
    D3 u = glm::normalize(glm::cross(n, ref));
    return {origin, u, glm::cross(n, u), n};
}

double cross2(const D2& a, const D2& b) { return a.x * b.y - a.y * b.x; }

/** Andrew's monotone chain, counter-clockwise. */
std::vector<D2> hull(std::vector<D2> p)
{
    std::ranges::sort(p, [](const D2& a, const D2& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    if (p.size() < 3)
        return p;
    std::vector<D2> h(2 * p.size());
    size_t k = 0;
    for (size_t i = 0; i < p.size(); ++i)
    {
        while (k >= 2 && cross2(h[k - 1] - h[k - 2], p[i] - h[k - 2]) <= 0.0)
            --k;
        h[k++] = p[i];
    }
    for (size_t i = p.size() - 1, t = k + 1; i > 0; --i)
    {
        while (k >= t && cross2(h[k - 1] - h[k - 2], p[i - 1] - h[k - 2]) <= 0.0)
            --k;
        h[k++] = p[i - 1];
    }
    h.resize(k - 1);
    return h;
}

/** p is on the inside of the line a to b (left of it, about n), or on it. */
bool insideOf(const D3& a, const D3& b, const D3& p, const D3& n) { return glm::dot(glm::cross(b - a, p - a), n) >= -1e-9 * (1.0 + glm::length2(b - a)); }

/**
 * A strap pulled round a counter-clockwise loop: from a point it runs straight to the furthest point ahead it reaches in a
 * line no longer than span without cutting into the loop, so hollows narrower than span are bridged straight. It starts on an
 * extreme point, which any strap touches.
 */
std::vector<D3> spanHollows(const std::vector<D3>& loop, const D3& n, double span)
{
    const size_t m = loop.size();
    if (m < 4)
        return loop;
    const D3 u = glm::abs(n.x) < 0.9 ? glm::normalize(glm::cross(n, D3(1, 0, 0))) : glm::normalize(glm::cross(n, D3(0, 1, 0)));
    size_t first = 0;
    for (size_t k = 1; k < m; ++k)
        if (glm::dot(loop[k], u) > glm::dot(loop[first], u))
            first = k;
    // Bridges shorter than span can still skip a long way round a deep, narrow pocket; this bounds the search.
    const double window = span * 8.0;
    std::vector<D3> out;
    for (size_t at = 0; at < m;)
    {
        const D3& a = loop[(first + at) % m];
        out.push_back(a);
        size_t best = 1;
        double arc = glm::distance(a, loop[(first + at + 1) % m]);
        for (size_t step = 2; at + step <= m && step < m && arc <= window; ++step)
        {
            const D3& b = loop[(first + at + step) % m];
            arc += glm::distance(loop[(first + at + step - 1) % m], b);
            if (glm::distance(a, b) > span)
                continue;
            bool clear = true;
            for (size_t k = 1; k < step && clear; ++k)
                clear = insideOf(a, b, loop[(first + at + k) % m], n);
            if (clear)
                best = step;
        }
        at += best;
    }
    return out;
}

/**
 * Points moved out by distance square to their edges (tangent x up is out), mitred at corners so straight runs stay
 * distance off. Very sharp corners are held to three times the distance.
 */
std::vector<D3> offsetOut(const std::vector<D3>& p, bool closed, const D3& up, double distance)
{
    const size_t m = p.size();
    std::vector<D3> out = p;
    auto edgeOut = [&](size_t i, size_t j) {
        D3 o = glm::cross(p[j] - p[i], up);
        return glm::length(o) > 1e-12 ? glm::normalize(o) : D3(0.0);
    };
    for (size_t i = 0; i < m; ++i)
    {
        const bool hasIn = closed || i > 0, hasOut = closed || i + 1 < m;
        const D3 nIn = hasIn ? edgeOut((i + m - 1) % m, i) : D3(0.0);
        const D3 nOut = hasOut ? edgeOut(i, (i + 1) % m) : D3(0.0);
        D3 dir = nIn + nOut;
        if (glm::length(dir) < 1e-9)
            dir = hasIn ? nIn : nOut;
        if (glm::length(dir) < 1e-12)
            continue;
        dir = glm::normalize(dir);
        const double c = std::max(hasIn ? glm::dot(dir, nIn) : glm::dot(dir, nOut), 1.0 / 3.0);
        out[i] += dir * (distance / c);
    }
    return out;
}

bool insidePolygon(const std::vector<D2>& poly, const D2& p)
{
    bool in = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
        if ((poly[i].y > p.y) != (poly[j].y > p.y) && p.x < (poly[j].x - poly[i].x) * (p.y - poly[i].y) / (poly[j].y - poly[i].y) + poly[i].x)
            in = !in;
    return in;
}

bool overlaps(const std::vector<D2>& a, const std::vector<D2>& b)
{
    return std::ranges::any_of(a, [&](const D2& p) { return insidePolygon(b, p); }) || std::ranges::any_of(b, [&](const D2& p) { return insidePolygon(a, p); });
}

/**
 * The loops widened to what the strap spans across its width: cuts in planes either side of the slash's, those touching the
 * loops flattened onto its plane and joined with them, so the strap rides over whatever rises under its edges.
 */
Splines widthEnvelope(std::span<const geo::SectionMesh> meshes, const D3& origin, const D3& n, const Splines& loops, double width)
{
    // Planes no more than half a cm apart across the width, so a ridge between two of them can't poke through.
    const int kSteps = std::clamp(int(std::ceil(width * 0.5 / 0.5)), 3, 12);
    const PlaneBasis pb = basisOf(origin, n);
    std::vector<std::vector<D2>> flat;
    for (const Spline& s : loops)
    {
        flat.emplace_back();
        for (const D3& p : s.points)
            flat.back().push_back(pb.to2(p));
    }
    std::vector<Splines> operands;
    for (const Spline& s : loops)
        operands.push_back({s});
    const size_t mainCount = flat.size();
    for (int side : {-1, 1})
        for (int k = 1; k <= kSteps; ++k)
        {
            const double d = side * width * 0.5 * k / kSteps;
            for (geo::SectionChain& c : geo::sectionChains(meshes, origin + n * d, n))
            {
                dropRepeats(c.points, c.closed);
                if (!c.closed || c.points.size() < 3)
                    continue;
                std::vector<D2> f;
                for (const D3& p : c.points)
                    f.push_back(pb.to2(p));
                if (!std::any_of(flat.begin(), flat.begin() + std::ptrdiff_t(mainCount), [&](const std::vector<D2>& m) { return overlaps(m, f); }))
                    continue;
                std::vector<D3> onPlane;
                for (const D2& q : f)
                    onPlane.push_back(pb.to3(q));
                if (newellAlong(onPlane, n) < 0.0)
                    std::ranges::reverse(onPlane);
                operands.push_back({polySpline(std::move(onPlane), true)});
            }
        }
    if (operands.size() == loops.size())
        return loops;
    CurveBooleanOptions bo;
    bo.op = BooleanOp::Union;
    bo.normal = n;
    Splines out;
    for (const Spline& s : curveBoolean(operands, bo).curves)
    {
        if (!s.closed)
            continue;
        std::vector<D3> pts = tessellate(s).points;
        dropRepeats(pts, true);
        if (pts.size() >= 3 && newellAlong(pts, n) > 0.0)
            out.push_back(polySpline(std::move(pts), true));
    }
    return out.empty() ? loops : out;
}

/** Long runs split so no piece is longer than step; the points already there, corners among them, stay. */
std::vector<D3> subdivide(const std::vector<D3>& p, bool closed, double step)
{
    std::vector<D3> out;
    const size_t segs = closed ? p.size() : p.size() - 1;
    for (size_t i = 0; i < segs; ++i)
    {
        const D3& a = p[i];
        const D3& b = p[(i + 1) % p.size()];
        const int pieces = std::max(1, int(std::ceil(glm::distance(a, b) / step)));
        for (int k = 0; k < pieces; ++k)
            out.push_back(glm::mix(a, b, double(k) / pieces));
    }
    if (!closed)
        out.push_back(p.back());
    return out;
}

/** The unit normal of the face nearest p (by centroid), world space; zero with no faces. */
D3 surfaceNormalNear(std::span<const geo::SectionMesh> meshes, const D3& p)
{
    D3 best(0.0);
    double bestDist = std::numeric_limits<double>::max();
    for (const geo::SectionMesh& sm : meshes)
    {
        const glm::dmat4 toLocal = glm::inverse(sm.world);
        const D3 local = D3(toLocal * glm::dvec4(p, 1.0));
        const glm::dmat3 normalToWorld = glm::transpose(glm::dmat3(toLocal));
        for (geo::Index f = 0; f < sm.mesh->faceCapacity(); ++f)
            if (sm.mesh->faceValid(f))
                if (const double d = glm::distance2(sm.mesh->faceCentroid(f), local); d < bestDist)
                {
                    bestDist = d;
                    const D3 nw = normalToWorld * sm.mesh->faceAreaVector(f);
                    best = glm::length(nw) > 1e-12 ? glm::normalize(nw) : D3(0.0);
                }
    }
    return best;
}

/** Outward in-plane direction at each point of an open run: tangent x up. */
std::vector<D3> sideNormals(const std::vector<D3>& p, const D3& up)
{
    std::vector<D3> out(p.size(), D3(0.0));
    for (size_t i = 0; i < p.size(); ++i)
    {
        D3 t = p[std::min(i + 1, p.size() - 1)] - p[i > 0 ? i - 1 : 0];
        D3 s = glm::cross(t, up);
        out[i] = glm::length(s) > 1e-12 ? glm::normalize(s) : D3(0.0);
    }
    return out;
}

/** Corner points of the band's cross-section, x across the width, y outward. */
Profile bandProfile(double width, double thickness, double bevel)
{
    const double w = width * 0.5, h = thickness * 0.5;
    const double c = std::min({bevel, w * 0.9, h * 0.9});
    if (c <= 1e-6)
        return rectProfile(width, thickness);
    Profile p;
    p.points = {D3(-w + c, -h, 0), D3(w - c, -h, 0), D3(w, -h + c, 0), D3(w, h - c, 0),
                D3(w - c, h, 0),   D3(-w + c, h, 0), D3(-w, h - c, 0), D3(-w, -h + c, 0)};
    p.hard.assign(p.points.size(), 1);
    return p;
}

void tagFaces(geo::Mesh& m, StrapPart part)
{
    auto v = m.faceAttrs().getOrAdd<int32_t>("strapPart");
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
            v[f] = int32_t(part);
}
} // namespace
} // namespace strapimpl

using D3 = glm::dvec3;

D3 StrapSlash::normal() const
{
    // Points on both rays at the same depth: with a ray's direction they span the plane, perspective or orthographic.
    const D3 a = start + glm::normalize(startDir) * 100.0, b = end + glm::normalize(endDir) * 100.0;
    D3 n = glm::cross(b - a, startDir);
    return glm::length(n) > 1e-12 ? glm::normalize(n) : D3(0.0);
}

bool StrapSlash::between(const D3& p) const
{
    const D3 n = normal();
    const double s1 = glm::dot(glm::cross(startDir, p - start), n);
    const double s2 = glm::dot(glm::cross(endDir, p - end), n);
    if (s1 * s2 > 0.0)
        return false;
    const D3 d1 = glm::normalize(startDir), d2 = glm::normalize(endDir);
    const D3 w = start - end;
    const double b = glm::dot(d1, d2), den = 1.0 - b * b;
    if (den < 1e-9)
        return true;
    // Converging rays meet at the eye; the mirrored wedge behind it is not what was slashed.
    const double t1 = (b * glm::dot(d2, w) - glm::dot(d1, w)) / den;
    const D3 eye = start + d1 * t1;
    return glm::dot(p - eye, d1 + d2) > 0.0;
}

StrapPathResult strapPath(std::span<const geo::SectionMesh> meshes, const StrapSlash& slash, const StrapPathOptions& o)
{
    LILA_ZONE();

    StrapPathResult r;
    const D3 n = slash.normal();
    if (glm::length(n) < 0.5)
    {
        r.warning = "the slash has no length";
        return r;
    }
    r.up = n;
    std::vector<geo::SectionChain> chains = geo::sectionChains(meshes, slash.start, n);
    Splines loops;
    std::vector<D3> open;
    double openLength = 0.0;
    for (geo::SectionChain& c : chains)
    {
        if (!std::ranges::any_of(c.points, [&](const D3& p) { return slash.between(p); }))
            continue;
        strapimpl::dropRepeats(c.points, c.closed);
        if (c.closed && c.points.size() >= 3)
        {
            if (strapimpl::newellAlong(c.points, n) < 0.0)
                std::ranges::reverse(c.points);
            loops.push_back(strapimpl::polySpline(std::move(c.points), true));
        }
        else if (!c.closed && c.points.size() >= 2)
        {
            double len = 0.0;
            for (size_t i = 1; i < c.points.size(); ++i)
                len += glm::distance(c.points[i - 1], c.points[i]);
            if (len > openLength)
            {
                openLength = len;
                open = std::move(c.points);
            }
        }
    }
    r.loops = int(loops.size());
    if (!loops.empty() && o.width > 0.0)
        loops = strapimpl::widthEnvelope(meshes, slash.start, n, loops, o.width);

    std::vector<D3> pts;
    bool closed = true;
    if (!loops.empty())
    {
        const strapimpl::PlaneBasis pb = strapimpl::basisOf(slash.start, n);
        auto hullLoop = [&] {
            std::vector<strapimpl::D2> flat;
            for (const Spline& s : loops)
                for (const D3& p : s.points)
                    flat.push_back(pb.to2(p));
            std::vector<D3> out;
            for (const strapimpl::D2& p : strapimpl::hull(std::move(flat)))
                out.push_back(pb.to3(p));
            return out;
        };
        if (loops.size() > 1 || o.taut)
            pts = hullLoop();
        else if (o.bridge > 0.0)
            pts = strapimpl::spanHollows(loops.front().points, n, o.bridge * 2.0);
        else
            pts = loops.front().points;
    }
    else if (!open.empty())
    {
        closed = false;
        // The outside of an open run is the side its faces point to, or the side facing the eye when they're edge on.
        const D3 toEye = -glm::normalize(slash.startDir + slash.endDir);
        std::vector<D3> side = strapimpl::sideNormals(open, n);
        double facing = 0.0, eyeward = 0.0;
        for (size_t i = 0; i < open.size(); ++i)
        {
            facing += glm::dot(side[i], strapimpl::surfaceNormalNear(meshes, open[i]));
            eyeward += glm::dot(side[i], toEye);
        }
        if ((std::abs(facing) > 1e-6 ? facing : eyeward) < 0.0)
            std::ranges::reverse(open);
        pts = std::move(open);
        if (o.bridge > 0.0 || o.taut)
            r.warning = "bridge and taut need a closed loop; the open cut is followed as it is";
    }
    else
    {
        r.warning = chains.empty() ? "the slash misses the meshes" : "nothing between the slash's ends";
        return r;
    }

    strapimpl::dropRepeats(pts, closed);
    if (o.tolerance > 0.0)
    {
        std::vector<D3> kept;
        for (size_t i : simplifyIndices(pts, o.tolerance, closed))
            kept.push_back(pts[i]);
        pts = std::move(kept);
    }
    if (pts.size() < (closed ? 3u : 2u))
    {
        r.warning = "the cut is too small for a strap";
        return r;
    }
    if (closed && strapimpl::newellAlong(pts, n) < 0.0)
        std::ranges::reverse(pts);
    // Simplifying cuts chords up to the tolerance into curved surfaces; lifting by it as well keeps the strap outside.
    if (const double lift = std::max(0.0, o.gap) + std::max(0.0, o.tolerance); lift > 0.0)
        pts = strapimpl::offsetOut(pts, closed, n, lift);
    r.path = strapimpl::polySpline(std::move(pts), closed);
    return r;
}

StrapResult makeStrap(const Spline& path, const StrapOptions& o)
{
    LILA_ZONE();

    StrapResult r;
    Polyline pl = tessellate(path);
    if (pl.size() < 2)
    {
        r.warnings.push_back("the path is too short");
        return r;
    }
    D3 up = o.up;
    if (pl.closed)
    {
        D3 a(0.0);
        for (size_t i = 0; i < pl.size(); ++i)
            a += glm::cross(pl.points[i], pl.points[(i + 1) % pl.size()]);
        up = a;
    }
    else if (glm::length(up) < 1e-12)
        up = planeNormal(pl.points, 1e-3);
    if (glm::length(up) < 1e-12)
    {
        up = D3(0, 1, 0);
        r.warnings.push_back("the path has no plane; its outside is a guess");
    }
    up = glm::normalize(o.flip ? -up : up);

    const bool rope = o.style == StrapStyle::Rope;
    const double width = std::max(o.width, 1e-3);
    const double thickness = rope ? width : std::max(o.thickness, 1e-4);
    FrameOptions fo;
    fo.mode = FrameMode::FixedUp;
    fo.up = up;

    // Profiles are centred on the path; moving it out by half the thickness puts the underside on the surface.
    std::vector<D3> lifted = strapimpl::offsetOut(pl.points, pl.closed, up, thickness * 0.5);
    // Rope twists between rings, so a long straight run needs rings along it or the strands wring flat.
    if (rope)
        lifted = strapimpl::subdivide(lifted, pl.closed, std::max(std::min(o.lay / 12.0, width * 0.5), 0.05));
    Polyline centre = polylineFrom(lifted, pl.closed);
    Path p = makePath(centre, frames(centre, fo));
    r.length = p.length;

    SweepOptions so;
    so.vPerCm = o.vPerCm;
    if (!pl.closed)
        so.startCap = so.endCap = CapMode::Ngon;

    if (rope)
    {
        const int strands = std::clamp(o.strands, 1, 12);
        const double R = width * 0.5;
        // Touching strands round the centre fill the rope's width; a little overlap hides the seam between them.
        const double rs = strands == 1 ? R : R / (1.0 + 1.0 / std::sin(glm::pi<double>() / strands)) * 1.1;
        const double d = strands == 1 ? 0.0 : R - rs;
        double turns = p.length / std::max(o.lay, 0.1);
        if (p.closed)
            turns = std::max(1.0, std::round(turns));
        const double rate = glm::two_pi<double>() * turns / std::max(p.length, 1e-9);
        so.twist = [rate](double s) { return s * rate; };
        for (int k = 0; k < strands; ++k)
        {
            const double a = glm::two_pi<double>() * k / strands;
            Profile prof = circleProfile(std::clamp(o.strandSides, 3, 64), rs);
            for (D3& q : prof.points)
                q += D3(std::cos(a) * d, std::sin(a) * d, 0.0);
            r.mesh.append(sweep(prof, p, so).mesh);
        }
    }
    else
        r.mesh = sweep(strapimpl::bandProfile(width, thickness, o.bevel), p, so).mesh;
    strapimpl::tagFaces(r.mesh, StrapPart::Body);

    if (o.buckle && o.buckleLength > 0.0)
    {
        const double len = std::min(o.buckleLength, p.length * 0.5);
        double s0 = std::clamp(o.buckleAt, 0.0, 1.0) * p.length;
        if (!p.closed)
            s0 = std::clamp(s0, len * 0.5, p.length - len * 0.5);
        double frac = 0.0;
        const size_t seg = centre.segmentAt(s0, &frac);
        const D3 at = centre.pointAtLength(s0);
        const D3 t = glm::normalize(centre.tangent(seg) * (1.0 - frac) + centre.tangent((seg + 1) % centre.size()) * frac);
        const double scale = std::max(o.buckleScale, 1.0);
        // The buckle's underside sits level with the strap's, so it stands out from the surface rather than into it.
        const double height = rope ? width * scale : thickness + 2.0 * std::max(thickness, width * 0.08);
        const D3 out = glm::normalize(glm::cross(t, up));
        const D3 mid = at + out * ((height - thickness) * 0.5);
        std::vector<D3> ends = {mid - t * (len * 0.5), mid + t * (len * 0.5)};
        Polyline bl = polylineFrom(ends, false);
        Path bp = makePath(bl, frames(bl, fo));
        SweepOptions bo;
        bo.startCap = bo.endCap = CapMode::Ngon;
        bo.vPerCm = o.vPerCm;
        geo::Mesh block = sweep(rope ? circleProfile(std::clamp(o.strandSides * 2, 6, 64), width * 0.5 * scale) : strapimpl::bandProfile(width * scale, height, o.bevel), bp, bo).mesh;
        strapimpl::tagFaces(block, StrapPart::Buckle);
        r.mesh.append(block);
    }
    return r;
}
} // namespace lila::curve
