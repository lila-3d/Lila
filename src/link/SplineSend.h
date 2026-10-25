#pragma once

#include "core/Export.h"

#include "curve/Spline.h"
#include "scene/Plug.h"

#include <nlohmann/json.hpp>

#include <string>

namespace lila::scene
{
class Graph;
}

/**
 * Curves to Unreal splines: a curve mapped with kind "spline" (unrealSendCurve, or unrealMap kind=spline) arrives through
 * LilaLink's setSpline as an actor of spline components, or as landscape spline control points and segments, optionally
 * with spline meshes along it, and is updated in place on every save.
 */
namespace lila::link
{
using json = nlohmann::json;

/** What a LilaLink plugin must report in hello's features to take setSpline. */
inline constexpr const char* kSplineFeature = "splines";

/**
 * setSpline's "splines" for world-space splines, converted to Unreal space (Lila's forward, right and up become Unreal's X,
 * Y and Z, cm as they are) relative to origin (world, Lila space). Each spline becomes {closed, points}; every point is
 * {position, arriveTangent, leaveTangent, up, roll, scale}: tangents are the Hermite derivatives Unreal's spline points
 * take (three times the Bezier handle offsets; NURBS are converted to Bezier first), up is the world up turned about the
 * tangent by the point's tilt, roll the same tilt in Unreal's degrees, and scale (1, radius, radius).
 */
LILA_SCENE_API json splinePoints(const curve::Splines& world, const glm::dvec3& origin);

/**
 * setSpline params for a curve node and its mapping: {id, label, mode, location, splines, mesh?, landscape?, width?, ...}.
 * id is the mapping's splineId (the node's full path when first mapped), so a re-send updates the same actor. The
 * mapping's splineMesh may name a Lila mesh mapped to Unreal (its contentPath is sent) or an Unreal asset path. Throws
 * std::invalid_argument when the node is not a curve or has nothing to send.
 */
LILA_SCENE_API json splineParams(scene::Graph& g, scene::NodeId curve, const json& mapping);

/** Registers unrealSendCurve. */
LILA_SCENE_API void registerSplineCommands();
} // namespace lila::link
