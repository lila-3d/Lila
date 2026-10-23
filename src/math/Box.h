#pragma once

#include "core/Export.h"

#include "math/Transform.h"

#include <array>
#include <span>

namespace lila::math
{
/** Axis-aligned box. A default box is invalid (empty); adding a point makes it valid. */
struct LILA_FOUNDATION_API Box
{
    Vector min;
    Vector max;
    bool isValid = false;

    Box() = default;
    Box(const Vector& inMin, const Vector& inMax) : min(inMin), max(inMax), isValid(true) {}
    explicit Box(std::span<const Vector> points);

    static Box buildAABB(const Vector& origin, const Vector& extent) { return {origin - extent, origin + extent}; }

    Box& operator+=(const Vector& p);
    Box& operator+=(const Box& b);
    Box operator+(const Vector& p) const { return Box(*this) += p; }
    Box operator+(const Box& b) const { return Box(*this) += b; }
    bool operator==(const Box&) const = default;
    bool equals(const Box& b, double tolerance = kKindaSmallNumber) const;

    Vector getCenter() const { return (min + max) * 0.5; }
    /** Half size. */
    Vector getExtent() const { return (max - min) * 0.5; }
    Vector getSize() const { return max - min; }
    double getVolume() const;
    /** Strictly inside; points on a face are outside. */
    bool isInside(const Vector& p) const;
    bool isInsideOrOn(const Vector& p) const;
    bool isInside(const Box& other) const { return isInside(other.min) && isInside(other.max); }
    /** Touching counts as intersecting. */
    bool intersect(const Box& other) const;
    /** The overlapping region; invalid when they do not intersect. */
    Box overlap(const Box& other) const;
    Box expandBy(double w) const { return {min - Vector(w), max + Vector(w)}; }
    Box expandBy(const Vector& negative, const Vector& positive) const { return {min - negative, max + positive}; }
    Box shiftBy(const Vector& offset) const { return {min + offset, max + offset}; }
    Box moveTo(const Vector& center) const { return shiftBy(center - getCenter()); }
    Vector getClosestPointTo(const Vector& p) const;
    double computeSquaredDistanceToPoint(const Vector& p) const;
    /** Bounds of the eight transformed corners. */
    Box transformBy(const Transform& t) const;
    Box transformBy(const Matrix& m) const;
    Box inverseTransformBy(const Transform& t) const;
    std::array<Vector, 8> getVertices() const;
};
} // namespace lila::math
