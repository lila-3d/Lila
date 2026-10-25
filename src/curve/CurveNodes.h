#pragma once

#include "core/Export.h"
#include "curve/Spline.h"
#include "scene/Transform.h"

#include <nlohmann/json.hpp>

/**
 * Curves in the scene: the "curve" shape node (Maya's nurbsCurve, any spline kind), the history nodes that turn curves
 * into meshes (sweep, revolve, loft, patch) and the curve to curve ones (fillet, offset, rebuild, project), and the
 * commands for all of them.
 */
namespace lila::curve
{
/** A plug value holding splines. Kind "curve". */
struct LILA_SCENE_API CurveData final : scene::Data
{
    Splines splines;

    const char* kind() const override { return "curve"; }
    nlohmann::json toJson() const override;
    size_t bytes() const override;
    static scene::DataPtr fromJson(const nlohmann::json& j);
};

LILA_SCENE_API scene::DataPtr makeCurveData(Splines s);
/** The splines a plug value holds; empty for anything else. */
LILA_SCENE_API const Splines& splinesOf(const scene::PlugValue& v);
LILA_SCENE_API nlohmann::json splineToJson(const Spline& s);
/** Throws std::invalid_argument on a malformed spline. */
LILA_SCENE_API Spline splineFromJson(const nlohmann::json& j);

/**
 * A curve in the hierarchy. `create` holds its splines in object space (or is fed by a curve history node); `local`
 * repeats them and `worldSpace` carries them through the world matrix, which is what history nodes read. With a mesh
 * connected to liveSurface, `local` holds the points conformed to that surface (see conformToSurface), so editing them
 * keeps the curve on it.
 */
class LILA_SCENE_API CurveShape : public scene::Transform
{
public:
    enum Attr
    {
        kCreate = scene::Transform::kCount,
        kLocal,
        kWorldSpace,
        kDisplayHull,
        kDisplayPoints,
        kDisplayComb,
        kCombScale,
        kCombSamples,
        kTessMode,
        kTessSegments,
        kLiveSurface,
        kLiveSurfaceMatrix,
        kSurfaceOffset,
        kCount
    };

    void compute(const scene::EvalContext& ctx) override;
};

/** Component ids of curve points, for vertex mode: spline, point and which part of it (the point or a handle). */
enum class PointRole : uint32_t
{
    Point = 0,
    HandleL = 1,
    HandleR = 2,
    Edit = 3, // an edit point (span boundary) of a NURBS
};
LILA_SCENE_API uint32_t componentId(size_t spline, size_t point, PointRole role);
LILA_SCENE_API void decodeComponent(uint32_t id, size_t* spline, size_t* point, PointRole* role);

/** The splines a curve node shows, object space (its local output). */
LILA_SCENE_API const Splines& shapeSplines(scene::Graph& g, scene::NodeId node);
/** Where curve components are, object space: every point, handle and edit point of the node's splines by component id. */
LILA_SCENE_API std::vector<std::pair<uint32_t, glm::dvec3>> componentPositions(const Splines& s);
/**
 * Object-space frame per component id, columns U, V and N (zero for an unknown id): U the tangent where the curve passes
 * the point (a handle's own direction for a handle), N the rotation-minimising binormal, which on a planar curve lies in its
 * plane, perpendicular to the tangent, and V = N x U, the plane normal there.
 */
LILA_SCENE_API std::vector<glm::dmat3> componentFrames(const Splines& s, const std::vector<uint32_t>& ids);
/** Splines with the given components moved to object-space positions. Handles follow their anchor; edit points refit. */
LILA_SCENE_API Splines movedComponents(const Splines& s, const std::vector<std::pair<uint32_t, glm::dvec3>>& moves);

LILA_SCENE_API void registerTypes();
LILA_SCENE_API void registerCommands();
} // namespace lila::curve
