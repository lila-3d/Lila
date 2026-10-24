#include "geo/Conform.h"

#include "geo/Bvh.h"

#include "core/Profile.h"

#include <glm/gtc/constants.hpp>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <optional>

namespace lila::geo
{
namespace
{
using Points = std::vector<glm::dvec3>;
constexpr double kTau = glm::two_pi<double>();

glm::dvec3 centroidOf(std::span<const glm::dvec3> pts)
{
    glm::dvec3 c(0.0);
    for (const glm::dvec3& p : pts)
        c += p;
    return pts.empty() ? c : c / double(pts.size());
}

/** Eigen decomposition of a symmetric 3x3 by cyclic Jacobi rotations; values descending, vectors as columns. */
void symmetricEigen(glm::dmat3 a, glm::dvec3& values, glm::dmat3& vectors)
{
    glm::dmat3 v(1.0);
    for (int sweep = 0; sweep < 64; ++sweep)
    {
        int p = 0, q = 1;
        double big = std::abs(a[1][0]);
        if (std::abs(a[2][0]) > big)
            p = 0, q = 2, big = std::abs(a[2][0]);
        if (std::abs(a[2][1]) > big)
            p = 1, q = 2, big = std::abs(a[2][1]);
        if (big < 1e-14)
            break;
        double theta = (a[q][q] - a[p][p]) / (2.0 * a[q][p]);
        double t = (theta >= 0.0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
        double c = 1.0 / std::sqrt(t * t + 1.0), s = t * c;
        glm::dmat3 r(1.0);
        r[p][p] = c, r[q][q] = c, r[q][p] = s, r[p][q] = -s;
        a = glm::transpose(r) * a * r;
        v = v * r;
    }
    std::array<int, 3> order{0, 1, 2};
    std::sort(order.begin(), order.end(), [&](int i, int j) { return a[i][i] > a[j][j]; });
    for (int k = 0; k < 3; ++k)
    {
        values[k] = a[order[k]][order[k]];
        vectors[k] = v[order[k]];
    }
}

/** Principal axes of the points about their centroid: column 0 the longest spread, column 2 the plane normal. */
glm::dmat3 principalAxes(std::span<const glm::dvec3> pts, glm::dvec3* spread = nullptr)
{
    glm::dvec3 c = centroidOf(pts);
    glm::dmat3 cov(0.0);
    for (const glm::dvec3& p : pts)
        cov += glm::outerProduct(p - c, p - c);
    glm::dvec3 values;
    glm::dmat3 vectors;
    symmetricEigen(cov, values, vectors);
    if (spread)
        *spread = values;
    return vectors;
}

glm::dvec3 anyPerpendicular(const glm::dvec3& n)
{
    glm::dvec3 a = std::abs(n.x) < 0.9 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 1, 0);
    return glm::normalize(glm::cross(n, a));
}

/** An orthonormal frame in a plane through origin; (u, v, n) is right-handed. */
struct PlaneFrame
{
    glm::dvec3 origin{0.0}, u{1, 0, 0}, v{0, 1, 0}, n{0, 0, 1};

    PlaneFrame() = default;
    PlaneFrame(const glm::dvec3& o, const glm::dvec3& normal) : origin(o), n(glm::normalize(normal))
    {
        u = anyPerpendicular(n);
        v = glm::cross(n, u);
    }
    glm::dvec2 flat(const glm::dvec3& p) const { return {glm::dot(p - origin, u), glm::dot(p - origin, v)}; }
    double height(const glm::dvec3& p) const { return glm::dot(p - origin, n); }
    glm::dvec3 lift(const glm::dvec2& q, double h = 0.0) const { return origin + u * q.x + v * q.y + n * h; }
};

/** Newell normal of a closed polygon, unnormalised; zero when it encloses no area. */
glm::dvec3 newell(std::span<const glm::dvec3> pts)
{
    glm::dvec3 c = centroidOf(pts), sum(0.0);
    for (size_t i = 0; i < pts.size(); ++i)
        sum += glm::cross(pts[i] - c, pts[(i + 1) % pts.size()] - c);
    return sum;
}

double extentOf(std::span<const glm::dvec3> pts)
{
    glm::dvec3 lo(std::numeric_limits<double>::max()), hi(-std::numeric_limits<double>::max());
    for (const glm::dvec3& p : pts)
        lo = glm::min(lo, p), hi = glm::max(hi, p);
    return pts.empty() ? 0.0 : glm::length(hi - lo);
}

/** Frame of a loop's plane: its winding normal when it has one, else the points' flattest direction. */
PlaneFrame loopFrame(std::span<const glm::dvec3> pts, bool closed)
{
    glm::dvec3 c = centroidOf(pts);
    glm::dvec3 nw = closed ? newell(pts) : glm::dvec3(0.0);
    double scale = extentOf(pts);
    if (glm::length(nw) > 1e-9 * scale * scale)
        return PlaneFrame(c, nw);
    return PlaneFrame(c, principalAxes(pts)[2]);
}

/** Least-squares (Kasa) circle through 2D points. False when they are near collinear. */
bool fitCircle(std::span<const glm::dvec2> q, glm::dvec2& center, double& radius)
{
    if (q.size() < 3)
        return false;
    glm::dvec2 mean(0.0);
    for (const glm::dvec2& p : q)
        mean += p;
    mean /= double(q.size());
    double s = 0.0;
    for (const glm::dvec2& p : q)
        s += glm::dot(p - mean, p - mean);
    s = std::sqrt(s / double(q.size()));
    if (s < 1e-12)
        return false;
    glm::dmat3 m(0.0);
    glm::dvec3 rhs(0.0);
    for (const glm::dvec2& p : q)
    {
        glm::dvec2 d = (p - mean) / s;
        glm::dvec3 row(d.x, d.y, 1.0);
        m += glm::outerProduct(row, row);
        rhs -= row * glm::dot(d, d);
    }
    const double n = double(q.size());
    if (std::abs(glm::determinant(m)) < 1e-9 * n * n * n)
        return false;
    glm::dvec3 sol = glm::inverse(m) * rhs;
    glm::dvec2 c(-sol.x * 0.5, -sol.y * 0.5);
    double r2 = glm::dot(c, c) - sol.z;
    if (r2 <= 0.0 || glm::length(c) > 1e3)
        return false;
    center = mean + c * s;
    radius = std::sqrt(r2) * s;
    return true;
}

/** Least-squares sphere. False for near coplanar points, whose sphere is undetermined. */
bool fitSphere(std::span<const glm::dvec3> pts, glm::dvec3& center, double& radius)
{
    if (pts.size() < 4)
        return false;
    glm::dvec3 spread;
    principalAxes(pts, &spread);
    if (spread[2] < 1e-8 * spread[0])
        return false;
    glm::dvec3 mean = centroidOf(pts);
    double s = 0.0;
    for (const glm::dvec3& p : pts)
        s += glm::dot(p - mean, p - mean);
    s = std::sqrt(s / double(pts.size()));
    glm::dmat4 m(0.0);
    glm::dvec4 rhs(0.0);
    for (const glm::dvec3& p : pts)
    {
        glm::dvec3 d = (p - mean) / s;
        glm::dvec4 row(d, 1.0);
        m += glm::outerProduct(row, row);
        rhs -= row * glm::dot(d, d);
    }
    if (std::abs(glm::determinant(m)) < 1e-12 * std::pow(double(pts.size()), 4.0))
        return false;
    glm::dvec4 sol = glm::inverse(m) * rhs;
    glm::dvec3 c(-sol.x * 0.5, -sol.y * 0.5, -sol.z * 0.5);
    double r2 = glm::dot(c, c) - sol.w;
    if (r2 <= 0.0 || glm::length(c) > 1e2)
        return false;
    center = mean + c * s;
    radius = std::sqrt(r2) * s;
    return true;
}

Points positionsOf(const Mesh& mesh, std::span<const Index> ids)
{
    Points out;
    out.reserve(ids.size());
    for (Index p : ids)
        out.push_back(mesh.position(p));
    return out;
}

/** Fraction of the way along the chain at each point: by length (keep) or by count (even). Closed chains end short of 1. */
std::vector<double> chainFractions(std::span<const glm::dvec3> pts, bool closed, Spacing spacing)
{
    const size_t n = pts.size();
    std::vector<double> f(n, 0.0);
    const size_t steps = closed ? n : n - 1;
    if (n < 2 || steps == 0)
        return f;
    std::vector<double> cum(n + 1, 0.0);
    for (size_t i = 0; i < steps; ++i)
        cum[i + 1] = cum[i] + glm::length(pts[(i + 1) % n] - pts[i]);
    for (size_t i = 0; i < n; ++i)
        f[i] = spacing == Spacing::Even || cum[steps] <= 1e-12 ? double(i) / double(steps) : cum[i] / cum[steps];
    return f;
}

double wrapAngle(double a)
{
    a = std::fmod(a, kTau);
    return a < 0.0 ? a + kTau : a;
}

/** Start angle that puts points at start + tau * f[i] as near as possible to their angles phi[i]. */
double bestStart(std::span<const double> phi, std::span<const double> f)
{
    double s = 0.0, c = 0.0;
    for (size_t i = 0; i < phi.size(); ++i)
    {
        s += std::sin(phi[i] - kTau * f[i]);
        c += std::cos(phi[i] - kTau * f[i]);
    }
    return std::atan2(s, c);
}

glm::dvec2 onPolygon(double theta, double start, int sides, const glm::dvec2& c, double r)
{
    const double step = kTau / double(sides);
    double rel = wrapAngle(theta - start) / step;
    int k = std::min(int(rel), sides - 1);
    double t = rel - double(k);
    double a0 = start + step * double(k), a1 = a0 + step;
    glm::dvec2 p0 = c + r * glm::dvec2(std::cos(a0), std::sin(a0)), p1 = c + r * glm::dvec2(std::cos(a1), std::sin(a1));
    return glm::mix(p0, p1, t);
}

Points circleOfLoop(const Points& pts, const CircleOptions& o)
{
    const size_t n = pts.size();
    if (n < 3)
        return pts;
    PlaneFrame frame = loopFrame(pts, true);
    std::vector<glm::dvec2> q(n);
    for (size_t i = 0; i < n; ++i)
        q[i] = frame.flat(pts[i]);
    glm::dvec2 c(0.0);
    double fitted = 0.0;
    if (!fitCircle(q, c, fitted))
    {
        for (const glm::dvec2& p : q)
            c += p;
        c /= double(n);
    }
    double r = 0.0;
    std::vector<double> phi(n);
    for (size_t i = 0; i < n; ++i)
    {
        r += glm::length(q[i] - c);
        phi[i] = std::atan2(q[i].y - c.y, q[i].x - c.x);
    }
    r = o.radius > 0.0 ? o.radius : r / double(n);
    std::vector<double> f = chainFractions(pts, true, o.spacing == Spacing::Even ? Spacing::Even : Spacing::Keep);
    const double rot = glm::radians(o.rotation);
    const double start = bestStart(phi, f) + rot;
    Points out(n);
    for (size_t i = 0; i < n; ++i)
    {
        double theta = o.spacing == Spacing::Project ? phi[i] + rot : start + kTau * f[i];
        glm::dvec2 at = o.sides >= 3 ? onPolygon(theta, start, o.sides, c, r) : c + r * glm::dvec2(std::cos(theta), std::sin(theta));
        out[i] = frame.lift(at, o.flatten ? 0.0 : frame.height(pts[i]));
    }
    return out;
}

/** An open chain onto the arc of its best-fit circle between its ends; unchanged when it is straight. */
Points circleOfArc(const Points& pts, const CircleOptions& o)
{
    const size_t n = pts.size();
    if (n < 3)
        return pts;
    PlaneFrame frame(centroidOf(pts), principalAxes(pts)[2]);
    std::vector<glm::dvec2> q(n);
    for (size_t i = 0; i < n; ++i)
        q[i] = frame.flat(pts[i]);
    glm::dvec2 c;
    double fitted;
    if (!fitCircle(q, c, fitted))
        return pts;
    const double r = o.radius > 0.0 ? o.radius : fitted;
    std::vector<double> phi(n);
    phi[0] = std::atan2(q[0].y - c.y, q[0].x - c.x);
    for (size_t i = 1; i < n; ++i)
    {
        double a = std::atan2(q[i].y - c.y, q[i].x - c.x);
        double d = std::remainder(a - phi[i - 1], kTau);
        phi[i] = phi[i - 1] + d;
    }
    std::vector<double> f = chainFractions(pts, false, o.spacing);
    Points out(n);
    for (size_t i = 0; i < n; ++i)
    {
        double theta = o.spacing == Spacing::Project ? phi[i] : phi[0] + (phi[n - 1] - phi[0]) * f[i];
        out[i] = frame.lift(c + r * glm::dvec2(std::cos(theta), std::sin(theta)), o.flatten ? 0.0 : frame.height(pts[i]));
    }
    return out;
}

/** Loose points ordered by their angle about their centroid in their best-fit plane, as a loop. */
std::vector<Index> angleOrder(const Mesh& mesh, std::span<const Index> ids)
{
    Points pts = positionsOf(mesh, ids);
    PlaneFrame frame(centroidOf(pts), principalAxes(pts)[2]);
    std::vector<std::pair<double, Index>> keyed;
    for (size_t i = 0; i < ids.size(); ++i)
    {
        glm::dvec2 q = frame.flat(pts[i]);
        keyed.push_back({std::atan2(q.y, q.x), ids[i]});
    }
    std::sort(keyed.begin(), keyed.end());
    std::vector<Index> out;
    for (auto& [a, p] : keyed)
        out.push_back(p);
    return out;
}

void addMoves(PointMoves& moves, std::span<const Index> ids, const Points& to)
{
    for (size_t i = 0; i < ids.size(); ++i)
        moves.push_back({ids[i], to[i]});
}

Points rectangleOfLoop(const Points& pts, const RectangleOptions& o)
{
    const size_t n = pts.size();
    if (n < 4)
        return pts;
    PlaneFrame frame = loopFrame(pts, true);
    std::vector<glm::dvec2> q(n);
    for (size_t i = 0; i < n; ++i)
        q[i] = frame.flat(pts[i]);

    // The smallest bounding rectangle has a side along a hull edge, and every hull edge is some chord of the loop's own edges' directions.
    auto boundsAt = [&](double a, glm::dvec2& lo, glm::dvec2& hi) {
        glm::dvec2 e1(std::cos(a), std::sin(a)), e2(-e1.y, e1.x);
        lo = glm::dvec2(std::numeric_limits<double>::max()), hi = -lo;
        for (const glm::dvec2& p : q)
        {
            glm::dvec2 l(glm::dot(p, e1), glm::dot(p, e2));
            lo = glm::min(lo, l), hi = glm::max(hi, l);
        }
        return (hi.x - lo.x) * (hi.y - lo.y);
    };
    double bestAngle = 0.0, bestArea = std::numeric_limits<double>::max();
    for (size_t i = 0; i < n; ++i)
    {
        glm::dvec2 d = q[(i + 1) % n] - q[i];
        if (glm::dot(d, d) < 1e-24)
            continue;
        double a = std::fmod(std::atan2(d.y, d.x) + kTau, glm::half_pi<double>());
        glm::dvec2 lo, hi;
        if (double area = boundsAt(a, lo, hi); area < bestArea - 1e-12 * std::abs(area))
            bestArea = area, bestAngle = a;
    }
    glm::dvec2 lo, hi;
    boundsAt(bestAngle, lo, hi);
    glm::dvec2 e1(std::cos(bestAngle), std::sin(bestAngle)), e2(-e1.y, e1.x);
    glm::dvec2 center = e1 * ((lo.x + hi.x) * 0.5) + e2 * ((lo.y + hi.y) * 0.5);
    double hx = (hi.x - lo.x) * 0.5, hy = (hi.y - lo.y) * 0.5;
    if (o.square)
        hx = hy = (hx + hy) * 0.5;
    const double rot = glm::radians(o.rotation);
    glm::dvec2 r1 = e1 * std::cos(rot) + e2 * std::sin(rot), r2(-r1.y, r1.x);
    const std::array<glm::dvec2, 4> corner = {center - r1 * hx - r2 * hy, center + r1 * hx - r2 * hy, center + r1 * hx + r2 * hy,
                                              center - r1 * hx + r2 * hy};

    std::array<size_t, 4> at{};
    for (int k = 0; k < 4; ++k)
    {
        double best = std::numeric_limits<double>::max();
        for (size_t i = 0; i < n; ++i)
            if (double d = glm::length(q[i] - corner[k]); d < best)
                best = d, at[k] = i;
    }
    auto ahead = [&](int k) { return (at[k] + n - at[0]) % n; };
    if (!(ahead(1) > 0 && ahead(1) < ahead(2) && ahead(2) < ahead(3)))
        for (int k = 1; k < 4; ++k)
            at[k] = (at[0] + size_t(std::lround(double(k) * double(n) / 4.0))) % n;

    Points out(n);
    for (int k = 0; k < 4; ++k)
    {
        const size_t from = at[k], count = (at[(k + 1) % 4] + n - from) % n;
        const glm::dvec2 a = corner[k], b = corner[(k + 1) % 4];
        Points side;
        for (size_t i = 0; i <= count; ++i)
            side.push_back(pts[(from + i) % n]);
        std::vector<double> f = chainFractions(side, false, o.spacing == Spacing::Even ? Spacing::Even : Spacing::Keep);
        for (size_t i = 0; i < count; ++i)
        {
            const size_t idx = (from + i) % n;
            double t = f[i];
            if (o.spacing == Spacing::Project && i > 0)
            {
                glm::dvec2 ab = b - a;
                double len2 = glm::dot(ab, ab);
                t = len2 > 1e-24 ? std::clamp(glm::dot(q[idx] - a, ab) / len2, 0.0, 1.0) : 0.0;
            }
            out[idx] = frame.lift(glm::mix(a, b, t), o.flatten ? 0.0 : frame.height(pts[idx]));
        }
    }
    return out;
}

/** A polyline with its cumulative lengths, for walking along a loop. */
struct Polyline
{
    Points p;
    std::vector<double> cum; // cum[i]: length to point i; cum.back(): whole length
    bool closed = false;

    Polyline(Points pts, bool isClosed) : p(std::move(pts)), closed(isClosed)
    {
        const size_t steps = closed ? p.size() : p.size() - 1;
        cum.assign(steps + 1, 0.0);
        for (size_t i = 0; i < steps; ++i)
            cum[i + 1] = cum[i] + glm::length(p[(i + 1) % p.size()] - p[i]);
    }
    double length() const { return cum.back(); }
    size_t segments() const { return cum.size() - 1; }

    /** Nearest point, its arc length and squared distance. */
    glm::dvec3 nearest(const glm::dvec3& x, double* along = nullptr, double* dist2 = nullptr) const
    {
        glm::dvec3 best = p[0];
        double bestD = glm::dot(x - p[0], x - p[0]), bestS = 0.0;
        for (size_t i = 0; i < segments(); ++i)
        {
            const glm::dvec3 a = p[i], b = p[(i + 1) % p.size()], ab = b - a;
            double len2 = glm::dot(ab, ab);
            double t = len2 > 1e-24 ? std::clamp(glm::dot(x - a, ab) / len2, 0.0, 1.0) : 0.0;
            glm::dvec3 at = a + ab * t;
            if (double d = glm::dot(x - at, x - at); d < bestD)
                bestD = d, best = at, bestS = cum[i] + (cum[i + 1] - cum[i]) * t;
        }
        if (along)
            *along = bestS;
        if (dist2)
            *dist2 = bestD;
        return best;
    }

    glm::dvec3 at(double s) const
    {
        const double len = length();
        if (len <= 0.0)
            return p[0];
        s = closed ? std::fmod(std::fmod(s, len) + len, len) : std::clamp(s, 0.0, len);
        size_t i = size_t(std::upper_bound(cum.begin(), cum.end(), s) - cum.begin());
        i = std::clamp<size_t>(i, 1, segments()) - 1;
        double seg = cum[i + 1] - cum[i];
        double t = seg > 0.0 ? (s - cum[i]) / seg : 0.0;
        return glm::mix(p[i], p[(i + 1) % p.size()], t);
    }
};

/** Path through ordered edges, or nothing when they do not join up end to end. */
std::optional<EdgePath> pathOfEdges(const Mesh& mesh, std::span<const Index> edges)
{
    if (edges.size() < 2)
        return std::nullopt;
    const Index e0 = edges[0], e1 = edges[1];
    Index start = mesh.edgePoint(e0, 0);
    if (start == mesh.edgePoint(e1, 0) || start == mesh.edgePoint(e1, 1))
        start = mesh.edgePoint(e0, 1);
    EdgePath path;
    Index p = start;
    for (Index e : edges)
    {
        if (mesh.edgePoint(e, 0) != p && mesh.edgePoint(e, 1) != p)
            return std::nullopt;
        path.points.push_back(p);
        path.edges.push_back(e);
        p = mesh.edgeOtherPoint(e, p);
    }
    path.closed = p == start;
    if (!path.closed)
        path.points.push_back(p);
    return path;
}

std::vector<Index> facesCoveredBy(const Mesh& mesh, const std::vector<uint8_t>& in, std::span<const Index> points)
{
    std::vector<Index> faces;
    for (Index p : points)
        for (Index f : mesh.pointFaces(p))
        {
            bool all = true;
            mesh.forEachFaceCorner(f, [&](Index c) { all = all && in[mesh.cornerPoint(c)]; });
            if (all)
                faces.push_back(f);
        }
    return uniqueSorted(faces);
}

void appendShape(ShapeSelection& to, ShapeSelection&& from)
{
    std::move(from.chains.begin(), from.chains.end(), std::back_inserter(to.chains));
    to.interior.insert(to.interior.end(), from.interior.begin(), from.interior.end());
    to.loose.insert(to.loose.end(), from.loose.begin(), from.loose.end());
}

/** Nearest point on any of the target's edges, for targets without faces. */
glm::dvec3 nearestOnEdges(const Mesh& target, const glm::dvec3& x)
{
    glm::dvec3 best = x;
    double bestD = std::numeric_limits<double>::max();
    for (Index e = 0; e < target.edgeCapacity(); ++e)
    {
        if (!target.edgeValid(e))
            continue;
        const glm::dvec3 a = target.position(target.edgePoint(e, 0)), ab = target.position(target.edgePoint(e, 1)) - a;
        double len2 = glm::dot(ab, ab);
        double t = len2 > 1e-24 ? std::clamp(glm::dot(x - a, ab) / len2, 0.0, 1.0) : 0.0;
        glm::dvec3 at = a + ab * t;
        if (double d = glm::dot(x - at, x - at); d < bestD)
            bestD = d, best = at;
    }
    return best;
}

/** Closed chain points spread onto a loop: one to one when the counts match, else by spacing at the best offset and direction. */
Points ontoLoop(const Points& src, const Polyline& loop, Spacing spacing)
{
    const size_t n = src.size(), m = loop.p.size();
    Points out(n);
    if (spacing == Spacing::Project)
    {
        for (size_t i = 0; i < n; ++i)
            out[i] = loop.nearest(src[i]);
        return out;
    }
    double bestCost = std::numeric_limits<double>::max();
    if (n == m)
    {
        for (bool forward : {true, false})
            for (size_t shift = 0; shift < m; ++shift)
            {
                auto slot = [&](size_t i) { return forward ? (shift + i) % m : (shift + m - i % m) % m; };
                double cost = 0.0;
                for (size_t i = 0; i < n && cost < bestCost; ++i)
                    cost += glm::dot(src[i] - loop.p[slot(i)], src[i] - loop.p[slot(i)]);
                if (cost < bestCost)
                {
                    bestCost = cost;
                    for (size_t i = 0; i < n; ++i)
                        out[i] = loop.p[slot(i)];
                }
            }
        return out;
    }
    const double len = loop.length();
    std::vector<double> f = chainFractions(src, true, spacing);
    std::vector<double> along(n);
    for (size_t i = 0; i < n; ++i)
        loop.nearest(src[i], &along[i]);
    for (double dir : {1.0, -1.0})
    {
        std::vector<double> phi(n), g(n);
        for (size_t i = 0; i < n; ++i)
            phi[i] = kTau * along[i] / len, g[i] = dir * f[i];
        const double start = bestStart(phi, g) / kTau * len;
        Points cand(n);
        double cost = 0.0;
        for (size_t i = 0; i < n; ++i)
        {
            cand[i] = loop.at(start + dir * f[i] * len);
            cost += glm::dot(src[i] - cand[i], src[i] - cand[i]);
        }
        if (cost < bestCost)
            bestCost = cost, out = std::move(cand);
    }
    return out;
}

double meanDistance(const Points& pts, const Polyline& loop)
{
    double sum = 0.0;
    for (const glm::dvec3& p : pts)
    {
        double d2 = 0.0;
        loop.nearest(p, nullptr, &d2);
        sum += std::sqrt(d2);
    }
    return sum / double(std::max<size_t>(1, pts.size()));
}
} // namespace

ShapeSelection shapeOfEdges(const Mesh& mesh, std::span<const Index> edges)
{
    LILA_ZONE();

    ShapeSelection s;
    std::vector<Index> valid;
    for (Index e : edges)
        if (mesh.edgeValid(e))
            valid.push_back(e);
    s.chains = chainEdges(mesh, uniqueSorted(valid));
    return s;
}

ShapeSelection shapeOfFaces(const Mesh& mesh, std::span<const Index> faces)
{
    LILA_ZONE();

    ShapeSelection s;
    std::vector<Index> valid;
    for (Index f : faces)
        if (mesh.faceValid(f))
            valid.push_back(f);
    valid = uniqueSorted(valid);
    s.chains = chainEdges(mesh, regionBoundaryEdges(mesh, valid));
    std::vector<uint8_t> onBorder(mesh.pointCapacity(), 0);
    for (const EdgePath& c : s.chains)
        for (Index p : c.points)
            onBorder[p] = 1;
    for (Index p : pointsOfFaces(mesh, valid))
        if (!onBorder[p])
            s.interior.push_back(p);
    return s;
}

ShapeSelection shapeOfPoints(const Mesh& mesh, std::span<const Index> points)
{
    LILA_ZONE();

    std::vector<Index> valid;
    for (Index p : points)
        if (mesh.pointValid(p))
            valid.push_back(p);
    valid = uniqueSorted(valid);
    std::vector<uint8_t> in = makeMask(mesh.pointCapacity(), valid);

    ShapeSelection s;
    if (std::vector<Index> covered = facesCoveredBy(mesh, in, valid); !covered.empty())
    {
        for (Index p : pointsOfFaces(mesh, covered))
            in[p] = 0;
        appendShape(s, shapeOfFaces(mesh, covered));
    }
    std::vector<Index> rest, joining;
    for (Index p : valid)
        if (in[p])
            rest.push_back(p);
    for (Index p : rest)
        mesh.forEachPointEdge(p, [&](Index e) {
            if (in[mesh.edgeOtherPoint(e, p)])
                joining.push_back(e);
        });
    std::vector<EdgePath> chains = chainEdges(mesh, uniqueSorted(joining));
    for (const EdgePath& c : chains)
        for (Index p : c.points)
            in[p] = 0;
    std::move(chains.begin(), chains.end(), std::back_inserter(s.chains));
    for (Index p : rest)
        if (in[p])
            s.loose.push_back(p);
    return s;
}

std::vector<Index> shapePoints(const ShapeSelection& sel)
{
    std::vector<Index> all(sel.interior.begin(), sel.interior.end());
    all.insert(all.end(), sel.loose.begin(), sel.loose.end());
    for (const EdgePath& c : sel.chains)
        all.insert(all.end(), c.points.begin(), c.points.end());
    return uniqueSorted(all);
}

PointMoves straighten(const Mesh& mesh, const ShapeSelection& sel, const StraightenOptions& opts)
{
    LILA_ZONE();

    PointMoves moves;
    auto onLine = [&](std::span<const Index> ids, const glm::dvec3& a, const glm::dvec3& b, const std::vector<double>& f) {
        const glm::dvec3 ab = b - a;
        const double len2 = glm::dot(ab, ab);
        for (size_t i = 0; i < ids.size(); ++i)
        {
            double t = f[i];
            if (opts.spacing == Spacing::Project)
                t = len2 > 1e-24 ? glm::dot(mesh.position(ids[i]) - a, ab) / len2 : 0.0;
            moves.push_back({ids[i], a + ab * t});
        }
    };
    for (const EdgePath& c : sel.chains)
    {
        if (c.closed || c.points.size() < 3)
            continue;
        Points pts = positionsOf(mesh, c.points);
        glm::dvec3 a = pts.front(), b = pts.back();
        if (opts.fitLine)
        {
            glm::dvec3 o = centroidOf(pts), d = principalAxes(pts)[0];
            a = o + d * glm::dot(a - o, d);
            b = o + d * glm::dot(b - o, d);
        }
        onLine(c.points, a, b, chainFractions(pts, false, opts.spacing));
    }
    if (sel.loose.size() >= 2)
    {
        Points pts = positionsOf(mesh, sel.loose);
        glm::dvec3 o = centroidOf(pts), d = principalAxes(pts)[0];
        std::vector<std::pair<double, Index>> keyed;
        for (size_t i = 0; i < pts.size(); ++i)
            keyed.push_back({glm::dot(pts[i] - o, d), sel.loose[i]});
        std::sort(keyed.begin(), keyed.end());
        std::vector<Index> ids;
        for (auto& [t, p] : keyed)
            ids.push_back(p);
        Points ordered = positionsOf(mesh, ids);
        const glm::dvec3 a = o + d * keyed.front().first, b = o + d * keyed.back().first;
        std::vector<double> f(ids.size());
        for (size_t i = 0; i < ids.size(); ++i)
            f[i] = opts.spacing == Spacing::Even ? double(i) / double(ids.size() - 1)
                                                 : (keyed[i].first - keyed.front().first) / std::max(1e-12, keyed.back().first - keyed.front().first);
        onLine(ids, a, b, f);
    }
    return moves;
}

PointMoves circularize(const Mesh& mesh, const ShapeSelection& sel, const CircleOptions& opts)
{
    LILA_ZONE();

    PointMoves moves;
    for (const EdgePath& c : sel.chains)
    {
        Points pts = positionsOf(mesh, c.points);
        addMoves(moves, c.points, c.closed ? circleOfLoop(pts, opts) : circleOfArc(pts, opts));
    }
    if (sel.loose.size() >= 3)
    {
        std::vector<Index> ids = angleOrder(mesh, sel.loose);
        addMoves(moves, ids, circleOfLoop(positionsOf(mesh, ids), opts));
    }
    return moves;
}

PointMoves rectangularize(const Mesh& mesh, const ShapeSelection& sel, const RectangleOptions& opts)
{
    LILA_ZONE();

    PointMoves moves;
    for (const EdgePath& c : sel.chains)
        if (c.closed)
            addMoves(moves, c.points, rectangleOfLoop(positionsOf(mesh, c.points), opts));
    if (sel.loose.size() >= 4)
    {
        std::vector<Index> ids = angleOrder(mesh, sel.loose);
        addMoves(moves, ids, rectangleOfLoop(positionsOf(mesh, ids), opts));
    }
    return moves;
}

PointMoves spherize(const Mesh& mesh, std::span<const Index> points, SphereFit fit, double radius)
{
    LILA_ZONE();

    Points pts = positionsOf(mesh, points);
    if (pts.size() < 2)
        return {};
    glm::dvec3 c;
    double r = 0.0;
    if (fit != SphereFit::Best || !fitSphere(pts, c, r) || r > 100.0 * extentOf(pts))
        c = centroidOf(pts);
    double mean = 0.0;
    for (const glm::dvec3& p : pts)
        mean += glm::length(p - c);
    r = radius > 0.0 ? radius : mean / double(pts.size());
    PointMoves moves;
    for (size_t i = 0; i < pts.size(); ++i)
        if (glm::dvec3 d = pts[i] - c; glm::length(d) > 1e-12)
            moves.push_back({points[i], c + glm::normalize(d) * r});
    return moves;
}

PointMoves cylinderize(const Mesh& mesh, std::span<const Index> points, int axis, double radius)
{
    LILA_ZONE();

    Points pts = positionsOf(mesh, points);
    if (pts.size() < 3)
        return {};
    std::vector<glm::dvec3> axes;
    if (axis >= 0 && axis <= 2)
    {
        glm::dvec3 a(0.0);
        a[axis] = 1.0;
        axes.push_back(a);
    }
    else
    {
        glm::dmat3 pa = principalAxes(pts);
        axes = {pa[0], pa[1], pa[2]};
    }
    const glm::dvec3 o = centroidOf(pts);
    double bestResidual = std::numeric_limits<double>::max();
    PlaneFrame best;
    glm::dvec2 bestCenter(0.0);
    double bestRadius = 0.0;
    for (const glm::dvec3& a : axes)
    {
        PlaneFrame frame(o, a);
        std::vector<glm::dvec2> q(pts.size());
        for (size_t i = 0; i < pts.size(); ++i)
            q[i] = frame.flat(pts[i]);
        glm::dvec2 c;
        double r;
        if (!fitCircle(q, c, r))
            continue;
        double residual = 0.0, mean = 0.0;
        for (const glm::dvec2& p : q)
        {
            double d = glm::length(p - c);
            residual += (d - r) * (d - r);
            mean += d;
        }
        residual /= r * r;
        if (residual < bestResidual)
            bestResidual = residual, best = frame, bestCenter = c, bestRadius = mean / double(q.size());
    }
    if (bestResidual == std::numeric_limits<double>::max())
        return {};
    const double r = radius > 0.0 ? radius : bestRadius;
    PointMoves moves;
    for (size_t i = 0; i < pts.size(); ++i)
    {
        glm::dvec2 d = best.flat(pts[i]) - bestCenter;
        if (glm::length(d) > 1e-12)
            moves.push_back({points[i], best.lift(bestCenter + glm::normalize(d) * r, best.height(pts[i]))});
    }
    return moves;
}

PointMoves flattenToPlane(const Mesh& mesh, std::span<const Index> points)
{
    LILA_ZONE();

    Points pts = positionsOf(mesh, points);
    if (pts.size() < 3)
        return {};
    const glm::dvec3 o = centroidOf(pts), n = principalAxes(pts)[2];
    PointMoves moves;
    for (size_t i = 0; i < pts.size(); ++i)
        moves.push_back({points[i], pts[i] - n * glm::dot(pts[i] - o, n)});
    return moves;
}

PointMoves alignToLine(const Mesh& mesh, std::span<const Index> points)
{
    LILA_ZONE();

    Points pts = positionsOf(mesh, points);
    if (pts.size() < 2)
        return {};
    const glm::dvec3 o = centroidOf(pts), d = principalAxes(pts)[0];
    PointMoves moves;
    for (size_t i = 0; i < pts.size(); ++i)
        moves.push_back({points[i], o + d * glm::dot(pts[i] - o, d)});
    return moves;
}

std::vector<EdgePath> candidateLoops(const Mesh& target)
{
    LILA_ZONE();

    std::vector<EdgePath> out = boundaryLoops(target);
    std::vector<uint8_t> seen(target.edgeCapacity(), 0);
    for (const EdgePath& b : out)
        for (Index e : b.edges)
            seen[e] = 1;
    std::vector<Index> loose;
    for (Index e = 0; e < target.edgeCapacity(); ++e)
    {
        if (!target.edgeValid(e) || seen[e])
            continue;
        if (target.edgeCorner(e) == kInvalid)
        {
            loose.push_back(e);
            continue;
        }
        std::vector<Index> loop = edgeLoop(target, e);
        for (Index x : loop)
            seen[x] = 1;
        if (std::optional<EdgePath> path = pathOfEdges(target, loop); path && path->closed && path->points.size() >= 3)
            out.push_back(std::move(*path));
    }
    for (Index f = 0; f < target.faceCapacity(); ++f)
        if (target.faceValid(f) && target.faceSize(f) >= 5)
        {
            EdgePath path;
            path.points = target.facePoints(f);
            path.closed = true;
            out.push_back(std::move(path));
        }
    for (EdgePath& c : chainEdges(target, loose))
        if (c.closed && c.points.size() >= 3)
            out.push_back(std::move(c));
    return out;
}

PointMoves conformToMesh(const Mesh& mesh, const ShapeSelection& sel, const Mesh& target, const ConformOptions& opts)
{
    LILA_ZONE();

    const bool hasFaces = target.numFaces() > 0;
    MeshBvh bvh;
    if (hasFaces)
        bvh.build(target);
    auto nearestSurface = [&](const glm::dvec3& x) {
        if (!hasFaces)
            return nearestOnEdges(target, x);
        BvhClosest c;
        return bvh.closestPoint(x, c) ? c.point : x;
    };

    PointMoves moves;
    std::vector<Index> all = shapePoints(sel);
    if (opts.mode == ConformMode::Vertex)
    {
        std::vector<Index> targetPoints;
        for (Index p = 0; p < target.pointCapacity(); ++p)
            if (target.pointValid(p))
                targetPoints.push_back(p);
        if (targetPoints.empty())
            return moves;
        moves.resize(all.size());
        tbb::parallel_for(size_t(0), all.size(), [&](size_t i) {
            const glm::dvec3 x = mesh.position(all[i]);
            glm::dvec3 best = x;
            double bestD = std::numeric_limits<double>::max();
            for (Index t : targetPoints)
                if (double d = glm::dot(target.position(t) - x, target.position(t) - x); d < bestD)
                    bestD = d, best = target.position(t);
            moves[i] = {all[i], best};
        });
        return moves;
    }

    std::vector<Polyline> loops;
    if (opts.mode == ConformMode::Loop)
        for (const EdgePath& c : candidateLoops(target))
            loops.emplace_back(positionsOf(target, c.points), true);
    if (opts.mode == ConformMode::Surface || loops.empty())
    {
        moves.resize(all.size());
        tbb::parallel_for(size_t(0), all.size(), [&](size_t i) { moves[i] = {all[i], nearestSurface(mesh.position(all[i]))}; });
        return moves;
    }

    std::vector<uint8_t> done(mesh.pointCapacity(), 0);
    for (const EdgePath& c : sel.chains)
    {
        Points pts = positionsOf(mesh, c.points);
        const Polyline* best = nullptr;
        double bestD = std::numeric_limits<double>::max();
        for (const Polyline& l : loops)
            // Equal counts break ties, so a ring picks the target ring it can land on one to one.
            if (double d = meanDistance(pts, l) * (l.p.size() == pts.size() ? 1.0 : 1.0 + 1e-6); d < bestD)
                bestD = d, best = &l;
        Points to = c.closed ? ontoLoop(pts, *best, opts.spacing) : Points{};
        if (!c.closed)
            for (const glm::dvec3& p : pts)
                to.push_back(best->nearest(p));
        for (size_t i = 0; i < c.points.size(); ++i)
            if (!done[c.points[i]])
            {
                done[c.points[i]] = 1;
                moves.push_back({c.points[i], to[i]});
            }
    }
    for (Index p : sel.loose)
        if (!done[p])
            moves.push_back({p, nearestSurface(mesh.position(p))});
    return moves;
}

void followInterior(const Mesh& mesh, std::span<const Index> interior, PointMoves& moves)
{
    LILA_ZONE();

    if (interior.empty() || moves.empty())
        return;
    enum : uint8_t
    {
        Out,
        Fixed,
        Free
    };
    std::vector<uint8_t> state(mesh.pointCapacity(), Out);
    std::vector<glm::dvec3> d(mesh.pointCapacity(), glm::dvec3(0.0));
    for (const PointMove& m : moves)
    {
        state[m.point] = Fixed;
        d[m.point] = m.position - mesh.position(m.point);
    }
    std::vector<Index> free;
    for (Index p : interior)
        if (mesh.pointValid(p) && state[p] == Out)
        {
            state[p] = Free;
            free.push_back(p);
        }
    if (free.empty())
        return;

    // Start from an inverse-distance blend so the iterations only have to smooth, not travel.
    const size_t stride = std::max<size_t>(1, moves.size() / 256);
    tbb::parallel_for(size_t(0), free.size(), [&](size_t i) {
        const glm::dvec3 x = mesh.position(free[i]);
        glm::dvec3 sum(0.0);
        double wsum = 0.0;
        for (size_t k = 0; k < moves.size(); k += stride)
        {
            const glm::dvec3 diff = mesh.position(moves[k].point) - x;
            const double w = 1.0 / std::max(1e-12, glm::dot(diff, diff));
            sum += d[moves[k].point] * w;
            wsum += w;
        }
        d[free[i]] = sum / wsum;
    });

    const double tol = std::max(1e-12, extentOf(positionsOf(mesh, free)) * 1e-9);
    const int iterations = int(std::clamp<size_t>(size_t(2e7) / free.size(), 50, 5000));
    for (int it = 0; it < iterations; ++it)
    {
        double change = 0.0;
        for (Index p : free)
        {
            glm::dvec3 sum(0.0);
            int count = 0;
            mesh.forEachPointEdge(p, [&](Index e) {
                Index q = mesh.edgeOtherPoint(e, p);
                if (state[q] != Out)
                {
                    sum += d[q];
                    ++count;
                }
            });
            if (count == 0)
                continue;
            glm::dvec3 next = sum / double(count);
            change = std::max(change, glm::length(next - d[p]));
            d[p] = next;
        }
        if (change < tol)
            break;
    }
    for (Index p : free)
        moves.push_back({p, mesh.position(p) + d[p]});
}
} // namespace lila::geo
