#pragma once

// Puppeteer's posing maths, free of the scene so tests can check it directly. Angles are degrees, matrices are
// column-major glm with translation in column 3. The rig element maths (reverse foot, local rotations) is base Lila's.

#include <lila/RigElements.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <utility>
#include <vector>

namespace puppeteer::math
{
using namespace lila::scene::rigmath;

/** The eased IK weight older scenes' per-key ik channel gave, kept so they evaluate as saved until migrated. */
inline double ikWeight(double flag)
{
    const double x = std::clamp(flag, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

/**
 * Inherit rotate off: a child keeps its world rotation while its parent turns. Given the parent's new world rotation
 * and the child's world rotation before, the child's new R term.
 */
inline glm::dmat3 keepWorldRotation(const glm::dmat3& parentWorldAfter, const glm::dmat3& childOrient, const glm::dmat3& childWorldBefore)
{
    return localRotationFor(parentWorldAfter, childOrient, childWorldBefore);
}

/**
 * Toon scale compensation: a child's scale so its world size stays put when its parent's scale is multiplied by
 * ratio (per parent axis). childLocal is the child's rotation in the parent's space (orient * R). Exact when the axes
 * line up; otherwise the scale each child axis sees along its own direction.
 */
inline glm::dvec3 compensatedScale(const glm::dvec3& childScale, const glm::dvec3& ratio, const glm::dmat3& childLocal)
{
    glm::dvec3 out = childScale;
    for (int i = 0; i < 3; ++i)
    {
        const glm::dvec3 axis = glm::normalize(childLocal[i]);
        const double seen = glm::length(axis * ratio);
        out[i] = seen > 1e-12 ? childScale[i] / seen : childScale[i];
    }
    return out;
}

/** World rotation that turns `from` (a bone direction) onto `to`, the shortest way. */
inline glm::dmat3 aimDelta(const glm::dvec3& from, const glm::dvec3& to)
{
    const double lf = glm::length(from), lt = glm::length(to);
    if (lf < 1e-12 || lt < 1e-12)
        return glm::dmat3(1.0);
    const glm::dvec3 a = from / lf, b = to / lt;
    const double c = glm::dot(a, b);
    if (c < -1.0 + 1e-12)
    {
        // Opposite: half a turn about any axis across them.
        glm::dvec3 axis = glm::cross(glm::dvec3(1, 0, 0), a);
        if (glm::length(axis) < 1e-6)
            axis = glm::cross(glm::dvec3(0, 1, 0), a);
        return glm::mat3_cast(glm::angleAxis(kPi, glm::normalize(axis)));
    }
    const glm::dvec3 axis = glm::cross(a, b);
    return glm::mat3_cast(glm::normalize(glm::dquat(1.0 + c, axis.x, axis.y, axis.z)));
}

// Breakdown -------------------------------------------------------------------------------------------------------

/**
 * The breakdown slider: weight 0 keeps the current value, -1 reaches the previous key's, +1 the next key's. Weights in
 * between blend linearly and weights past +-1 overshoot along the same line.
 */
inline double breakdown(double current, double previous, double next, double weight)
{
    return weight < 0.0 ? current + (previous - current) * -weight : current + (next - current) * weight;
}

inline glm::dvec3 breakdown(const glm::dvec3& current, const glm::dvec3& previous, const glm::dvec3& next, double weight)
{
    return {breakdown(current.x, previous.x, next.x, weight), breakdown(current.y, previous.y, next.y, weight),
            breakdown(current.z, previous.z, next.z, weight)};
}

/** Rotations blend the short way along the great arc, and overshoot along it. */
inline glm::dquat breakdown(const glm::dquat& current, const glm::dquat& previous, const glm::dquat& next, double weight)
{
    const glm::dquat to = weight < 0.0 ? previous : next;
    glm::dquat from = current;
    if (glm::dot(from, to) < 0.0)
        from = -from;
    const glm::dquat delta = glm::normalize(glm::inverse(from) * to);
    const glm::dvec3 v = rotationLog(glm::mat3_cast(delta)) * std::abs(weight);
    const double angle = glm::length(v);
    const glm::dquat step = angle < 1e-15 ? glm::dquat(1.0, 0.0, 0.0, 0.0) : glm::angleAxis(angle, v / angle);
    return glm::normalize(from * step);
}

/** Slider travel (pixels, signed) to a breakdown weight: nothing inside the dead zone, then +-1 at full, overshooting past it. */
inline double breakdownWeight(double travel, double deadZone, double full)
{
    const double mag = std::abs(travel) - deadZone;
    if (mag <= 0.0)
        return 0.0;
    return (travel < 0.0 ? -1.0 : 1.0) * mag / std::max(full - deadZone, 1.0);
}

// Spinner -------------------------------------------------------------------------------------------------------

/**
 * Toon scale: one axis scaled by s, the other two by s^-k so the segment keeps (k = 1/2) or partly keeps its volume.
 * Plain scale is k = 0.
 */
inline glm::dvec3 toonScale(int axis, double s, double k)
{
    const double other = std::pow(std::max(s, 1e-6), -k);
    glm::dvec3 out(other);
    out[axis] = s;
    return out;
}

/**
 * Free trackball: a cursor travel of delta pixels (x right, y down) turns about the screen axis across it, deg per
 * pixel. viewToWorld turns view-space directions (x right, y up, z toward the viewer) into world ones. Returns the
 * world axis (unit) and the angle in degrees; zero angle for no travel.
 */
inline std::pair<glm::dvec3, double> trackball(const glm::dvec2& delta, const glm::dmat3& viewToWorld, double degPerPx)
{
    const double len = glm::length(delta);
    if (len < 1e-9)
        return {glm::dvec3(0.0, 1.0, 0.0), 0.0};
    // Dragging right turns about screen up, dragging down about screen right: the ball rolls under the cursor.
    const glm::dvec3 viewAxis = glm::normalize(glm::dvec3(delta.y, delta.x, 0.0));
    return {glm::normalize(viewToWorld * viewAxis), len * degPerPx};
}

/**
 * Sticky aim: a joint's new world rotation when its primary axis is re-aimed along aim. The secondary axis keeps the
 * world direction it had (previousSecondary) as far as the new aim allows, or with plane set leans along it instead
 * (the chain's plane: sticky 2nd), flipped toward previousSecondary when keepDirection is on.
 */
inline glm::dmat3 stickyAimFrame(const glm::dvec3& primaryLocal, const glm::dvec3& secondaryLocal, const glm::dvec3& aim,
                                 const glm::dvec3& previousSecondary, const glm::dvec3* plane, bool keepDirection)
{
    glm::dvec3 up = previousSecondary;
    if (plane && glm::length(*plane) > 1e-9)
    {
        up = *plane;
        if (keepDirection && glm::dot(up, previousSecondary) < 0.0)
            up = -up;
    }
    const glm::dvec3 a = glm::normalize(aim);
    glm::dvec3 u = up - a * glm::dot(up, a);
    if (glm::length(u) < 1e-9)
        u = std::abs(a.y) < 0.9 ? glm::dvec3(0, 1, 0) - a * a.y : glm::dvec3(1, 0, 0) - a * a.x;
    u = glm::normalize(u);
    // World axes for the two local axes, the third from their cross product with the local handedness.
    const glm::dvec3 third = glm::cross(primaryLocal, secondaryLocal);
    glm::dmat3 local(primaryLocal, secondaryLocal, third);
    glm::dmat3 world(a, u, glm::cross(a, u));
    return world * glm::transpose(local);
}

// Discrete stepping -----------------------------------------------------------------------------------------------

inline double quantize(double v, double step) { return step > 0.0 ? std::round(v / step) * step : v; }
} // namespace puppeteer::math
