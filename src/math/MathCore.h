#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

/**
 * lila::math: Unreal-flavoured vector math on top of glm, double precision.
 *
 * Coordinate system. Lila is right-handed, measured in cm, Y-up by default with Z-up available
 * per scene (setUpAxis). Each up axis has a semantic basis that Rotator, the directional
 * constants and the "2D" (ground plane) helpers read:
 *
 *     up axis   forward   right   up
 *     Y         +Z        -X      +Y
 *     Z         +X        -Y      +Z
 *
 * right = cross(forward, up). Unreal is left-handed Z-up with X forward, Y right, Z up, so
 * Lila's forward/right/up map onto Unreal's +X/+Y/+Z exactly (see Unreal.h).
 *
 * Rotator angles mean what they mean in Unreal, measured on the semantic axes: positive yaw turns
 * forward toward right, positive pitch turns forward toward up, positive roll turns up toward
 * right. Roll is applied first, then pitch, then yaw. A Rotator therefore carries the same
 * numbers in Lila and in Unreal.
 *
 * Everything else (Quat, axis-angle, rotateAngleAxis, cross products, Matrix) is plain
 * right-handed math in Lila space: a positive angle about an axis follows the right-hand rule.
 * In Lila space a positive yaw is a negative right-handed turn about up, because Unreal's
 * handedness is the opposite.
 *
 * Composition follows Unreal: for Transform and Matrix, A * B applies A first, then B. Quat
 * multiplication is the usual one: A * B applies B first, then A.
 */
namespace lila::math
{
inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kSmallNumber = 1.0e-8;
inline constexpr double kKindaSmallNumber = 1.0e-4;
inline constexpr double kBigNumber = 3.4e38;

inline constexpr double radiansToDegrees(double r) { return r * (180.0 / kPi); }
inline constexpr double degreesToRadians(double d) { return d * (kPi / 180.0); }

inline bool isNearlyEqual(double a, double b, double tolerance = kSmallNumber)
{
    return std::abs(a - b) <= tolerance;
}
inline bool isNearlyZero(double a, double tolerance = kSmallNumber) { return std::abs(a) <= tolerance; }

template <typename T> constexpr T clamp(T v, T lo, T hi) { return v < lo ? lo : (v < hi ? v : hi); }
inline constexpr double lerp(double a, double b, double alpha) { return a + alpha * (b - a); }
inline constexpr double square(double a) { return a * a; }

/** Wraps an angle in degrees into [0, 360). */
double clampAxis(double degrees);
/** Wraps an angle in degrees into (-180, 180]. */
double normalizeAxis(double degrees);
/** Clamps an angle to the arc [minDegrees, maxDegrees] taking the shorter way around; result in (-180, 180]. */
double clampAngle(double degrees, double minDegrees, double maxDegrees);
/** Wraps into [-180, 180]. */
double unwindDegrees(double degrees);
/** Wraps into [-pi, pi]. */
double unwindRadians(double radians);

/** Wraps value into [min, max], treating the range as cyclic. */
double wrap(double value, double minValue, double maxValue);
double gridSnap(double value, double grid);
double getRangePct(double minValue, double maxValue, double value);
double mapRangeUnclamped(double value, double inMin, double inMax, double outMin, double outMax);
double mapRangeClamped(double value, double inMin, double inMax, double outMin, double outMax);

/** Eases toward target; speed <= 0 snaps. */
double finterpTo(double current, double target, double deltaTime, double interpSpeed);
/** Moves toward target at a constant rate of interpSpeed units per second. */
double finterpToConstant(double current, double target, double deltaTime, double interpSpeed);

enum class UpAxis : uint8_t
{
    Y,
    Z,
};

/** Process-wide up axis. The scene sets it when its up axis changes; reads are lock-free. */
void setUpAxis(UpAxis axis);
UpAxis upAxis();

/** Semantic directions for the current up axis, in Lila space. See the table above. */
struct AxisBasis
{
    glm::dvec3 forward;
    glm::dvec3 right;
    glm::dvec3 up;
};

const AxisBasis& axisBasis();
const AxisBasis& axisBasis(UpAxis axis);
} // namespace lila::math
