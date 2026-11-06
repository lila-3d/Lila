// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cmath>

namespace groomlab {

// Minimal vector type for the core. Conversion to Imath/MFloatPoint/GfVec3f
// happens at the host and file-format boundaries.
struct Vec3f {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    constexpr Vec3f() = default;
    constexpr Vec3f(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    constexpr Vec3f operator+(const Vec3f& o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3f operator-(const Vec3f& o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vec3f operator*(float s) const { return {x * s, y * s, z * s}; }
    constexpr Vec3f operator/(float s) const { return {x / s, y / s, z / s}; }
    constexpr Vec3f operator-() const { return {-x, -y, -z}; }

    Vec3f& operator+=(const Vec3f& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3f& operator-=(const Vec3f& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3f& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }

    constexpr bool operator==(const Vec3f& o) const { return x == o.x && y == o.y && z == o.z; }
    constexpr bool operator!=(const Vec3f& o) const { return !(*this == o); }

    float length() const { return std::sqrt(dot(*this)); }
    constexpr float dot(const Vec3f& o) const { return x * o.x + y * o.y + z * o.z; }
    constexpr Vec3f cross(const Vec3f& o) const {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
    Vec3f normalized() const {
        const float len = length();
        return len > 0.0f ? *this / len : Vec3f{};
    }
};

constexpr Vec3f operator*(float s, const Vec3f& v) { return v * s; }

}  // namespace groomlab
