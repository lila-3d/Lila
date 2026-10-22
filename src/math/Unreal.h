#pragma once

#include "math/Box.h"

namespace lila::math
{
/**
 * Conversions between Lila space (right-handed, current up axis, cm) and Unreal space
 * (left-handed Z-up, X forward, Y right, cm). Lila forward/right/up become Unreal +X/+Y/+Z.
 *
 * Vectors (positions and directions) map component-wise. Quats map as a change of handedness.
 * Rotators carry the same numbers in both spaces, so their conversion is the identity. Scale is
 * per local axis and maps by permutation without sign. Matrices map as B^T M B and come out in
 * FMatrix row layout, ready to copy into an FMatrix element for element.
 */
Vector toUnreal(const Vector& v);
Vector fromUnreal(const Vector& v);
Vector scaleToUnreal(const Vector& s);
Vector scaleFromUnreal(const Vector& s);
Quat toUnreal(const Quat& q);
Quat fromUnreal(const Quat& q);
inline Rotator toUnreal(const Rotator& r) { return r; }
inline Rotator fromUnreal(const Rotator& r) { return r; }
Transform toUnreal(const Transform& t);
Transform fromUnreal(const Transform& t);
Matrix toUnreal(const Matrix& m);
Matrix fromUnreal(const Matrix& m);
Box toUnreal(const Box& b);
Box fromUnreal(const Box& b);
} // namespace lila::math
