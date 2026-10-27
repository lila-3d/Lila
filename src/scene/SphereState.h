#pragma once

#include "core/Export.h"

#include <glm/glm.hpp>

#include <cstdint>

namespace lila::scene
{
/** The sphere the latest polySpherize used, in world space, for the viewport tool to draw and edit. */
struct SphereState
{
    glm::dvec3 center{0.0};
    /** cm. */
    double radius = 0.0;
    /** Semi-axes relative to the radius, along the frame's axes. */
    glm::dvec3 scale{1.0};
    /** Euler degrees, XYZ: the world turn of the ellipsoid's axes. */
    glm::dvec3 rotation{0.0};
    /** Counts runs, so a reader can tell when it changed. */
    uint64_t serial = 0;
};

LILA_SCENE_API const SphereState& lastSphere();
} // namespace lila::scene
