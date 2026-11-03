#pragma once

#include <glm/glm.hpp>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <cstddef>

namespace lila::deform
{
/** Runs fn(i) for every point index in [0, n) across TBB workers. */
template <class F> void parallelPoints(size_t n, F&& fn)
{
    tbb::parallel_for(tbb::blocked_range<size_t>(0, n, 256), [&](const tbb::blocked_range<size_t>& r) {
        for (size_t i = r.begin(); i != r.end(); ++i)
            fn(i);
    });
}

/** Closest point on triangle abc to p, with its barycentrics (Ericson, Real-Time Collision Detection 5.1.5). */
inline glm::dvec3 closestOnTriangle(const glm::dvec3& p, const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c, glm::dvec3& bary)
{
    glm::dvec3 ab = b - a, ac = c - a, ap = p - a;
    double d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
    if (d1 <= 0.0 && d2 <= 0.0)
        return bary = {1, 0, 0}, a;
    glm::dvec3 bp = p - b;
    double d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
    if (d3 >= 0.0 && d4 <= d3)
        return bary = {0, 1, 0}, b;
    double vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0)
    {
        double v = d1 / (d1 - d3);
        bary = {1 - v, v, 0};
        return a + ab * v;
    }
    glm::dvec3 cp = p - c;
    double d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
    if (d6 >= 0.0 && d5 <= d6)
        return bary = {0, 0, 1}, c;
    double vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0)
    {
        double w = d2 / (d2 - d6);
        bary = {1 - w, 0, w};
        return a + ac * w;
    }
    double va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0)
    {
        double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        bary = {0, 1 - w, w};
        return b + (c - b) * w;
    }
    double denom = 1.0 / (va + vb + vc);
    double v = vb * denom, w = vc * denom;
    bary = {1 - v - w, v, w};
    return a + ab * v + ac * w;
}

/** Rigid frame of triangle abc: x along ab, z along the normal. Identity for a degenerate triangle. */
inline glm::dmat3 triangleFrame(const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c)
{
    glm::dvec3 x = b - a;
    glm::dvec3 n = glm::cross(b - a, c - a);
    double lx = glm::length(x), ln = glm::length(n);
    if (lx < 1e-15 || ln < 1e-15)
        return glm::dmat3(1.0);
    x /= lx;
    n /= ln;
    return {x, glm::cross(n, x), n};
}
} // namespace lila::deform
