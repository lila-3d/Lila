#pragma once

#include "scene/ImagePlane.h"
#include "scene/RenderSettings.h"
#include "scene/Transform.h"

#include <optional>

namespace lila::scene
{
/**
 * A scene camera, as Maya's camera shape: lens, film back and clipping on a locator-like transform. It looks down its
 * local -Z with +Y up. Film apertures are in inches and the focal length in mm, as Maya shows them; distances in cm.
 */
class Camera : public Locator
{
public:
    enum Attr
    {
        kFocalLength = Locator::kLocalScale + 1,
        kHorizontalFilmAperture,
        kVerticalFilmAperture,
        kFilmFit,
        kNearClipPlane,
        kFarClipPlane,
        kOrthographic,
        kOrthographicWidth,
        kLocked,
        kOverscan,
        kDisplayResolution,
        kDisplayFilmGate,
        kDisplayGateMask,
        kDisplayGateMaskOpacity,
        kCount
    };
};

namespace camera
{
/** The camera's frame from its node and world matrix, as image planes lay out against it. */
imagePlane::CameraFrame frame(const Node& cam, const glm::dmat4& world);

/** Film back half extents: tangents of the half angles for perspective, cm for orthographic. Before the film fit. */
glm::dvec2 filmExtents(const Node& cam);
FilmFit filmFit(const Node& cam);

/** Half extents a view of `aspect` (width over height) shows through the camera, after its film fit. */
glm::dvec2 viewExtents(const Node& cam, double aspect, double overscan = 1.0, std::optional<double> gateAspect = std::nullopt);

/** Horizontal and vertical angle of view in degrees, as a view of `aspect` shows it. Perspective cameras only. */
glm::dvec2 angleOfView(const Node& cam, double aspect);

/**
 * Reversed-Z projection in Vulkan clip space (y down, depth 1 at the near plane) for view half extents in the film's
 * units. A far clip of 0 or less is an infinite far plane; orthographic projections always need one.
 */
glm::dmat4 projection(glm::dvec2 viewExtents, bool ortho, double nearClip, double farClip);
/** The camera's projection for a view of `aspect`, with its own clipping planes. */
glm::dmat4 projection(const Node& cam, double aspect, double overscan = 1.0, std::optional<double> gateAspect = std::nullopt);

/** World to view: the inverse of the camera's world matrix with its scale and shear taken out. */
glm::dmat4 viewMatrix(const glm::dmat4& world);

/**
 * World matrix that puts a camera at eye looking at lookAt with up as near its up as it can be, keeping the scale of
 * `world` (the camera's current world matrix).
 */
glm::dmat4 placedWorld(const glm::dmat4& world, const glm::dvec3& eye, const glm::dvec3& lookAt, const glm::dvec3& up);

/** A camera node's gate display and film fit, as RenderSettings::camera() reports them for it. */
CameraSettings settings(const Node& cam);
/** Writes the settings onto the node's attributes (undoably when undo is on). */
void setSettings(Graph& g, NodeId cam, const CameraSettings& c);

/**
 * Moves gate settings that older scenes keyed by a camera node's name in the render settings onto that node, so they
 * follow renames. Built-in view names stay where they are.
 */
void migrateNamedSettings(Graph& g, RenderSettings& r);
} // namespace camera

/** The camera type. Idempotent; needs the locator type first. */
void registerCameraTypes();
/** The viewPlace command. Idempotent. */
void registerCameraCommands();
} // namespace lila::scene
