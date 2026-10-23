#pragma once

#include "core/Export.h"

#include "math/Color.h"
#include "math/Transform.h"

namespace lila::math
{
/** Rotator whose forward points from start to target, zero roll. */
LILA_FOUNDATION_API Rotator findLookAtRotation(const Vector& start, const Vector& target);

/**
 * Rotators built from local axes. X, Y, Z name the rotator's own axes as in Unreal: X forward,
 * Y right, Z up (the semantic axes of MathCore.h), not Lila's world X/Y/Z. The first named axis
 * is kept exactly, the second only fixes the roll.
 */
LILA_FOUNDATION_API Rotator makeRotFromX(const Vector& x);
LILA_FOUNDATION_API Rotator makeRotFromY(const Vector& y);
LILA_FOUNDATION_API Rotator makeRotFromZ(const Vector& z);
LILA_FOUNDATION_API Rotator makeRotFromXY(const Vector& x, const Vector& y);
LILA_FOUNDATION_API Rotator makeRotFromXZ(const Vector& x, const Vector& z);
LILA_FOUNDATION_API Rotator makeRotFromYX(const Vector& y, const Vector& x);
LILA_FOUNDATION_API Rotator makeRotFromYZ(const Vector& y, const Vector& z);
LILA_FOUNDATION_API Rotator makeRotFromZX(const Vector& z, const Vector& x);
LILA_FOUNDATION_API Rotator makeRotFromZY(const Vector& z, const Vector& y);
LILA_FOUNDATION_API Rotator makeRotationFromAxes(const Vector& forward, const Vector& right, const Vector& up);

/** a then b. */
LILA_FOUNDATION_API Rotator composeRotators(const Rotator& a, const Rotator& b);
/** a - b, normalized. */
LILA_FOUNDATION_API Rotator normalizedDeltaRotator(const Rotator& a, const Rotator& b);
LILA_FOUNDATION_API Rotator rlerp(const Rotator& a, const Rotator& b, double alpha, bool shortestPath);

LILA_FOUNDATION_API Vector vinterpTo(const Vector& current, const Vector& target, double deltaTime, double interpSpeed);
LILA_FOUNDATION_API Vector vinterpToConstant(const Vector& current, const Vector& target, double deltaTime, double interpSpeed);
LILA_FOUNDATION_API Vector2 vinterpTo(const Vector2& current, const Vector2& target, double deltaTime, double interpSpeed);
LILA_FOUNDATION_API Rotator rinterpTo(const Rotator& current, const Rotator& target, double deltaTime, double interpSpeed);
/** interpSpeed is degrees per second. */
LILA_FOUNDATION_API Rotator rinterpToConstant(const Rotator& current, const Rotator& target, double deltaTime, double interpSpeed);
LILA_FOUNDATION_API Quat qinterpTo(const Quat& current, const Quat& target, double deltaTime, double interpSpeed);
/** interpSpeed is radians per second. */
LILA_FOUNDATION_API Quat qinterpToConstant(const Quat& current, const Quat& target, double deltaTime, double interpSpeed);
LILA_FOUNDATION_API LinearColor cinterpTo(const LinearColor& current, const LinearColor& target, double deltaTime, double interpSpeed);
LILA_FOUNDATION_API Transform tinterpTo(const Transform& current, const Transform& target, double deltaTime, double interpSpeed);

/** Unit vector from from to to; zero when they coincide. */
LILA_FOUNDATION_API Vector getDirectionUnitVector(const Vector& from, const Vector& to);
/** Yaw and pitch in degrees of a direction. */
LILA_FOUNDATION_API void getYawPitchFromVector(const Vector& v, double& yaw, double& pitch);
} // namespace lila::math
