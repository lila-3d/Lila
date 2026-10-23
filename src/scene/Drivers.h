#pragma once

#include "core/Export.h"

#include "scene/AnimCurve.h"
#include "scene/Node.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>
#include <vector>

namespace lila::scene
{
class Graph;

/**
 * Drivers: values computed from other values instead of keyed.
 *
 * A `poseDriver` reads a transform's rotation against its rest and outputs 1 at a stored target pose, falling to 0
 * over a radius in degrees: the pose reader behind corrective morphs. A `drivenChannel` maps one plug onto another
 * through a curve of (driver value, driven value) keys, set driven key style; any keyable plug can be driven this way
 * (morph weights, material parameters, transform channels, node group inputs). A channel with a curveName is also
 * baked into an Unreal animation curve of that name on export.
 */
namespace drivers
{
enum PoseAttr
{
    kInMatrix,
    kRestMatrix,
    kTargetMatrix,
    kTwistAxis,
    kRadius,
    kTwistWeight,
    kFalloff,
    kInWeight,
    kOutput,
};

enum ChannelAttr
{
    kInput,
    kKeys,
    kPreInfinity,
    kPostInfinity,
    kCurveName,
    kChannelOutput,
};

enum class Falloff : int64_t
{
    Linear,
    Smooth,
    Smoother,
};

struct PoseReading
{
    double swing = 0.0; // degrees between the twist axis now and at the target
    double twist = 0.0; // degrees of twist about the axis away from the target's
    double distance = 0.0; // swing + twistWeight * twist
};

/** Rotations relative to rest (both from local matrices, scale removed) compared the way poseDriver does. */
LILA_SCENE_API PoseReading readPose(const glm::dmat3& rest, const glm::dmat3& current, const glm::dmat3& target, int twistAxis, double twistWeight);
/** 1 at distance 0, 0 at radius and beyond, shaped by the falloff. */
LILA_SCENE_API double falloffWeight(double distance, double radius, Falloff f);

/** The poseDriver nodes, scene order. */
LILA_SCENE_API std::vector<NodeId> poseDrivers(Graph& g);
/** The drivenChannel nodes, scene order. */
LILA_SCENE_API std::vector<NodeId> channels(Graph& g);
LILA_SCENE_API bool isPoseDriver(const Node& n);
LILA_SCENE_API bool isChannel(const Node& n);

/** A channel's mapping: keys as (driver value, driven value), stored as an anim curve's keys (their time is the input). */
LILA_SCENE_API anim::Curve channelCurve(const Node& channel);
LILA_SCENE_API void setChannelCurve(Graph& g, NodeId channel, const anim::Curve& c);

/**
 * Creates a poseDriver reading `transform` (its local matrix): rest is the transform's rotate at `restRotate` (degrees;
 * a joint keeps its jointOrient), the target its pose now. radius <= 0 picks the angle between rest and target, so the
 * output ramps from 0 at rest to 1 at the pose. Undoable.
 */
LILA_SCENE_API NodeId createPoseDriver(Graph& g, NodeId transform, const std::string& name, int twistAxis, double radius,
                                       double twistWeight, Falloff falloff, const glm::dvec3& restRotate = glm::dvec3(0.0));

/** Creates a drivenChannel from driver (invalid: none, set its input by hand) to driven (invalid: curve only). Undoable. */
LILA_SCENE_API NodeId createChannel(Graph& g, const PlugRef& driver, const PlugRef& driven, const anim::Curve& curve, const std::string& curveName,
                                    const std::string& name = {});

/** Every value a drivers export needs, as JSON: pose drivers, driven channels and the morph targets they set. */
LILA_SCENE_API json describe(Graph& g);

LILA_SCENE_API void registerTypes();
LILA_SCENE_API void registerCommands();
} // namespace drivers
} // namespace lila::scene
