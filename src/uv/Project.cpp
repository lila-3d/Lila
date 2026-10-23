#include "uv/Project.h"

#include "core/Profile.h"

#include <Eigen/Eigenvalues>

#include <glm/gtc/constants.hpp>
#include <glm/gtx/euler_angles.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace lila::uv
{
namespace
{
constexpr double kPi = glm::pi<double>();
constexpr double kTwoPi = glm::two_pi<double>();

double wrapAngle(double a)
{
    while (a > kPi)
        a -= kTwoPi;
    while (a < -kPi)
        a += kTwoPi;
    return a;
}

double wrapPositive(double a)
{
    a = std::fmod(a, kTwoPi);
    return a < 0.0 ? a + kTwoPi : a;
}

/** Eigenpairs of a symmetric matrix, largest eigenvalue first. */
void eigen(const Eigen::Matrix3d& m, glm::dvec3& values, glm::dmat3& vectors)
{
    Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> es(m);
    for (int i = 0; i < 3; ++i)
    {
        Eigen::Vector3d v = es.eigenvectors().col(2 - i);
        values[i] = es.eigenvalues()(2 - i);
        vectors[i] = glm::normalize(glm::dvec3(v.x(), v.y(), v.z()));
    }
}

Eigen::Vector3d ev(const glm::dvec3& v) { return {v.x, v.y, v.z}; }

/** What the fits read from the faces, gathered once. */
struct FaceSet
{
    std::vector<Index> faces;
    std::vector<double> area;
    std::vector<glm::dvec3> normal;
    std::vector<Index> points; // each once
    double totalArea = 0.0;
};

FaceSet gather(const geo::Mesh& mesh, std::span<const Index> faces)
{
    FaceSet s;
    s.faces = liveFaces(mesh, faces);
    std::vector<uint8_t> seen(mesh.pointCapacity(), 0);
    for (Index f : s.faces)
    {
        double a = mesh.faceArea(f);
        s.area.push_back(a);
        s.normal.push_back(mesh.faceNormal(f));
        s.totalArea += a;
        mesh.forEachFaceCorner(f, [&](Index c) {
            Index p = mesh.cornerPoint(c);
            if (!seen[p])
            {
                seen[p] = 1;
                s.points.push_back(p);
            }
        });
    }
    return s;
}

/** Principal axes of the faces' points, area weighted: columns by decreasing variance. */
glm::dmat3 principalAxes(const geo::Mesh& mesh, const FaceSet& s, glm::dvec3& values)
{
    double w = 0.0;
    glm::dvec3 c(0.0);
    for (size_t i = 0; i < s.faces.size(); ++i)
    {
        c += s.area[i] * mesh.faceCentroid(s.faces[i]);
        w += s.area[i];
    }
    glm::dvec3 center = w > 0.0 ? c / w : glm::dvec3(0.0);
    Eigen::Matrix3d cov = Eigen::Matrix3d::Zero();
    for (size_t i = 0; i < s.faces.size(); ++i)
    {
        Index f = s.faces[i];
        double a = s.area[i] / std::max<double>(1.0, double(mesh.faceSize(f)));
        mesh.forEachFaceCorner(f, [&](Index cr) {
            Eigen::Vector3d v = ev(mesh.position(mesh.cornerPoint(cr)) - center);
            cov += a * v * v.transpose();
        });
    }
    glm::dmat3 out(1.0);
    eigen(cov, values, out);
    return out;
}

/** Right-handed orthonormal columns from roughly orthogonal ones, keeping the first column's direction. */
glm::dmat3 orthonormal(glm::dmat3 m)
{
    m[0] = glm::normalize(m[0]);
    m[1] = glm::normalize(m[1] - glm::dot(m[1], m[0]) * m[0]);
    m[2] = glm::cross(m[0], m[1]);
    return m;
}

/** The axes permuted and flipped (still right handed) so each column lines up best with the reference's. */
glm::dmat3 alignTo(const glm::dmat3& m, const glm::dmat3& ref)
{
    static constexpr int kPerms[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    glm::dmat3 best = m;
    double bestScore = -1e30;
    for (const auto& perm : kPerms)
        for (int signs = 0; signs < 8; ++signs)
        {
            glm::dmat3 c;
            for (int i = 0; i < 3; ++i)
                c[i] = m[perm[i]] * ((signs >> i) & 1 ? -1.0 : 1.0);
            if (glm::determinant(c) <= 0.0)
                continue;
            double score = glm::dot(c[0], ref[0]) + glm::dot(c[1], ref[1]) + glm::dot(c[2], ref[2]);
            if (score > bestScore + 1e-12)
                bestScore = score, best = c;
        }
    return best;
}

/** X and Y for a Z, through planeFrame in the basis' own coordinates, so object or world axes stay put. */
glm::dmat3 frameFromZ(const glm::dvec3& z, const glm::dmat3& basis)
{
    glm::dvec3 zl = glm::normalize(glm::transpose(basis) * z);
    glm::dvec3 ul, vl;
    planeFrame(zl, ul, vl);
    return orthonormal(glm::dmat3(basis * ul, basis * vl, z));
}

/** Area weighted covariance of the normals of faces within 60 degrees of perpendicular to axis; sets the side area. */
Eigen::Matrix3d sideNormalCovariance(const FaceSet& s, const glm::dvec3& axis, double& sideArea, double& residual)
{
    Eigen::Matrix3d cov = Eigen::Matrix3d::Zero();
    sideArea = 0.0, residual = 0.0;
    for (size_t i = 0; i < s.faces.size(); ++i)
    {
        double d = glm::dot(s.normal[i], axis);
        if (std::abs(d) >= 0.5)
            continue;
        Eigen::Vector3d n = ev(s.normal[i]);
        cov += s.area[i] * n * n.transpose();
        sideArea += s.area[i];
        residual += s.area[i] * d * d;
    }
    return cov;
}

/**
 * The cylinder axis: the direction most perpendicular to the side faces' normals (smallest eigenvector of their
 * covariance), refined from several seeds so caps and ends fall out of the side set. Scored by how perpendicular the
 * side normals are and how much of the surface is side.
 */
glm::dvec3 cylinderAxis(const FaceSet& s, const glm::dvec3& longest)
{
    Eigen::Matrix3d all = Eigen::Matrix3d::Zero();
    for (size_t i = 0; i < s.faces.size(); ++i)
        all += s.area[i] * ev(s.normal[i]) * ev(s.normal[i]).transpose();
    glm::dvec3 vals;
    glm::dmat3 vecs;
    eigen(all, vals, vecs);
    std::array<glm::dvec3, 4> seeds = {vecs[2], vecs[1], vecs[0], longest};
    glm::dvec3 best = longest;
    double bestScore = std::numeric_limits<double>::max();
    double total = std::max(s.totalArea, 1e-30);
    for (glm::dvec3 a : seeds)
    {
        double sideArea = 0.0, residual = 0.0;
        for (int it = 0; it < 8; ++it)
        {
            Eigen::Matrix3d cov = sideNormalCovariance(s, a, sideArea, residual);
            if (sideArea <= 0.0)
                break;
            eigen(cov, vals, vecs);
            glm::dvec3 next = vecs[2] * (glm::dot(vecs[2], a) < 0.0 ? -1.0 : 1.0);
            bool done = glm::dot(next, a) > 1.0 - 1e-12;
            a = next;
            if (done)
                break;
        }
        sideNormalCovariance(s, a, sideArea, residual);
        if (sideArea <= 0.0)
            continue;
        double score = residual / total + (1.0 - sideArea / total);
        if (score < bestScore - 1e-9)
            bestScore = score, best = a;
    }
    return best;
}

/** The sign that makes the vector's largest component positive, so a Y-up cylinder points up. */
glm::dvec3 canonicalSign(const glm::dvec3& v)
{
    glm::dvec3 a = glm::abs(v);
    int k = a.x >= a.y && a.x >= a.z ? 0 : a.y >= a.z ? 1 : 2;
    return v[k] < 0.0 ? -v : v;
}

/** The basis column closest to a direction, signed to match it. */
glm::dvec3 nearestColumn(const glm::dmat3& basis, const glm::dvec3& d)
{
    int k = 0;
    for (int i = 1; i < 3; ++i)
        if (std::abs(glm::dot(basis[i], d)) > std::abs(glm::dot(basis[k], d)))
            k = i;
    return glm::dot(basis[k], d) < 0.0 ? -basis[k] : basis[k];
}

/** Box axes from the principal axes, falling back to object axes where the variances tie (a cube has no principal axis). */
glm::dmat3 boxAxes(const glm::dmat3& pca, const glm::dvec3& vals)
{
    auto tie = [&](int a, int b) { return vals[a] - vals[b] <= 0.1 * std::max(vals[0], 1e-30); };
    glm::dmat3 id(1.0);
    glm::dmat3 m;
    if (tie(0, 2))
        return id;
    if (tie(0, 1) || tie(1, 2))
    {
        glm::dvec3 unique = tie(0, 1) ? pca[2] : pca[0];
        glm::dvec3 other = id[0];
        for (int i = 1; i < 3; ++i)
            if (std::abs(glm::dot(id[i], unique)) < std::abs(glm::dot(other, unique)))
                other = id[i];
        m = orthonormal(glm::dmat3(unique, other, glm::dvec3(0.0)));
    }
    else
        m = orthonormal(pca);
    return alignTo(m, id);
}

std::vector<glm::dvec2> convexHull(std::vector<glm::dvec2> p)
{
    std::sort(p.begin(), p.end(), [](const glm::dvec2& a, const glm::dvec2& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    p.erase(std::unique(p.begin(), p.end()), p.end());
    if (p.size() < 3)
        return p;
    auto cross = [](const glm::dvec2& o, const glm::dvec2& a, const glm::dvec2& b) {
        return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
    };
    std::vector<glm::dvec2> h(2 * p.size());
    size_t k = 0;
    for (size_t i = 0; i < p.size(); ++i)
    {
        while (k >= 2 && cross(h[k - 2], h[k - 1], p[i]) <= 0.0)
            --k;
        h[k++] = p[i];
    }
    for (size_t i = p.size() - 1, t = k + 1; i > 0; --i)
    {
        while (k >= t && cross(h[k - 2], h[k - 1], p[i - 1]) <= 0.0)
            --k;
        h[k++] = p[i - 1];
    }
    h.resize(k - 1);
    return h;
}

/**
 * The in-plane X of a planar frame: along a side of the minimum-area rectangle around the points, whichever of its
 * four directions is nearest the plane's default u, so upright content stays upright.
 */
glm::dvec3 rectangleAxis(const geo::Mesh& mesh, const FaceSet& s, const glm::dvec3& z)
{
    glm::dvec3 u0, v0;
    planeFrame(z, u0, v0);
    std::vector<glm::dvec2> pts;
    pts.reserve(s.points.size());
    for (Index p : s.points)
        pts.push_back({glm::dot(mesh.position(p), u0), glm::dot(mesh.position(p), v0)});
    std::vector<glm::dvec2> hull = convexHull(std::move(pts));
    if (hull.size() < 3)
        return u0;
    glm::dvec2 bestDir(1.0, 0.0);
    double bestArea = std::numeric_limits<double>::max();
    for (size_t i = 0; i < hull.size(); ++i)
    {
        glm::dvec2 e = hull[(i + 1) % hull.size()] - hull[i];
        double len = glm::length(e);
        if (len < 1e-12)
            continue;
        e /= len;
        glm::dvec2 n(-e.y, e.x);
        double lo0 = 1e300, hi0 = -1e300, lo1 = 1e300, hi1 = -1e300;
        for (const glm::dvec2& q : hull)
        {
            lo0 = std::min(lo0, glm::dot(q, e)), hi0 = std::max(hi0, glm::dot(q, e));
            lo1 = std::min(lo1, glm::dot(q, n)), hi1 = std::max(hi1, glm::dot(q, n));
        }
        double area = (hi0 - lo0) * (hi1 - lo1);
        if (area < bestArea * (1.0 - 1e-9))
            bestArea = area, bestDir = e;
    }
    glm::dvec2 options[4] = {bestDir, -bestDir, {-bestDir.y, bestDir.x}, {bestDir.y, -bestDir.x}};
    glm::dvec2 pick = *std::max_element(std::begin(options), std::end(options), [](const glm::dvec2& a, const glm::dvec2& b) { return a.x < b.x; });
    return glm::normalize(pick.x * u0 + pick.y * v0);
}

/** Ritter's bounding sphere, then one pass pulling the centre toward the farthest points. */
void boundingSphere(const geo::Mesh& mesh, const FaceSet& s, glm::dvec3& center, double& radius)
{
    if (s.points.empty())
        return;
    auto farthest = [&](const glm::dvec3& from) {
        glm::dvec3 best = from;
        double d = -1.0;
        for (Index p : s.points)
            if (double l = glm::length(mesh.position(p) - from); l > d)
                d = l, best = mesh.position(p);
        return best;
    };
    glm::dvec3 a = farthest(mesh.position(s.points.front())), b = farthest(a);
    center = (a + b) * 0.5;
    radius = glm::length(b - a) * 0.5;
    for (Index p : s.points)
    {
        glm::dvec3 q = mesh.position(p);
        double d = glm::length(q - center);
        if (d > radius)
        {
            double grown = (radius + d) * 0.5;
            center += (q - center) * ((grown - radius) / d);
            radius = grown;
        }
    }
}

/** Least squares circle through 2D points (Kasa). False when they are nearly collinear. */
bool fitCircle(const std::vector<glm::dvec2>& pts, glm::dvec2& center, double& radius)
{
    if (pts.size() < 3)
        return false;
    Eigen::Matrix3d A = Eigen::Matrix3d::Zero();
    Eigen::Vector3d b = Eigen::Vector3d::Zero();
    glm::dvec2 lo(1e300), hi(-1e300);
    for (const glm::dvec2& p : pts)
    {
        Eigen::Vector3d r(p.x, p.y, 1.0);
        double z = -(p.x * p.x + p.y * p.y);
        A += r * r.transpose();
        b += r * z;
        lo = glm::min(lo, p), hi = glm::max(hi, p);
    }
    Eigen::Vector3d x = A.ldlt().solve(b);
    if (!x.allFinite())
        return false;
    center = {-x(0) / 2.0, -x(1) / 2.0};
    double r2 = glm::dot(center, center) - x(2);
    double extent = glm::length(hi - lo);
    if (!(r2 > 0.0) || std::sqrt(r2) > 10.0 * extent)
        return false;
    radius = std::sqrt(r2);
    return true;
}

bool isSide(const glm::dvec3& n, const glm::dvec3& axis) { return std::abs(glm::dot(n, axis)) < 0.7; }

/**
 * Where U wraps for a cylinder or sphere, as an angle about Z from X. Seams cost most where they would cut through faces,
 * less on the side the camera sees, and are cheaper along existing seams and hard edges.
 */
double chooseSeam(const geo::Mesh& mesh, const FaceSet& s, const ProjectFrame& fr, const glm::dvec3& viewLocal)
{
    LILA_ZONE();

    glm::dmat3 toLocal = glm::transpose(fr.axes);
    auto local = [&](Index p) { return toLocal * (mesh.position(p) - fr.center); };
    double eps = 1e-9 * std::max({fr.radius, fr.height, 1e-9});

    struct Span
    {
        double lo, hi, w;
    };
    std::vector<Span> spans;
    std::vector<double> cand;
    double sideArea = 0.0;
    for (size_t i = 0; i < s.faces.size(); ++i)
    {
        Index f = s.faces[i];
        glm::dvec3 nl = toLocal * s.normal[i];
        glm::dvec3 cl = toLocal * (mesh.faceCentroid(f) - fr.center);
        if (std::abs(nl.z) > 0.8 || glm::length(glm::dvec2(cl)) <= eps)
            continue;
        double base = std::atan2(cl.y, cl.x);
        double lo = 0.0, hi = 0.0;
        bool onAxis = false;
        mesh.forEachFaceCorner(f, [&](Index c) {
            glm::dvec3 q = local(mesh.cornerPoint(c));
            if (glm::length(glm::dvec2(q)) <= eps)
            {
                onAxis = true;
                return;
            }
            double th = std::atan2(q.y, q.x);
            double d = wrapAngle(th - base);
            lo = std::min(lo, d), hi = std::max(hi, d);
            cand.push_back(th);
        });
        if (onAxis || hi - lo > kPi)
            continue;
        spans.push_back({base + lo, base + hi, s.area[i]});
        sideArea += s.area[i];
    }
    for (int k = 0; k < 720; ++k)
        cand.push_back(-kPi + kPi * k / 360.0);
    std::sort(cand.begin(), cand.end());
    std::vector<double> c;
    for (double a : cand)
        if (c.empty() || a - c.back() > 1e-7)
            c.push_back(a);
    const size_t n = c.size();

    // Area of the faces each candidate cuts through, by a difference array over the sorted candidates.
    std::vector<double> diff(n + 1, 0.0);
    auto addRange = [&](double lo, double hi, double w) {
        size_t a = size_t(std::upper_bound(c.begin(), c.end(), lo) - c.begin());
        size_t b = size_t(std::lower_bound(c.begin(), c.end(), hi) - c.begin());
        if (a < b)
            diff[a] += w, diff[b] -= w;
    };
    const double inset = 1e-6;
    for (const Span& sp : spans)
    {
        double lo = sp.lo + inset, hi = sp.hi - inset;
        if (lo >= hi)
            continue;
        addRange(lo, std::min(hi, kPi), sp.w);
        if (lo < -kPi)
            addRange(lo + kTwoPi, kPi, sp.w);
        if (hi > kPi)
            addRange(-kPi - 1.0, hi - kTwoPi, sp.w);
    }

    // Existing seams and hard edges running along the axis at a candidate angle.
    std::vector<double> bonus(n, 0.0);
    std::span<const bool> seam = mesh.edgeAttrs().get<bool>(geo::attr::kSeam);
    std::span<const bool> hard = mesh.edgeAttrs().get<bool>(geo::attr::kHard);
    if (!seam.empty() || !hard.empty())
    {
        std::vector<uint8_t> done(mesh.edgeCapacity(), 0);
        for (Index f : s.faces)
            mesh.forEachFaceCorner(f, [&](Index cr) {
                Index e = mesh.cornerEdge(cr);
                if (done[e] || !((!seam.empty() && seam[e]) || (!hard.empty() && hard[e])))
                    return;
                done[e] = 1;
                glm::dvec3 a = local(mesh.edgePoint(e, 0)), b = local(mesh.edgePoint(e, 1));
                if (glm::length(glm::dvec2(a)) <= eps || glm::length(glm::dvec2(b)) <= eps)
                    return;
                double ta = std::atan2(a.y, a.x);
                if (std::abs(wrapAngle(ta - std::atan2(b.y, b.x))) > 1e-5)
                    return;
                size_t k = size_t(std::lower_bound(c.begin(), c.end(), ta - 1e-6) - c.begin());
                if (k < n && std::abs(c[k] - ta) < 1e-5)
                    bonus[k] += std::abs(a.z - b.z) / std::max(fr.height, eps);
            });
    }

    glm::dvec2 view(viewLocal);
    bool hasView = glm::length(view) > 1e-6;
    if (hasView)
        view = glm::normalize(view);
    double best = 0.0, bestCost = std::numeric_limits<double>::max(), run = 0.0;
    for (size_t k = 0; k < n; ++k)
    {
        run += diff[k];
        double cut = sideArea > 0.0 ? std::max(0.0, run) / sideArea : 0.0;
        double seen = hasView ? (1.0 - (std::cos(c[k]) * view.x + std::sin(c[k]) * view.y)) * 0.5 : 0.0;
        double cost = 4.0 * cut + seen - 2.0 * std::min(bonus[k], 1.0);
        if (cost < bestCost - 1e-9)
            bestCost = cost, best = c[k];
    }
    return best;
}

/** Object-space basis the World and Object fits use. */
glm::dmat3 fitBasis(const ProjectOptions& o)
{
    if (o.fit != ProjectFit::World)
        return glm::dmat3(1.0);
    glm::dmat3 inv = glm::inverse(o.worldLinear);
    return std::abs(glm::determinant(o.worldLinear)) > 1e-30 ? orthonormal(inv) : glm::dmat3(1.0);
}

glm::dmat3 fitAxes(const geo::Mesh& mesh, const FaceSet& s, const ProjectOptions& o)
{
    glm::dvec3 vals;
    glm::dmat3 pca = principalAxes(mesh, s, vals);
    glm::dmat3 basis = fitBasis(o);
    bool autoFit = o.fit == ProjectFit::Auto;
    int k = o.axis == ProjectAxis::BestFit ? -1 : int(o.axis);
    switch (o.type)
    {
    case Projection::Planar:
    {
        glm::dvec3 sum(0.0);
        for (Index f : s.faces)
            sum += mesh.faceAreaVector(f);
        double scale = 0.0;
        for (double a : s.area)
            scale += a;
        glm::dvec3 n = glm::length(sum) > 1e-6 * scale ? glm::normalize(sum) : pca[2];
        glm::dvec3 z = k >= 0 ? basis[k] : autoFit ? n : nearestColumn(basis, n);
        if (!autoFit)
            return frameFromZ(z, basis);
        glm::dvec3 x = rectangleAxis(mesh, s, z);
        return glm::dmat3(x, glm::cross(z, x), z);
    }
    case Projection::Box: return autoFit ? boxAxes(pca, vals) : basis;
    case Projection::Cylindrical:
    {
        glm::dvec3 axis = canonicalSign(cylinderAxis(s, pca[0]));
        glm::dvec3 z = k >= 0 ? basis[k] : autoFit ? axis : nearestColumn(basis, axis);
        return frameFromZ(z, basis);
    }
    case Projection::Spherical:
    {
        bool distinct = vals[0] > 1.1 * vals[1];
        glm::dvec3 z = k >= 0 ? basis[k] : autoFit && distinct ? canonicalSign(pca[0]) : basis[1];
        return frameFromZ(z, basis);
    }
    default: return glm::dmat3(1.0);
    }
}

void fitSizes(const geo::Mesh& mesh, const FaceSet& s, const ProjectOptions& o, ProjectFrame& fr)
{
    glm::dmat3 toLocal = glm::transpose(fr.axes);
    glm::dvec3 lo(1e300), hi(-1e300);
    for (Index p : s.points)
    {
        glm::dvec3 q = toLocal * mesh.position(p);
        lo = glm::min(lo, q), hi = glm::max(hi, q);
    }
    if (s.points.empty())
        lo = hi = glm::dvec3(0.0);
    glm::dvec3 mid = (lo + hi) * 0.5;
    glm::dvec3 ext = hi - lo;
    if (o.center)
    {
        glm::dvec3 cl = toLocal * *o.center;
        ext = 2.0 * glm::max(glm::abs(hi - cl), glm::abs(lo - cl));
        mid = cl;
    }
    double tiny = 1e-9 * std::max({ext.x, ext.y, ext.z, 1e-9});

    if (o.type == Projection::Planar || o.type == Projection::Box)
    {
        fr.center = fr.axes * mid;
        fr.width = ext.x, fr.height = ext.y, fr.depth = ext.z;
    }
    else if (o.type == Projection::Cylindrical)
    {
        std::vector<glm::dvec2> side;
        std::vector<uint8_t> seen(mesh.pointCapacity(), 0);
        for (size_t i = 0; i < s.faces.size(); ++i)
            if (isSide(s.normal[i], fr.axes[2]))
                mesh.forEachFaceCorner(s.faces[i], [&](Index c) {
                    Index p = mesh.cornerPoint(c);
                    if (!seen[p])
                        seen[p] = 1, side.push_back(glm::dvec2(toLocal * mesh.position(p)));
                });
        if (side.empty())
            for (Index p : s.points)
                side.push_back(glm::dvec2(toLocal * mesh.position(p)));
        glm::dvec2 c2 = glm::dvec2(mid);
        double r = 0.0;
        if (!o.center && !fitCircle(side, c2, r))
        {
            c2 = glm::dvec2(0.0);
            for (const glm::dvec2& q : side)
                c2 += q;
            c2 /= double(std::max<size_t>(side.size(), 1));
        }
        r = 0.0;
        for (const glm::dvec2& q : side)
            r += glm::length(q - c2);
        fr.radius = side.empty() ? 1.0 : r / double(side.size());
        fr.center = fr.axes * glm::dvec3(c2, mid.z);
        fr.height = ext.z;
    }
    else if (o.type == Projection::Spherical)
    {
        if (o.center)
        {
            fr.center = *o.center;
            double r = 0.0;
            for (Index p : s.points)
                r = std::max(r, glm::length(mesh.position(p) - fr.center));
            fr.radius = r;
        }
        else
            boundingSphere(mesh, s, fr.center, fr.radius);
    }
    fr.width = std::max(fr.width, tiny), fr.height = std::max(fr.height, tiny), fr.depth = std::max(fr.depth, tiny);
    fr.radius = std::max(fr.radius, tiny);
}

ProjectFrame resolve(const geo::Mesh& mesh, const FaceSet& s, const ProjectOptions& o)
{
    LILA_ZONE();

    ProjectFrame fr;
    if (o.type == Projection::Camera || s.faces.empty())
        return fr;
    fr.axes = o.rotation ? eulerFrame(*o.rotation) : fitAxes(mesh, s, o);
    fitSizes(mesh, s, o, fr);
    if (o.center)
        fr.center = *o.center;
    auto use = [](const std::optional<double>& v, double& out) {
        if (v && *v > 0.0)
            out = *v;
    };
    use(o.width, fr.width);
    use(o.height, fr.height);
    use(o.depth, fr.depth);
    use(o.radius, fr.radius);
    if (o.seamAngle)
        fr.seamAngle = glm::radians(*o.seamAngle);
    else if (o.type == Projection::Cylindrical || o.type == Projection::Spherical)
    {
        // Without a camera the seam goes at the back, as seen from the front view.
        glm::dvec3 view = o.viewDir && glm::length(*o.viewDir) > 1e-12 ? glm::normalize(*o.viewDir) : glm::dvec3(0.0, 0.0, -1.0);
        fr.seamAngle = chooseSeam(mesh, s, fr, glm::transpose(fr.axes) * view);
    }
    return fr;
}
} // namespace

void planeFrame(const glm::dvec3& n, glm::dvec3& u, glm::dvec3& v)
{
    glm::dvec3 up = std::abs(n.y) > 0.99 ? glm::dvec3(0.0, 0.0, n.y > 0.0 ? -1.0 : 1.0) : glm::dvec3(0.0, 1.0, 0.0);
    u = glm::normalize(glm::cross(up, n));
    v = glm::cross(n, u);
}

glm::dvec3 frameEuler(const glm::dmat3& axes)
{
    double x = 0.0, y = 0.0, z = 0.0;
    glm::extractEulerAngleXYZ(glm::dmat4(axes), x, y, z);
    return glm::degrees(glm::dvec3(x, y, z));
}

glm::dmat3 eulerFrame(const glm::dvec3& degrees)
{
    glm::dvec3 r = glm::radians(degrees);
    return glm::dmat3(glm::eulerAngleXYZ(r.x, r.y, r.z));
}

ProjectFrame fitProjection(const geo::Mesh& mesh, std::span<const Index> faces, const ProjectOptions& opts)
{
    return resolve(mesh, gather(mesh, faces), opts);
}

bool project(geo::Mesh& mesh, std::span<const Index> faceIds, std::string_view uvSet, const ProjectOptions& opts, ProjectFrame* used)
{
    LILA_ZONE();

    FaceSet s = gather(mesh, faceIds);
    const std::vector<Index>& faces = s.faces;
    if (faces.empty())
        return false;
    std::span<glm::vec2> uv = uvLayer(mesh, uvSet);
    ProjectFrame fr = resolve(mesh, s, opts);
    if (used)
        *used = fr;
    glm::dmat3 toLocal = glm::transpose(fr.axes);
    auto local = [&](Index p) { return toLocal * (mesh.position(p) - fr.center); };
    // The frame's rectangle in object units; normalize fits it into 0-1.
    glm::dvec2 rect(1.0);

    switch (opts.type)
    {
    case Projection::Planar:
    {
        rect = {fr.width, fr.height};
        for (Index f : faces)
            mesh.forEachFaceCorner(f, [&](Index c) {
                glm::dvec3 q = local(mesh.cornerPoint(c));
                uv[c] = glm::vec2(float(q.x + fr.width * 0.5), float(q.y + fr.height * 0.5));
            });
        break;
    }
    case Projection::Box:
    {
        glm::dvec3 size(fr.width, fr.height, fr.depth);
        double side = std::max({size.x, size.y, size.z});
        rect = {side, side};
        for (size_t i = 0; i < faces.size(); ++i)
        {
            glm::dvec3 fn = toLocal * s.normal[i];
            glm::dvec3 a = glm::abs(fn);
            int k = a.x >= a.y && a.x >= a.z ? 0 : a.y >= a.z ? 1 : 2;
            glm::dvec3 n(0.0);
            n[k] = fn[k] >= 0.0 ? 1.0 : -1.0;
            glm::dvec3 u, v;
            planeFrame(n, u, v);
            double eu = glm::dot(glm::abs(u), size), evv = glm::dot(glm::abs(v), size);
            mesh.forEachFaceCorner(faces[i], [&](Index c) {
                glm::dvec3 q = local(mesh.cornerPoint(c));
                uv[c] = glm::vec2(float(glm::dot(q, u) + eu * 0.5), float(glm::dot(q, v) + evv * 0.5));
            });
        }
        break;
    }
    case Projection::Cylindrical:
    case Projection::Spherical:
    {
        bool sphere = opts.type == Projection::Spherical;
        double r = fr.radius;
        rect = sphere ? glm::dvec2(kTwoPi * r, kPi * r) : glm::dvec2(kTwoPi * r, fr.height);
        auto angleOf = [&](const glm::dvec3& q) { return std::atan2(q.y, q.x) - fr.seamAngle; };
        for (Index f : faces)
        {
            glm::dvec3 fc = toLocal * (mesh.faceCentroid(f) - fr.center);
            double base = glm::length(glm::dvec2(fc)) > 1e-9 * r ? wrapPositive(angleOf(fc)) : 0.0;
            mesh.forEachFaceCorner(f, [&](Index c) {
                glm::dvec3 q = local(mesh.cornerPoint(c));
                // Points on the axis have no angle of their own; they take the face's.
                double th = glm::length(glm::dvec2(q)) > 1e-9 * r ? base + wrapAngle(angleOf(q) - base) : base;
                double second = q.z + fr.height * 0.5;
                if (sphere)
                {
                    double len = glm::length(q);
                    second = (len > 0.0 ? std::asin(std::clamp(q.z / len, -1.0, 1.0)) : 0.0) * r + kPi * r * 0.5;
                }
                uv[c] = glm::vec2(float(th * r), float(second));
            });
        }
        break;
    }
    case Projection::Camera:
    {
        for (Index f : faces)
            mesh.forEachFaceCorner(f, [&](Index c) {
                glm::dvec4 clip = opts.camera * glm::dvec4(mesh.position(mesh.cornerPoint(c)), 1.0);
                double iw = std::abs(clip.w) > 1e-12 ? 1.0 / clip.w : 1.0;
                uv[c] = glm::vec2(float(clip.x * iw * 0.5 + 0.5), float(clip.y * iw * 0.5 + 0.5));
            });
        break;
    }
    default: return false;
    }

    bool camera = opts.type == Projection::Camera;
    double ext = std::max(rect.x, rect.y);
    float s0 = opts.normalize && !camera && ext > 0.0 ? float(1.0 / ext) : 1.0f;
    glm::vec2 scale = glm::vec2(opts.scale) * s0, offset(opts.offset);
    if (scale != glm::vec2(1.0f) || offset != glm::vec2(0.0f))
        for (Index f : faces)
            mesh.forEachFaceCorner(f, [&](Index c) { uv[c] = uv[c] * scale + offset; });
    if (opts.markSeams)
        markSeamsFromUvs(mesh, faces, uvSet);
    return true;
}
} // namespace lila::uv
