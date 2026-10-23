#pragma once

// Rig element maths (reverse foot, twist, local rotations), free of the scene so tests can check it directly. Angles are
// degrees, matrices are column-major glm with translation in column 3.

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace lila::scene::rigmath
{
inline constexpr double kPi = 3.14159265358979323846;
inline double radians(double degrees) { return degrees * kPi / 180.0; }

/** Rotation part of m with any scale divided out. */
inline glm::dmat3 rotationOf(const glm::dmat4& m)
{
    glm::dmat3 r(m);
    for (int i = 0; i < 3; ++i)
        r[i] /= std::max(glm::length(r[i]), 1e-300);
    return r;
}

inline glm::dmat4 rotateAbout(const glm::dvec3& pivot, const glm::dvec3& axis, double degrees)
{
    if (degrees == 0.0)
        return glm::dmat4(1.0);
    glm::dmat4 m = glm::translate(glm::dmat4(1.0), pivot);
    m = glm::rotate(m, radians(degrees), axis);
    return glm::translate(m, -pivot);
}

/**
 * The R term (rotate, before rotateAxis) a joint needs for its world rotation to be `world` under a parent whose
 * world rotation is parentWorld, given its jointOrient as a matrix: world = parent * orient * R.
 */
inline glm::dmat3 localRotationFor(const glm::dmat3& parentWorld, const glm::dmat3& orient, const glm::dmat3& world)
{
    return glm::transpose(orient) * glm::transpose(parentWorld) * world;
}

/** The signed angle (degrees) that turns a onto b around axis, both first flattened across the axis. */
inline double signedAngle(const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& axis)
{
    const glm::dvec3 n = glm::normalize(axis);
    const glm::dvec3 pa = a - n * glm::dot(a, n), pb = b - n * glm::dot(b, n);
    if (glm::length(pa) < 1e-12 || glm::length(pb) < 1e-12)
        return 0.0;
    return std::atan2(glm::dot(glm::cross(pa, pb), n), glm::dot(pa, pb)) * 180.0 / kPi;
}

inline glm::dvec3 rotationLog(const glm::dmat3& r)
{
    const glm::dquat q = glm::normalize(glm::quat_cast(r));
    const glm::dquat w = q.w < 0.0 ? -q : q;
    const glm::dvec3 v(w.x, w.y, w.z);
    const double s = glm::length(v);
    if (s < 1e-15)
        return v * 2.0;
    return v * (2.0 * std::atan2(s, w.w) / s);
}

/** The twist (degrees, in (-180, 180]) of rotation r about the unit axis: swing-twist, twist taken first. */
inline double twistAngle(const glm::dmat3& r, const glm::dvec3& axis)
{
    glm::dquat q = glm::normalize(glm::quat_cast(r));
    const double along = glm::dot(glm::dvec3(q.x, q.y, q.z), axis);
    if (std::abs(along) < 1e-15 && std::abs(q.w) < 1e-15)
        return 0.0;
    double a = 2.0 * std::atan2(along, q.w) * 180.0 / kPi;
    if (a > 180.0)
        a -= 360.0;
    else if (a <= -180.0)
        a += 360.0;
    return a;
}

// Reverse foot ----------------------------------------------------------------------------------------------------

/**
 * Pivots in the foot control's space, where +Z points from heel to toe, +Y is up off the sole and the sole lies at
 * y = 0. inner and outer are the sole's two edges the foot banks over.
 */
struct FootPivots
{
    glm::dvec3 heel{0.0};
    glm::dvec3 ball{0.0, 0.0, 10.0};
    glm::dvec3 toe{0.0, 0.0, 18.0};
    glm::dvec3 inner{-4.0, 0.0, 10.0};
    glm::dvec3 outer{4.0, 0.0, 10.0};
};

struct FootChannels
{
    double roll = 0.0;  // negative rocks back on the heel, positive lifts the heel onto the ball, then the toe
    double twist = 0.0; // swivel on the ball around the sole's up axis
    double bank = 0.0;  // positive tips onto the outer edge, negative onto the inner
};

/**
 * Reverse foot limits. The ball takes the roll up to ballLimit (bend), then hands it to the toe until all of it is on the
 * toe at toeLimit (unbend). Roll stays within [-heelMax, toeMax], bank within +-bankMax, twist within +-twistMax, and a
 * channel nearer zero than zeroSnap counts as zero.
 */
struct FootLimits
{
    double ballLimit = 30.0;
    double toeLimit = 60.0;
    double heelMax = 60.0;
    double toeMax = 90.0;
    double bankMax = 45.0;
    double twistMax = 90.0;
    double zeroSnap = 0.0;
};

/** The channels as the foot uses them: clamped to the limits, near-zero values snapped to zero. */
inline FootChannels limitChannels(FootChannels c, const FootLimits& l)
{
    auto snap = [&](double v) { return std::abs(v) < l.zeroSnap ? 0.0 : v; };
    c.roll = snap(std::clamp(c.roll, -std::max(l.heelMax, 0.0), std::max(l.toeMax, 0.0)));
    c.bank = snap(std::clamp(c.bank, -std::max(l.bankMax, 0.0), std::max(l.bankMax, 0.0)));
    c.twist = snap(std::clamp(c.twist, -std::max(l.twistMax, 0.0), std::max(l.twistMax, 0.0)));
    return c;
}

struct RollSplit
{
    double heel = 0.0;
    double ball = 0.0;
    double toe = 0.0;
};

/** How a roll divides between heel, ball and toe. heel + ball + toe == roll always. */
inline RollSplit splitRoll(double roll, const FootLimits& limits)
{
    RollSplit s;
    if (roll <= 0.0)
    {
        s.heel = roll;
        return s;
    }
    const double b = std::max(limits.ballLimit, 0.0);
    const double t = std::max(limits.toeLimit, b + 1e-9);
    if (roll <= b)
        s.ball = roll;
    else if (roll <= t)
        s.ball = b * (1.0 - (roll - b) / (t - b));
    s.toe = roll - s.ball;
    return s;
}

/**
 * The reverse foot in the control's space. ankle carries the ankle's rest placement to its posed one (apply it before
 * the rest matrix: control * ankle * restAnkle). ball does the same for the ball joint, which keeps its rest angle to
 * the ground while the heel lifts, so the ball roll bends the foot at the ball.
 */
struct FootSolve
{
    glm::dmat4 ankle{1.0};
    glm::dmat4 ball{1.0};
};

inline FootSolve solveFoot(const FootPivots& p, const FootChannels& raw, const FootLimits& limits)
{
    // Zero snapping belongs to editing the channels; the solve takes whatever they hold, within the limits.
    FootLimits clampOnly = limits;
    clampOnly.zeroSnap = 0.0;
    const FootChannels c = limitChannels(raw, clampOnly);
    const RollSplit r = splitRoll(c.roll, limits);
    const glm::dvec3 x(1, 0, 0), y(0, 1, 0), z(0, 0, 1);
    // Tip toward whichever edge is opposite the pivot: the sign makes the far edge rise for either foot.
    const double side = (p.inner.x - p.outer.x) >= 0.0 ? 1.0 : -1.0;
    const glm::dvec3 bankPivot = c.bank >= 0.0 ? p.outer : p.inner;
    const glm::dmat4 heel = rotateAbout(p.heel, x, r.heel);
    const glm::dmat4 bank = rotateAbout(bankPivot, z, c.bank * side);
    const glm::dmat4 toe = rotateAbout(p.toe, x, r.toe);
    const glm::dmat4 twist = rotateAbout(p.ball, y, c.twist);
    const glm::dmat4 ball = rotateAbout(p.ball, x, r.ball);
    FootSolve s;
    s.ball = heel * bank * toe * twist;
    s.ankle = s.ball * ball;
    return s;
}

/** Channels whose foot rotation best matches target (control space), by Gauss-Newton from start. */
inline FootChannels channelsForRotation(const FootPivots& p, const FootLimits& limits, const glm::dmat3& target, FootChannels start = {})
{
    auto residual = [&](const FootChannels& c) {
        const glm::dmat3 got = rotationOf(solveFoot(p, c, limits).ankle);
        return rotationLog(glm::transpose(got) * target);
    };
    FootChannels c = start;
    for (int it = 0; it < 50; ++it)
    {
        const glm::dvec3 e = residual(c);
        if (glm::length(e) < 1e-12)
            break;
        glm::dmat3 J;
        constexpr double h = 1e-4;
        for (int k = 0; k < 3; ++k)
        {
            FootChannels d = c;
            (k == 0 ? d.roll : k == 1 ? d.twist : d.bank) += h;
            // The residual shrinks as the channels approach the target, so the Jacobian is its negated change.
            J[k] = (e - residual(d)) / h;
        }
        if (std::abs(glm::determinant(J)) < 1e-18)
            break;
        const glm::dvec3 step = glm::inverse(J) * e;
        c.roll += step.x;
        c.twist += step.y;
        c.bank += step.z;
    }
    return c;
}

/**
 * Pivots from the skinned foot's sole: pts are the foot's mesh points in the control's space (+Z heel to toe, +Y up).
 * The sole is the lowest band of them; the heel and toe pivots sit at its back and front, the ball pivot under the ball
 * joint (ballZ, ballX), and the edge pivots at the sole's sides level with the ball. innerSign is the side (+1 or -1 on
 * X) the inner edge faces. Pivots come back at the sole's height, which ground (when given) receives. Empty with fewer
 * than three points.
 */
inline std::optional<FootPivots> pivotsFromSole(const std::vector<glm::dvec3>& pts, double ballX, double ballZ, double innerSign,
                                               double* ground = nullptr)
{
    if (pts.size() < 3)
        return std::nullopt;
    double minY = pts[0].y, minZ = pts[0].z, maxZ = pts[0].z;
    for (const glm::dvec3& p : pts)
    {
        minY = std::min(minY, p.y);
        minZ = std::min(minZ, p.z);
        maxZ = std::max(maxZ, p.z);
    }
    const double length = std::max(maxZ - minZ, 1e-6);
    const double band = minY + std::max(0.12 * length, 0.25);
    std::vector<glm::dvec3> sole;
    for (const glm::dvec3& p : pts)
        if (p.y <= band)
            sole.push_back(p);
    double heelZ = sole[0].z, toeZ = sole[0].z, sumX = 0.0;
    for (const glm::dvec3& p : sole)
    {
        heelZ = std::min(heelZ, p.z);
        toeZ = std::max(toeZ, p.z);
        sumX += p.x;
    }
    const double cx = sumX / double(sole.size());
    double minX = 1e300, maxX = -1e300;
    for (const glm::dvec3& p : sole)
        if (std::abs(p.z - ballZ) <= 0.2 * length)
        {
            minX = std::min(minX, p.x);
            maxX = std::max(maxX, p.x);
        }
    if (minX > maxX)
        minX = maxX = cx;
    FootPivots f;
    f.heel = {cx, minY, heelZ};
    f.toe = {cx, minY, toeZ};
    f.ball = {ballX, minY, ballZ};
    f.inner = {innerSign > 0.0 ? maxX : minX, minY, ballZ};
    f.outer = {innerSign > 0.0 ? minX : maxX, minY, ballZ};
    if (ground)
        *ground = minY;
    return f;
}

/** Where the control goes for a snap to the sole: the current ankle with the channels zeroed keeps the pose. */
inline glm::dmat4 snapControl(const glm::dmat4& ankleWorld, const glm::dmat4& restAnkle)
{
    return ankleWorld * glm::inverse(restAnkle);
}

struct FootAlign
{
    glm::dmat4 control{1.0};
    FootChannels channels;
};

/**
 * Levels the control (sole flat, heading kept) and puts the pose into the channels and the control's position, so the
 * ankle ends where it was.
 */
inline FootAlign alignControl(const glm::dmat4& ankleWorld, const glm::dmat4& restAnkle, const FootPivots& p, const FootLimits& limits)
{
    const glm::dmat4 snapped = snapControl(ankleWorld, restAnkle);
    glm::dvec3 fwd = glm::dvec3(snapped[2]);
    fwd.y = 0.0;
    if (glm::length(fwd) < 1e-9)
        fwd = glm::dvec3(0.0, 0.0, 1.0);
    fwd = glm::normalize(fwd);
    const glm::dvec3 up(0.0, 1.0, 0.0);
    const glm::dvec3 side = glm::normalize(glm::cross(up, fwd));
    glm::dmat3 level(side, up, fwd);
    const glm::dmat3 want = glm::transpose(level) * rotationOf(ankleWorld) * glm::transpose(rotationOf(restAnkle));
    FootAlign a;
    a.channels = channelsForRotation(p, limits, want);
    const glm::dmat4 local = solveFoot(p, a.channels, limits).ankle * restAnkle;
    a.control = glm::dmat4(level);
    a.control[3] = glm::dvec4(glm::dvec3(ankleWorld[3]) - level * glm::dvec3(local[3]), 1.0);
    return a;
}

/** The control matrix that puts the ankle at ankleWorld with the channels as they are: the inverse of the foot's solve. */
inline glm::dmat4 controlFor(const glm::dmat4& ankleWorld, const glm::dmat4& restAnkle, const FootPivots& p, const FootChannels& c,
                             const FootLimits& limits)
{
    return ankleWorld * glm::inverse(solveFoot(p, c, limits).ankle * restAnkle);
}
} // namespace lila::scene::rigmath
