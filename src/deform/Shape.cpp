#include "deform/Shape.h"

#include "core/Profile.h"
#include "deform/DeformUtil.h"
#include "deform/EigenGlm.h"

#include <Eigen/Dense>
#include <Eigen/Sparse>
#include <glm/gtc/quaternion.hpp>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace lila::deform
{
namespace
{
/** Lamé-free Kelvinlet constants for shear modulus 1: a = 1 / (4 pi mu), b = a / (4 (1 - nu)). */
struct Kelvin
{
    double a, b;
    explicit Kelvin(double nu)
    {
        nu = std::clamp(nu, -0.99, 0.5);
        a = 1.0 / (4.0 * std::numbers::pi);
        b = a / (4.0 * (1.0 - nu));
    }
};

/** Scale ratio between the radii of a multi-scale brush. */
constexpr double kScaleRatio = 1.5;

/** Radii and weights of a 1-, 2- or 3-scale brush: the weights cancel the far field's 1/r (and eps^2/r^3) terms. */
int brushScales(const KelvinletBrush& b, double eps[3], double w[3])
{
    const int n = std::clamp(b.scales, 1, 3);
    const double r = kScaleRatio, e = std::max(b.radius, 1e-9);
    eps[0] = e;
    eps[1] = e * r;
    eps[2] = e * r * r;
    if (n == 1)
        w[0] = 1.0;
    else if (n == 2)
    {
        w[0] = 1.0;
        w[1] = -1.0;
    }
    else
    {
        w[0] = 1.0;
        w[1] = -(1.0 + r * r) / (r * r);
        w[2] = 1.0 / (r * r);
    }
    return n;
}

/** The radial profile shared by twist and scale, 1/r_e^3 + 3 e^2 / (2 r_e^5). */
double affineProfile(double r2, double eps)
{
    double re2 = r2 + eps * eps;
    double re = std::sqrt(re2);
    double re3 = re2 * re;
    return 1.0 / re3 + 1.5 * eps * eps / (re3 * re2);
}

/** The profile of a multi-scale twist or scale brush, 1 at the centre. */
double brushProfile(const glm::dvec3& r, const KelvinletBrush& b)
{
    double eps[3], w[3];
    int n = brushScales(b, eps, w);
    double num = 0.0, den = 0.0;
    double r2 = glm::dot(r, r);
    for (int k = 0; k < n; ++k)
    {
        num += w[k] * affineProfile(r2, eps[k]);
        den += w[k] * affineProfile(0.0, eps[k]);
    }
    return std::abs(den) > 0.0 ? num / den : 0.0;
}

glm::dvec3 rotateBy(const glm::dvec3& p, const glm::dvec3& axis, double angle)
{
    double c = std::cos(angle), s = std::sin(angle);
    return p * c + glm::cross(axis, p) * s + axis * glm::dot(axis, p) * (1.0 - c);
}

glm::dmat3 outer(const glm::dvec3& a, const glm::dvec3& b) { return glm::dmat3(a * b.x, a * b.y, a * b.z); }

double trace(const glm::dmat3& m) { return m[0][0] + m[1][1] + m[2][2]; }

// ------------------------------------------------------------------------------------------------ Green coordinates

double sign(double v) { return v > 0.0 ? 1.0 : (v < 0.0 ? -1.0 : 0.0); }

/** Lipman et al. 2008, appendix: the integral over the triangle (p, v1, v2) of 1 / |y - eta|^3-type kernels, closed form. */
double gcTriInt(const glm::dvec3& p, const glm::dvec3& v1, const glm::dvec3& v2, const glm::dvec3& eta)
{
    const glm::dvec3 a = v2 - v1, b = p - v1;
    const double la = glm::length(a), lb = glm::length(b);
    const glm::dvec3 c1 = v1 - p, c2 = v2 - p;
    const double lc1 = glm::length(c1), lc2 = glm::length(c2);
    if (la < 1e-14 || lb < 1e-14 || lc1 < 1e-14 || lc2 < 1e-14)
        return 0.0;
    // atan2 rather than acos: points whose projection lies almost on an edge line need tiny angles to stay exact.
    const double alpha = std::atan2(glm::length(glm::cross(a, b)), glm::dot(a, b));
    const double beta = std::atan2(glm::length(glm::cross(c1, c2)), glm::dot(c1, c2));
    const double lambda = lb * lb * std::sin(alpha) * std::sin(alpha);
    const glm::dvec3 pe = p - eta;
    const double c = glm::dot(pe, pe);
    const double sc = std::sqrt(c), sl = std::sqrt(lambda);
    auto term = [&](double theta) {
        const double S = std::sin(theta), C = std::cos(theta);
        if (S == 0.0)
            return 0.0;
        const double logArg =
            (2.0 * sl * S * S / ((1.0 - C) * (1.0 - C))) * (1.0 - 2.0 * c * C / (c * (1.0 + C) + lambda + std::sqrt(lambda * lambda + lambda * c * S * S)));
        const double logTerm = sl > 0.0 && logArg > 0.0 ? sl * std::log(logArg) : 0.0;
        return -sign(S) * 0.5 * (2.0 * sc * std::atan(sc * C / std::sqrt(lambda + S * S * c)) + logTerm);
    };
    // pi - alpha - beta is the triangle's angle at v2, measured directly so it never goes negative by rounding.
    const double gamma = std::atan2(glm::length(glm::cross(v1 - v2, p - v2)), glm::dot(v1 - v2, p - v2));
    const double i1 = term(std::numbers::pi - alpha), i2 = term(gamma);
    return -1.0 / (4.0 * std::numbers::pi) * std::abs(i1 - i2 - sc * beta);
}

// ------------------------------------------------------------------------------------------------ Somigliana

/** Kelvin kernel K(r) for shear modulus 1: displacement at x from a unit load at y, r = y - x. */
glm::dmat3 kelvinKernel(const glm::dvec3& r, double nu)
{
    double rho = glm::length(r);
    glm::dvec3 u = r / rho;
    double k = 1.0 / (16.0 * std::numbers::pi * (1.0 - nu) * rho);
    return k * ((3.0 - 4.0 * nu) * glm::dmat3(1.0) + outer(u, u));
}

/** Traction kernel T(x, y) for the outward normal n at y, r = y - x: -integral over a closed surface around x is the identity. */
glm::dmat3 tractionKernel(const glm::dvec3& r, const glm::dvec3& n, double nu)
{
    double rho = glm::length(r);
    glm::dvec3 u = r / rho;
    double drdn = glm::dot(u, n);
    double k = -1.0 / (8.0 * std::numbers::pi * (1.0 - nu) * rho * rho);
    glm::dmat3 m = drdn * ((1.0 - 2.0 * nu) * glm::dmat3(1.0) + 3.0 * outer(u, u)) - (1.0 - 2.0 * nu) * (outer(u, n) - outer(n, u));
    return k * m;
}

/** Degree 5, 7 point triangle rule (Radon): barycentric points and weights summing to 1. */
struct QuadPoint
{
    double l0, l1, l2, w;
};
const std::array<QuadPoint, 7>& quadRule()
{
    static const std::array<QuadPoint, 7> rule = [] {
        const double s = std::sqrt(15.0);
        const double a1 = (6.0 - s) / 21.0, b1 = (9.0 + 2.0 * s) / 21.0;
        const double a2 = (6.0 + s) / 21.0, b2 = (9.0 - 2.0 * s) / 21.0;
        const double w1 = (155.0 - s) / 1200.0, w2 = (155.0 + s) / 1200.0;
        return std::array<QuadPoint, 7>{{{1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0, 0.225},
                                        {a1, a1, b1, w1},
                                        {a1, b1, a1, w1},
                                        {b1, a1, a1, w1},
                                        {a2, a2, b2, w2},
                                        {a2, b2, a2, w2},
                                        {b2, a2, a2, w2}}};
    }();
    return rule;
}

/**
 * Integrates the Somigliana kernels over a piece of a cage triangle (corners given as barycentrics of the whole
 * triangle), subdividing pieces close to x. Adds into the three corner matrices (traction kernel times hat function)
 * and the face matrix (Kelvin kernel).
 */
void somiglianaPiece(const glm::dvec3 tri[3], const glm::dvec3& n, const glm::dvec3& x, double nu, const glm::dvec3 bary[3], int depth,
                     glm::dmat3 corner[3], glm::dmat3& face)
{
    glm::dvec3 p[3];
    for (int k = 0; k < 3; ++k)
        p[k] = tri[0] * bary[k].x + tri[1] * bary[k].y + tri[2] * bary[k].z;
    const double size = std::max({glm::length(p[1] - p[0]), glm::length(p[2] - p[1]), glm::length(p[0] - p[2])});
    const glm::dvec3 centre = (p[0] + p[1] + p[2]) / 3.0;
    if (depth > 0 && glm::length(centre - x) < 2.0 * size)
    {
        glm::dvec3 m01 = (bary[0] + bary[1]) * 0.5, m12 = (bary[1] + bary[2]) * 0.5, m20 = (bary[2] + bary[0]) * 0.5;
        const glm::dvec3 sub[4][3] = {{bary[0], m01, m20}, {m01, bary[1], m12}, {m20, m12, bary[2]}, {m01, m12, m20}};
        for (const auto& s : sub)
            somiglianaPiece(tri, n, x, nu, s, depth - 1, corner, face);
        return;
    }
    const double area = 0.5 * glm::length(glm::cross(p[1] - p[0], p[2] - p[0]));
    if (area <= 0.0)
        return;
    for (const QuadPoint& q : quadRule())
    {
        glm::dvec3 b = bary[0] * q.l0 + bary[1] * q.l1 + bary[2] * q.l2;
        glm::dvec3 y = tri[0] * b.x + tri[1] * b.y + tri[2] * b.z;
        glm::dvec3 r = y - x;
        if (glm::dot(r, r) < 1e-24)
            continue;
        double w = q.w * area;
        glm::dmat3 T = tractionKernel(r, n, nu) * (-w);
        corner[0] += T * b.x;
        corner[1] += T * b.y;
        corner[2] += T * b.z;
        face += kelvinKernel(r, nu) * w;
    }
}
} // namespace

// ------------------------------------------------------------------------------------------------ kelvinlets

glm::dvec3 kelvinletGrab(const glm::dvec3& r, const glm::dvec3& force, double eps, double nu)
{
    const Kelvin k(nu);
    double re2 = glm::dot(r, r) + eps * eps;
    double re = std::sqrt(re2);
    double re3 = re2 * re;
    return ((k.a - k.b) / re + k.a * eps * eps / (2.0 * re3)) * force + (k.b / re3) * glm::dot(r, force) * r;
}

glm::dvec3 kelvinletAffine(const glm::dvec3& r, const glm::dmat3& F, double eps, double nu)
{
    const Kelvin k(nu);
    double re2 = glm::dot(r, r) + eps * eps;
    double re = std::sqrt(re2);
    double re3 = re2 * re, re5 = re3 * re2;
    double profile = 1.0 / re3 + 1.5 * eps * eps / re5;
    glm::dmat3 sym = F + glm::transpose(F) + trace(F) * glm::dmat3(1.0);
    return -k.a * profile * (F * r) + k.b * ((sym * r) / re3 - (3.0 / re5) * glm::dot(r, F * r) * r);
}

glm::dvec3 kelvinletGrabBrush(const glm::dvec3& r, const glm::dvec3& displacement, const KelvinletBrush& b)
{
    double eps[3], w[3];
    int n = brushScales(b, eps, w);
    const Kelvin k(b.poisson);
    double centre = 0.0;
    for (int i = 0; i < n; ++i)
        centre += w[i] * (1.5 * k.a - k.b) / eps[i];
    if (std::abs(centre) < 1e-300)
        return glm::dvec3(0.0);
    glm::dvec3 f = displacement / centre;
    glm::dvec3 u(0.0);
    for (int i = 0; i < n; ++i)
        u += w[i] * kelvinletGrab(r, f, eps[i], b.poisson);
    return u;
}

glm::dvec3 kelvinletTwistBrush(const glm::dvec3& r, const glm::dvec3& rotation, const KelvinletBrush& b)
{
    double angle = glm::length(rotation);
    if (angle < 1e-15)
        return glm::dvec3(0.0);
    return rotateBy(r, rotation / angle, angle * brushProfile(r, b)) - r;
}

glm::dvec3 kelvinletScaleBrush(const glm::dvec3& r, double amount, const KelvinletBrush& b) { return r * (amount * brushProfile(r, b)); }

glm::dvec3 kelvinletPinchBrush(const glm::dvec3& r, const glm::dmat3& G, const KelvinletBrush& b)
{
    double eps[3], w[3];
    int n = brushScales(b, eps, w);
    const Kelvin k(b.poisson);
    // The raw kernel's gradient at the centre is (2b - 5a/2) F / eps^3.
    double centre = 0.0;
    for (int i = 0; i < n; ++i)
        centre += w[i] * (2.0 * k.b - 2.5 * k.a) / (eps[i] * eps[i] * eps[i]);
    if (std::abs(centre) < 1e-300)
        return glm::dvec3(0.0);
    glm::dmat3 F = G / centre;
    glm::dvec3 u(0.0);
    for (int i = 0; i < n; ++i)
        u += w[i] * kelvinletAffine(r, F, eps[i], b.poisson);
    return u;
}

void polarDecompose(const glm::dmat3& M, glm::dmat3& R, glm::dmat3& S)
{
    Eigen::JacobiSVD<Eigen::Matrix3d> svd(toEigen(M), Eigen::ComputeFullU | Eigen::ComputeFullV);
    Eigen::Matrix3d U = svd.matrixU(), V = svd.matrixV();
    Eigen::Matrix3d r = U * V.transpose();
    if (r.determinant() < 0.0)
    {
        U.col(2) *= -1.0;
        r = U * V.transpose();
    }
    R = toGlm(r);
    glm::dmat3 s = glm::transpose(R) * M;
    S = (s + glm::transpose(s)) * 0.5;
}

KelvinletMotion kelvinletMotion(const glm::dmat3& M, const glm::dvec3& t, const KelvinletWeights& w)
{
    KelvinletMotion m;
    m.grab = t * w.grab;
    glm::dmat3 R, S;
    polarDecompose(M, R, S);
    glm::dquat q = glm::normalize(glm::quat_cast(R));
    if (q.w < 0.0)
        q = -q;
    glm::dvec3 v(q.x, q.y, q.z);
    double s = glm::length(v);
    if (s > 1e-15)
        m.twist = v / s * (2.0 * std::atan2(s, q.w)) * w.twist;
    glm::dmat3 E = S - glm::dmat3(1.0);
    double iso = trace(E) / 3.0;
    m.scale = iso * w.scale;
    m.pinch = (E - iso * glm::dmat3(1.0)) * w.pinch;
    return m;
}

glm::dvec3 kelvinletDisplacement(const glm::dvec3& r, const KelvinletMotion& m, const KelvinletBrush& b)
{
    glm::dvec3 u(0.0);
    if (glm::dot(m.grab, m.grab) > 0.0)
        u += kelvinletGrabBrush(r, m.grab, b);
    if (glm::dot(m.twist, m.twist) > 0.0)
        u += kelvinletTwistBrush(r, m.twist, b);
    if (m.scale != 0.0)
        u += kelvinletScaleBrush(r, m.scale, b);
    bool pinch = false;
    for (int c = 0; c < 3; ++c)
        pinch = pinch || glm::dot(m.pinch[c], m.pinch[c]) > 1e-30;
    if (pinch)
        u += kelvinletPinchBrush(r, m.pinch, b);
    return u;
}

// ------------------------------------------------------------------------------------------------ cage coordinates

void greenCoordinates(std::span<const glm::dvec3> cage, std::span<const uint32_t> triangles, const glm::dvec3& x, std::span<double> phi, std::span<double> psi)
{
    std::fill(phi.begin(), phi.end(), 0.0);
    std::fill(psi.begin(), psi.end(), 0.0);
    for (size_t t = 0; t + 2 < triangles.size(); t += 3)
    {
        const uint32_t id[3] = {triangles[t], triangles[t + 1], triangles[t + 2]};
        glm::dvec3 v[3] = {cage[id[0]] - x, cage[id[1]] - x, cage[id[2]] - x};
        glm::dvec3 nrm = glm::cross(v[1] - v[0], v[2] - v[0]);
        double len = glm::length(nrm);
        if (len < 1e-20)
            continue;
        nrm /= len;
        const glm::dvec3 p = glm::dot(v[0], nrm) * nrm;
        double s[3], I[3], II[3];
        glm::dvec3 N[3];
        for (int l = 0; l < 3; ++l)
        {
            const glm::dvec3& a = v[l];
            const glm::dvec3& b = v[(l + 1) % 3];
            // A projection on (or within rounding of) the edge's line makes a sliver that contributes nothing.
            const double side = glm::dot(glm::cross(a - p, b - p), nrm);
            s[l] = std::abs(side) > 1e-10 * glm::dot(b - a, b - a) ? sign(side) : 0.0;
            I[l] = s[l] != 0.0 ? gcTriInt(p, a, b, glm::dvec3(0.0)) : 0.0;
            II[l] = gcTriInt(glm::dvec3(0.0), b, a, glm::dvec3(0.0));
            glm::dvec3 q = glm::cross(b, a);
            double ql = glm::length(q);
            N[l] = ql > 1e-20 ? q / ql : glm::dvec3(0.0);
        }
        const double sumI = -std::abs(s[0] * I[0] + s[1] * I[1] + s[2] * I[2]);
        psi[t / 3] = -sumI;
        glm::dvec3 w = nrm * sumI + N[0] * II[0] + N[1] * II[1] + N[2] * II[2];
        if (glm::length(w) > 1e-12)
            for (int l = 0; l < 3; ++l)
            {
                const glm::dvec3& Nn = N[(l + 1) % 3];
                double d = glm::dot(Nn, v[l]);
                if (std::abs(d) > 1e-20)
                    phi[id[l]] += glm::dot(Nn, w) / d;
            }
    }
}

std::vector<glm::dvec3> greenFaceTerms(std::span<const glm::dvec3> rest, std::span<const glm::dvec3> deformed, std::span<const uint32_t> triangles)
{
    std::vector<glm::dvec3> out(triangles.size() / 3, glm::dvec3(0.0));
    for (size_t t = 0; t + 2 < triangles.size(); t += 3)
    {
        const uint32_t a = triangles[t], b = triangles[t + 1], c = triangles[t + 2];
        glm::dvec3 u1 = rest[b] - rest[a], u2 = rest[c] - rest[a];
        glm::dvec3 d1 = deformed[b] - deformed[a], d2 = deformed[c] - deformed[a];
        glm::dvec3 n0 = glm::cross(u1, u2), n1 = glm::cross(d1, d2);
        double area = 0.5 * glm::length(n0), l1 = glm::length(n1);
        if (area < 1e-20 || l1 < 1e-20)
            continue;
        double num = glm::dot(d1, d1) * glm::dot(u2, u2) - 2.0 * glm::dot(d1, d2) * glm::dot(u1, u2) + glm::dot(d2, d2) * glm::dot(u1, u1);
        double s = std::sqrt(std::max(num, 0.0)) / (std::sqrt(8.0) * area);
        out[t / 3] = n1 / l1 * s - n0 / (2.0 * area);
    }
    return out;
}

void somiglianaCoordinates(std::span<const glm::dvec3> cage, std::span<const uint32_t> triangles, const glm::dvec3& x, double nu,
                           std::span<double> vertexCoords, std::span<double> faceCoords)
{
    nu = std::clamp(nu, 0.0, 0.49);
    std::fill(vertexCoords.begin(), vertexCoords.end(), 0.0);
    std::fill(faceCoords.begin(), faceCoords.end(), 0.0);
    const size_t nv = cage.size();
    thread_local std::vector<glm::dmat3> C;
    C.assign(nv, glm::dmat3(0.0));
    const glm::dvec3 corners[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (size_t t = 0; t + 2 < triangles.size(); t += 3)
    {
        const uint32_t id[3] = {triangles[t], triangles[t + 1], triangles[t + 2]};
        const glm::dvec3 tri[3] = {cage[id[0]], cage[id[1]], cage[id[2]]};
        glm::dvec3 n = glm::cross(tri[1] - tri[0], tri[2] - tri[0]);
        double len = glm::length(n);
        if (len < 1e-20)
            continue;
        n /= len;
        glm::dmat3 corner[3] = {glm::dmat3(0.0), glm::dmat3(0.0), glm::dmat3(0.0)};
        glm::dmat3 face(0.0);
        somiglianaPiece(tri, n, x, nu, corners, 6, corner, face);
        for (int k = 0; k < 3; ++k)
            C[id[k]] += corner[k];
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r)
                faceCoords[(t / 3) * 9 + r * 3 + c] = face[c][r];
    }
    glm::dmat3 sum(0.0);
    for (const glm::dmat3& m : C)
        sum += m;
    glm::dmat3 norm = std::abs(glm::determinant(sum)) > 1e-12 ? glm::inverse(sum) : glm::dmat3(1.0);
    for (size_t v = 0; v < nv; ++v)
    {
        glm::dmat3 m = norm * C[v];
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r)
                vertexCoords[v * 9 + r * 3 + c] = m[c][r];
    }
}

namespace
{
/** Least-squares linear part of the map rest[ids] -> moved[ids]; false when the points are too flat to tell. */
bool affineFit(std::span<const glm::dvec3> rest, std::span<const glm::dvec3> moved, std::span<const uint32_t> ids, glm::dmat3& A)
{
    glm::dvec3 c(0.0), d(0.0);
    for (uint32_t i : ids)
    {
        c += rest[i];
        d += moved[i];
    }
    c /= double(ids.size());
    d /= double(ids.size());
    glm::dmat3 M(0.0), N(0.0);
    for (uint32_t i : ids)
    {
        M += outer(rest[i] - c, rest[i] - c);
        N += outer(moved[i] - d, rest[i] - c);
    }
    double scale = trace(M);
    if (ids.size() < 4 || scale <= 0.0 || std::abs(glm::determinant(M)) < 1e-9 * scale * scale * scale)
        return false;
    A = N * glm::inverse(M);
    return true;
}
} // namespace

std::vector<glm::dvec3> somiglianaTractions(std::span<const glm::dvec3> rest, std::span<const glm::dvec3> deformed, std::span<const uint32_t> triangles,
                                            const glm::dmat3& rot, const glm::dvec3& trans, const SomiglianaParams& p)
{
    const double nu = std::clamp(p.poisson, 0.0, 0.49);
    const double lambda = 2.0 * nu / (1.0 - 2.0 * nu);
    const glm::dmat3 rotT = glm::transpose(rot);
    const size_t nv = std::min(rest.size(), deformed.size());
    std::vector<glm::dvec3> local(nv);
    for (size_t i = 0; i < nv; ++i)
        local[i] = rotT * (deformed[i] - trans);
    std::vector<std::vector<uint32_t>> ring(nv);
    for (size_t t = 0; t + 2 < triangles.size(); t += 3)
        for (int k = 0; k < 3; ++k)
            for (int m = 0; m < 3; ++m)
                ring[triangles[t + k]].push_back(triangles[t + m]);
    std::vector<uint32_t> all(nv);
    for (size_t i = 0; i < nv; ++i)
        all[i] = uint32_t(i);
    glm::dmat3 global(1.0);
    const bool haveGlobal = affineFit(rest, local, all, global);

    std::vector<glm::dvec3> out(triangles.size() / 3, glm::dvec3(0.0));
    std::vector<uint32_t> ids;
    for (size_t t = 0; t + 2 < triangles.size(); t += 3)
    {
        const uint32_t a = triangles[t], b = triangles[t + 1], c = triangles[t + 2];
        glm::dvec3 e1 = rest[b] - rest[a], e2 = rest[c] - rest[a];
        glm::dvec3 n = glm::cross(e1, e2);
        double len = glm::length(n);
        if (len < 1e-20)
            continue;
        n /= len;
        glm::dvec3 f1 = local[b] - local[a], f2 = local[c] - local[a];
        // The cage says nothing about stretch across a face, so it comes from the affine fit of the face's neighbourhood.
        ids.clear();
        for (uint32_t v : {a, b, c})
            ids.insert(ids.end(), ring[v].begin(), ring[v].end());
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        glm::dmat3 A;
        glm::dvec3 across;
        if (affineFit(rest, local, ids, A))
            across = A * n;
        else if (haveGlobal)
            across = global * n;
        else
            across = glm::normalize(glm::cross(f1, f2));
        glm::dmat3 F = glm::dmat3(f1, f2, across) * glm::inverse(glm::dmat3(e1, e2, n));
        glm::dmat3 R, S;
        polarDecompose(F, R, S);
        glm::dmat3 E = S - glm::dmat3(1.0);
        double inPlane = trace(E) - glm::dot(n, E * n);
        E -= p.bulge * inPlane * outer(n, n);
        glm::dmat3 sigma = lambda * trace(E) * glm::dmat3(1.0) + 2.0 * E;
        out[t / 3] = R * (sigma * n);
    }
    return out;
}

namespace
{
bool rayHitsTriangle(const glm::dvec3& o, const glm::dvec3& d, const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c)
{
    glm::dvec3 e1 = b - a, e2 = c - a;
    glm::dvec3 p = glm::cross(d, e2);
    double det = glm::dot(e1, p);
    if (std::abs(det) < 1e-20)
        return false;
    double inv = 1.0 / det;
    glm::dvec3 t = o - a;
    double u = glm::dot(t, p) * inv;
    if (u < 0.0 || u > 1.0)
        return false;
    glm::dvec3 q = glm::cross(t, e1);
    double v = glm::dot(d, q) * inv;
    if (v < 0.0 || u + v > 1.0)
        return false;
    return glm::dot(e2, q) * inv > 0.0;
}
} // namespace

std::vector<float> harmonicCoordinates(std::span<const glm::dvec3> cage, std::span<const uint32_t> triangles, std::span<const glm::dvec3> points,
                                       int resolution)
{
    LILA_ZONE();

    const size_t nv = cage.size(), nt = triangles.size() / 3;
    std::vector<float> out(points.size() * nv, 0.0f);
    if (nv == 0 || nt == 0)
        return out;
    glm::dvec3 lo(1e300), hi(-1e300);
    for (const glm::dvec3& v : cage)
    {
        lo = glm::min(lo, v);
        hi = glm::max(hi, v);
    }
    const double h = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z, 1e-9}) / double(std::clamp(resolution, 4, 256));
    lo -= glm::dvec3(2.0 * h);
    const glm::ivec3 dim = glm::ivec3(glm::ceil((hi - lo) / h)) + glm::ivec3(3);
    const size_t nodes = size_t(dim.x) * size_t(dim.y) * size_t(dim.z);
    auto at = [&](int i, int j, int k) { return size_t(i) + size_t(dim.x) * (size_t(j) + size_t(dim.y) * size_t(k)); };
    auto coord = [&](size_t n) {
        return glm::ivec3(int(n % size_t(dim.x)), int((n / size_t(dim.x)) % size_t(dim.y)), int(n / (size_t(dim.x) * size_t(dim.y))));
    };
    auto pos = [&](const glm::ivec3& c) { return lo + glm::dvec3(c) * h; };

    // Inside by ray parity, along a direction no grid line or cage edge is likely to graze.
    const glm::dvec3 dir = glm::normalize(glm::dvec3(1.0, 1.31e-3, 2.17e-3));
    std::vector<char> inside(nodes, 0);
    tbb::parallel_for(0, dim.z, [&](int k) {
        for (int j = 0; j < dim.y; ++j)
            for (int i = 0; i < dim.x; ++i)
            {
                glm::dvec3 o = pos({i, j, k});
                int hits = 0;
                for (size_t t = 0; t < nt; ++t)
                    hits += rayHitsTriangle(o, dir, cage[triangles[t * 3]], cage[triangles[t * 3 + 1]], cage[triangles[t * 3 + 2]]);
                inside[at(i, j, k)] = char(hits & 1);
            }
    });

    // Unknowns are the inside nodes; their outside neighbours are the boundary, valued by the cage's hat functions.
    std::vector<int> unknown(nodes, -1), boundary(nodes, -1);
    std::vector<size_t> unknownNode, boundaryNode;
    const glm::ivec3 steps[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (int k = 1; k + 1 < dim.z; ++k)
        for (int j = 1; j + 1 < dim.y; ++j)
            for (int i = 1; i + 1 < dim.x; ++i)
            {
                size_t n = at(i, j, k);
                if (!inside[n])
                    continue;
                unknown[n] = int(unknownNode.size());
                unknownNode.push_back(n);
                for (const glm::ivec3& s : steps)
                {
                    size_t m = at(i + s.x, j + s.y, k + s.z);
                    if (!inside[m] && boundary[m] < 0)
                    {
                        boundary[m] = int(boundaryNode.size());
                        boundaryNode.push_back(m);
                    }
                }
            }
    std::vector<std::array<std::pair<uint32_t, double>, 3>> hat(boundaryNode.size());
    tbb::parallel_for(size_t(0), boundaryNode.size(), [&](size_t b) {
        glm::dvec3 p = pos(coord(boundaryNode[b]));
        double best = 1e300;
        for (size_t t = 0; t < nt; ++t)
        {
            const uint32_t* id = &triangles[t * 3];
            glm::dvec3 bary;
            double d = glm::length(closestOnTriangle(p, cage[id[0]], cage[id[1]], cage[id[2]], bary) - p);
            if (d < best)
            {
                best = d;
                hat[b] = {{{id[0], bary.x}, {id[1], bary.y}, {id[2], bary.z}}};
            }
        }
    });

    const int nu = int(unknownNode.size());
    Eigen::MatrixXd solved;
    if (nu > 0)
    {
        Eigen::MatrixXd rhs = Eigen::MatrixXd::Zero(nu, Eigen::Index(nv));
        std::vector<Eigen::Triplet<double>> trip;
        trip.reserve(size_t(nu) * 7);
        for (int u = 0; u < nu; ++u)
        {
            glm::ivec3 c = coord(unknownNode[size_t(u)]);
            trip.emplace_back(u, u, 6.0);
            for (const glm::ivec3& s : steps)
            {
                size_t m = at(c.x + s.x, c.y + s.y, c.z + s.z);
                if (unknown[m] >= 0)
                    trip.emplace_back(u, unknown[m], -1.0);
                else if (boundary[m] >= 0)
                    for (const auto& [v, w] : hat[size_t(boundary[m])])
                        rhs(u, Eigen::Index(v)) += w;
            }
        }
        Eigen::SparseMatrix<double> A(nu, nu);
        A.setFromTriplets(trip.begin(), trip.end());
        Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> solver(A);
        if (solver.info() == Eigen::Success)
            solved = solver.solve(rhs);
    }

    // A grid node's coordinates: solved inside, hat functions on the boundary, none elsewhere.
    auto nodeRow = [&](size_t n, std::vector<double>& row) {
        std::fill(row.begin(), row.end(), 0.0);
        if (unknown[n] >= 0 && solved.size())
        {
            for (size_t v = 0; v < nv; ++v)
                row[v] = solved(unknown[n], Eigen::Index(v));
            return true;
        }
        if (boundary[n] >= 0)
        {
            for (const auto& [v, w] : hat[size_t(boundary[n])])
                row[v] += w;
            return true;
        }
        return false;
    };
    tbb::parallel_for(tbb::blocked_range<size_t>(0, points.size(), 64), [&](const tbb::blocked_range<size_t>& r) {
        std::vector<double> row(nv), acc(nv);
        for (size_t p = r.begin(); p != r.end(); ++p)
        {
            glm::dvec3 g = glm::clamp((points[p] - lo) / h, glm::dvec3(0.0), glm::dvec3(dim - glm::ivec3(2)));
            glm::ivec3 c = glm::clamp(glm::ivec3(glm::floor(g)), glm::ivec3(0), dim - glm::ivec3(2));
            glm::dvec3 f = g - glm::dvec3(c);
            std::fill(acc.begin(), acc.end(), 0.0);
            double total = 0.0;
            for (int corner = 0; corner < 8; ++corner)
            {
                glm::ivec3 o(corner & 1, (corner >> 1) & 1, (corner >> 2) & 1);
                double w = (o.x ? f.x : 1.0 - f.x) * (o.y ? f.y : 1.0 - f.y) * (o.z ? f.z : 1.0 - f.z);
                if (w <= 0.0 || !nodeRow(at(c.x + o.x, c.y + o.y, c.z + o.z), row))
                    continue;
                for (size_t v = 0; v < nv; ++v)
                    acc[v] += row[v] * w;
                total += w;
            }
            if (total <= 0.0)
            {
                // Away from the cage: the nearest boundary node's hat functions.
                double best = 1e300;
                size_t pick = 0;
                for (size_t n : boundaryNode)
                    if (double d = glm::length(pos(coord(n)) - points[p]); d < best)
                    {
                        best = d;
                        pick = n;
                    }
                if (boundaryNode.empty() || !nodeRow(pick, acc))
                    continue;
            }
            double sum = 0.0;
            for (double v : acc)
                sum += v;
            if (std::abs(sum) < 1e-12)
                continue;
            for (size_t v = 0; v < nv; ++v)
                out[p * nv + v] = float(acc[v] / sum);
        }
    });
    return out;
}

// ------------------------------------------------------------------------------------------------ simple set

glm::dvec3 shear(const glm::dvec3& p, const ShearParams& o)
{
    double y = std::clamp(p.y, std::min(o.low, o.high), std::max(o.low, o.high));
    return {p.x + o.shearX * y, p.y, p.z + o.shearZ * y};
}

void cast(std::vector<glm::dvec3>& points, CastShape shape, double factor, double radius)
{
    auto measure = [shape](const glm::dvec3& p) {
        switch (shape)
        {
        case CastShape::Sphere:
            return glm::length(p);
        case CastShape::Cylinder:
            return std::sqrt(p.x * p.x + p.z * p.z);
        case CastShape::Box:
            return std::max({std::abs(p.x), std::abs(p.y), std::abs(p.z)});
        }
        return 0.0;
    };
    if (radius <= 0.0)
    {
        double sum = 0.0;
        for (const glm::dvec3& p : points)
            sum += measure(p);
        radius = points.empty() ? 0.0 : sum / double(points.size());
    }
    for (glm::dvec3& p : points)
    {
        double m = measure(p);
        if (m < 1e-12)
            continue;
        glm::dvec3 target = p;
        if (shape == CastShape::Cylinder)
        {
            target.x = p.x * radius / m;
            target.z = p.z * radius / m;
        }
        else
            target = p * (radius / m);
        p += (target - p) * factor;
    }
}

void smoothTaubin(std::vector<glm::dvec3>& points, const Adjacency& adj, int iterations, double lambda, double passBand, bool pinBoundary)
{
    lambda = std::clamp(lambda, 1e-6, 1.0);
    const double mu = 1.0 / (std::clamp(passBand, 0.0, 0.5) - 1.0 / lambda);
    std::vector<glm::dvec3> next(points.size());
    auto pass = [&](double step) {
        tbb::parallel_for(tbb::blocked_range<size_t>(0, points.size(), 512), [&](const tbb::blocked_range<size_t>& r) {
            for (size_t p = r.begin(); p != r.end(); ++p)
            {
                const uint32_t b = adj.offsets[p], e = adj.offsets[p + 1];
                if (b == e || (pinBoundary && adj.boundary[p]))
                {
                    next[p] = points[p];
                    continue;
                }
                glm::dvec3 avg(0.0);
                for (uint32_t k = b; k < e; ++k)
                    avg += points[adj.neighbors[k]];
                avg /= double(e - b);
                next[p] = points[p] + (avg - points[p]) * step;
            }
        });
        points.swap(next);
    };
    for (int i = 0; i < iterations; ++i)
    {
        pass(lambda);
        pass(mu);
    }
}

namespace
{
uint32_t hash3(int x, int y, int z, uint32_t seed)
{
    uint32_t h = seed * 0x9E3779B1u;
    h ^= uint32_t(x) * 0x85EBCA77u;
    h = (h << 13) | (h >> 19);
    h ^= uint32_t(y) * 0xC2B2AE3Du;
    h = (h << 17) | (h >> 15);
    h ^= uint32_t(z) * 0x27D4EB2Fu;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

double grad(uint32_t h, double x, double y, double z)
{
    // The twelve cube-edge directions of improved Perlin noise.
    switch (h % 12)
    {
    case 0: return x + y;
    case 1: return -x + y;
    case 2: return x - y;
    case 3: return -x - y;
    case 4: return x + z;
    case 5: return -x + z;
    case 6: return x - z;
    case 7: return -x - z;
    case 8: return y + z;
    case 9: return -y + z;
    case 10: return y - z;
    default: return -y - z;
    }
}

double fade(double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }
} // namespace

double noise3(const glm::dvec3& p, uint32_t seed)
{
    glm::dvec3 f = glm::floor(p);
    int x = int(f.x), y = int(f.y), z = int(f.z);
    glm::dvec3 d = p - f;
    double u = fade(d.x), v = fade(d.y), w = fade(d.z);
    auto g = [&](int i, int j, int k) { return grad(hash3(x + i, y + j, z + k, seed), d.x - i, d.y - j, d.z - k); };
    double x00 = glm::mix(g(0, 0, 0), g(1, 0, 0), u), x10 = glm::mix(g(0, 1, 0), g(1, 1, 0), u);
    double x01 = glm::mix(g(0, 0, 1), g(1, 0, 1), u), x11 = glm::mix(g(0, 1, 1), g(1, 1, 1), u);
    return glm::mix(glm::mix(x00, x10, v), glm::mix(x01, x11, v), w);
}

double fractalNoise(const glm::dvec3& p, uint32_t seed, int octaves)
{
    octaves = std::clamp(octaves, 1, 12);
    double sum = 0.0, amp = 1.0, norm = 0.0, freq = 1.0;
    for (int o = 0; o < octaves; ++o)
    {
        sum += noise3(p * freq, seed + uint32_t(o) * 1013u) * amp;
        norm += amp;
        amp *= 0.5;
        freq *= 2.0;
    }
    return sum / norm;
}

glm::dvec3 fractalNoise3(const glm::dvec3& p, uint32_t seed, int octaves)
{
    return {fractalNoise(p, seed, octaves), fractalNoise(p + glm::dvec3(31.416, 47.853, 12.679), seed + 7919u, octaves),
            fractalNoise(p + glm::dvec3(-17.31, 5.27, 63.11), seed + 15887u, octaves)};
}

// ------------------------------------------------------------------------------------------------ tension

std::vector<glm::vec2> directionalStrain(const geo::Mesh& topology, std::span<const glm::dvec3> rest, std::span<const glm::dvec3> deformed,
                                         std::string_view uvSet)
{
    const size_t n = std::min(rest.size(), deformed.size());
    std::vector<glm::vec2> out(n, glm::vec2(0.f));
    const std::span<const glm::vec2> uv = topology.cornerAttrs().get<glm::vec2>(uvSet);
    if (uv.empty())
        return out;
    std::vector<glm::dvec2> sum(n, glm::dvec2(0.0));
    std::vector<double> weight(n, 0.0);
    for (geo::Index f = 0; f < topology.faceCapacity(); ++f)
    {
        if (!topology.faceValid(f))
            continue;
        const std::vector<geo::Index> c = topology.faceCorners(f);
        for (size_t k = 1; k + 1 < c.size(); ++k)
        {
            const geo::Index ca[3] = {c[0], c[k], c[k + 1]};
            geo::Index p[3];
            bool ok = true;
            for (int i = 0; i < 3; ++i)
            {
                p[i] = topology.cornerPoint(ca[i]);
                ok = ok && p[i] < n;
            }
            if (!ok)
                continue;
            const glm::dvec2 du1 = glm::dvec2(uv[ca[1]] - uv[ca[0]]), du2 = glm::dvec2(uv[ca[2]] - uv[ca[0]]);
            const double det = du1.x * du2.y - du2.x * du1.y;
            if (std::abs(det) < 1e-14)
                continue;
            // The 3D directions uv's U and V run along, at rest and now: columns of each shape's gradient against uv.
            auto gradient = [&](std::span<const glm::dvec3> x, glm::dvec3& gu, glm::dvec3& gv) {
                const glm::dvec3 e1 = x[p[1]] - x[p[0]], e2 = x[p[2]] - x[p[0]];
                gu = (e1 * du2.y - e2 * du1.y) / det;
                gv = (e2 * du1.x - e1 * du2.x) / det;
            };
            glm::dvec3 ru, rv, du, dv;
            gradient(rest, ru, rv);
            gradient(deformed, du, dv);
            const double lu = glm::length(ru), lv = glm::length(rv);
            if (lu < 1e-12 || lv < 1e-12)
                continue;
            const glm::dvec2 s(glm::length(du) / lu - 1.0, glm::length(dv) / lv - 1.0);
            const double area = 0.5 * glm::length(glm::cross(rest[p[1]] - rest[p[0]], rest[p[2]] - rest[p[0]]));
            for (geo::Index q : p)
            {
                sum[q] += s * area;
                weight[q] += area;
            }
        }
    }
    for (size_t q = 0; q < n; ++q)
        if (weight[q] > 0.0)
            out[q] = glm::vec2(sum[q] / weight[q]);
    return out;
}

std::vector<float> tension(const geo::Mesh& topology, std::span<const glm::dvec3> rest, std::span<const glm::dvec3> deformed)
{
    const size_t n = std::min(rest.size(), deformed.size());
    std::vector<double> sum(n, 0.0);
    std::vector<uint32_t> count(n, 0);
    for (geo::Index e = 0; e < topology.edgeCapacity(); ++e)
    {
        if (!topology.edgeValid(e))
            continue;
        geo::Index a = topology.edgePoint(e, 0), b = topology.edgePoint(e, 1);
        if (a >= n || b >= n)
            continue;
        double r = glm::length(rest[a] - rest[b]);
        if (r < 1e-12)
            continue;
        double s = glm::length(deformed[a] - deformed[b]) / r - 1.0;
        sum[a] += s;
        sum[b] += s;
        ++count[a];
        ++count[b];
    }
    std::vector<float> out(n, 0.0f);
    for (size_t p = 0; p < n; ++p)
        if (count[p])
            out[p] = float(sum[p] / count[p]);
    return out;
}
} // namespace lila::deform
