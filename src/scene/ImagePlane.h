#pragma once

#include "scene/Transform.h"

#include <array>
#include <string>

namespace lila::scene
{
/**
 * Maya's imagePlane: a reference picture (a still or an image sequence) drawn as a textured quad, for modelling from
 * photos and blueprints. It is a transform, so it lists, hides, saves and undoes like any object.
 *
 * A free plane (camera empty) uses its world matrix: the image lies in the local XY plane facing +Z. A plane attached
 * to a camera (a view name: persp, front, top, right, ..., or a camera node) ignores its own matrix. On a perspective camera it rides
 * the film gate `depth` cm in front of the eye; on an orthographic one it stays fixed in the world, perpendicular to
 * the view axis and `depth` cm behind the origin, so panning and zooming over a blueprint keeps it in place.
 */
class ImagePlane : public Transform
{
public:
    enum Attr
    {
        kImageName = Transform::kCount,
        kUseFrameExtension,
        kFrameOffset,
        kCamera,
        kDisplayMode,
        kFit,
        kWidth,
        kHeight,
        kMaintainRatio,
        kOffsetX,
        kOffsetY,
        kDepth,
        kRotation,
        kAlphaGain,
        kBrightness,
        kContrast,
        kDisplayColor,
        kPlacement,
        kLocked,
        kCount
    };
};

namespace imagePlane
{
enum class Fit
{
    Fill,       // covers the frame, cropping the image
    Best,       // whole image inside the frame
    Horizontal, // frame width
    Vertical,   // frame height
    ToSize,     // stretched to the frame
};

enum class DisplayMode
{
    LookingThrough, // only in views of the attached camera
    AllViews,
};

enum class Placement
{
    Behind,  // under all geometry
    InScene, // depth tested like geometry
    InFront, // over all geometry
};

/** The plane's layout attrs, read off a node with settings(). */
struct Settings
{
    Fit fit = Fit::Best;
    double width = 100.0; // cm
    double height = 100.0;
    bool maintainRatio = true;
    glm::dvec2 offset{0.0}; // cm, within the plane
    double depth = 1000.0;  // cm
    double rotation = 0.0;  // degrees around the plane normal
};

/** Quad size in frame units and the part of the image it shows as (u0, v0, u1, v1), v0 at the image's top row. */
struct FitResult
{
    glm::dvec2 size{0.0};
    glm::dvec4 uv{0.0, 0.0, 1.0, 1.0};
};

/** Places an image of aspect (width / height) into a frame of size box. */
FitResult fit(Fit mode, glm::dvec2 box, double imageAspect);

/** A camera's frame and film half extents: tangents for perspective, cm for orthographic. */
struct CameraFrame
{
    glm::dvec3 eye{0.0};
    glm::dvec3 right{1, 0, 0};
    glm::dvec3 up{0, 1, 0};
    glm::dvec3 forward{0, 0, -1};
    bool ortho = false;
    glm::dvec2 film{0.5, 0.5};
};

/** World-space quad: centre and half-extent axes along the image's right and up; uv as in FitResult. */
struct Quad
{
    glm::dvec3 center{0.0};
    glm::dvec3 halfU{0.0};
    glm::dvec3 halfV{0.0};
    glm::dvec4 uv{0.0, 0.0, 1.0, 1.0};

    /** Top left, top right, bottom right, bottom left. */
    std::array<glm::dvec3, 4> corners() const;
    glm::dvec3 normal() const;
};

Settings settings(const Node& n);
Quad layoutFree(const Settings& s, const glm::dmat4& world, double imageAspect);
Quad layoutAttached(const Settings& s, const CameraFrame& cam, double imageAspect);

/**
 * The file for one frame of an image sequence: a run of '#' in the file name becomes the frame padded to its length,
 * otherwise the last run of digits in the name is replaced, keeping its padding. Names without either are returned as is.
 */
std::string frameFile(const std::string& path, int64_t frame);

/** A camera attr value as a view name ("side" is "right") or a camera node's name as is; empty for a free plane. */
std::string cameraName(const std::string& value);
/** A view name, a camera node in the current scene, or empty. */
bool validCamera(const std::string& value);
} // namespace imagePlane

/** The imagePlane type. Idempotent; needs the transform type first. */
void registerImagePlaneTypes();
/** The imagePlane command. Idempotent. */
void registerImagePlaneCommands();
} // namespace lila::scene
