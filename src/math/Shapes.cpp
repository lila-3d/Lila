#include "math/Box.h"
#include "math/Plane.h"

namespace lila::math
{
// Box ----------------------------------------------------------------------------------------

Box::Box(std::span<const Vector> points)
{
    for (const Vector& p : points)
        *this += p;
}

Box& Box::operator+=(const Vector& p)
{
    if (isValid)
    {
        min = min.componentMin(p);
        max = max.componentMax(p);
    }
    else
    {
        min = max = p;
        isValid = true;
    }
    return *this;
}

Box& Box::operator+=(const Box& b)
{
    if (!b.isValid)
        return *this;
    if (isValid)
    {
        min = min.componentMin(b.min);
        max = max.componentMax(b.max);
    }
    else
    {
        *this = b;
    }
    return *this;
}

bool Box::equals(const Box& b, double tolerance) const
{
    return isValid == b.isValid && min.equals(b.min, tolerance) && max.equals(b.max, tolerance);
}

double Box::getVolume() const
{
    if (!isValid)
        return 0.0;
    const Vector s = getSize();
    return s.x * s.y * s.z;
}

bool Box::isInside(const Vector& p) const
{
    return p.x > min.x && p.x < max.x && p.y > min.y && p.y < max.y && p.z > min.z && p.z < max.z;
}

bool Box::isInsideOrOn(const Vector& p) const
{
    return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y && p.z >= min.z && p.z <= max.z;
}

bool Box::intersect(const Box& other) const
{
    if (!isValid || !other.isValid)
        return false;
    for (int i = 0; i < 3; ++i)
        if (min[i] > other.max[i] || other.min[i] > max[i])
            return false;
    return true;
}

Box Box::overlap(const Box& other) const
{
    if (!intersect(other))
        return {};
    return {min.componentMax(other.min), max.componentMin(other.max)};
}

Vector Box::getClosestPointTo(const Vector& p) const { return p.boundToBox(min, max); }

double Box::computeSquaredDistanceToPoint(const Vector& p) const { return getClosestPointTo(p).distSquared(p); }

Box Box::transformBy(const Transform& t) const
{
    if (!isValid)
        return {};
    Box result;
    for (const Vector& v : getVertices())
        result += t.transformPosition(v);
    return result;
}

Box Box::transformBy(const Matrix& m) const
{
    if (!isValid)
        return {};
    Box result;
    for (const Vector& v : getVertices())
        result += m.transformPosition(v);
    return result;
}

Box Box::inverseTransformBy(const Transform& t) const
{
    if (!isValid)
        return {};
    Box result;
    for (const Vector& v : getVertices())
        result += t.inverseTransformPosition(v);
    return result;
}

std::array<Vector, 8> Box::getVertices() const
{
    return {Vector(min.x, min.y, min.z), Vector(min.x, min.y, max.z), Vector(min.x, max.y, min.z),
            Vector(min.x, max.y, max.z), Vector(max.x, min.y, min.z), Vector(max.x, min.y, max.z),
            Vector(max.x, max.y, min.z), Vector(max.x, max.y, max.z)};
}

// Plane --------------------------------------------------------------------------------------

Plane::Plane(const Vector& point, const Vector& normal)
{
    const Vector n = normal.getSafeNormal();
    x = n.x;
    y = n.y;
    z = n.z;
    w = point | n;
}

Plane::Plane(const Vector& a, const Vector& b, const Vector& c) : Plane(a, (b - a) ^ (c - a)) {}

bool Plane::equals(const Plane& b, double tolerance) const
{
    return std::abs(x - b.x) <= tolerance && std::abs(y - b.y) <= tolerance && std::abs(z - b.z) <= tolerance &&
           std::abs(w - b.w) <= tolerance;
}

Plane Plane::transformBy(const Matrix& m) const
{
    const glm::dmat3 normalMatrix = glm::transpose(glm::inverse(glm::dmat3(m.m)));
    return Plane(m.transformPosition(getOrigin()), Vector(normalMatrix * getNormal().toGlm()));
}

Vector linePlaneIntersection(const Vector& lineStart, const Vector& lineEnd, const Plane& plane)
{
    const Vector dir = lineEnd - lineStart;
    const double denom = dir | plane.getNormal();
    if (std::abs(denom) < kSmallNumber)
        return lineStart;
    return lineStart + dir * ((plane.w - (lineStart | plane.getNormal())) / denom);
}
} // namespace lila::math
