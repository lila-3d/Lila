#pragma once

#include "math/Rotator.h"

#include <glm/gtc/quaternion.hpp>

namespace lila::math
{
/**
 * Unit quaternion, (x, y, z, w) order. Right-handed in Lila space: Quat(axis, angle) turns by the
 * right-hand rule. A * B applies B first, then A.
 */
struct Quat
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double w = 1.0;

    constexpr Quat() = default;
    constexpr Quat(double inX, double inY, double inZ, double inW) : x(inX), y(inY), z(inZ), w(inW) {}
    /** Rotation of angleRadians about a unit axis. */
    Quat(const Vector& axis, double angleRadians);
    explicit Quat(const Rotator& r);
    Quat(const glm::dquat& q) : x(q.x), y(q.y), z(q.z), w(q.w) {}
    glm::dquat toGlm() const { return glm::dquat(w, x, y, z); }

    static constexpr Quat identity() { return {}; }
    static Quat makeFromRotator(const Rotator& r) { return Quat(r); }
    /** From a Lila-space rotation matrix (orthonormal, det +1). */
    static Quat fromGlmMat3(const glm::dmat3& m);
    /** Shortest rotation taking direction a onto direction b (any lengths). */
    static Quat findBetweenVectors(const Vector& a, const Vector& b);
    static Quat findBetweenNormals(const Vector& a, const Vector& b);
    /** Spherical interpolation along the shortest arc, result normalized. */
    static Quat slerp(const Quat& a, const Quat& b, double alpha);
    /** Spherical interpolation without flipping to the shortest arc. */
    static Quat slerpFullPath(const Quat& a, const Quat& b, double alpha);
    /** Normalized linear blend along the shortest arc. */
    static Quat fastLerp(const Quat& a, const Quat& b, double alpha);

    Quat operator*(const Quat& b) const { return Quat(toGlm() * b.toGlm()); }
    Vector operator*(const Vector& v) const { return rotateVector(v); }
    constexpr Quat operator*(double s) const { return {x * s, y * s, z * s, w * s}; }
    constexpr Quat operator+(const Quat& b) const { return {x + b.x, y + b.y, z + b.z, w + b.w}; }
    constexpr Quat operator-(const Quat& b) const { return {x - b.x, y - b.y, z - b.z, w - b.w}; }
    constexpr Quat operator-() const { return {-x, -y, -z, -w}; }
    Quat& operator*=(const Quat& b) { return *this = *this * b; }
    constexpr bool operator==(const Quat&) const = default;
    /** 4D dot product. */
    constexpr double operator|(const Quat& b) const { return x * b.x + y * b.y + z * b.z + w * b.w; }
    constexpr double dot(const Quat& b) const { return *this | b; }

    double size() const { return std::sqrt(sizeSquared()); }
    constexpr double sizeSquared() const { return x * x + y * y + z * z + w * w; }
    void normalize(double tolerance = kSmallNumber);
    Quat getNormalized(double tolerance = kSmallNumber) const;
    bool isNormalized() const { return std::abs(1.0 - sizeSquared()) < 0.01; }
    bool isIdentity(double tolerance = kSmallNumber) const { return equals(identity(), tolerance); }
    /** Same rotation within tolerance; q and -q are equal. */
    bool equals(const Quat& b, double tolerance = kKindaSmallNumber) const;
    bool containsNaN() const;

    /** Conjugate; equals the inverse for a unit quat. */
    constexpr Quat inverse() const { return {-x, -y, -z, w}; }
    Vector rotateVector(const Vector& v) const;
    Vector unrotateVector(const Vector& v) const;
    Rotator rotator() const { return Rotator(*this); }
    glm::dmat3 toGlmMat3() const;

    /** Angle in radians, [0, 2pi]. */
    double getAngle() const;
    Vector getRotationAxis() const;
    void toAxisAndAngle(Vector& axis, double& angleRadians) const;
    /** Angle in radians of the rotation between this and b, [0, pi]. */
    double angularDistance(const Quat& b) const;
    /** Negates this if needed so it lies on the same hemisphere as other. */
    void enforceShortestArcWith(const Quat& other);

    /** The rotated Lila X, Y, Z axes. */
    Vector getAxisX() const { return rotateVector(Vector::xAxis()); }
    Vector getAxisY() const { return rotateVector(Vector::yAxis()); }
    Vector getAxisZ() const { return rotateVector(Vector::zAxis()); }
    /** The rotated semantic axes for the current up axis. */
    Vector getForwardVector() const { return rotateVector(Vector::forward()); }
    Vector getRightVector() const { return rotateVector(Vector::right()); }
    Vector getUpVector() const { return rotateVector(Vector::up()); }
};
} // namespace lila::math
