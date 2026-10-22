#include "math/Math.h"

#include <atomic>

namespace lila::math
{
namespace
{
std::atomic<UpAxis> gUpAxis{UpAxis::Y};

constexpr AxisBasis kYUp{{0.0, 0.0, 1.0}, {-1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}};
constexpr AxisBasis kZUp{{1.0, 0.0, 0.0}, {0.0, -1.0, 0.0}, {0.0, 0.0, 1.0}};
} // namespace

void setUpAxis(UpAxis axis) { gUpAxis.store(axis, std::memory_order_relaxed); }
UpAxis upAxis() { return gUpAxis.load(std::memory_order_relaxed); }
const AxisBasis& axisBasis() { return axisBasis(upAxis()); }
const AxisBasis& axisBasis(UpAxis axis) { return axis == UpAxis::Z ? kZUp : kYUp; }

double clampAxis(double degrees)
{
    double a = std::fmod(degrees, 360.0);
    if (a < 0.0)
        a += 360.0;
    // A tiny negative input rounds up to exactly 360 above.
    return a >= 360.0 ? 0.0 : a;
}

double normalizeAxis(double degrees)
{
    double a = clampAxis(degrees);
    return a > 180.0 ? a - 360.0 : a;
}

double clampAngle(double degrees, double minDegrees, double maxDegrees)
{
    const double halfRange = clampAxis(maxDegrees - minDegrees) * 0.5;
    const double center = clampAxis(minDegrees + halfRange);
    const double delta = clamp(normalizeAxis(degrees - center), -halfRange, halfRange);
    return normalizeAxis(center + delta);
}

double unwindDegrees(double degrees)
{
    double a = std::fmod(degrees, 360.0);
    if (a > 180.0)
        a -= 360.0;
    else if (a < -180.0)
        a += 360.0;
    return a;
}

double unwindRadians(double radians)
{
    double a = std::fmod(radians, 2.0 * kPi);
    if (a > kPi)
        a -= 2.0 * kPi;
    else if (a < -kPi)
        a += 2.0 * kPi;
    return a;
}

double wrap(double value, double minValue, double maxValue)
{
    const double size = maxValue - minValue;
    if (size == 0.0)
        return maxValue;
    if (value < minValue)
        return maxValue - std::fmod(minValue - value, size);
    if (value > maxValue)
        return minValue + std::fmod(value - maxValue, size);
    return value;
}

double gridSnap(double value, double grid)
{
    if (grid == 0.0)
        return value;
    return std::floor((value + 0.5 * grid) / grid) * grid;
}

double getRangePct(double minValue, double maxValue, double value)
{
    const double divisor = maxValue - minValue;
    if (isNearlyZero(divisor))
        return value >= maxValue ? 1.0 : 0.0;
    return (value - minValue) / divisor;
}

double mapRangeUnclamped(double value, double inMin, double inMax, double outMin, double outMax)
{
    return lerp(outMin, outMax, getRangePct(inMin, inMax, value));
}

double mapRangeClamped(double value, double inMin, double inMax, double outMin, double outMax)
{
    return lerp(outMin, outMax, clamp(getRangePct(inMin, inMax, value), 0.0, 1.0));
}

double finterpTo(double current, double target, double deltaTime, double interpSpeed)
{
    if (interpSpeed <= 0.0)
        return target;
    const double dist = target - current;
    if (dist * dist < kSmallNumber)
        return target;
    return current + dist * clamp(deltaTime * interpSpeed, 0.0, 1.0);
}

double finterpToConstant(double current, double target, double deltaTime, double interpSpeed)
{
    const double dist = target - current;
    if (dist * dist < kSmallNumber)
        return target;
    const double step = interpSpeed * deltaTime;
    return current + clamp(dist, -step, step);
}

// Unreal conversions -------------------------------------------------------------------------

namespace
{
glm::dmat3 basisMatrix()
{
    const AxisBasis& b = axisBasis();
    return glm::dmat3(b.forward, b.right, b.up);
}

Vector absComponents(const glm::dvec3& v) { return {std::abs(v.x), std::abs(v.y), std::abs(v.z)}; }
} // namespace

Vector toUnreal(const Vector& v)
{
    const AxisBasis& b = axisBasis();
    const glm::dvec3 g = v.toGlm();
    return {glm::dot(g, b.forward), glm::dot(g, b.right), glm::dot(g, b.up)};
}

Vector fromUnreal(const Vector& v)
{
    const AxisBasis& b = axisBasis();
    return Vector(b.forward * v.x + b.right * v.y + b.up * v.z);
}

Vector scaleToUnreal(const Vector& s)
{
    const AxisBasis& b = axisBasis();
    return {absComponents(b.forward) | s, absComponents(b.right) | s, absComponents(b.up) | s};
}

Vector scaleFromUnreal(const Vector& s)
{
    const AxisBasis& b = axisBasis();
    return absComponents(b.forward) * s.x + absComponents(b.right) * s.y + absComponents(b.up) * s.z;
}

// The basis change flips handedness, so the rotation axis maps as a pseudovector (negated).
Quat toUnreal(const Quat& q)
{
    const Vector axis = toUnreal(Vector(q.x, q.y, q.z));
    return {-axis.x, -axis.y, -axis.z, q.w};
}

Quat fromUnreal(const Quat& q)
{
    const Vector axis = fromUnreal(Vector(q.x, q.y, q.z));
    return {-axis.x, -axis.y, -axis.z, q.w};
}

Transform toUnreal(const Transform& t)
{
    return Transform(toUnreal(t.rotation), toUnreal(t.translation), scaleToUnreal(t.scale3d));
}

Transform fromUnreal(const Transform& t)
{
    return Transform(fromUnreal(t.rotation), fromUnreal(t.translation), scaleFromUnreal(t.scale3d));
}

Matrix toUnreal(const Matrix& m)
{
    const glm::dmat4 b(basisMatrix());
    return Matrix(glm::transpose(b) * m.m * b);
}

Matrix fromUnreal(const Matrix& m)
{
    const glm::dmat4 b(basisMatrix());
    return Matrix(b * m.m * glm::transpose(b));
}

Box toUnreal(const Box& b)
{
    if (!b.isValid)
        return b;
    const Vector p = toUnreal(b.min);
    const Vector q = toUnreal(b.max);
    return {p.componentMin(q), p.componentMax(q)};
}

Box fromUnreal(const Box& b)
{
    if (!b.isValid)
        return b;
    const Vector p = fromUnreal(b.min);
    const Vector q = fromUnreal(b.max);
    return {p.componentMin(q), p.componentMax(q)};
}

// Kismet-style helpers -----------------------------------------------------------------------

namespace
{
/** Rotator from axes in Unreal-frame components, where X = Y x Z algebraically. */
Rotator fromUnrealFrame(const Vector& x, const Vector& y, const Vector& z)
{
    return rotatorFromUnrealAxes(x.toGlm(), y.toGlm(), z.toGlm());
}

constexpr Vector kUeX{1.0, 0.0, 0.0};
constexpr Vector kUeY{0.0, 1.0, 0.0};
constexpr Vector kUeZ{0.0, 0.0, 1.0};

Rotator frameXY(const Vector& inX, const Vector& inY)
{
    const Vector x = inX.getSafeNormal();
    const Vector z = (x ^ inY).getSafeNormal();
    return fromUnrealFrame(x, z ^ x, z);
}

Rotator frameXZ(const Vector& inX, const Vector& inZ)
{
    const Vector x = inX.getSafeNormal();
    const Vector y = (inZ ^ x).getSafeNormal();
    return fromUnrealFrame(x, y, x ^ y);
}

Rotator frameYX(const Vector& inY, const Vector& inX)
{
    const Vector y = inY.getSafeNormal();
    const Vector z = (inX ^ y).getSafeNormal();
    return fromUnrealFrame(y ^ z, y, z);
}

Rotator frameYZ(const Vector& inY, const Vector& inZ)
{
    const Vector y = inY.getSafeNormal();
    const Vector x = (y ^ inZ).getSafeNormal();
    return fromUnrealFrame(x, y, x ^ y);
}

Rotator frameZX(const Vector& inZ, const Vector& inX)
{
    const Vector z = inZ.getSafeNormal();
    const Vector y = (z ^ inX).getSafeNormal();
    return fromUnrealFrame(y ^ z, y, z);
}

Rotator frameZY(const Vector& inZ, const Vector& inY)
{
    const Vector z = inZ.getSafeNormal();
    const Vector x = (inY ^ z).getSafeNormal();
    return fromUnrealFrame(x, z ^ x, z);
}

bool nearlyParallel(const Vector& unit, const Vector& axis) { return std::abs(unit | axis) > 1.0 - kKindaSmallNumber; }
} // namespace

Rotator findLookAtRotation(const Vector& start, const Vector& target) { return makeRotFromX(target - start); }

Rotator makeRotFromX(const Vector& x) { return x.rotation(); }

Rotator makeRotFromY(const Vector& y)
{
    const Vector u = toUnreal(y).getSafeNormal();
    return frameYZ(u, nearlyParallel(u, kUeZ) ? kUeX : kUeZ);
}

Rotator makeRotFromZ(const Vector& z)
{
    const Vector u = toUnreal(z).getSafeNormal();
    return frameZX(u, nearlyParallel(u, kUeX) ? -kUeZ : kUeX);
}

Rotator makeRotFromXY(const Vector& x, const Vector& y) { return frameXY(toUnreal(x), toUnreal(y)); }
Rotator makeRotFromXZ(const Vector& x, const Vector& z) { return frameXZ(toUnreal(x), toUnreal(z)); }
Rotator makeRotFromYX(const Vector& y, const Vector& x) { return frameYX(toUnreal(y), toUnreal(x)); }
Rotator makeRotFromYZ(const Vector& y, const Vector& z) { return frameYZ(toUnreal(y), toUnreal(z)); }
Rotator makeRotFromZX(const Vector& z, const Vector& x) { return frameZX(toUnreal(z), toUnreal(x)); }
Rotator makeRotFromZY(const Vector& z, const Vector& y) { return frameZY(toUnreal(z), toUnreal(y)); }

Rotator makeRotationFromAxes(const Vector& forward, const Vector& right, const Vector& up)
{
    return fromUnrealFrame(toUnreal(forward).getSafeNormal(), toUnreal(right).getSafeNormal(),
                           toUnreal(up).getSafeNormal());
}

Rotator composeRotators(const Rotator& a, const Rotator& b) { return Rotator(b.quaternion() * a.quaternion()); }

Rotator normalizedDeltaRotator(const Rotator& a, const Rotator& b) { return (a - b).getNormalized(); }

Rotator rlerp(const Rotator& a, const Rotator& b, double alpha, bool shortestPath)
{
    if (shortestPath)
        return Rotator(Quat::slerp(a.quaternion(), b.quaternion(), alpha));
    return a + (b - a) * alpha;
}

Vector vinterpTo(const Vector& current, const Vector& target, double deltaTime, double interpSpeed)
{
    if (interpSpeed <= 0.0)
        return target;
    const Vector dist = target - current;
    if (dist.sizeSquared() < kKindaSmallNumber)
        return target;
    return current + dist * clamp(deltaTime * interpSpeed, 0.0, 1.0);
}

Vector vinterpToConstant(const Vector& current, const Vector& target, double deltaTime, double interpSpeed)
{
    const Vector delta = target - current;
    const double size = delta.size();
    const double step = interpSpeed * deltaTime;
    if (size > step)
        return step > 0.0 ? current + delta / size * step : current;
    return target;
}

Vector2 vinterpTo(const Vector2& current, const Vector2& target, double deltaTime, double interpSpeed)
{
    if (interpSpeed <= 0.0)
        return target;
    const Vector2 dist = target - current;
    if (dist.sizeSquared() < kKindaSmallNumber)
        return target;
    return current + dist * clamp(deltaTime * interpSpeed, 0.0, 1.0);
}

Rotator rinterpTo(const Rotator& current, const Rotator& target, double deltaTime, double interpSpeed)
{
    if (deltaTime == 0.0 || current == target)
        return current;
    if (interpSpeed <= 0.0)
        return target;
    const Rotator delta = (target - current).getNormalized();
    if (delta.isNearlyZero())
        return target;
    return (current + delta * clamp(deltaTime * interpSpeed, 0.0, 1.0)).getNormalized();
}

Rotator rinterpToConstant(const Rotator& current, const Rotator& target, double deltaTime, double interpSpeed)
{
    if (deltaTime == 0.0 || current == target)
        return current;
    if (interpSpeed <= 0.0)
        return target;
    const double step = interpSpeed * deltaTime;
    const Rotator delta = (target - current).getNormalized();
    const Rotator moved(current.pitch + clamp(delta.pitch, -step, step), current.yaw + clamp(delta.yaw, -step, step),
                        current.roll + clamp(delta.roll, -step, step));
    return moved.getNormalized();
}

Quat qinterpTo(const Quat& current, const Quat& target, double deltaTime, double interpSpeed)
{
    if (interpSpeed <= 0.0 || current.equals(target, kSmallNumber))
        return target;
    return Quat::slerp(current, target, clamp(deltaTime * interpSpeed, 0.0, 1.0));
}

Quat qinterpToConstant(const Quat& current, const Quat& target, double deltaTime, double interpSpeed)
{
    if (interpSpeed <= 0.0 || current.equals(target, kSmallNumber))
        return target;
    const double angle = current.angularDistance(target);
    if (isNearlyZero(angle))
        return target;
    return Quat::slerp(current, target, clamp(deltaTime * interpSpeed / angle, 0.0, 1.0));
}

LinearColor cinterpTo(const LinearColor& current, const LinearColor& target, double deltaTime, double interpSpeed)
{
    if (interpSpeed <= 0.0)
        return target;
    const LinearColor dist = target - current;
    if ((dist.r * dist.r + dist.g * dist.g + dist.b * dist.b + dist.a * dist.a) < kKindaSmallNumber)
        return target;
    return current + dist * float(clamp(deltaTime * interpSpeed, 0.0, 1.0));
}

Transform tinterpTo(const Transform& current, const Transform& target, double deltaTime, double interpSpeed)
{
    if (interpSpeed <= 0.0)
        return target;
    return Transform::blend(current, target, clamp(deltaTime * interpSpeed, 0.0, 1.0));
}

Vector getDirectionUnitVector(const Vector& from, const Vector& to) { return (to - from).getSafeNormal(); }

void getYawPitchFromVector(const Vector& v, double& yaw, double& pitch)
{
    const Rotator r = v.rotation();
    yaw = r.yaw;
    pitch = r.pitch;
}
} // namespace lila::math
