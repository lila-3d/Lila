#include "retopo/Retopo.h"

#include <algorithm>
#include <cmath>

namespace lila::retopo
{
AngleSnap angleSnap(const glm::dvec3& anchor, const glm::dvec3& target, const glm::dvec3& surfaceNormal, const glm::dmat3& frame,
                    double stepDegrees)
{
    int k = 2;
    double best = -1.0;
    for (int i = 0; i < 3; ++i)
        if (double d = std::abs(glm::dot(surfaceNormal, frame[i])); d > best)
        {
            best = d;
            k = i;
        }
    const glm::dvec3 u = frame[(k + 1) % 3], v = frame[(k + 2) % 3];
    const glm::dvec3 offset = target - anchor;
    const double x = glm::dot(offset, u), y = glm::dot(offset, v);

    AngleSnap out;
    out.axis = frame[k];
    out.point = anchor + u * x + v * y;
    if (stepDegrees <= 0.0 || (x == 0.0 && y == 0.0))
    {
        out.degrees = glm::degrees(std::atan2(y, x));
        return out;
    }
    double degrees = std::round(glm::degrees(std::atan2(y, x)) / stepDegrees) * stepDegrees;
    degrees = std::remainder(degrees, 360.0);
    if (degrees <= -180.0)
        degrees += 360.0;
    const double r = glm::radians(degrees);
    const glm::dvec3 dir = u * std::cos(r) + v * std::sin(r);
    out.degrees = degrees;
    out.point = anchor + dir * std::max(0.0, x * std::cos(r) + y * std::sin(r));
    return out;
}

glm::dmat3 rotationFrame(const glm::dmat4& world)
{
    glm::dvec3 x(world[0]), y(world[1]);
    const double lx = glm::length(x);
    if (lx < 1e-12)
        return glm::dmat3(1.0);
    x /= lx;
    y -= x * glm::dot(x, y);
    const double ly = glm::length(y);
    if (ly < 1e-12)
        return glm::dmat3(1.0);
    y /= ly;
    glm::dvec3 z = glm::cross(x, y);
    // A mirrored matrix keeps its handedness, so its own z axis direction survives.
    if (glm::dot(z, glm::dvec3(world[2])) < 0.0)
        z = -z;
    return glm::dmat3(x, y, z);
}
} // namespace lila::retopo
