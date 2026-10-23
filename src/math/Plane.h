#pragma once

#include "core/Export.h"

#include "math/Matrix.h"

namespace lila::math
{
/** Plane of points p with dot(normal, p) == w. The point-normal and three-point constructors normalize. */
struct LILA_FOUNDATION_API Plane
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double w = 0.0;

    constexpr Plane() = default;
    constexpr Plane(double inX, double inY, double inZ, double inW) : x(inX), y(inY), z(inZ), w(inW) {}
    Plane(const Vector& normal, double inW) : x(normal.x), y(normal.y), z(normal.z), w(inW) {}
    Plane(const Vector& point, const Vector& normal);
    /** Through three points; the normal follows the right-hand rule on a, b, c. */
    Plane(const Vector& a, const Vector& b, const Vector& c);

    constexpr bool operator==(const Plane&) const = default;
    bool equals(const Plane& b, double tolerance = kKindaSmallNumber) const;

    Vector getNormal() const { return {x, y, z}; }
    Vector getOrigin() const { return getNormal() * w; }
    bool isValid() const { return !getNormal().isNearlyZero(); }
    /** Signed distance for a unit normal; positive on the normal's side. */
    double planeDot(const Vector& p) const { return x * p.x + y * p.y + z * p.z - w; }
    Plane flip() const { return {-x, -y, -z, -w}; }
    Vector projectPoint(const Vector& p) const { return p - getNormal() * planeDot(p); }
    Plane transformBy(const Matrix& m) const;
};

/** Where the segment's infinite line crosses the plane; start when parallel. */
LILA_FOUNDATION_API Vector linePlaneIntersection(const Vector& lineStart, const Vector& lineEnd, const Plane& plane);
} // namespace lila::math
