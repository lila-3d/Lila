#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

/**
 * The Overlap maths, free of the scene so the tests can check it by hand: one bone's secondary motion as a positional
 * and a rotation-vector deviation from its animated target, each an implicit spring-damper, with feed-forward terms,
 * a soft limit, squash and stretch, and the chain and global settings that shape it. Everything is double precision
 * and runs one fixed step at a time; the extension owns time, hierarchy and the scene.
 */
namespace overlap
{
constexpr double kSmall = 1e-8;
constexpr double kPi = 3.14159265358979323846;

struct Translation
{
    double weight = 1.0;
    double stiffness = 250.0;
    double damping = 12.0;
    double maxOffset = 15.0;
    bool relevant() const { return weight > kSmall && maxOffset > kSmall; }
};

struct Rotation
{
    double weight = 1.0;
    double stiffness = 250.0;
    double damping = 12.0;
    double maxAngle = 30.0; // degrees
    bool relevant() const { return weight > kSmall && maxAngle > kSmall; }
};

enum class StretchAxis
{
    Auto,
    X,
    Y,
    Z,
};

struct Scale
{
    /** Squash and stretch from the deviation: 0 off, negative inverts. */
    double amount = 0.0;
    /** Deviation that reaches the full amount. */
    double reference = 10.0;
    /** Seconds the stretch takes to follow the deviation. */
    double smoothing = 0.08;
    /** Seconds a deviation must hold before it counts as the new neutral (gravity sag, a steady trail). 0 off. */
    double restAdapt = 0.5;
    StretchAxis axis = StretchAxis::Auto;
    bool preserveVolume = true;
    double minScale = 0.6;
    double maxScale = 1.8;
    bool relevant() const { return std::abs(amount) > kSmall && reference > kSmall; }
};

struct Response
{
    /** Resistance to being dragged by the animation: scales the target's acceleration as a pseudo-force. */
    double inertia = 1.0;
    /** Trail proportional to the target's speed, fed forward rather than integrated. */
    double velocityInfluence = 0.0;
    /** Snap on acceleration, fed forward. */
    double accelerationInfluence = 0.0;
    double gravityScale = 0.0;
    /** Share of the wind (and other external force) this bone feels. */
    double windScale = 1.0;
};

struct Style
{
    /** Scales the deviation on output only, so the integrated state stays physical at any value. */
    double exaggeration = 1.0;
    /** Blend between the animation and the solved pose. */
    double weight = 1.0;
};

struct BoneSettings
{
    Translation translation;
    Rotation rotation;
    Scale scale;
    Response response;
    Style style;

    bool relevant() const
    {
        return style.weight > kSmall && style.exaggeration > kSmall && (translation.relevant() || rotation.relevant() || scale.relevant());
    }
};

/** How a chain's settings spread from its root (alpha 0) to its tip (alpha 1). */
struct ChainBias
{
    /** 0 even, +1 fades out toward the root, -1 toward the tip. */
    double bias = 0.0;
    /** 1 linear; above 1 holds the strong end longer. */
    double falloff = 1.0;
    double tipStiffnessScale = 1.0;
    double tipExaggerationScale = 1.0;
    /** The settings describe the whole chain's motion rather than every bone's: each bone's share is normalised. */
    bool distribute = true;

    double shapedAlpha(double chainAlpha) const { return std::pow(std::clamp(chainAlpha, 0.0, 1.0), std::max(falloff, 0.01)); }

    double weightAt(double shaped) const
    {
        const double b = std::clamp(bias, -1.0, 1.0);
        return b >= 0.0 ? (1.0 - b) + (1.0 - (1.0 - b)) * shaped : 1.0 + ((1.0 + b) - 1.0) * shaped;
    }

    /** One bone's share of the chain's motion. */
    double distribution(double chainAlpha, int count) const
    {
        const double result = weightAt(shapedAlpha(chainAlpha));
        if (!distribute || count <= 1)
            return result;
        double total = 0.0;
        const double last = double(count - 1);
        for (int i = 0; i < count; ++i)
            total += weightAt(shapedAlpha(double(i) / last));
        return total > kSmall ? result / total : result;
    }
};

/** The settings of one bone of a chain, at alpha 0 for its first bone and 1 for its last. */
inline BoneSettings chainBone(const BoneSettings& chain, const ChainBias& b, double chainAlpha, int count)
{
    BoneSettings r = chain;
    const double alpha = b.shapedAlpha(chainAlpha);
    r.style.weight *= b.distribution(chainAlpha, count);
    const double stiff = 1.0 + (std::max(b.tipStiffnessScale, 0.01) - 1.0) * alpha;
    r.translation.stiffness *= stiff;
    r.rotation.stiffness *= stiff;
    // Damping tracks the square root of stiffness, which keeps the damping ratio (the bounce) constant along the chain.
    const double damp = std::sqrt(stiff);
    r.translation.damping *= damp;
    r.rotation.damping *= damp;
    r.style.exaggeration *= 1.0 + (std::max(b.tipExaggerationScale, 0.0) - 1.0) * alpha;
    return r;
}

/** Character-wide multipliers: 1 is always a no-op. */
struct Global
{
    double weight = 1.0;
    double stiffness = 1.0;
    double damping = 1.0;
    double maxOffset = 1.0;
    double maxAngle = 1.0;
    double inertia = 1.0;
    double gravity = 1.0;
    double exaggeration = 1.0;
    double squash = 1.0;

    BoneSettings apply(const BoneSettings& in) const
    {
        BoneSettings r = in;
        r.translation.stiffness *= stiffness;
        r.translation.damping *= damping;
        r.translation.maxOffset *= maxOffset;
        r.rotation.stiffness *= stiffness;
        r.rotation.damping *= damping;
        r.rotation.maxAngle *= maxAngle;
        r.response.inertia *= inertia;
        r.response.gravityScale *= gravity;
        r.style.exaggeration *= exaggeration;
        r.style.weight *= weight;
        r.scale.amount *= squash;
        return r;
    }
};

/** Per-step inputs shared by a bone's solve, all in its simulation space. */
struct Context
{
    glm::dvec3 gravity{0.0};
    glm::dvec3 force{0.0};  // linear acceleration from wind and the like
    glm::dvec3 torque{0.0}; // angular acceleration, radians
    /** Unit axis in the bone's own space that squash and stretch acts along. */
    glm::dvec3 stretchAxis{1.0, 0.0, 0.0};
};

struct BoneState
{
    glm::dvec3 offset{0.0};
    glm::dvec3 velocity{0.0};
    glm::dvec3 rotation{0.0}; // rotation vector, radians
    glm::dvec3 angularVelocity{0.0};
    glm::dvec3 feedForward{0.0};
    glm::dvec3 feedForwardRotation{0.0};
    double stretch = 1.0;
    glm::dvec3 stretchBaseline{0.0};
    glm::dvec3 prevTarget{0.0};
    glm::dvec3 prevTargetVelocity{0.0};
    glm::dquat prevTargetRotation{1.0, 0.0, 0.0, 0.0};
    glm::dvec3 prevTargetAngularVelocity{0.0};

    static constexpr size_t kDoubles = 35;

    void write(double* out) const
    {
        auto v = [&](const glm::dvec3& x) {
            out[0] = x.x, out[1] = x.y, out[2] = x.z;
            out += 3;
        };
        v(offset), v(velocity), v(rotation), v(angularVelocity), v(feedForward), v(feedForwardRotation);
        *out++ = stretch;
        v(stretchBaseline), v(prevTarget), v(prevTargetVelocity);
        out[0] = prevTargetRotation.w, out[1] = prevTargetRotation.x, out[2] = prevTargetRotation.y, out[3] = prevTargetRotation.z;
        out += 4;
        v(prevTargetAngularVelocity);
    }

    void read(const double* in)
    {
        auto v = [&](glm::dvec3& x) {
            x = {in[0], in[1], in[2]};
            in += 3;
        };
        v(offset), v(velocity), v(rotation), v(angularVelocity), v(feedForward), v(feedForwardRotation);
        stretch = *in++;
        v(stretchBaseline), v(prevTarget), v(prevTargetVelocity);
        prevTargetRotation = glm::dquat(in[0], in[1], in[2], in[3]);
        in += 4;
        v(prevTargetAngularVelocity);
    }
};

/** Shortest-arc rotation vector (axis * angle, radians) of a rotation. */
inline glm::dvec3 toRotationVector(glm::dquat q)
{
    q = glm::normalize(q);
    if (q.w < 0.0)
        q = -q;
    const glm::dvec3 v(q.x, q.y, q.z);
    const double s = glm::length(v);
    if (s < 1e-12)
        return v * 2.0;
    return v * (2.0 * std::atan2(s, q.w) / s);
}

inline glm::dquat fromRotationVector(const glm::dvec3& r)
{
    const double angle = glm::length(r);
    if (angle < 1e-12)
        return glm::normalize(glm::dquat(1.0, r.x * 0.5, r.y * 0.5, r.z * 0.5));
    const glm::dvec3 axis = r / angle;
    const double s = std::sin(angle * 0.5);
    return glm::normalize(glm::dquat(std::cos(angle * 0.5), axis.x * s, axis.y * s, axis.z * s));
}

inline glm::dvec3 clampLength(const glm::dvec3& v, double maxLength)
{
    const double l = glm::length(v);
    return l > maxLength && l > 0.0 ? v * (std::max(maxLength, 0.0) / l) : v;
}

/** Backward Euler on v' = -k x - c v + F: unconditionally stable, so stiffness and damping trade accuracy, never safety. */
inline void solveSpring(glm::dvec3& x, glm::dvec3& v, const glm::dvec3& force, double k, double c, double h)
{
    const double denom = 1.0 + h * c + h * h * k;
    v = (v + (force - x * k) * h) / denom;
    x += v * h;
}

/**
 * Eases the deviation into its limit: above half the limit it saturates with tanh, and only the outward velocity bleeds
 * off, in proportion to how saturated it is, so the bound never chatters.
 */
inline void clampDeviation(glm::dvec3& x, glm::dvec3& v, double maxSize)
{
    if (maxSize <= kSmall)
    {
        x = glm::dvec3(0.0);
        v = glm::dvec3(0.0);
        return;
    }
    const double half = maxSize * 0.5;
    const double size = glm::length(x);
    if (size <= half)
        return;
    const double range = maxSize - half;
    const glm::dvec3 dir = x / size;
    x = dir * (half + range * std::tanh((size - half) / range));
    const double outward = glm::dot(v, dir);
    if (outward > 0.0)
        v -= dir * (outward * std::clamp((size - half) / range, 0.0, 1.0));
}

/** Share of the remaining distance an exponential follower with this time constant covers in one step. */
inline double followRate(double timeConstant, double h) { return timeConstant > kSmall ? 1.0 - std::exp(-h / timeConstant) : 1.0; }

/** Eases the stretch toward its bounds without ever reaching them. */
inline double saturateStretch(double excess, double minScale, double maxScale)
{
    const double range = excess >= 0.0 ? std::max(maxScale - 1.0, kSmall) : std::max(1.0 - minScale, kSmall);
    return 1.0 + range * std::tanh(excess / range);
}

/** A rest state at the target, so the first step cannot fire an impulse. */
inline void reset(BoneState& s, const glm::dvec3& target, const glm::dquat& targetRotation)
{
    s = BoneState{};
    s.prevTarget = target;
    s.prevTargetRotation = glm::normalize(targetRotation);
}

inline void solveStretch(const BoneSettings& b, BoneState& s, const glm::dquat& targetRotation, double h, const Context& ctx)
{
    const Scale& sc = b.scale;
    if (!sc.relevant())
    {
        s.stretchBaseline = glm::dvec3(0.0);
        s.stretch = 1.0;
        return;
    }
    const glm::dvec3 total = s.offset + s.feedForward;
    if (sc.restAdapt > kSmall)
        s.stretchBaseline += (total - s.stretchBaseline) * followRate(sc.restAdapt, h);
    else
        s.stretchBaseline = glm::dvec3(0.0);
    const glm::dvec3 axis = targetRotation * ctx.stretchAxis;
    const double along = glm::dot(total - s.stretchBaseline, axis) * std::max(b.style.exaggeration, 0.0) / std::max(sc.reference, kSmall);
    const double minScale = std::max(sc.minScale, 0.01);
    const double target = saturateStretch(sc.amount * along, minScale, std::max(sc.maxScale, minScale));
    s.stretch += (target - s.stretch) * followRate(sc.smoothing, h);
}

/** One fixed step of h seconds toward a target sampled at the step's end. */
inline void integrate(const BoneSettings& b, BoneState& s, const glm::dvec3& target, const glm::dquat& targetRotationIn, double h, const Context& ctx)
{
    const glm::dquat targetRotation = glm::normalize(targetRotationIn);
    const double inv = 1.0 / h;
    const glm::dvec3 linearVelocity = (target - s.prevTarget) * inv;
    const glm::dvec3 linearAccel = (linearVelocity - s.prevTargetVelocity) * inv;
    // Shortest arc, so a hemisphere flip in the keys cannot read as a full turn.
    const glm::dvec3 angularVelocity = toRotationVector(targetRotation * glm::inverse(s.prevTargetRotation)) * inv;
    const glm::dvec3 angularAccel = (angularVelocity - s.prevTargetAngularVelocity) * inv;

    const Response& r = b.response;
    // Dragged by the animation: the pseudo-force opposes the target's acceleration.
    const glm::dvec3 force = ctx.gravity * r.gravityScale + ctx.force * r.windScale - linearAccel * r.inertia;
    solveSpring(s.offset, s.velocity, force, b.translation.stiffness, b.translation.damping, h);
    clampDeviation(s.offset, s.velocity, std::max(b.translation.maxOffset, 0.0));

    const glm::dvec3 torque = ctx.torque * r.windScale - angularAccel * r.inertia;
    solveSpring(s.rotation, s.angularVelocity, torque, b.rotation.stiffness, b.rotation.damping, h);
    // Also keeps the deviation inside one quaternion cover, so it never wraps into a spin.
    clampDeviation(s.rotation, s.angularVelocity, std::min(std::max(b.rotation.maxAngle, 0.0) * kPi / 180.0, kPi * 0.99));

    s.feedForward = -(linearVelocity * r.velocityInfluence + linearAccel * r.accelerationInfluence);
    s.feedForwardRotation = -(angularVelocity * r.velocityInfluence + angularAccel * r.accelerationInfluence);

    solveStretch(b, s, targetRotation, h, ctx);

    s.prevTarget = target;
    s.prevTargetVelocity = linearVelocity;
    s.prevTargetRotation = targetRotation;
    s.prevTargetAngularVelocity = angularVelocity;
}

struct Pose
{
    glm::dvec3 position{0.0};
    glm::dquat rotation{1.0, 0.0, 0.0, 0.0};
    /** Multiplier for the bone's own scale, on its local axes. */
    glm::dvec3 stretch{1.0};
};

/**
 * The output from the state: deviation times exaggeration and weight, clamped, on top of the target, then blended with
 * the target by alpha (the style weight times anything pinning the bone).
 */
inline Pose compose(const BoneSettings& b, const BoneState& s, const glm::dvec3& target, const glm::dquat& targetRotation, double alpha,
                    const glm::dvec3& stretchAxis)
{
    Pose out;
    const double ex = std::max(b.style.exaggeration, 0.0);
    const glm::dvec3 offset = clampLength((s.offset + s.feedForward) * (ex * b.translation.weight), b.translation.maxOffset);
    if (b.scale.relevant())
    {
        const double st = std::max(s.stretch, 0.01);
        const double perp = b.scale.preserveVolume ? 1.0 / std::sqrt(st) : 1.0;
        const glm::dvec3 along = glm::abs(stretchAxis);
        out.stretch = glm::dvec3(perp + (st - perp) * along.x, perp + (st - perp) * along.y, perp + (st - perp) * along.z);
    }
    const glm::dvec3 rv = clampLength((s.rotation + s.feedForwardRotation) * (ex * b.rotation.weight), b.rotation.maxAngle * kPi / 180.0);
    const glm::dquat stylized = glm::normalize(fromRotationVector(rv) * targetRotation);
    const glm::dvec3 stylizedPos = target + offset;
    const double a = std::clamp(alpha, 0.0, 1.0);
    if (a >= 1.0 - kSmall)
    {
        out.position = stylizedPos;
        out.rotation = stylized;
        return out;
    }
    out.stretch = glm::dvec3(1.0) + (out.stretch - glm::dvec3(1.0)) * a;
    out.position = target + (stylizedPos - target) * a;
    out.rotation = glm::normalize(glm::slerp(glm::normalize(targetRotation), stylized, a));
    return out;
}

enum class Shape
{
    Sphere,
    Capsule,
};

/** A collider in some space: a sphere at a, or a capsule from a to b. */
struct Collider
{
    Shape shape = Shape::Sphere;
    glm::dvec3 a{0.0}, b{0.0};
    double radius = 1.0;
};

/** Pushes a point of radius `radius` out of the collider. Returns the push (zero when clear). */
inline glm::dvec3 pushOut(const glm::dvec3& p, const Collider& c, double radius)
{
    glm::dvec3 closest = c.a;
    if (c.shape == Shape::Capsule)
    {
        const glm::dvec3 ab = c.b - c.a;
        const double len2 = glm::dot(ab, ab);
        const double t = len2 > 1e-12 ? std::clamp(glm::dot(p - c.a, ab) / len2, 0.0, 1.0) : 0.0;
        closest = c.a + ab * t;
    }
    const glm::dvec3 d = p - closest;
    const double dist = glm::length(d);
    const double reach = c.radius + radius;
    if (dist >= reach)
        return glm::dvec3(0.0);
    // Dead centre has no direction: push straight up rather than pick an arbitrary one per platform.
    const glm::dvec3 n = dist > 1e-12 ? d / dist : glm::dvec3(0.0, 1.0, 0.0);
    return n * (reach - dist);
}
} // namespace overlap
