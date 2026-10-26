#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace lila::geo
{
/**
 * The per-copy transform maths shared by the array modifier, the live array group and duplicate count=. Matrices are
 * column vectors.
 */

/** Scale, then Euler XYZ rotation (degrees, X applied first), then translate. */
inline glm::dmat4 trsMatrix(const glm::dvec3& translate, const glm::dvec3& rotate, const glm::dvec3& scale)
{
    glm::dmat4 m = glm::translate(glm::dmat4(1.0), translate);
    m = glm::rotate(m, glm::radians(rotate.z), glm::dvec3(0.0, 0.0, 1.0));
    m = glm::rotate(m, glm::radians(rotate.y), glm::dvec3(0.0, 1.0, 0.0));
    m = glm::rotate(m, glm::radians(rotate.x), glm::dvec3(1.0, 0.0, 0.0));
    return glm::scale(m, scale);
}

/** `local` applied in `frame`'s space (its origin the pivot, its axes the step's axes): frame * local * frame^-1. */
inline glm::dmat4 aboutFrame(const glm::dmat4& frame, const glm::dmat4& local) { return frame * local * glm::inverse(frame); }

/** A frame from a pivot point and axes (the columns of `axes`). */
inline glm::dmat4 pivotFrame(const glm::dvec3& pivot, const glm::dmat3& axes = glm::dmat3(1.0))
{
    glm::dmat4 f(axes);
    f[3] = glm::dvec4(pivot, 1.0);
    return f;
}

/** step^1 * base .. step^count * base: each repeat applies the step once more on the left. */
inline std::vector<glm::dmat4> repeatMatrices(const glm::dmat4& base, const glm::dmat4& step, int count)
{
    std::vector<glm::dmat4> out;
    out.reserve(size_t(std::max(count, 0)));
    glm::dmat4 cur = base;
    for (int i = 0; i < count; ++i)
    {
        cur = step * cur;
        out.push_back(cur);
    }
    return out;
}

enum class ArrayMode
{
    Linear,
    Radial,
    Grid,
};

/**
 * Where array copies go, in the space of the mesh being repeated. Linear and radial copies each apply one step
 * transform to the previous copy about the centre frame, so a linear step with a rotation curves and a radial step with
 * a rise climbs a helix. Grid copies are translations only.
 */
struct ArrayLayout
{
    ArrayMode mode = ArrayMode::Linear;
    /** Copies including the original, linear and radial. */
    int count = 3;
    /** Copies including the original along each axis, grid. */
    glm::ivec3 gridCount{3, 3, 1};
    /** Step between copies (linear), or spacing along each axis (grid), in the centre frame. */
    glm::dvec3 offset{0.0};
    /** Extra Euler XYZ rotation per copy, degrees, about the centre (linear and radial). */
    glm::dvec3 rotate{0.0};
    /** Scale per copy, about the centre (linear and radial). */
    glm::dvec3 scale{1.0};
    /** Radial: the centre frame's axis the copies turn about, 0 x, 1 y, 2 z. */
    int axis = 1;
    /** Radial: the angle the copies span (degrees), or with stepAngle the angle between neighbours. */
    double angle = 360.0;
    bool stepAngle = false;
    /** Radial: distance each copy climbs along the axis, for helices. */
    double rise = 0.0;
    /** The centre frame (origin is the pivot, axes orient radial and grid layouts), in mesh space. */
    glm::dmat4 frame{1.0};
};

/** Copies the layout makes, the original included. */
inline int arrayCopyCount(const ArrayLayout& l)
{
    if (l.mode == ArrayMode::Grid)
        return std::max(l.gridCount.x, 1) * std::max(l.gridCount.y, 1) * std::max(l.gridCount.z, 1);
    return std::max(l.count, 1);
}

/** True when a radial layout spans a whole turn (by total angle, or step angle times count), without rise. */
inline bool arrayClosesRing(const ArrayLayout& l)
{
    if (l.mode != ArrayMode::Radial || l.count < 2 || std::abs(l.rise) > 0.0)
        return false;
    const double span = l.stepAngle ? std::abs(l.angle) * l.count : std::abs(l.angle);
    return std::abs(span - 360.0) < 1e-6;
}

/** Degrees between neighbouring radial copies: a full turn shares out over count, a partial arc puts copies at both ends. */
inline double arrayStepAngle(const ArrayLayout& l)
{
    if (l.stepAngle)
        return l.angle;
    const int n = std::max(l.count, 1);
    if (std::abs(std::abs(l.angle) - 360.0) < 1e-6)
        return l.angle / n;
    return n > 1 ? l.angle / (n - 1) : 0.0;
}

/** The transform from one linear or radial copy to the next, in the centre frame. */
inline glm::dmat4 arrayStep(const ArrayLayout& l)
{
    glm::dmat4 step(1.0);
    if (l.mode == ArrayMode::Radial)
    {
        glm::dvec3 axis(0.0);
        axis[std::clamp(l.axis, 0, 2)] = 1.0;
        step = glm::rotate(glm::dmat4(1.0), glm::radians(arrayStepAngle(l)), axis);
        step = glm::translate(step, axis * l.rise);
    }
    return step * trsMatrix(l.offset, l.rotate, l.scale);
}

/** Grid cell of copy k: x varies fastest. */
inline glm::ivec3 arrayGridCell(const ArrayLayout& l, int k)
{
    const int nx = std::max(l.gridCount.x, 1), ny = std::max(l.gridCount.y, 1);
    return {k % nx, (k / nx) % ny, k / (nx * ny)};
}

/** Copy k's transform in mesh space; copy 0 is the original. */
inline glm::dmat4 arrayCopyTransform(const ArrayLayout& l, int k)
{
    glm::dmat4 local(1.0);
    if (l.mode == ArrayMode::Grid)
        local = glm::translate(glm::dmat4(1.0), l.offset * glm::dvec3(arrayGridCell(l, k)));
    else
    {
        const glm::dmat4 step = arrayStep(l);
        for (int i = 0; i < k; ++i)
            local = local * step;
    }
    return aboutFrame(l.frame, local);
}

/** Every copy's transform in mesh space, copy 0 (the original, identity) first. */
inline std::vector<glm::dmat4> arrayCopyTransforms(const ArrayLayout& l)
{
    const int n = arrayCopyCount(l);
    std::vector<glm::dmat4> out;
    out.reserve(size_t(n));
    if (l.mode == ArrayMode::Grid)
    {
        for (int k = 0; k < n; ++k)
            out.push_back(arrayCopyTransform(l, k));
        return out;
    }
    out.push_back(glm::dmat4(1.0));
    const glm::dmat4 step = aboutFrame(l.frame, arrayStep(l));
    for (const glm::dmat4& m : repeatMatrices(glm::dmat4(1.0), step, n - 1))
        out.push_back(m);
    return out;
}
} // namespace lila::geo
