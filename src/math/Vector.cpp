#include "math/Plane.h"
#include "math/Quat.h"
#include "math/Vector.h"

namespace lila::math
{
// Vector2 ------------------------------------------------------------------------------------

bool Vector2::normalize(double tolerance)
{
    const double sq = sizeSquared();
    if (sq <= tolerance)
        return false;
    *this = *this / std::sqrt(sq);
    return true;
}

Vector2 Vector2::getSafeNormal(double tolerance) const
{
    const double sq = sizeSquared();
    if (sq == 1.0)
        return *this;
    if (sq <= tolerance)
        return zero();
    return *this / std::sqrt(sq);
}

bool Vector2::isNearlyZero(double tolerance) const { return std::abs(x) <= tolerance && std::abs(y) <= tolerance; }

bool Vector2::equals(const Vector2& b, double tolerance) const
{
    return std::abs(x - b.x) <= tolerance && std::abs(y - b.y) <= tolerance;
}

Vector2 Vector2::getRotated(double angleDegrees) const
{
    const double s = std::sin(degreesToRadians(angleDegrees));
    const double c = std::cos(degreesToRadians(angleDegrees));
    return {c * x - s * y, s * x + c * y};
}

Vector2 Vector2::clampAxes(double minAxis, double maxAxis) const
{
    return {clamp(x, minAxis, maxAxis), clamp(y, minAxis, maxAxis)};
}

// Vector -------------------------------------------------------------------------------------

Vector Vector::flatten() const
{
    const Vector up = axisBasis().up;
    return *this - up * (*this | up);
}

// Tolerances compare squared sizes, as in Unreal.
bool Vector::normalize(double tolerance)
{
    const double sq = sizeSquared();
    if (sq <= tolerance)
        return false;
    *this = *this / std::sqrt(sq);
    return true;
}

Vector Vector::getSafeNormal(double tolerance, const Vector& resultIfZero) const
{
    const double sq = sizeSquared();
    if (sq == 1.0)
        return *this;
    if (sq <= tolerance)
        return resultIfZero;
    return *this / std::sqrt(sq);
}

Vector Vector::getSafeNormal2D(double tolerance, const Vector& resultIfZero) const
{
    return flatten().getSafeNormal(tolerance, resultIfZero);
}

bool Vector::isNearlyZero(double tolerance) const
{
    return std::abs(x) <= tolerance && std::abs(y) <= tolerance && std::abs(z) <= tolerance;
}

bool Vector::isUnit(double lengthSquaredTolerance) const
{
    return std::abs(1.0 - sizeSquared()) < lengthSquaredTolerance;
}

bool Vector::isUniform(double tolerance) const
{
    return std::abs(x - y) <= tolerance && std::abs(x - z) <= tolerance && std::abs(y - z) <= tolerance;
}

bool Vector::equals(const Vector& b, double tolerance) const
{
    return std::abs(x - b.x) <= tolerance && std::abs(y - b.y) <= tolerance && std::abs(z - b.z) <= tolerance;
}

bool Vector::containsNaN() const { return !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z); }

Vector Vector::clampSize(double minSize, double maxSize) const
{
    const double s = size();
    const Vector dir = s > kSmallNumber ? *this / s : zero();
    return dir * clamp(s, minSize, maxSize);
}

Vector Vector::clampSize2D(double minSize, double maxSize) const
{
    const Vector flat = flatten();
    const Vector vertical = *this - flat;
    return flat.clampSize(minSize, maxSize) + vertical;
}

Vector Vector::clampMaxSize(double maxSize) const
{
    if (maxSize < kKindaSmallNumber)
        return zero();
    const double sq = sizeSquared();
    if (sq > maxSize * maxSize)
        return *this * (maxSize / std::sqrt(sq));
    return *this;
}

Vector Vector::projection() const
{
    const double rz = 1.0 / z;
    return {x * rz, y * rz, 1.0};
}

Vector Vector::projectOnTo(const Vector& target) const
{
    const double sq = target.sizeSquared();
    if (sq <= kSmallNumber)
        return zero();
    return target * ((*this | target) / sq);
}

Vector Vector::mirrorByVector(const Vector& mirrorNormal) const
{
    return *this - mirrorNormal * (2.0 * (*this | mirrorNormal));
}

Vector Vector::mirrorByPlane(const Plane& plane) const
{
    return *this - plane.getNormal() * (2.0 * plane.planeDot(*this));
}

Vector Vector::rotateAngleAxis(double angleDegrees, const Vector& axis) const
{
    return Quat(axis.getSafeNormal(), degreesToRadians(angleDegrees)).rotateVector(*this);
}

double Vector::headingAngle() const
{
    const AxisBasis& b = axisBasis();
    const glm::dvec3 v = toGlm();
    return std::atan2(glm::dot(v, b.right), glm::dot(v, b.forward));
}

double Vector::cosineAngle2D(const Vector& b) const
{
    return flatten().getSafeNormal() | b.flatten().getSafeNormal();
}

Vector Vector::componentMin(const Vector& b) const
{
    return {std::min(x, b.x), std::min(y, b.y), std::min(z, b.z)};
}

Vector Vector::componentMax(const Vector& b) const
{
    return {std::max(x, b.x), std::max(y, b.y), std::max(z, b.z)};
}

Vector Vector::getSignVector() const
{
    return {x >= 0.0 ? 1.0 : -1.0, y >= 0.0 ? 1.0 : -1.0, z >= 0.0 ? 1.0 : -1.0};
}

Vector Vector::gridSnap(double grid) const
{
    return {math::gridSnap(x, grid), math::gridSnap(y, grid), math::gridSnap(z, grid)};
}

Vector Vector::boundToCube(double radius) const
{
    return {clamp(x, -radius, radius), clamp(y, -radius, radius), clamp(z, -radius, radius)};
}

Vector Vector::boundToBox(const Vector& minV, const Vector& maxV) const
{
    return {clamp(x, minV.x, maxV.x), clamp(y, minV.y, maxV.y), clamp(z, minV.z, maxV.z)};
}

Rotator Vector::rotation() const
{
    const AxisBasis& b = axisBasis();
    const glm::dvec3 v = toGlm();
    const double f = glm::dot(v, b.forward);
    const double r = glm::dot(v, b.right);
    const double u = glm::dot(v, b.up);
    return {radiansToDegrees(std::atan2(u, std::sqrt(f * f + r * r))), radiansToDegrees(std::atan2(r, f)), 0.0};
}

Rotator Vector::toRotator() const { return rotation(); }

Quat Vector::toOrientationQuat() const { return rotation().quaternion(); }

// Vector4 ------------------------------------------------------------------------------------

Vector4 Vector4::getSafeNormal(double tolerance) const
{
    const double sq = sizeSquared3();
    if (sq <= tolerance)
        return {};
    const double inv = 1.0 / std::sqrt(sq);
    return {x * inv, y * inv, z * inv, 0.0};
}

bool Vector4::equals(const Vector4& b, double tolerance) const
{
    return std::abs(x - b.x) <= tolerance && std::abs(y - b.y) <= tolerance && std::abs(z - b.z) <= tolerance &&
           std::abs(w - b.w) <= tolerance;
}
} // namespace lila::math
