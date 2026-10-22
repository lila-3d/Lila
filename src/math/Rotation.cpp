#include "math/Quat.h"

namespace lila::math
{
// Rotator ------------------------------------------------------------------------------------

void rotatorToUnrealAxes(const Rotator& r, glm::dvec3& forward, glm::dvec3& right, glm::dvec3& up)
{
    const double sp = std::sin(degreesToRadians(r.pitch));
    const double cp = std::cos(degreesToRadians(r.pitch));
    const double sy = std::sin(degreesToRadians(r.yaw));
    const double cy = std::cos(degreesToRadians(r.yaw));
    const double sr = std::sin(degreesToRadians(r.roll));
    const double cr = std::cos(degreesToRadians(r.roll));

    // Pitch and yaw alone give forward, right and up; roll then turns right and up about forward.
    forward = {cp * cy, cp * sy, sp};
    const glm::dvec3 right0{-sy, cy, 0.0};
    const glm::dvec3 up0{-sp * cy, -sp * sy, cp};
    right = cr * right0 - sr * up0;
    up = sr * right0 + cr * up0;
}

Rotator rotatorFromUnrealAxes(const glm::dvec3& forward, const glm::dvec3& right, const glm::dvec3& up)
{
    const double horizontal = std::sqrt(forward.x * forward.x + forward.y * forward.y);
    const double pitch = std::atan2(forward.z, horizontal);
    // Looking straight up or down, yaw comes from right, which stays horizontal at zero roll.
    const double yaw = horizontal < 1.0e-9 ? std::atan2(-right.x, right.y) : std::atan2(forward.y, forward.x);

    const double sp = std::sin(pitch);
    const double cp = std::cos(pitch);
    const double sy = std::sin(yaw);
    const double cy = std::cos(yaw);
    const glm::dvec3 right0{-sy, cy, 0.0};
    const glm::dvec3 up0{-sp * cy, -sp * sy, cp};
    const double roll = std::atan2(glm::dot(up, right0), glm::dot(up, up0));
    return {radiansToDegrees(pitch), radiansToDegrees(yaw), radiansToDegrees(roll)};
}

Rotator::Rotator(const Quat& q) : Rotator(fromGlmMat3(q.toGlmMat3())) {}

bool Rotator::isNearlyZero(double tolerance) const
{
    return std::abs(normalizeAxis(pitch)) <= tolerance && std::abs(normalizeAxis(yaw)) <= tolerance &&
           std::abs(normalizeAxis(roll)) <= tolerance;
}

bool Rotator::equals(const Rotator& b, double tolerance) const { return (*this - b).isNearlyZero(tolerance); }

bool Rotator::containsNaN() const { return !std::isfinite(pitch) || !std::isfinite(yaw) || !std::isfinite(roll); }

void Rotator::normalize()
{
    pitch = normalizeAxis(pitch);
    yaw = normalizeAxis(yaw);
    roll = normalizeAxis(roll);
}

Rotator Rotator::getNormalized() const
{
    Rotator r = *this;
    r.normalize();
    return r;
}

Rotator Rotator::getDenormalized() const { return {clampAxis(pitch), clampAxis(yaw), clampAxis(roll)}; }

Rotator Rotator::add(double deltaPitch, double deltaYaw, double deltaRoll) const
{
    return {pitch + deltaPitch, yaw + deltaYaw, roll + deltaRoll};
}

Rotator Rotator::gridSnap(const Rotator& grid) const
{
    return {math::gridSnap(pitch, grid.pitch), math::gridSnap(yaw, grid.yaw), math::gridSnap(roll, grid.roll)};
}

double Rotator::getManhattanDistance(const Rotator& b) const
{
    const Rotator d = (*this - b).getNormalized();
    return std::abs(d.pitch) + std::abs(d.yaw) + std::abs(d.roll);
}

Rotator Rotator::getInverse() const { return Rotator(quaternion().inverse()); }

Quat Rotator::quaternion() const { return Quat::fromGlmMat3(toGlmMat3()); }

glm::dmat3 Rotator::toGlmMat3() const
{
    glm::dvec3 f, r, u;
    rotatorToUnrealAxes(*this, f, r, u);
    const AxisBasis& b = axisBasis();
    const glm::dmat3 basis(b.forward, b.right, b.up);
    const glm::dmat3 rotated(basis * f, basis * r, basis * u);
    // Takes forward/right/up onto the rotated axes; the basis is orthonormal so its inverse is its transpose.
    return rotated * glm::transpose(basis);
}

Rotator Rotator::fromGlmMat3(const glm::dmat3& m)
{
    const AxisBasis& b = axisBasis();
    const glm::dmat3 toUe = glm::transpose(glm::dmat3(b.forward, b.right, b.up));
    return rotatorFromUnrealAxes(toUe * (m * b.forward), toUe * (m * b.right), toUe * (m * b.up));
}

Vector Rotator::getForwardVector() const { return Vector(toGlmMat3() * axisBasis().forward); }
Vector Rotator::getRightVector() const { return Vector(toGlmMat3() * axisBasis().right); }
Vector Rotator::getUpVector() const { return Vector(toGlmMat3() * axisBasis().up); }
Vector Rotator::rotateVector(const Vector& v) const { return Vector(toGlmMat3() * v.toGlm()); }
Vector Rotator::unrotateVector(const Vector& v) const { return Vector(glm::transpose(toGlmMat3()) * v.toGlm()); }

// Quat ---------------------------------------------------------------------------------------

Quat::Quat(const Vector& axis, double angleRadians)
{
    const double s = std::sin(angleRadians * 0.5);
    x = axis.x * s;
    y = axis.y * s;
    z = axis.z * s;
    w = std::cos(angleRadians * 0.5);
}

Quat::Quat(const Rotator& r) : Quat(r.quaternion()) {}

Quat Quat::fromGlmMat3(const glm::dmat3& m) { return Quat(glm::quat_cast(m)).getNormalized(); }

Quat Quat::findBetweenVectors(const Vector& a, const Vector& b)
{
    const double norms = std::sqrt(a.sizeSquared() * b.sizeSquared());
    const double w = norms + (a | b);
    if (w < 1.0e-6 * norms)
    {
        // Opposite: any axis perpendicular to a works.
        const Vector axis = std::abs(a.x) > std::abs(a.z) ? Vector(-a.y, a.x, 0.0) : Vector(0.0, -a.z, a.y);
        return Quat(axis.x, axis.y, axis.z, 0.0).getNormalized();
    }
    const Vector c = a ^ b;
    return Quat(c.x, c.y, c.z, w).getNormalized();
}

Quat Quat::findBetweenNormals(const Vector& a, const Vector& b) { return findBetweenVectors(a, b); }

Quat Quat::slerpFullPath(const Quat& a, const Quat& b, double alpha)
{
    const double cosom = clamp(a | b, -1.0, 1.0);
    const double omega = std::acos(cosom);
    const double sinom = std::sin(omega);
    if (std::abs(sinom) < 1.0e-6)
        return (a * (1.0 - alpha) + b * alpha).getNormalized();
    const double s0 = std::sin((1.0 - alpha) * omega) / sinom;
    const double s1 = std::sin(alpha * omega) / sinom;
    return (a * s0 + b * s1).getNormalized();
}

Quat Quat::slerp(const Quat& a, const Quat& b, double alpha)
{
    return slerpFullPath(a, (a | b) < 0.0 ? -b : b, alpha);
}

Quat Quat::fastLerp(const Quat& a, const Quat& b, double alpha)
{
    const Quat bb = (a | b) < 0.0 ? -b : b;
    return (a * (1.0 - alpha) + bb * alpha).getNormalized();
}

void Quat::normalize(double tolerance)
{
    const double sq = sizeSquared();
    if (sq >= tolerance)
        *this = *this * (1.0 / std::sqrt(sq));
    else
        *this = identity();
}

Quat Quat::getNormalized(double tolerance) const
{
    Quat q = *this;
    q.normalize(tolerance);
    return q;
}

bool Quat::equals(const Quat& b, double tolerance) const
{
    const bool same = std::abs(x - b.x) <= tolerance && std::abs(y - b.y) <= tolerance &&
                      std::abs(z - b.z) <= tolerance && std::abs(w - b.w) <= tolerance;
    const bool negated = std::abs(x + b.x) <= tolerance && std::abs(y + b.y) <= tolerance &&
                         std::abs(z + b.z) <= tolerance && std::abs(w + b.w) <= tolerance;
    return same || negated;
}

bool Quat::containsNaN() const
{
    return !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(w);
}

Vector Quat::rotateVector(const Vector& v) const
{
    const Vector q(x, y, z);
    const Vector t = (q ^ v) * 2.0;
    return v + t * w + (q ^ t);
}

Vector Quat::unrotateVector(const Vector& v) const
{
    const Vector q(-x, -y, -z);
    const Vector t = (q ^ v) * 2.0;
    return v + t * w + (q ^ t);
}

glm::dmat3 Quat::toGlmMat3() const { return glm::mat3_cast(toGlm()); }

double Quat::getAngle() const { return 2.0 * std::acos(clamp(w, -1.0, 1.0)); }

Vector Quat::getRotationAxis() const
{
    const double s = std::sqrt(std::max(1.0 - w * w, 0.0));
    if (s >= 1.0e-4)
        return Vector(x / s, y / s, z / s);
    return Vector::xAxis();
}

void Quat::toAxisAndAngle(Vector& axis, double& angleRadians) const
{
    angleRadians = getAngle();
    axis = getRotationAxis();
}

double Quat::angularDistance(const Quat& b) const
{
    const double inner = *this | b;
    return std::acos(clamp(2.0 * inner * inner - 1.0, -1.0, 1.0));
}

void Quat::enforceShortestArcWith(const Quat& other)
{
    if ((*this | other) < 0.0)
        *this = -*this;
}
} // namespace lila::math
