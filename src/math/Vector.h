#pragma once

#include "core/Export.h"

#include "math/MathCore.h"

#include <cstdint>

namespace lila::math
{
struct Rotator;
struct Quat;
struct Plane;

struct LILA_FOUNDATION_API Vector2
{
    double x = 0.0;
    double y = 0.0;

    constexpr Vector2() = default;
    constexpr Vector2(double inX, double inY) : x(inX), y(inY) {}
    constexpr explicit Vector2(double v) : x(v), y(v) {}
    constexpr Vector2(const glm::dvec2& v) : x(v.x), y(v.y) {}
    glm::dvec2 toGlm() const { return {x, y}; }

    static constexpr Vector2 zero() { return {0.0, 0.0}; }
    static constexpr Vector2 one() { return {1.0, 1.0}; }
    static Vector2 unit45Deg() { return {std::sqrt(0.5), std::sqrt(0.5)}; }

    double& operator[](int i) { return i == 0 ? x : y; }
    double operator[](int i) const { return i == 0 ? x : y; }

    constexpr Vector2 operator-() const { return {-x, -y}; }
    constexpr Vector2 operator+(const Vector2& b) const { return {x + b.x, y + b.y}; }
    constexpr Vector2 operator-(const Vector2& b) const { return {x - b.x, y - b.y}; }
    constexpr Vector2 operator*(const Vector2& b) const { return {x * b.x, y * b.y}; }
    constexpr Vector2 operator/(const Vector2& b) const { return {x / b.x, y / b.y}; }
    constexpr Vector2 operator*(double s) const { return {x * s, y * s}; }
    constexpr Vector2 operator/(double s) const { return {x / s, y / s}; }
    Vector2& operator+=(const Vector2& b) { return *this = *this + b; }
    Vector2& operator-=(const Vector2& b) { return *this = *this - b; }
    Vector2& operator*=(double s) { return *this = *this * s; }
    Vector2& operator/=(double s) { return *this = *this / s; }
    constexpr bool operator==(const Vector2&) const = default;
    /** Dot product. */
    constexpr double operator|(const Vector2& b) const { return x * b.x + y * b.y; }
    /** 2D cross product (z of the 3D cross). */
    constexpr double operator^(const Vector2& b) const { return x * b.y - y * b.x; }

    constexpr double dot(const Vector2& b) const { return *this | b; }
    constexpr double cross(const Vector2& b) const { return *this ^ b; }
    double size() const { return std::sqrt(x * x + y * y); }
    constexpr double sizeSquared() const { return x * x + y * y; }
    double dist(const Vector2& b) const { return (*this - b).size(); }
    constexpr double distSquared(const Vector2& b) const { return (*this - b).sizeSquared(); }
    bool normalize(double tolerance = kSmallNumber);
    Vector2 getSafeNormal(double tolerance = kSmallNumber) const;
    bool isNearlyZero(double tolerance = kKindaSmallNumber) const;
    constexpr bool isZero() const { return x == 0.0 && y == 0.0; }
    bool equals(const Vector2& b, double tolerance = kKindaSmallNumber) const;
    Vector2 getAbs() const { return {std::abs(x), std::abs(y)}; }
    double getMax() const { return std::max(x, y); }
    double getMin() const { return std::min(x, y); }
    double getAbsMax() const { return std::max(std::abs(x), std::abs(y)); }
    double getAbsMin() const { return std::min(std::abs(x), std::abs(y)); }
    Vector2 componentMin(const Vector2& b) const { return {std::min(x, b.x), std::min(y, b.y)}; }
    Vector2 componentMax(const Vector2& b) const { return {std::max(x, b.x), std::max(y, b.y)}; }
    Vector2 getSignVector() const { return {x >= 0.0 ? 1.0 : -1.0, y >= 0.0 ? 1.0 : -1.0}; }
    /** Rotated counter-clockwise (right-handed) by angleDegrees. */
    Vector2 getRotated(double angleDegrees) const;
    Vector2 clampAxes(double minAxis, double maxAxis) const;
    static Vector2 lerp(const Vector2& a, const Vector2& b, double alpha) { return a + (b - a) * alpha; }
};

inline constexpr Vector2 operator*(double s, const Vector2& v) { return v * s; }

struct LILA_FOUNDATION_API Vector
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    constexpr Vector() = default;
    constexpr Vector(double inX, double inY, double inZ) : x(inX), y(inY), z(inZ) {}
    constexpr explicit Vector(double v) : x(v), y(v), z(v) {}
    constexpr Vector(const glm::dvec3& v) : x(v.x), y(v.y), z(v.z) {}
    constexpr Vector(const Vector2& v, double inZ) : x(v.x), y(v.y), z(inZ) {}
    glm::dvec3 toGlm() const { return {x, y, z}; }

    static constexpr Vector zero() { return {0.0, 0.0, 0.0}; }
    static constexpr Vector one() { return {1.0, 1.0, 1.0}; }
    static constexpr Vector xAxis() { return {1.0, 0.0, 0.0}; }
    static constexpr Vector yAxis() { return {0.0, 1.0, 0.0}; }
    static constexpr Vector zAxis() { return {0.0, 0.0, 1.0}; }
    /** Semantic directions; they follow the current up axis. */
    static Vector up() { return axisBasis().up; }
    static Vector down() { return -axisBasis().up; }
    static Vector forward() { return axisBasis().forward; }
    static Vector backward() { return -axisBasis().forward; }
    static Vector right() { return axisBasis().right; }
    static Vector left() { return -axisBasis().right; }

    double& operator[](int i) { return i == 0 ? x : (i == 1 ? y : z); }
    double operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }

    constexpr Vector operator-() const { return {-x, -y, -z}; }
    constexpr Vector operator+(const Vector& b) const { return {x + b.x, y + b.y, z + b.z}; }
    constexpr Vector operator-(const Vector& b) const { return {x - b.x, y - b.y, z - b.z}; }
    constexpr Vector operator*(const Vector& b) const { return {x * b.x, y * b.y, z * b.z}; }
    constexpr Vector operator/(const Vector& b) const { return {x / b.x, y / b.y, z / b.z}; }
    constexpr Vector operator+(double s) const { return {x + s, y + s, z + s}; }
    constexpr Vector operator-(double s) const { return {x - s, y - s, z - s}; }
    constexpr Vector operator*(double s) const { return {x * s, y * s, z * s}; }
    constexpr Vector operator/(double s) const { return {x / s, y / s, z / s}; }
    Vector& operator+=(const Vector& b) { return *this = *this + b; }
    Vector& operator-=(const Vector& b) { return *this = *this - b; }
    Vector& operator*=(const Vector& b) { return *this = *this * b; }
    Vector& operator/=(const Vector& b) { return *this = *this / b; }
    Vector& operator*=(double s) { return *this = *this * s; }
    Vector& operator/=(double s) { return *this = *this / s; }
    constexpr bool operator==(const Vector&) const = default;
    /** Dot product. */
    constexpr double operator|(const Vector& b) const { return x * b.x + y * b.y + z * b.z; }
    /** Cross product. */
    constexpr Vector operator^(const Vector& b) const
    {
        return {y * b.z - z * b.y, z * b.x - x * b.z, x * b.y - y * b.x};
    }

    constexpr double dot(const Vector& b) const { return *this | b; }
    constexpr Vector cross(const Vector& b) const { return *this ^ b; }

    double size() const { return std::sqrt(sizeSquared()); }
    constexpr double sizeSquared() const { return x * x + y * y + z * z; }
    /** Ground-plane ("2D") variants drop the component along the current up axis. */
    Vector flatten() const;
    double size2D() const { return flatten().size(); }
    double sizeSquared2D() const { return flatten().sizeSquared(); }

    double dist(const Vector& b) const { return (*this - b).size(); }
    constexpr double distSquared(const Vector& b) const { return (*this - b).sizeSquared(); }
    double dist2D(const Vector& b) const { return (*this - b).size2D(); }
    double distSquared2D(const Vector& b) const { return (*this - b).sizeSquared2D(); }

    /** Normalizes in place; false (and unchanged) if the size is within tolerance of zero. */
    bool normalize(double tolerance = kSmallNumber);
    Vector getSafeNormal(double tolerance = kSmallNumber, const Vector& resultIfZero = zero()) const;
    Vector getSafeNormal2D(double tolerance = kSmallNumber, const Vector& resultIfZero = zero()) const;
    Vector getUnsafeNormal() const { return *this / size(); }

    bool isNearlyZero(double tolerance = kKindaSmallNumber) const;
    constexpr bool isZero() const { return x == 0.0 && y == 0.0 && z == 0.0; }
    bool isNormalized() const { return std::abs(1.0 - sizeSquared()) < 0.01; }
    bool isUnit(double lengthSquaredTolerance = kKindaSmallNumber) const;
    bool isUniform(double tolerance = kKindaSmallNumber) const;
    bool equals(const Vector& b, double tolerance = kKindaSmallNumber) const;
    bool containsNaN() const;

    Vector clampSize(double minSize, double maxSize) const;
    Vector clampSize2D(double minSize, double maxSize) const;
    Vector clampMaxSize(double maxSize) const;

    /** Homogeneous divide by z: (x/z, y/z, 1). */
    Vector projection() const;
    Vector projectOnTo(const Vector& target) const;
    Vector projectOnToNormal(const Vector& normal) const { return normal * (*this | normal); }
    /** Removes the component along planeNormal (assumed unit). */
    Vector projectOnPlane(const Vector& planeNormal) const { return *this - projectOnToNormal(planeNormal); }
    /** Reflects across the plane whose normal is given (assumed unit). */
    Vector mirrorByVector(const Vector& mirrorNormal) const;
    Vector mirrorByPlane(const Plane& plane) const;
    /** Right-handed rotation of angleDegrees about a unit axis. */
    Vector rotateAngleAxis(double angleDegrees, const Vector& axis) const;

    /** Angle in radians on the ground plane from forward toward right, in [-pi, pi]. */
    double headingAngle() const;
    /** Cosine of the angle between the ground-plane projections. */
    double cosineAngle2D(const Vector& b) const;

    Vector getAbs() const { return {std::abs(x), std::abs(y), std::abs(z)}; }
    double getMax() const { return std::max({x, y, z}); }
    double getMin() const { return std::min({x, y, z}); }
    double getAbsMax() const { return std::max({std::abs(x), std::abs(y), std::abs(z)}); }
    double getAbsMin() const { return std::min({std::abs(x), std::abs(y), std::abs(z)}); }
    Vector componentMin(const Vector& b) const;
    Vector componentMax(const Vector& b) const;
    Vector getSignVector() const;
    Vector gridSnap(double grid) const;
    Vector boundToCube(double radius) const;
    Vector boundToBox(const Vector& minV, const Vector& maxV) const;

    /** Rotator facing along this vector, zero roll. */
    Rotator rotation() const;
    Rotator toRotator() const;
    /** Quat facing along this vector, zero roll. */
    Quat toOrientationQuat() const;

    static Vector lerp(const Vector& a, const Vector& b, double alpha) { return a + (b - a) * alpha; }
};

inline constexpr Vector operator*(double s, const Vector& v) { return v * s; }

struct LILA_FOUNDATION_API Vector4
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double w = 0.0;

    constexpr Vector4() = default;
    constexpr Vector4(double inX, double inY, double inZ, double inW) : x(inX), y(inY), z(inZ), w(inW) {}
    constexpr Vector4(const Vector& v, double inW = 1.0) : x(v.x), y(v.y), z(v.z), w(inW) {}
    constexpr Vector4(const glm::dvec4& v) : x(v.x), y(v.y), z(v.z), w(v.w) {}
    glm::dvec4 toGlm() const { return {x, y, z, w}; }
    constexpr Vector xyz() const { return {x, y, z}; }

    double& operator[](int i) { return i == 0 ? x : (i == 1 ? y : (i == 2 ? z : w)); }
    double operator[](int i) const { return i == 0 ? x : (i == 1 ? y : (i == 2 ? z : w)); }

    constexpr Vector4 operator-() const { return {-x, -y, -z, -w}; }
    constexpr Vector4 operator+(const Vector4& b) const { return {x + b.x, y + b.y, z + b.z, w + b.w}; }
    constexpr Vector4 operator-(const Vector4& b) const { return {x - b.x, y - b.y, z - b.z, w - b.w}; }
    constexpr Vector4 operator*(const Vector4& b) const { return {x * b.x, y * b.y, z * b.z, w * b.w}; }
    constexpr Vector4 operator/(const Vector4& b) const { return {x / b.x, y / b.y, z / b.z, w / b.w}; }
    constexpr Vector4 operator*(double s) const { return {x * s, y * s, z * s, w * s}; }
    constexpr Vector4 operator/(double s) const { return {x / s, y / s, z / s, w / s}; }
    constexpr bool operator==(const Vector4&) const = default;

    constexpr double dot3(const Vector4& b) const { return x * b.x + y * b.y + z * b.z; }
    constexpr double dot4(const Vector4& b) const { return x * b.x + y * b.y + z * b.z + w * b.w; }
    double size() const { return std::sqrt(sizeSquared()); }
    constexpr double sizeSquared() const { return x * x + y * y + z * z + w * w; }
    double size3() const { return xyz().size(); }
    constexpr double sizeSquared3() const { return xyz().sizeSquared(); }
    /** Normal of the xyz part; w becomes 0. */
    Vector4 getSafeNormal(double tolerance = kSmallNumber) const;
    bool isNearlyZero3(double tolerance = kKindaSmallNumber) const { return xyz().isNearlyZero(tolerance); }
    bool equals(const Vector4& b, double tolerance = kKindaSmallNumber) const;
};

inline constexpr Vector4 operator*(double s, const Vector4& v) { return v * s; }

struct IntPoint
{
    int32_t x = 0;
    int32_t y = 0;

    constexpr IntPoint() = default;
    constexpr IntPoint(int32_t inX, int32_t inY) : x(inX), y(inY) {}

    constexpr IntPoint operator+(const IntPoint& b) const { return {x + b.x, y + b.y}; }
    constexpr IntPoint operator-(const IntPoint& b) const { return {x - b.x, y - b.y}; }
    constexpr IntPoint operator*(int32_t s) const { return {x * s, y * s}; }
    constexpr IntPoint operator/(int32_t s) const { return {x / s, y / s}; }
    constexpr bool operator==(const IntPoint&) const = default;
    int32_t getMax() const { return std::max(x, y); }
    int32_t getMin() const { return std::min(x, y); }
    double size() const { return std::sqrt(double(x) * x + double(y) * y); }
    constexpr int64_t sizeSquared() const { return int64_t(x) * x + int64_t(y) * y; }
};

struct IntVector
{
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;

    constexpr IntVector() = default;
    constexpr IntVector(int32_t inX, int32_t inY, int32_t inZ) : x(inX), y(inY), z(inZ) {}
    /** Truncates toward zero. */
    explicit IntVector(const Vector& v) : x(int32_t(v.x)), y(int32_t(v.y)), z(int32_t(v.z)) {}
    constexpr Vector toVector() const { return {double(x), double(y), double(z)}; }

    constexpr IntVector operator+(const IntVector& b) const { return {x + b.x, y + b.y, z + b.z}; }
    constexpr IntVector operator-(const IntVector& b) const { return {x - b.x, y - b.y, z - b.z}; }
    constexpr IntVector operator*(int32_t s) const { return {x * s, y * s, z * s}; }
    constexpr IntVector operator/(int32_t s) const { return {x / s, y / s, z / s}; }
    constexpr bool operator==(const IntVector&) const = default;
    int32_t getMax() const { return std::max({x, y, z}); }
    int32_t getMin() const { return std::min({x, y, z}); }
    double size() const { return toVector().size(); }
};
} // namespace lila::math
