#pragma once

#include <glm/glm.hpp>

#include <cmath>

/** Pure manipulator math: increment snapping and plane constraints. Header-only so the tests can reach it. */
namespace lila::ui::gizmo
{
/** Default increment snap steps: 1 cm, 2.5 degrees, 0.1. */
inline constexpr double kDefaultMoveStep = 1.0;
inline constexpr double kDefaultRotateStep = 2.5;
inline constexpr double kDefaultScaleStep = 0.1;

/** Rotate snap presets: common angles, and power-of-two divisions of a turn (360/n). */
inline constexpr double kRotateSteps[] = {1.0, 2.5, 5.0, 10.0, 15.0, 30.0, 45.0, 90.0};
inline constexpr int kTurnDivisions[] = {256, 128, 64, 32, 16, 8};

inline constexpr double turnDivision(int n) { return 360.0 / n; }

/** Nearest multiple of step; a step of zero or less leaves v alone. Never returns -0. */
inline double snapValue(double v, double step)
{
    if (!(step > 0.0))
        return v;
    return std::round(v / step) * step + 0.0;
}

/**
 * Snaps a drag delta in increments of step, measured along the drag frame's axes from the drag start (UE's default).
 * mask picks the frame axes the handle moves along; the others pass through.
 */
inline glm::dvec3 snapRelative(const glm::dvec3& delta, const glm::dmat3& frame, const glm::dvec3& mask, double step)
{
    glm::dvec3 local = glm::transpose(frame) * delta;
    for (int i = 0; i < 3; ++i)
        if (mask[i] != 0.0)
            local[i] = snapValue(local[i], step);
    return frame * local;
}

/**
 * Snaps a drag delta so start + delta lands on a grid of step, in the frame's coordinates (the world grid when the frame
 * is identity). Unmasked frame axes keep the start's coordinate, so an axis drag never leaves its line.
 */
inline glm::dvec3 snapAbsolute(const glm::dvec3& start, const glm::dvec3& delta, const glm::dmat3& frame, const glm::dvec3& mask,
                               double step)
{
    glm::dvec3 local = glm::transpose(frame) * (start + delta);
    for (int i = 0; i < 3; ++i)
        if (mask[i] != 0.0)
            local[i] = snapValue(local[i], step);
    return frame * local - start;
}

/** Degrees, unwrapped: a drag past 180 keeps counting rather than jumping to -180. */
inline double snapAngle(double degrees, double step) { return snapValue(degrees, step); }

/** Scale factors snap their change from 1 (the drag start), so 1.26 with step 0.25 is 1.25. */
inline double snapScale(double factor, double step) { return 1.0 + snapValue(factor - 1.0, step); }

/** Removes the component along normal (unit length), keeping a move in the plane perpendicular to it. */
inline glm::dvec3 constrainToPlane(const glm::dvec3& delta, const glm::dvec3& normal)
{
    return delta - normal * glm::dot(delta, normal);
}

/**
 * Index of the axis whose screen direction best lines up with a drag, either way along it: Maya's Shift+MMB axis pick
 * for move and scale. axes are the screen-space vectors of the three manipulator axes; any shorter than minLength
 * (looking down the axis) can't be picked. -1 when none can.
 */
inline int axisAlongDrag(const glm::dvec2 axes[3], const glm::dvec2& drag, double minLength = 1e-6)
{
    double dl = glm::length(drag);
    if (dl < 1e-12)
        return -1;
    int best = -1;
    double bestScore = -1.0;
    for (int i = 0; i < 3; ++i)
    {
        double al = glm::length(axes[i]);
        if (al < minLength)
            continue;
        double score = std::abs(glm::dot(axes[i], drag)) / (al * dl);
        if (score > bestScore)
        {
            bestScore = score;
            best = i;
        }
    }
    return best;
}

/**
 * The world axis a trackball turns about for a screen drag (x right, y down): a horizontal drag spins about the
 * camera's up, a vertical one about its right.
 */
inline glm::dvec3 trackballAxis(const glm::dvec2& drag, const glm::dvec3& camRight, const glm::dvec3& camUp)
{
    glm::dvec3 a = camUp * drag.x + camRight * drag.y;
    double l = glm::length(a);
    return l > 1e-12 ? a / l : glm::dvec3(0.0);
}

/** Column of frame closest to `direction`, either sign: the rotate ring a Shift+MMB drag picks. */
inline int axisNearest(const glm::dmat3& frame, const glm::dvec3& direction)
{
    int best = 0;
    for (int i = 1; i < 3; ++i)
        if (std::abs(glm::dot(frame[i], direction)) > std::abs(glm::dot(frame[best], direction)))
            best = i;
    return best;
}

/** Parameter along the line p + t*a (unit a) of its closest point to the ray o + s*d (unit d); 0 when they run parallel. */
inline double lineParam(const glm::dvec3& p, const glm::dvec3& a, const glm::dvec3& o, const glm::dvec3& d)
{
    glm::dvec3 w = p - o;
    double b = glm::dot(a, d), dd = glm::dot(a, w), e = glm::dot(d, w);
    double denom = 1.0 - b * b;
    if (std::abs(denom) < 1e-9)
        return 0.0;
    // Clamped as well as guarded: MSVC warns C4723 when it folds a constant parallel call down to /0.
    return (b * e - dd) / std::max(denom, 1e-9);
}

/** Ray o + t*d against the plane through p with normal n. False when the ray runs parallel to it. */
inline bool rayPlane(const glm::dvec3& o, const glm::dvec3& d, const glm::dvec3& p, const glm::dvec3& n, glm::dvec3& hit)
{
    double denom = glm::dot(d, n);
    if (std::abs(denom) < 1e-9)
        return false;
    hit = o + d * (glm::dot(p - o, n) / (std::abs(denom) < 1e-9 ? 1.0 : denom)); // see lineParam
    return true;
}

/**
 * Wraps the cursor at a view's edges during a drag. Moves queued before a warp still carry positions from the side the
 * cursor left; map() shifts those across the warp, so the drag sees one continuous motion instead of a jump back.
 */
class CursorWrap
{
public:
    /** Where the cursor goes from p to wrap at the edges of a width x height view; p itself when it is not at one. */
    static glm::dvec2 target(const glm::dvec2& p, double width, double height, double margin = 2.0)
    {
        glm::dvec2 to = p;
        if (p.x <= margin)
            to.x = width - margin - 2.0;
        else if (p.x >= width - margin)
            to.x = margin + 2.0;
        if (p.y <= margin)
            to.y = height - margin - 2.0;
        else if (p.y >= height - margin)
            to.y = margin + 2.0;
        return to;
    }

    void warped(const glm::dvec2& from, const glm::dvec2& to)
    {
        mFrom = from;
        mTo = to;
        mPending = true;
    }

    /** p as if it arrived after the last warp. A move nearer where the cursor left than where it went predates it. */
    glm::dvec2 map(const glm::dvec2& p)
    {
        if (!mPending)
            return p;
        if (glm::length(p - mTo) <= glm::length(p - mFrom))
        {
            mPending = false;
            return p;
        }
        return p + (mTo - mFrom);
    }

    void reset() { mPending = false; }

private:
    glm::dvec2 mFrom{0.0};
    glm::dvec2 mTo{0.0};
    bool mPending = false;
};
} // namespace lila::ui::gizmo
