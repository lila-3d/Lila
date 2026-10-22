#pragma once

#include "math/Vector.h"

#include <glm/mat3x3.hpp>

namespace lila::math
{
struct Quat;
struct Matrix;

/**
 * Pitch, yaw, roll in degrees with Unreal's meaning on Lila's semantic axes (MathCore.h): yaw turns
 * forward toward right about up, pitch turns forward toward up about right, roll turns up toward
 * right about forward. Applied roll, then pitch, then yaw. The numbers match an FRotator for the
 * same orientation once positions go through toUnreal.
 */
struct Rotator
{
    double pitch = 0.0;
    double yaw = 0.0;
    double roll = 0.0;

    constexpr Rotator() = default;
    constexpr Rotator(double inPitch, double inYaw, double inRoll) : pitch(inPitch), yaw(inYaw), roll(inRoll) {}
    constexpr explicit Rotator(double v) : pitch(v), yaw(v), roll(v) {}
    explicit Rotator(const Quat& q);

    static constexpr Rotator zero() { return {}; }

    constexpr Rotator operator+(const Rotator& b) const { return {pitch + b.pitch, yaw + b.yaw, roll + b.roll}; }
    constexpr Rotator operator-(const Rotator& b) const { return {pitch - b.pitch, yaw - b.yaw, roll - b.roll}; }
    constexpr Rotator operator-() const { return {-pitch, -yaw, -roll}; }
    constexpr Rotator operator*(double s) const { return {pitch * s, yaw * s, roll * s}; }
    Rotator& operator+=(const Rotator& b) { return *this = *this + b; }
    Rotator& operator-=(const Rotator& b) { return *this = *this - b; }
    Rotator& operator*=(double s) { return *this = *this * s; }
    constexpr bool operator==(const Rotator&) const = default;

    bool isNearlyZero(double tolerance = kKindaSmallNumber) const;
    constexpr bool isZero() const { return pitch == 0.0 && yaw == 0.0 && roll == 0.0; }
    /** Same orientation angles within tolerance, comparing each axis modulo 360. */
    bool equals(const Rotator& b, double tolerance = kKindaSmallNumber) const;
    bool containsNaN() const;

    /** Wraps every axis into (-180, 180] in place. */
    void normalize();
    Rotator getNormalized() const;
    /** Every axis wrapped into [0, 360). */
    Rotator getDenormalized() const;
    Rotator clamp() const { return getDenormalized(); }
    Rotator add(double deltaPitch, double deltaYaw, double deltaRoll) const;
    Rotator gridSnap(const Rotator& grid) const;
    /** Sum of the absolute normalized per-axis differences. */
    double getManhattanDistance(const Rotator& b) const;

    Rotator getInverse() const;
    Quat quaternion() const;
    /** Rotation matrix in Lila space; columns are the rotated Lila X, Y, Z axes. */
    glm::dmat3 toGlmMat3() const;

    /** Forward direction. */
    Vector vector() const { return getForwardVector(); }
    Vector getForwardVector() const;
    Vector getRightVector() const;
    Vector getUpVector() const;
    Vector rotateVector(const Vector& v) const;
    Vector unrotateVector(const Vector& v) const;

    /** From a Lila-space rotation matrix (orthonormal, det +1). */
    static Rotator fromGlmMat3(const glm::dmat3& m);
};

inline constexpr Rotator operator*(double s, const Rotator& r) { return r * s; }

/**
 * Rotator to and from axes expressed in Unreal-frame components (x forward, y right, z up).
 * These are the core of the Rotator definition; everything else goes through them.
 */
void rotatorToUnrealAxes(const Rotator& r, glm::dvec3& forward, glm::dvec3& right, glm::dvec3& up);
Rotator rotatorFromUnrealAxes(const glm::dvec3& forward, const glm::dvec3& right, const glm::dvec3& up);
} // namespace lila::math
