#pragma once

#include "core/Export.h"

#include "uv/Chart.h"

#include <glm/glm.hpp>

#include <optional>
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

/** Where an unspecified projection frame comes from. */
enum class ProjectFit : uint8_t
{
    Auto,   // fitted to the faces: principal axes, normals, minimum-area rectangle
    World,  // world axes (through ProjectOptions::worldLinear), sized to the faces
    Object, // the mesh's own axes, sized to the faces
};

/**
 * The shape a projection maps through, in object space. Z is the projection axis: the plane's normal, the cylinder's
 * axis, the sphere's pole. Planar and box use width (X), height (Y) and depth (Z); cylinder radius and height (Z);
 * sphere radius. U wraps at seamAngle about Z from X.
 */
struct ProjectFrame
{
    glm::dvec3 center{0.0};
    glm::dmat3 axes{1.0}; // right handed, columns X, Y, Z
    double width = 1.0;
    double height = 1.0;
    double depth = 1.0;
    double radius = 1.0;
    double seamAngle = 0.0; // radians
};

struct ProjectOptions
{
    Projection type = Projection::Planar;
    ProjectAxis axis = ProjectAxis::BestFit;
    ProjectFit fit = ProjectFit::Auto;
    /** Object space to clip space, for Camera. */
    glm::dmat4 camera{1.0};
    /** Object to world, for ProjectFit::World. */
    glm::dmat3 worldLinear{1.0};
    /** Fit the frame's rectangle into 0-1 keeping proportions; otherwise uvs are in object units. Not for Camera. */
    bool normalize = true;
    /** Seams where the projection splits, cleared where it joins. */
    bool markSeams = true;
    /** With markSeams, the seams already cut inside the faces stay: the projection only adds. */
    bool keepSeams = true;

    /** Overrides of the fitted frame. rotation is XYZ euler degrees of the frame's axes; seamAngle is degrees. */
    std::optional<glm::dvec3> center;
    std::optional<glm::dvec3> rotation;
    std::optional<double> width, height, depth, radius, seamAngle;
    /** Applied last: uv = uv * scale + offset. */
    glm::dvec2 scale{1.0};
    glm::dvec2 offset{0.0};
    /** Object space direction the camera looks along; the seam goes on the far side. */
    std::optional<glm::dvec3> viewDir;
};

/** The frame project() would use for the faces (all when empty): fitted, then the options' overrides applied. */
LILA_SCENE_API ProjectFrame fitProjection(const geo::Mesh& mesh, std::span<const Index> faces, const ProjectOptions& opts = {});

/** Projects the faces (all when empty) into the uv set. Returns false on unknown input. */
LILA_SCENE_API bool project(geo::Mesh& mesh, std::span<const Index> faces, std::string_view uvSet, const ProjectOptions& opts = {},
                            ProjectFrame* used = nullptr);

/** XYZ euler degrees of a rotation frame, and back. */
LILA_SCENE_API glm::dvec3 frameEuler(const glm::dmat3& axes);
LILA_SCENE_API glm::dmat3 eulerFrame(const glm::dvec3& degrees);

/** u, v with u x v = n, Y-up friendly: +Z gives X/Y, +Y gives X/-Z. */
LILA_SCENE_API void planeFrame(const glm::dvec3& n, glm::dvec3& u, glm::dvec3& v);
} // namespace lila::uv
