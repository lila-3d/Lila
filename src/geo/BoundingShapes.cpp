#include "geo/BoundingShapes.h"

#include "core/Profile.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>
#include <utility>

namespace lila::geo
{
namespace
{
using Points = std::vector<glm::dvec3>;

std::optional<glm::dmat3> frameFromNormal(const glm::dvec3& normal, const glm::dvec3& hint)
{
    double len = glm::length(normal);
    if (len < 1e-30)
        return std::nullopt;
    glm::dvec3 w = normal / len;
    glm::dvec3 u = hint - glm::dot(hint, w) * w;
    if (glm::length(u) < 1e-30)
        u = glm::cross(w, std::abs(w.x) < 0.9 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 1, 0));
    u = glm::normalize(u);
    return glm::dmat3(u, glm::cross(w, u), w);
}

double span(std::span<const glm::dvec3> points)
{
    glm::dvec3 lo = points[0], hi = points[0];
    for (const glm::dvec3& p : points)
        lo = glm::min(lo, p), hi = glm::max(hi, p);
    glm::dvec3 d = hi - lo;
    return std::max({d.x, d.y, d.z});
}

/** Indices of the points extreme along evenly spread (Fibonacci) directions. */
std::set<size_t> extremeIndices(std::span<const glm::dvec3> points, int directions)
{
    std::set<size_t> keep;
    const double golden = glm::pi<double>() * (3.0 - std::sqrt(5.0));
    for (int k = 0; k < directions; ++k)
    {
        double y = 1.0 - (2.0 * k + 1.0) / directions;
        double r = std::sqrt(std::max(0.0, 1.0 - y * y));
        glm::dvec3 d(std::cos(golden * k) * r, y, std::sin(golden * k) * r);
        size_t lo = 0, hi = 0;
        double vlo = glm::dot(points[0], d), vhi = vlo;
        for (size_t i = 1; i < points.size(); ++i)
        {
            double v = glm::dot(points[i], d);
            if (v < vlo)
                vlo = v, lo = i;
            else if (v > vhi)
                vhi = v, hi = i;
        }
        keep.insert(lo);
        keep.insert(hi);
    }
    return keep;
}

Points extremeSubset(std::span<const glm::dvec3> points, int directions)
{
    Points out;
    for (size_t i : extremeIndices(points, directions))
        out.push_back(points[i]);
    return out;
}

/** Points the orientation search runs on; large clouds are cut down to their extremes. */
Points searchSet(std::span<const glm::dvec3> points)
{
    if (points.size() <= 6000)
        return {points.begin(), points.end()};
    return extremeSubset(points, 160);
}

using Tri = std::array<size_t, 3>;

/** Incremental 3D hull: outward triangles indexing `points`, or empty when the cloud is flat or a line. */
std::vector<Tri> convexHull(const Points& points)
{
    LILA_ZONE();

    const size_t n = points.size();
    if (n < 4)
        return {};
    const double eps = std::max(span(points), 1.0) * 1e-10;

    size_t i0 = 0, i1 = 0;
    for (size_t i = 0; i < n; ++i)
    {
        if (points[i].x < points[i0].x)
            i0 = i;
        if (points[i].x > points[i1].x)
            i1 = i;
    }
    glm::dvec3 base = points[i1] - points[i0];
    if (glm::length(base) < eps)
        return {};
    size_t i2 = SIZE_MAX;
    double best = eps;
    for (size_t i = 0; i < n; ++i)
        if (double d = glm::length(glm::cross(base, points[i] - points[i0])); d > best)
            best = d, i2 = i;
    if (i2 == SIZE_MAX)
        return {};
    glm::dvec3 normal = glm::cross(base, points[i2] - points[i0]);
    size_t i3 = SIZE_MAX;
    best = eps;
    for (size_t i = 0; i < n; ++i)
        if (double d = std::abs(glm::dot(normal, points[i] - points[i0])); d > best)
            best = d, i3 = i;
    if (i3 == SIZE_MAX)
        return {};

    const std::array<size_t, 4> seed{i0, i1, i2, i3};
    glm::dvec3 centroid = (points[i0] + points[i1] + points[i2] + points[i3]) * 0.25;
    std::vector<Tri> faces;
    for (Tri f : {Tri{i0, i1, i2}, Tri{i0, i2, i3}, Tri{i0, i3, i1}, Tri{i1, i3, i2}})
    {
        const glm::dvec3 &a = points[f[0]], &b = points[f[1]], &c = points[f[2]];
        if (glm::dot(glm::cross(b - a, c - a), centroid - a) > 0.0)
            std::swap(f[1], f[2]);
        faces.push_back(f);
    }

    std::vector<Tri> hidden;
    std::set<std::pair<size_t, size_t>> edges;
    for (size_t idx = 0; idx < n; ++idx)
    {
        if (std::find(seed.begin(), seed.end(), idx) != seed.end())
            continue;
        const glm::dvec3& p = points[idx];
        hidden.clear();
        edges.clear();
        for (const Tri& f : faces)
        {
            const glm::dvec3& a = points[f[0]];
            glm::dvec3 nrm = glm::cross(points[f[1]] - a, points[f[2]] - a);
            double mag = glm::length(nrm);
            if (mag >= 1e-30 && glm::dot(nrm, p - a) / mag > eps)
                for (int e = 0; e < 3; ++e)
                    edges.insert({f[e], f[(e + 1) % 3]});
            else
                hidden.push_back(f);
        }
        if (edges.empty())
            continue;
        for (auto [a, b] : edges)
            if (!edges.contains({b, a}))
                hidden.push_back({a, b, idx});
        faces.swap(hidden);
    }
    return faces.size() >= 4 ? faces : std::vector<Tri>{};
}

/** Andrew's monotone chain, counter-clockwise. */
std::vector<glm::dvec2> hull2d(std::vector<glm::dvec2> pts)
{
    std::sort(pts.begin(), pts.end(), [](const glm::dvec2& a, const glm::dvec2& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
    if (pts.size() <= 2)
        return pts;
    auto cross = [](const glm::dvec2& o, const glm::dvec2& a, const glm::dvec2& b) {
        return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
    };
    std::vector<glm::dvec2> out(pts.size() * 2);
    size_t k = 0;
    for (size_t i = 0; i < pts.size(); ++i)
    {
        while (k >= 2 && cross(out[k - 2], out[k - 1], pts[i]) <= 0.0)
            --k;
        out[k++] = pts[i];
    }
    for (size_t i = pts.size() - 1, t = k + 1; i > 0; --i)
    {
        while (k >= t && cross(out[k - 2], out[k - 1], pts[i - 1]) <= 0.0)
            --k;
        out[k++] = pts[i - 1];
    }
    out.resize(k - 1);
    return out;
}

/** Rotating calipers: the unit edge direction whose bounding rectangle has the least area. */
glm::dvec2 minAreaRectDirection(const std::vector<glm::dvec2>& pts)
{
    std::vector<glm::dvec2> hull = hull2d(pts);
    if (hull.size() < 3)
    {
        if (hull.size() == 2 && glm::length(hull[1] - hull[0]) > 0.0)
            return glm::normalize(hull[1] - hull[0]);
        return {1.0, 0.0};
    }
    double bestArea = std::numeric_limits<double>::max();
    glm::dvec2 bestDir(1.0, 0.0);
    for (size_t i = 0; i < hull.size(); ++i)
    {
        glm::dvec2 d = hull[(i + 1) % hull.size()] - hull[i];
        double len = glm::length(d);
        if (len < 1e-30)
            continue;
        glm::dvec2 u = d / len;
        glm::dvec2 lo(std::numeric_limits<double>::max()), hi(-std::numeric_limits<double>::max());
        for (const glm::dvec2& p : hull)
        {
            glm::dvec2 q(glm::dot(p, u), -p.x * u.y + p.y * u.x);
            lo = glm::min(lo, q), hi = glm::max(hi, q);
        }
        if (double area = (hi.x - lo.x) * (hi.y - lo.y); area < bestArea)
            bestArea = area, bestDir = u;
    }
    return bestDir;
}

/** Jacobi eigen-decomposition of a symmetric 3x3; eigenvectors are the columns. */
std::pair<glm::dvec3, glm::dmat3> jacobi(glm::dmat3 a)
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
    return {{a[0][0], a[1][1], a[2][2]}, v};
}

std::optional<glm::dmat3> principalAxes(const Points& points)
{
    glm::dvec3 mean(0.0);
    for (const glm::dvec3& p : points)
        mean += p;
    mean /= double(points.size());
    glm::dmat3 cov(0.0);
    for (const glm::dvec3& p : points)
    {
        glm::dvec3 d = p - mean;
        cov += glm::outerProduct(d, d);
    }
    auto [values, vectors] = jacobi(cov);
    std::array<int, 3> order{0, 1, 2};
    std::sort(order.begin(), order.end(), [&](int a, int b) { return values[a] > values[b]; });
    return frameFromNormal(vectors[order[2]], vectors[order[0]]);
}

struct Extents
{
    glm::dvec3 lo, hi;
    glm::dvec3 size() const { return hi - lo; }
};

Extents extents(std::span<const glm::dvec3> points, const glm::dmat3& axes)
{
    glm::dmat3 t = glm::transpose(axes);
    Extents e{t * points[0], t * points[0]};
    for (const glm::dvec3& p : points)
    {
        glm::dvec3 q = t * p;
        e.lo = glm::min(e.lo, q), e.hi = glm::max(e.hi, q);
    }
    return e;
}

double volume(const glm::dvec3& size, double floor)
{
    return std::max(size.x, floor) * std::max(size.y, floor) * std::max(size.z, floor);
}

glm::dmat3 rotateAbout(const glm::dmat3& axes, int index, double angle)
{
    int i = (index + 1) % 3, j = (index + 2) % 3;
    double c = std::cos(angle), s = std::sin(angle);
    glm::dmat3 out = axes;
    out[i] = c * axes[i] + s * axes[j];
    out[j] = -s * axes[i] + c * axes[j];
    return out;
}

glm::dmat3 polish(const Points& points, glm::dmat3 best, double floor)
{
    double bestVolume = volume(extents(points, best).size(), floor);
    double sweep = glm::radians(4.0);
    constexpr int kSteps = 10;
    for (int pass = 0; pass < 6; ++pass)
    {
        bool moved = false;
        for (int index = 0; index < 3; ++index)
            for (int k = -kSteps; k <= kSteps; ++k)
            {
                if (k == 0)
                    continue;
                glm::dmat3 trial = rotateAbout(best, index, sweep * k / kSteps);
                if (double v = volume(extents(points, trial).size(), floor); v < bestVolume * (1.0 - 1e-12))
                    bestVolume = v, best = trial, moved = true;
            }
        sweep *= 0.35;
        if (!moved && sweep < glm::radians(0.002))
            break;
    }
    return frameFromNormal(best[2], best[0]).value_or(best);
}

/** Permutes and flips the columns to sit closest to X, Y and Z, keeping the frame right-handed. */
glm::dmat3 canonical(const glm::dmat3& axes)
{
    std::array<int, 3> perm{0, 1, 2}, bestPerm = perm;
    double bestScore = -1.0;
    do
    {
        double score = 0.0;
        for (int i = 0; i < 3; ++i)
            score += std::abs(axes[perm[i]][i]);
        if (score > bestScore + 1e-12)
            bestScore = score, bestPerm = perm;
    } while (std::next_permutation(perm.begin(), perm.end()));
    glm::dmat3 out;
    for (int i = 0; i < 3; ++i)
        out[i] = axes[bestPerm[i]] * (axes[bestPerm[i]][i] < 0.0 ? -1.0 : 1.0);
    if (glm::determinant(out) < 0.0)
    {
        int weakest = 0;
        for (int i = 1; i < 3; ++i)
            if (std::abs(out[i][i]) < std::abs(out[weakest][weakest]))
                weakest = i;
        out[weakest] = -out[weakest];
    }
    return out;
}

struct Ball
{
    glm::dvec3 center{0.0};
    double radius2 = -1.0;
    bool contains(const glm::dvec3& p, double eps) const { return glm::dot(p - center, p - center) <= radius2 + eps; }
};

/** The smallest ball with every support point on its surface (circumsphere of up to four points). */
std::optional<Ball> ballThrough(const std::vector<glm::dvec3>& s)
{
    if (s.empty())
        return Ball{};
    const glm::dvec3& p0 = s[0];
    const size_t k = s.size() - 1;
    if (k == 0)
        return Ball{p0, 0.0};
    // Solve for c - p0 = sum(l_j v_j) with 2 v_i.(c - p0) = v_i.v_i.
    std::array<glm::dvec3, 3> v;
    for (size_t i = 0; i < k; ++i)
        v[i] = s[i + 1] - p0;
    double m[3][4];
    for (size_t i = 0; i < k; ++i)
    {
        for (size_t j = 0; j < k; ++j)
            m[i][j] = 2.0 * glm::dot(v[i], v[j]);
        m[i][k] = glm::dot(v[i], v[i]);
    }
    double scale = 0.0;
    for (size_t i = 0; i < k; ++i)
        scale = std::max(scale, m[i][i]);
    for (size_t c = 0; c < k; ++c)
    {
        size_t pivot = c;
        for (size_t r = c + 1; r < k; ++r)
            if (std::abs(m[r][c]) > std::abs(m[pivot][c]))
                pivot = r;
        if (std::abs(m[pivot][c]) <= scale * 1e-14)
            return std::nullopt;
        std::swap(m[c], m[pivot]);
        for (size_t r = 0; r < k; ++r)
            if (r != c)
            {
                double f = m[r][c] / m[c][c];
                for (size_t j = c; j <= k; ++j)
                    m[r][j] -= f * m[c][j];
            }
    }
    glm::dvec3 offset(0.0);
    for (size_t i = 0; i < k; ++i)
        offset += v[i] * (m[i][k] / m[i][i]);
    return Ball{p0 + offset, glm::dot(offset, offset)};
}

/** Move-to-front Welzl (Gaertner's miniball): recursion depth is bounded by the four support points. */
Ball welzl(std::vector<glm::dvec3>& pts, size_t end, std::vector<glm::dvec3>& support, double eps)
{
    Ball ball = ballThrough(support).value_or(Ball{});
    if (support.size() == 4)
        return ball;
    for (size_t i = 0; i < end; ++i)
    {
        if (ball.radius2 >= 0.0 && ball.contains(pts[i], eps))
            continue;
        glm::dvec3 p = pts[i];
        support.push_back(p);
        Ball grown = welzl(pts, i, support, eps);
        support.pop_back();
        if (grown.radius2 >= 0.0 && grown.contains(p, eps))
            ball = grown;
        else
        {
            // Degenerate support: grow the current ball just enough to take p in.
            glm::dvec3 d = p - ball.center;
            double r = std::sqrt(std::max(ball.radius2, 0.0)), len = glm::length(d);
            double nr = (r + len) * 0.5;
            ball.center += d / std::max(len, 1e-300) * (nr - r);
            ball.radius2 = nr * nr;
        }
        std::rotate(pts.begin(), pts.begin() + long(i), pts.begin() + long(i) + 1);
    }
    return ball;
}

Ball minimumBall(std::vector<glm::dvec3> pts)
{
    // A fixed shuffle keeps the expected linear time and makes the result repeatable.
    uint64_t state = 0x9E3779B97F4A7C15ull;
    for (size_t i = pts.size(); i > 1; --i)
    {
        state ^= state << 13, state ^= state >> 7, state ^= state << 17;
        std::swap(pts[i - 1], pts[state % i]);
    }
    std::vector<glm::dvec3> support;
    double eps = std::max(span(pts), 1.0) * 1e-12;
    return welzl(pts, pts.size(), support, eps);
}

/** The hull's vertices, which is all any of the fits need to look at. */
Points hullPoints(std::span<const glm::dvec3> points)
{
    Points sample = searchSet(points);
    std::vector<Tri> faces = convexHull(sample);
    if (faces.empty())
        return sample;
    std::set<size_t> used;
    for (const Tri& f : faces)
        used.insert(f.begin(), f.end());
    Points out;
    for (size_t i : used)
        out.push_back(sample[i]);
    return out;
}

/** Frame whose column 1 is `frame[axis]`, still right-handed. */
glm::dmat3 alongAxis(const glm::dmat3& frame, int axis)
{
    return glm::dmat3(frame[(axis + 2) % 3], frame[axis], frame[(axis + 1) % 3]);
}

/** Smallest circle around the points projected onto the plane across `axes[1]`, as a 2D centre and radius. */
std::pair<glm::dvec2, double> crossSectionCircle(const Points& pts, const glm::dmat3& axes)
{
    std::vector<glm::dvec3> flat;
    flat.reserve(pts.size());
    for (const glm::dvec3& p : pts)
        flat.push_back({glm::dot(p, axes[0]), glm::dot(p, axes[2]), 0.0});
    Ball b = minimumBall(flat);
    glm::dvec2 c(b.center.x, b.center.y);
    double r2 = 0.0;
    for (const glm::dvec3& q : flat)
        r2 = std::max(r2, glm::dot(glm::dvec2(q) - c, glm::dvec2(q) - c));
    return {c, std::sqrt(r2)};
}

RoundShape cylinderAlong(const Points& hull, std::span<const glm::dvec3> all, const glm::dmat3& axes)
{
    auto [c, radius] = crossSectionCircle(hull, axes);
    double lo = std::numeric_limits<double>::max(), hi = -lo, r2 = radius * radius;
    for (const glm::dvec3& p : all)
    {
        double t = glm::dot(p, axes[1]);
        lo = std::min(lo, t), hi = std::max(hi, t);
        glm::dvec2 q(glm::dot(p, axes[0]) - c.x, glm::dot(p, axes[2]) - c.y);
        r2 = std::max(r2, glm::dot(q, q));
    }
    return {axes, axes[0] * c.x + axes[2] * c.y + axes[1] * ((lo + hi) * 0.5), std::sqrt(r2), hi - lo};
}

/** For a radius, the shortest spine [t1 <= t0 means a sphere] whose capsule holds every point. */
std::pair<double, double> capsuleSpine(std::span<const glm::dvec3> pts, const glm::dmat3& axes, glm::dvec2 c, double r)
{
    double top = -std::numeric_limits<double>::max(), bottom = std::numeric_limits<double>::max();
    for (const glm::dvec3& p : pts)
    {
        double t = glm::dot(p, axes[1]);
        glm::dvec2 q(glm::dot(p, axes[0]) - c.x, glm::dot(p, axes[2]) - c.y);
        double reach = std::sqrt(std::max(0.0, r * r - glm::dot(q, q)));
        top = std::max(top, t - reach);
        bottom = std::min(bottom, t + reach);
    }
    return {bottom, top};
}

RoundShape capsuleAlong(const Points& hull, std::span<const glm::dvec3> all, const glm::dmat3& axes)
{
    auto [c, r0] = crossSectionCircle(hull, axes);
    for (const glm::dvec3& p : all)
    {
        glm::dvec2 q(glm::dot(p, axes[0]) - c.x, glm::dot(p, axes[2]) - c.y);
        r0 = std::max(r0, glm::length(q));
    }
    Extents e = extents(hull, axes);
    double rMax = std::max(r0, e.size().y * 0.5);
    double bestVolume = std::numeric_limits<double>::max(), bestR = r0;
    constexpr int kSteps = 64;
    for (int s = 0; s <= kSteps; ++s)
    {
        double r = r0 + (rMax - r0) * s / kSteps;
        auto [bottom, top] = capsuleSpine(hull, axes, c, r);
        double v = glm::pi<double>() * r * r * (std::max(0.0, top - bottom) + 4.0 / 3.0 * r);
        if (v < bestVolume)
            bestVolume = v, bestR = r;
    }
    auto [bottom, top] = capsuleSpine(all, axes, c, bestR);
    double length = std::max(0.0, top - bottom);
    return {axes, axes[0] * c.x + axes[2] * c.y + axes[1] * ((bottom + top) * 0.5), bestR, length + 2.0 * bestR};
}

/** The move that carries a vertex `offset` out along every one of its face planes (least squares past three). */
glm::dvec3 offsetMove(const std::vector<glm::dvec3>& normals, double offset)
{
    if (normals.empty())
        return glm::dvec3(0.0);
    if (normals.size() == 1)
        return normals[0] * offset;
    if (normals.size() == 2)
        return (normals[0] + normals[1]) * (offset / (1.0 + glm::dot(normals[0], normals[1])));
    glm::dmat3 ata(0.0);
    glm::dvec3 atb(0.0);
    for (const glm::dvec3& n : normals)
        ata += glm::outerProduct(n, n), atb += n * offset;
    if (std::abs(glm::determinant(ata)) < 1e-12)
    {
        glm::dvec3 sum(0.0);
        for (const glm::dvec3& n : normals)
            sum += n;
        return glm::normalize(sum) * offset;
    }
    return glm::inverse(ata) * atb;
}

template <typename Fit>
RoundShape bestRound(std::span<const glm::dvec3> points, const glm::dmat3& frame, int axis, Fit fit)
{
    Points hull = hullPoints(points);
    RoundShape best;
    double bestVolume = std::numeric_limits<double>::max();
    for (int a = 0; a < 3; ++a)
    {
        if (axis >= 0 && a != axis)
            continue;
        RoundShape s = fit(hull, points, alongAxis(frame, a));
        if (double v = s.radius * s.radius * s.height; v < bestVolume)
            bestVolume = v, best = s;
    }
    return best;
}
} // namespace

OrientedBox boxInFrame(std::span<const glm::dvec3> points, const glm::dmat3& axes)
{
    Extents e = extents(points, axes);
    return {axes, axes * ((e.lo + e.hi) * 0.5), e.size()};
}

std::optional<OrientedBox> minimumVolumeBox(std::span<const glm::dvec3> points)
{
    LILA_ZONE();

    if (points.size() < 2)
        return std::nullopt;
    Points sample = searchSet(points);
    const double floor = std::max(span(sample), 1.0) * 1e-9;

    std::vector<glm::dmat3> candidates;
    Points hull = sample;
    if (std::vector<Tri> faces = convexHull(sample); !faces.empty())
    {
        std::set<size_t> used;
        for (const Tri& f : faces)
            used.insert(f.begin(), f.end());
        hull.clear();
        for (size_t i : used)
            hull.push_back(sample[i]);

        std::vector<std::pair<double, glm::dvec3>> scored;
        for (const Tri& f : faces)
        {
            glm::dvec3 nrm = glm::cross(sample[f[1]] - sample[f[0]], sample[f[2]] - sample[f[0]]);
            if (double mag = glm::length(nrm); mag >= 1e-24)
                scored.push_back({mag, nrm / mag});
        }
        std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

        // Normals within about 3 degrees of one already tried give the same box.
        std::vector<glm::dvec3> normals;
        for (const auto& [mag, nrm] : scored)
        {
            if (std::any_of(normals.begin(), normals.end(), [&](const glm::dvec3& e) { return std::abs(glm::dot(nrm, e)) > 0.9986; }))
                continue;
            normals.push_back(nrm);
            if (normals.size() >= 400)
                break;
        }
        for (const glm::dvec3& nrm : normals)
        {
            std::optional<glm::dmat3> frame = frameFromNormal(nrm, {1, 0, 0});
            if (!frame)
                continue;
            std::vector<glm::dvec2> flat;
            flat.reserve(hull.size());
            for (const glm::dvec3& p : hull)
                flat.push_back({glm::dot(p, (*frame)[0]), glm::dot(p, (*frame)[1])});
            glm::dvec2 d = minAreaRectDirection(flat);
            glm::dvec3 u = (*frame)[0] * d.x + (*frame)[1] * d.y;
            candidates.push_back(glm::dmat3(u, glm::cross((*frame)[2], u), (*frame)[2]));
        }
    }
    if (std::optional<glm::dmat3> seed = principalAxes(sample))
        candidates.push_back(*seed);
    if (candidates.empty())
        return std::nullopt;

    glm::dmat3 best = candidates[0];
    double bestVolume = std::numeric_limits<double>::max();
    for (const glm::dmat3& axes : candidates)
        if (double v = volume(extents(hull, axes).size(), floor); v < bestVolume)
            bestVolume = v, best = axes;
    return boxInFrame(points, canonical(polish(hull, best, floor)));
}

RoundShape minimumSphere(std::span<const glm::dvec3> points)
{
    LILA_ZONE();

    if (points.empty())
        return {};
    Ball b = minimumBall(hullPoints(points));
    double r2 = 0.0;
    for (const glm::dvec3& p : points)
        r2 = std::max(r2, glm::dot(p - b.center, p - b.center));
    double r = std::sqrt(r2);
    return {glm::dmat3(1.0), b.center, r, 2.0 * r};
}

RoundShape fitCylinder(std::span<const glm::dvec3> points, const glm::dmat3& frame, int axis)
{
    LILA_ZONE();

    if (points.empty())
        return {};
    return bestRound(points, frame, axis, cylinderAlong);
}

RoundShape fitCapsule(std::span<const glm::dvec3> points, const glm::dmat3& frame, int axis)
{
    LILA_ZONE();

    if (points.empty())
        return {};
    return bestRound(points, frame, axis, capsuleAlong);
}

Mesh convexHullMesh(std::span<const glm::dvec3> points, int maxPoints, double offset)
{
    LILA_ZONE();

    if (points.size() < 4)
        return {};
    Points verts = hullPoints(points);
    maxPoints = std::max(maxPoints, 4);
    if (int(verts.size()) > maxPoints)
    {
        Points reduced;
        for (int dirs = 3; dirs <= maxPoints * 4; ++dirs)
        {
            Points trial = extremeSubset(verts, dirs);
            if (int(trial.size()) > maxPoints)
                break;
            reduced = std::move(trial);
        }
        if (reduced.size() >= 4)
            verts = std::move(reduced);
    }
    // Corners first: a point on a face or edge then never lands outside the hull built so far, so it never
    // becomes a vertex.
    {
        std::set<size_t> first = extremeIndices(verts, 256);
        Points ordered;
        for (size_t i : first)
            ordered.push_back(verts[i]);
        for (size_t i = 0; i < verts.size(); ++i)
            if (!first.contains(i))
                ordered.push_back(verts[i]);
        verts = std::move(ordered);
    }
    std::vector<Tri> faces = convexHull(verts);
    if (faces.empty())
        return {};
    std::vector<size_t> remap(verts.size(), SIZE_MAX);
    Points used;
    for (Tri& f : faces)
        for (size_t& i : f)
        {
            if (remap[i] == SIZE_MAX)
                remap[i] = used.size(), used.push_back(verts[i]);
            i = remap[i];
        }
    verts = std::move(used);

    if (offset != 0.0)
    {
        std::vector<std::vector<glm::dvec3>> planes(verts.size());
        for (const Tri& f : faces)
        {
            glm::dvec3 n = glm::normalize(glm::cross(verts[f[1]] - verts[f[0]], verts[f[2]] - verts[f[0]]));
            for (size_t i : f)
                if (std::none_of(planes[i].begin(), planes[i].end(), [&](const glm::dvec3& e) { return glm::dot(e, n) > 1.0 - 1e-9; }))
                    planes[i].push_back(n);
        }
        Points moved = verts;
        for (size_t i = 0; i < verts.size(); ++i)
            moved[i] += offsetMove(planes[i], offset);
        verts = std::move(moved);
    }

    std::vector<Index> sizes(faces.size(), 3), indices;
    for (const Tri& f : faces)
        for (size_t i : f)
            indices.push_back(Index(i));
    return Mesh::fromPolygons(verts, sizes, indices);
}
} // namespace lila::geo
