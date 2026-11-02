#pragma once

#include "core/Export.h"

#include "scene/Node.h"

#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <vector>

namespace lila::scene
{
class Graph;
}

/**
 * Spline chains and rubber hose limbs, rig elements in the sense of RigElements.h.
 *
 * A splineChain lays a run of joints along a curve by arc length: the chain node solves every joint at once and a
 * splineJoint node per joint hands its share to the joint's solvedRotate and ikBlend (the chain's weight), its ikStretch
 * (translate-based stretch, which Unreal takes as plain bone translation), its scale (volume squash, with the children
 * compensating it so nothing shears) and, for the first joint, its translate (the root rides the curve). Each splineJoint
 * carries a keyable FK offset applied on top of the solve. bakeToDeformSkeleton turns the result into plain keys, its
 * scale-safe layout keeping the squash Unreal-exact.
 *
 * A hoseLimb feeds a splineChain with a curve: a limb from the shoulder or hip to the wrist or ankle blending the jointed
 * elbow or knee into a smooth arc that keeps the limb's length, or a finger curling along a circular arc. A hoseHand
 * drives the curl, spread and fist of every finger on a hand.
 */
namespace lila::scene::spline
{
inline constexpr const char* kChainType = "splineChain";
inline constexpr const char* kJointType = "splineJoint";
inline constexpr const char* kHoseType = "hoseLimb";
inline constexpr const char* kHandType = "hoseHand";

enum class LengthMode : int64_t
{
    KeepLength, // joints keep their spacing and slide along the curve
    Stretch,    // joints spread with the curve's length, within min and max stretch
    Squash,     // stretch, and the joints thin or thicken to keep their volume
};

enum class UpMode : int64_t
{
    CurveFrames, // rotation-minimising frames starting from the up vector
    UpVector,    // every joint's up leans toward the up vector
    UpObjects,   // curve frames twisted to follow an object at each end
};

struct ChainOptions
{
    std::string name; // default <start>_spline
    LengthMode lengthMode = LengthMode::Stretch;
    UpMode upMode = UpMode::CurveFrames;
    /** In the space of the start joint's parent; default the start joint's axis most across the curve. */
    std::optional<glm::dvec3> upVector;
    NodeId upStart = kNoNode, upEnd = kNoNode;
    double minStretch = 0.25, maxStretch = 4.0;
};

/**
 * A spline chain laying the joints from start to end along the curve. The joints keep their current pose as the rest
 * (the joint nearest each point of the curve sits there). Throws std::invalid_argument.
 */
LILA_SCENE_API NodeId createSplineChain(Graph& g, NodeId start, NodeId end, NodeId curve, const ChainOptions& opts = {});

/** The chain's joints from start to end. */
LILA_SCENE_API std::vector<NodeId> splineChainJoints(Graph& g, NodeId chain);
/** The spline chain whose joints include joint, or kNoNode. */
LILA_SCENE_API NodeId splineChainContaining(Graph& g, NodeId joint);

struct HoseOptions
{
    std::string name;  // default <start>_hose
    int joints = 12;   // deform joints laid along the limb
    double hose = 1.0; // 0 jointed, 1 a smooth hose
    double autoArc = 1.0;
};
/**
 * A rubber hose limb from start (shoulder, hip) to end (wrist, ankle). With joints between them (the elbow or knee) the
 * limb is converted: its joints stop deforming and drive a new chain of hose joints under start's parent. Without, the
 * hose spans start to end and bows toward the pole as end nears start. Throws std::invalid_argument. Returns the hoseLimb.
 */
LILA_SCENE_API NodeId createHoseLimb(Graph& g, NodeId start, NodeId end, const HoseOptions& opts = {});

struct FingerOptions
{
    std::string name;            // hand node; default <hand>_hoseHand
    std::string curlAxis = "z";  // the base joint's axis fingers curl about
    double curlMax = 270.0;      // degrees of curl at curl 1
    double spreadMax = 20.0;     // degrees of spread at spread 1
};
/**
 * Hose fingers: each base joint's finger (down to its tip) becomes a spline chain on a curling arc, all driven by one
 * hoseHand. Throws std::invalid_argument. Returns the hand node then each finger's hoseLimb.
 */
LILA_SCENE_API std::vector<NodeId> createHoseFingers(Graph& g, const std::vector<NodeId>& bases, const FingerOptions& opts = {});

LILA_SCENE_API void registerTypes();
LILA_SCENE_API void registerCommands();
} // namespace lila::scene::spline
