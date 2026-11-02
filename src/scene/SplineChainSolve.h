#pragma once

// The spline chain's solve, free of the scene: where each joint goes along a rail and the frame it takes there.

#include "curve/Along.h"
#include "scene/RigMath.h"
#include "scene/SplineChain.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace lila::scene::spline::solve
{
/** What a chain remembers of its rest pose, one entry per joint. */
struct Rest
{
    std::vector<double> s;            // arc length along the curve
    std::vector<double> chord;        // distance from the previous joint's station (first unused)
    std::vector<double> bone;         // translate length (first unused)
    std::vector<glm::dmat3> offset;   // joint rotation in its curve frame
    double curveLength = 0.0;
};

struct Params
{
    LengthMode mode = LengthMode::Stretch;
    double slide = 0.0;
    double minStretch = 0.0, maxStretch = 1e9;
    double twistStart = 0.0, twistEnd = 0.0, roll = 0.0; // degrees
    int64_t ramp = 0;                                    // linear, easeIn, easeOut, smooth
    /** World up at each end (upObjects); the frames twist to follow them. */
    std::optional<glm::dvec3> upStart, upEnd;
    double spread = 1.0;
};

struct Solved
{
    std::vector<glm::dvec3> p;
    /** x along the chain toward the next joint (the curve's tangent at the last), y up. */
    std::vector<glm::dmat3> frame;
    /** The length of the stretch of chain starting at each joint over its rest length (the last joint's incoming one). */
    std::vector<double> ratio;
};

inline double ramp(int64_t kind, double u)
{
    u = std::clamp(u, 0.0, 1.0);
    switch (kind)
    {
    case 1: return u * u;
    case 2: return 1.0 - (1.0 - u) * (1.0 - u);
    case 3: return u * u * (3.0 - 2.0 * u);
    default: return u;
    }
}

/** The arc length past s0 whose point lies c from p0. */
inline double walk(const curve::Rail& r, double s0, const glm::dvec3& p0, double c)
{
    if (c <= 1e-12)
        return s0;
    auto dist = [&](double s) { return glm::length(r.at(s).p - p0); };
    double lo = s0, hi = s0 + c;
    for (int k = 0; k < 64 && dist(hi) < c; ++k)
    {
        lo = hi;
        hi += c;
    }
    for (int k = 0; k < 60; ++k)
    {
        const double mid = 0.5 * (lo + hi);
        (dist(mid) < c ? lo : hi) = mid;
    }
    return 0.5 * (lo + hi);
}

inline glm::dvec3 across(const glm::dvec3& v, const glm::dvec3& axis)
{
    return v - axis * glm::dot(v, axis);
}

inline Solved solve(const curve::Rail& rail, const Params& p, const Rest& rest)
{
    Solved out;
    const size_t n = rest.s.size();
    if (n == 0 || !rail.valid())
        return out;
    std::vector<double> s(n);
    out.p.resize(n);
    const double start = rest.s[0] + p.slide;
    if (p.mode == LengthMode::KeepLength)
    {
        s[0] = start;
        out.p[0] = rail.at(start).p;
        for (size_t i = 1; i < n; ++i)
        {
            s[i] = walk(rail, s[i - 1], out.p[i - 1], rest.chord[i]);
            out.p[i] = rail.at(s[i]).p;
        }
    }
    else
    {
        const double f = rest.curveLength > 1e-9 ? std::clamp(rail.length() / rest.curveLength, p.minStretch, p.maxStretch) : 1.0;
        for (size_t i = 0; i < n; ++i)
        {
            s[i] = start + (rest.s[i] - rest.s[0]) * f;
            out.p[i] = rail.at(s[i]).p;
        }
    }

    // Frames before twist: x toward the next joint, y the curve's normal across it.
    std::vector<glm::dvec3> x(n), y(n);
    for (size_t i = 0; i < n; ++i)
    {
        const curve::Rail::Station st = rail.at(s[i]);
        glm::dvec3 aim = i + 1 < n ? out.p[i + 1] - out.p[i] : st.f.t;
        aim = glm::length(aim) > 1e-12 ? glm::normalize(aim) : st.f.t;
        glm::dvec3 up = across(st.f.n, aim);
        if (glm::length(up) < 1e-9)
            up = across(st.f.b, aim);
        if (glm::length(up) < 1e-9)
            up = across(std::abs(aim.x) < 0.9 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 1, 0), aim);
        x[i] = aim;
        y[i] = glm::normalize(up);
    }

    double objStart = 0.0, objDiff = 0.0;
    if (p.upStart && p.upEnd)
    {
        objStart = rigmath::signedAngle(y.front(), *p.upStart, x.front());
        const double objEnd = rigmath::signedAngle(y.back(), *p.upEnd, x.back());
        objDiff = std::remainder(objEnd - objStart, 360.0);
    }
    const double span = rest.s[n - 1] - rest.s[0];
    out.frame.resize(n);
    for (size_t i = 0; i < n; ++i)
    {
        const double u = span > 1e-9 ? (rest.s[i] - rest.s[0]) / span : 0.0;
        const double r = ramp(p.ramp, u);
        const double twist = p.twistStart + (p.twistEnd - p.twistStart) * r + p.roll + objStart + objDiff * r * p.spread;
        const double a = rigmath::radians(twist), c = std::cos(a), sn = std::sin(a);
        const glm::dvec3 z = glm::cross(x[i], y[i]);
        const glm::dvec3 yy = c * y[i] + sn * z;
        out.frame[i] = glm::dmat3(x[i], yy, glm::cross(x[i], yy));
    }

    out.ratio.assign(n, 1.0);
    for (size_t i = 0; i + 1 < n; ++i)
        if (rest.chord[i + 1] > 1e-9)
            out.ratio[i] = glm::length(out.p[i + 1] - out.p[i]) / rest.chord[i + 1];
    if (n >= 2)
        out.ratio[n - 1] = out.ratio[n - 2];
    return out;
}
} // namespace lila::scene::spline::solve
