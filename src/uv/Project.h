#pragma once

#include "uv/Chart.h"

#include <glm/glm.hpp>

#include <span>
#include <string_view>

namespace lila::uv
{
enum class Projection : uint8_t
{
    Planar,
    Box,         // each face takes the plane its normal points at most, seams between the six
    Cylindrical,
    Spherical,
    Camera,      // through a view-projection matrix, screen space to 0-1
};

enum class ProjectAxis : uint8_t
{
    X,
    Y,
    Z,
    BestFit, // planar: area-weighted normal; cylinder and sphere: longest principal axis
};

struct ProjectOptions
{
    Projection type = Projection::Planar;
    ProjectAxis axis = ProjectAxis::BestFit;
    /** Object space to clip space, for Camera. */
    glm::dmat4 camera{1.0};
    /** Fit the result into 0-1 keeping proportions; otherwise uvs are in object units. Not for Camera. */
    bool normalize = true;
    /** Seams where the projection splits, cleared where it joins. */
    bool markSeams = true;
};

/** Projects the faces (all when empty) into the uv set. Returns false on unknown input. */
bool project(geo::Mesh& mesh, std::span<const Index> faces, std::string_view uvSet, const ProjectOptions& opts = {});

/** u, v with u x v = n, Y-up friendly: +Z gives X/Y, +Y gives X/-Z. */
void planeFrame(const glm::dvec3& n, glm::dvec3& u, glm::dvec3& v);
} // namespace lila::uv
