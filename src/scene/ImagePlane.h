#pragma once

#include "core/Export.h"

#include "scene/Transform.h"

#include <array>
#include <string>
#include <vector>

namespace lila::scene
{
/**
 * Maya's imagePlane: a reference picture (a still, an image sequence or a video) drawn as a textured quad, for modelling
 * from photos and blueprints and animating to reference footage. It is a transform, so it lists, hides, saves and undoes
 * like any object.
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
        kStartFrame,
        kFps,
        kMedia,
        kRegion,
        kKeyBackground,
        kKeyTolerance,
        kDrawOrder,
        kGuides,
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
LILA_SCENE_API FitResult fit(Fit mode, glm::dvec2 box, double imageAspect);

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
struct LILA_SCENE_API Quad
{
    glm::dvec3 center{0.0};
    glm::dvec3 halfU{0.0};
    glm::dvec3 halfV{0.0};
    glm::dvec4 uv{0.0, 0.0, 1.0, 1.0};

    /** Top left, top right, bottom right, bottom left. */
    std::array<glm::dvec3, 4> corners() const;
    glm::dvec3 normal() const;
};

LILA_SCENE_API Settings settings(const Node& n);

/** A cut plane's region: loops in its local XY, cm, filled even-odd. Empty for an uncut plane, which shows it all. */
using Region = std::vector<std::vector<glm::dvec2>>;
LILA_SCENE_API Region region(const Node& n);
LILA_SCENE_API std::string regionString(const Region& r);
/**
 * What a cut works on: the region, or for an uncut plane its picture's rectangle at aspect (width / height). Without an
 * aspect, a rectangle far past any picture its settings could lay out.
 */
LILA_SCENE_API Region shownRegion(const Node& n, double aspect);
/** The plane's guide lines, set by imagePlaneGuide: (x0, y0, x1, y1) in its local XY, cm, each from its start through its end. */
LILA_SCENE_API std::vector<glm::dvec4> guides(const Node& n);
/** drawOrder summed up the chain of image planes n is cut from: where it draws among planes with the same placement. */
LILA_SCENE_API int64_t drawOrder(const Graph& g, const Node& n);
LILA_SCENE_API Quad layoutFree(const Settings& s, const glm::dmat4& world, double imageAspect);
LILA_SCENE_API Quad layoutAttached(const Settings& s, const CameraFrame& cam, double imageAspect);

/**
 * The file for one frame of an image sequence: a run of '#' in the file name becomes the frame padded to its length,
 * otherwise the last run of digits in the name is replaced, keeping its padding. Names without either are returned as is.
 */
LILA_SCENE_API std::string frameFile(const std::string& path, int64_t frame);

/** A camera attr value as a view name ("side" is "right") or a camera node's name as is; empty for a free plane. */
LILA_SCENE_API std::string cameraName(const std::string& value);
/** A view name, a camera node in the current scene, or empty. */
LILA_SCENE_API bool validCamera(const std::string& value);
} // namespace imagePlane

/** The imagePlane type. Idempotent; needs the transform type first. */
LILA_SCENE_API void registerImagePlaneTypes();
/** The imagePlane and imagePlaneCut commands. Idempotent. */
LILA_SCENE_API void registerImagePlaneCommands();
/** imagePlaneCut alone; registerImagePlaneCommands calls it. */
void registerImagePlaneCutCommand();
} // namespace lila::scene
