#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>

namespace destruction
{
inline constexpr double kM = 0.01; // cm to m

/** A piece's instance transform as position, rotation and scale (no shear); a mirror shows as negative x scale. */
struct Decomposed
{
    glm::dvec3 position{0.0};
    glm::dquat rotation{1, 0, 0, 0};
    glm::dvec3 scale{1.0};
};

inline Decomposed decompose(const glm::dmat4& m)
{
    Decomposed d;
    d.position = glm::dvec3(m[3]);
    glm::dvec3 c[3] = {glm::dvec3(m[0]), glm::dvec3(m[1]), glm::dvec3(m[2])};
    for (int k = 0; k < 3; ++k)
        d.scale[k] = std::max(glm::length(c[k]), 1e-12);
    if (glm::dot(glm::cross(c[0], c[1]), c[2]) < 0.0)
        d.scale.x = -d.scale.x;
    glm::dmat3 r(c[0] / d.scale.x, c[1] / d.scale.y, c[2] / d.scale.z);
    d.rotation = glm::normalize(glm::quat_cast(r));
    return d;
}

inline glm::dmat4 compose(const glm::dvec3& p, const glm::dquat& q, const glm::dvec3& s)
{
    return glm::translate(glm::dmat4(1.0), p) * glm::mat4_cast(q) * glm::scale(glm::dmat4(1.0), s);
}
} // namespace destruction
