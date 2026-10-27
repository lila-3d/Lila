#pragma once

/**
 * Unreal-flavoured maths in Lila space, the same types and names as Python's lila.math: Vector, Vector2, Vector4,
 * Rotator, Quat, Matrix, Transform, LinearColor, Color, Box, Plane, the Kismet-style helpers and toUnreal / fromUnreal.
 * Thin over glm: Vector converts from glm::dvec3 and back with toGlm(), Quat likewise with glm::dquat. Also the scalar
 * helpers and the process-wide up axis (math::upAxis, math::axisBasis).
 */
#include "math/Math.h"
