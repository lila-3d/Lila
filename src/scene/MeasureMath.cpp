#include "scene/MeasureMath.h"

#include "core/PrefStore.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <stdexcept>

namespace lila::scene::measure
{
namespace
{
/** Eigen decomposition of a symmetric 3x3 by cyclic Jacobi rotations; values sorted descending, vectors as columns. */
void symmetricEigen(glm::dmat3 a, glm::dvec3& values, glm::dmat3& vectors)
{
    glm::dmat3 v(1.0);
    for (int sweep = 0; sweep < 50; ++sweep)
    {
        double off = a[0][1] * a[0][1] + a[0][2] * a[0][2] + a[1][2] * a[1][2];
        if (off < 1e-30)
            break;
        for (int p = 0; p < 2; ++p)
            for (int q = p + 1; q < 3; ++q)
            {
                if (std::abs(a[p][q]) < 1e-300)
                    continue;
                double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
                double t = (theta >= 0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
                double c = 1.0 / std::sqrt(t * t + 1.0), s = t * c;
                glm::dmat3 r(1.0);
                r[p][p] = c;
                r[q][q] = c;
                r[q][p] = s;
                r[p][q] = -s;
                a = glm::transpose(r) * a * r;
                v = v * r;
            }
    }
    int order[3] = {0, 1, 2};
    std::sort(order, order + 3, [&](int i, int j) { return a[i][i] > a[j][j]; });
    for (int k = 0; k < 3; ++k)
    {
        values[k] = a[order[k]][order[k]];
        vectors[k] = v[order[k]];
    }
}

double sq(double x) { return x * x; }
} // namespace

Plane fitPlane(std::span<const glm::dvec3> pts)
{
    if (pts.size() < 3)
        throw std::invalid_argument("need at least 3 points");
    glm::dvec3 c(0.0);
    for (const glm::dvec3& p : pts)
        c += p;
    c /= double(pts.size());
    glm::dmat3 cov(0.0);
    for (const glm::dvec3& p : pts)
    {
        glm::dvec3 d = p - c;
        cov += glm::outerProduct(d, d);
    }
    glm::dvec3 values;
    glm::dmat3 vectors;
    symmetricEigen(cov, values, vectors);
    if (values[0] <= 1e-18)
        throw std::invalid_argument("the points coincide");
    // Relative to the main spread, so the test doesn't depend on the scene's scale.
    if (values[1] <= values[0] * 1e-12)
        throw std::invalid_argument("the points are collinear");

    Plane out;
    out.origin = c;
    out.u = glm::normalize(vectors[0]);
    out.normal = glm::normalize(vectors[2]);
    out.v = glm::cross(out.normal, out.u);
    for (const glm::dvec3& p : pts)
        out.maxDeviation = std::max(out.maxDeviation, std::abs(glm::dot(p - c, out.normal)));
    return out;
}

Circle fitCircle(std::span<const glm::dvec3> pts)
{
    Plane plane = fitPlane(pts);
    const size_t n = pts.size();

    // Work in the plane, scaled to unit spread so the normal equations stay well conditioned.
    std::vector<glm::dvec2> q(n);
    double scale = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        glm::dvec3 d = pts[i] - plane.origin;
        q[i] = {glm::dot(d, plane.u), glm::dot(d, plane.v)};
        scale = std::max(scale, glm::length(q[i]));
    }
    for (glm::dvec2& p : q)
        p /= scale;

    // Kasa: minimise sum (x^2 + y^2 + D x + E y + F)^2, linear in D, E, F.
    glm::dmat3 ata(0.0);
    glm::dvec3 atb(0.0);
    for (const glm::dvec2& p : q)
    {
        glm::dvec3 row(p.x, p.y, 1.0);
        double b = -(p.x * p.x + p.y * p.y);
        ata += glm::outerProduct(row, row);
        atb += row * b;
    }
    if (std::abs(glm::determinant(ata)) < 1e-14 * double(n) * double(n) * double(n))
        throw std::invalid_argument("the points are collinear");
    glm::dvec3 def = glm::inverse(ata) * atb;
    glm::dvec2 center(-def.x / 2.0, -def.y / 2.0);
    double r2 = glm::dot(center, center) - def.z;
    if (!(r2 > 0.0))
        throw std::invalid_argument("the points are collinear");
    double radius = std::sqrt(r2);

    // Gauss-Newton with Levenberg damping on the geometric residual |p - c| - r; Kasa is biased on short arcs.
    auto cost = [&](const glm::dvec2& c, double r) {
        double s = 0.0;
        for (const glm::dvec2& p : q)
            s += sq(glm::length(p - c) - r);
        return s;
    };
    double lambda = 1e-3;
    double current = cost(center, radius);
    for (int it = 0; it < 100; ++it)
    {
        glm::dmat3 jtj(0.0);
        glm::dvec3 jtr(0.0);
        for (const glm::dvec2& p : q)
        {
            glm::dvec2 d = p - center;
            double len = glm::length(d);
            if (len < 1e-12)
                continue;
            glm::dvec3 j(-d.x / len, -d.y / len, -1.0);
            jtj += glm::outerProduct(j, j);
            jtr += j * (len - radius);
        }
        glm::dmat3 damped = jtj;
        for (int k = 0; k < 3; ++k)
            damped[k][k] *= 1.0 + lambda;
        if (std::abs(glm::determinant(damped)) < 1e-300)
            break;
        glm::dvec3 step = -(glm::inverse(damped) * jtr);
        glm::dvec2 nc = center + glm::dvec2(step.x, step.y);
        double nr = radius + step.z;
        double next = cost(nc, nr);
        if (next < current)
        {
            bool converged = glm::length(step) < 1e-13 * std::max(1.0, radius);
            center = nc;
            radius = nr;
            current = next;
            lambda = std::max(lambda * 0.3, 1e-12);
            if (converged)
                break;
        }
        else
        {
            lambda *= 10.0;
            if (lambda > 1e12)
                break;
        }
    }
    radius = std::abs(radius);

    Circle out;
    out.center = plane.origin + (plane.u * center.x + plane.v * center.y) * scale;
    out.normal = plane.normal;
    out.radius = radius * scale;
    out.rms = std::sqrt(current / double(n)) * scale;
    out.planarity = plane.maxDeviation;

    std::vector<double> angles(n);
    for (size_t i = 0; i < n; ++i)
        angles[i] = std::atan2(q[i].y - center.y, q[i].x - center.x);
    std::sort(angles.begin(), angles.end());
    double gap = angles.front() + 2.0 * std::numbers::pi - angles.back();
    for (size_t i = 1; i < n; ++i)
        gap = std::max(gap, angles[i] - angles[i - 1]);
    out.arcDegrees = (2.0 * std::numbers::pi - gap) * 180.0 / std::numbers::pi;
    return out;
}

double angleDegrees(const glm::dvec3& a, const glm::dvec3& vertex, const glm::dvec3& b)
{
    glm::dvec3 u = a - vertex, v = b - vertex;
    if (glm::length(u) < 1e-12 || glm::length(v) < 1e-12)
        throw std::invalid_argument("an arm of the angle has zero length");
    // atan2 of cross and dot stays accurate near 0 and 180, where acos loses digits.
    return std::atan2(glm::length(glm::cross(u, v)), glm::dot(u, v)) * 180.0 / std::numbers::pi;
}

int cornerOf(const std::array<glm::dvec3, 3>& pts)
{
    int best = 0;
    double longest = -1.0;
    for (int i = 0; i < 3; ++i)
        if (double d = glm::distance(pts[(i + 1) % 3], pts[(i + 2) % 3]); d > longest)
        {
            longest = d;
            best = i;
        }
    return best;
}

double polygonArea(std::span<const glm::dvec3> pts)
{
    glm::dvec3 n(0.0);
    for (size_t i = 0; i < pts.size(); ++i)
        n += glm::cross(pts[i], pts[(i + 1) % pts.size()]);
    return glm::length(n) * 0.5;
}

double polylineLength(std::span<const glm::dvec3> pts, bool closed)
{
    double len = 0.0;
    for (size_t i = 0; i + 1 < pts.size(); ++i)
        len += glm::distance(pts[i], pts[i + 1]);
    if (closed && pts.size() > 2)
        len += glm::distance(pts.back(), pts.front());
    return len;
}

std::string displayUnit()
{
    std::string u = PrefStore::get().value<std::string>("general", "/units/linear", "cm");
    return unitFactor(u) == 1.0 && u != "cm" ? "cm" : u;
}

double unitFactor(const std::string& unit)
{
    if (unit == "mm")
        return 10.0;
    if (unit == "m")
        return 0.01;
    if (unit == "km")
        return 1e-5;
    if (unit == "in")
        return 1.0 / 2.54;
    if (unit == "ft")
        return 1.0 / 30.48;
    if (unit == "yd")
        return 1.0 / 91.44;
    return 1.0;
}

int displayPrecision() { return std::clamp(PrefStore::get().value<int>("general", "/measure/precision", 2), 0, 8); }

namespace
{
std::string number(double v)
{
    int prec = displayPrecision();
    if (std::abs(v) < 0.5 * std::pow(10.0, -prec))
        v = 0.0;
    return std::format("{:.{}f}", v, prec);
}
} // namespace

std::string formatLength(double cm, bool withUnit)
{
    std::string u = displayUnit();
    std::string s = number(cm * unitFactor(u));
    return withUnit ? s + " " + u : s;
}

std::string formatArea(double cm2, bool withUnit)
{
    std::string u = displayUnit();
    double f = unitFactor(u);
    std::string s = number(cm2 * f * f);
    return withUnit ? s + " " + u + "\xC2\xB2" : s;
}

std::string formatAngle(double degrees, bool withUnit)
{
    std::string s = number(degrees);
    return withUnit ? s + "\xC2\xB0" : s;
}
} // namespace lila::scene::measure
