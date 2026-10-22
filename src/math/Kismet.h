#pragma once

#include "math/Color.h"
#include "math/Transform.h"

namespace lila::math
{
/** Rotator whose forward points from start to target, zero roll. */
Rotator findLookAtRotation(const Vector& start, const Vector& target);

/**
 * Rotators built from local axes. X, Y, Z name the rotator's own axes as in Unreal: X forward,
 * Y right, Z up (the semantic axes of MathCore.h), not Lila's world X/Y/Z. The first named axis
 * is kept exactly, the second only fixes the roll.
 */
Rotator makeRotFromX(const Vector& x);
Rotator makeRotFromY(const Vector& y);
Rotator makeRotFromZ(const Vector& z);
Rotator makeRotFromXY(const Vector& x, const Vector& y);
Rotator makeRotFromXZ(const Vector& x, const Vector& z);
Rotator makeRotFromYX(const Vector& y, const Vector& x);
Rotator makeRotFromYZ(const Vector& y, const Vector& z);
Rotator makeRotFromZX(const Vector& z, const Vector& x);
Rotator makeRotFromZY(const Vector& z, const Vector& y);
Rotator makeRotationFromAxes(const Vector& forward, const Vector& right, const Vector& up);

/** a then b. */
Rotator composeRotators(const Rotator& a, const Rotator& b);
/** a - b, normalized. */
Rotator normalizedDeltaRotator(const Rotator& a, const Rotator& b);
Rotator rlerp(const Rotator& a, const Rotator& b, double alpha, bool shortestPath);

Vector vinterpTo(const Vector& current, const Vector& target, double deltaTime, double interpSpeed);
Vector vinterpToConstant(const Vector& current, const Vector& target, double deltaTime, double interpSpeed);
Vector2 vinterpTo(const Vector2& current, const Vector2& target, double deltaTime, double interpSpeed);
Rotator rinterpTo(const Rotator& current, const Rotator& target, double deltaTime, double interpSpeed);
/** interpSpeed is degrees per second. */
Rotator rinterpToConstant(const Rotator& current, const Rotator& target, double deltaTime, double interpSpeed);
Quat qinterpTo(const Quat& current, const Quat& target, double deltaTime, double interpSpeed);
/** interpSpeed is radians per second. */
Quat qinterpToConstant(const Quat& current, const Quat& target, double deltaTime, double interpSpeed);
LinearColor cinterpTo(const LinearColor& current, const LinearColor& target, double deltaTime, double interpSpeed);
Transform tinterpTo(const Transform& current, const Transform& target, double deltaTime, double interpSpeed);

/** Unit vector from from to to; zero when they coincide. */
Vector getDirectionUnitVector(const Vector& from, const Vector& to);
/** Yaw and pitch in degrees of a direction. */
void getYawPitchFromVector(const Vector& v, double& yaw, double& pitch);
} // namespace lila::math
