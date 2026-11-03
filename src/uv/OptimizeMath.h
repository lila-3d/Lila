#pragma once

#include "uv/Chart.h"

#include <Eigen/SparseCore>
#include <glm/glm.hpp>

#include <cmath>
#include <vector>

/** Per-triangle geometry and sparse types shared by the uv solvers: local frames, Jacobians and 2x2 SVDs. */
namespace lila::uv::detail
{
using Triplet = Eigen::Triplet<double>;
using SpMat = Eigen::SparseMatrix<double>;

/** Gradient basis of a triangle in its own plane: grad f = sum f_j * g[j]. */
struct TriFrame
{
    glm::dvec2 g[3];
    double area = 0.0;
};

inline TriFrame triFrame(const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c)
{
    TriFrame t;
    glm::dvec3 e1 = b - a, e2 = c - a;
    double l1 = glm::length(e1);
    if (l1 <= 0.0)
        return t;
    glm::dvec3 x = e1 / l1;
    double px = glm::dot(e2, x);
    double py = glm::length(e2 - px * x);
    glm::dvec2 p[3] = {{0.0, 0.0}, {l1, 0.0}, {px, py}};
    t.area = 0.5 * l1 * py;
    if (t.area <= 0.0)
        return t;
    for (int j = 0; j < 3; ++j)
    {
        glm::dvec2 e = p[(j + 2) % 3] - p[(j + 1) % 3];
        t.g[j] = glm::dvec2(-e.y, e.x) / (2.0 * t.area);
    }
    return t;
}

inline std::vector<TriFrame> triFrames(const Chart& ch)
{
    std::vector<TriFrame> out(ch.tris.size());
    for (size_t i = 0; i < ch.tris.size(); ++i)
        out[i] = triFrame(ch.pos[ch.tris[i][0]], ch.pos[ch.tris[i][1]], ch.pos[ch.tris[i][2]]);
    return out;
}

/** [a b; c d]: rows are grad u and grad v. */
struct Jac2
{
    double a = 1.0, b = 0.0, c = 0.0, d = 1.0;
    double det() const { return a * d - b * c; }
};

inline Jac2 jacobian(const TriFrame& f, const glm::dvec2& u0, const glm::dvec2& u1, const glm::dvec2& u2)
{
    glm::dvec2 gu = u0.x * f.g[0] + u1.x * f.g[1] + u2.x * f.g[2];
    glm::dvec2 gv = u0.y * f.g[0] + u1.y * f.g[1] + u2.y * f.g[2];
    return {gu.x, gu.y, gv.x, gv.y};
}

/** Signed 2x2 SVD: M = Rot(phi) * diag(s1, s2) * Rot(theta), s1 >= |s2|; s2 < 0 when M flips. */
struct Svd2
{
    double s1, s2, phi, theta;
};

inline Svd2 svd2(const Jac2& m)
{
    double e = (m.a + m.d) * 0.5, f = (m.a - m.d) * 0.5, g = (m.c + m.b) * 0.5, h = (m.c - m.b) * 0.5;
    double q = std::sqrt(e * e + h * h), r = std::sqrt(f * f + g * g);
    double a1 = std::atan2(g, f), a2 = std::atan2(h, e);
    return {q + r, q - r, (a2 + a1) * 0.5, (a2 - a1) * 0.5};
}

/** Rot(phi) * diag(t1, t2) * Rot(theta). */
inline Jac2 compose(const Svd2& s, double t1, double t2)
{
    double cp = std::cos(s.phi), sp = std::sin(s.phi), ct = std::cos(s.theta), st = std::sin(s.theta);
    // Rot(phi) * diag(t1, t2)
    double m00 = cp * t1, m01 = -sp * t2, m10 = sp * t1, m11 = cp * t2;
    return {m00 * ct + m01 * st, -m00 * st + m01 * ct, m10 * ct + m11 * st, -m10 * st + m11 * ct};
}

inline double signedArea2(const glm::dvec2& a, const glm::dvec2& b, const glm::dvec2& c)
{
    glm::dvec2 e1 = b - a, e2 = c - a;
    return 0.5 * (e1.x * e2.y - e1.y * e2.x);
}

/**
 * Largest t in (0, 1] (times a safety factor) such that moving uv by t * dir flips no triangle that is
 * currently positive. dir and uv are per chart vertex.
 */
inline double maxFlipFreeStep(const Chart& ch, const std::vector<TriFrame>& fr, const std::vector<glm::dvec2>& uv,
                              const std::vector<glm::dvec2>& dir)
{
    double tMax = 1.0;
    for (size_t i = 0; i < ch.tris.size(); ++i)
    {
        if (fr[i].area <= 0.0)
            continue;
        const auto& t = ch.tris[i];
        Jac2 j0 = jacobian(fr[i], uv[t[0]], uv[t[1]], uv[t[2]]);
        if (j0.det() <= 0.0)
            continue;
        Jac2 jd = jacobian(fr[i], dir[t[0]], dir[t[1]], dir[t[2]]);
        double qa = jd.det();
        double qb = j0.a * jd.d + j0.d * jd.a - j0.b * jd.c - j0.c * jd.b;
        double qc = j0.det();
        double root = 1e30;
        if (std::abs(qa) < 1e-14 * std::max(1.0, std::abs(qc)))
        {
            if (qb < 0.0)
                root = -qc / qb;
        }
        else
        {
            double disc = qb * qb - 4.0 * qa * qc;
            if (disc >= 0.0)
            {
                double sq = std::sqrt(disc);
                double r1 = (-qb - sq) / (2.0 * qa), r2 = (-qb + sq) / (2.0 * qa);
                if (r1 > 0.0)
                    root = std::min(root, r1);
                if (r2 > 0.0)
                    root = std::min(root, r2);
            }
        }
        tMax = std::min(tMax, 0.9 * root);
    }
    return tMax;
}
} // namespace lila::uv::detail
