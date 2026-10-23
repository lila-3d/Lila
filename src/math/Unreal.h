#pragma once

#include "core/Export.h"

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
LILA_FOUNDATION_API Vector toUnreal(const Vector& v);
LILA_FOUNDATION_API Vector fromUnreal(const Vector& v);
LILA_FOUNDATION_API Vector scaleToUnreal(const Vector& s);
LILA_FOUNDATION_API Vector scaleFromUnreal(const Vector& s);
LILA_FOUNDATION_API Quat toUnreal(const Quat& q);
LILA_FOUNDATION_API Quat fromUnreal(const Quat& q);
inline Rotator toUnreal(const Rotator& r) { return r; }
inline Rotator fromUnreal(const Rotator& r) { return r; }
LILA_FOUNDATION_API Transform toUnreal(const Transform& t);
LILA_FOUNDATION_API Transform fromUnreal(const Transform& t);
LILA_FOUNDATION_API Matrix toUnreal(const Matrix& m);
LILA_FOUNDATION_API Matrix fromUnreal(const Matrix& m);
LILA_FOUNDATION_API Box toUnreal(const Box& b);
LILA_FOUNDATION_API Box fromUnreal(const Box& b);
} // namespace lila::math
