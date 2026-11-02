#pragma once

#include "core/Export.h"
#include "core/Registry.h"

#include "scene/Node.h"
#include "scene/Plug.h"

#include <glm/glm.hpp>

#include <functional>
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
    bool reskin = true; // move the limb joints' skin weights onto the hose joints
};
/**
 * A rubber hose limb from start (shoulder, hip) to end (wrist, ankle). With joints between them (the elbow or knee) the
 * limb is converted: its joints stop deforming and drive a new chain of hose joints under start's parent. Without, the
 * hose spans start to end and bows toward the pole as end nears start. Throws std::invalid_argument. Returns the hoseLimb.
 */
LILA_SCENE_API NodeId createHoseLimb(Graph& g, NodeId start, NodeId end, const HoseOptions& opts = {});

/**
 * Moves every skin weight of a hose limb's limb joints (start to end) onto its hose joints: each point's share goes to
 * the two hose joints either side of where it sits along the hose, and follows the hose from then on. Each hose joint
 * binds with the skinning of the limb joint it lies along, so with the limb in its bind pose the skin deforms exactly
 * as before. Undoable. Returns how many skinClusters changed. Throws std::invalid_argument.
 */
LILA_SCENE_API int reskinHoseLimb(Graph& g, NodeId hose);

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

/** The spline chain a hoseLimb lays its joints with, or kNoNode. */
LILA_SCENE_API NodeId hoseChain(Graph& g, NodeId hose);

/**
 * Dynamics on a spline chain: a simulation on the chain's joints, set up by the extension that provides the mode, posing
 * them on top of the chain's solve. Recording it keys the splineJoints' offsets on an additive layer, so the chain stays
 * live, a later edit to the curve still lands, and bakeToDeformSkeleton carries the motion to plain keys.
 */
struct DynamicsOptions
{
    /** false: follow-through added to the animation, which stays the target. true: simulated, the animation only roots it. */
    bool full = false;
    std::optional<double> stiffness;  // the provider's units (Overlap: rotation stiffness; Jolt: pose match Hz)
    std::optional<double> damping;    // Overlap: damping; Jolt: damping ratio
    std::optional<double> gravity;    // share of the scene's gravity, default 0 additive and 1 full
    std::optional<double> radius;     // collision radius of each segment, cm; default from the bone length
    std::vector<NodeId> colliders;    // meshes (Jolt) or transforms (Overlap collider spheres) the chain keeps out of
    /** Carried motion: the chain simulates in this transform's space (a saddle, a car seat) and feels its acceleration. */
    NodeId carrier = kNoNode;
    /** Share of the carrier's acceleration the chain feels: 1 like the world, 0 carried rigidly. */
    double carrierInertia = 1.0;
    std::string name;
};

struct DynamicsProvider
{
    std::string name; // the mode: "overlap", "jolt"
    std::string doc;
    /**
     * Sets up the simulation on joints (the chain's, start to end) and returns its node: the node the generic simulation
     * commands (simulate, recordSimulation) take. Throws std::invalid_argument.
     */
    std::function<NodeId(Graph& g, NodeId chain, const std::vector<NodeId>& joints, const DynamicsOptions& o)> attach;
};

/** Modes of splineDynamics, registered by the extensions that simulate them (ctx.add(dynamicsProviders(), ...)). */
LILA_SCENE_API Registry<DynamicsProvider>& dynamicsProviders();

/** Sets up dynamics on a chain with the named provider; returns the simulation node. Throws std::invalid_argument. */
LILA_SCENE_API NodeId addDynamics(Graph& g, NodeId chain, const std::string& mode, const DynamicsOptions& o = {});
/** The simulation node splineDynamics made for the chain, or kNoNode. */
LILA_SCENE_API NodeId dynamicsOf(Graph& g, NodeId chain);

/** The keyable channel a recording turns a spline-driven joint with, and the value it needs. */
struct OffsetKey
{
    PlugRef plug; // the splineJoint's offset
    glm::dvec3 value{0.0};
};
/**
 * For a joint a splineJoint drives (ikBlend above 0 at the current time): the offset that gives the joint `local`, its
 * rotation relative to its parent (rotation only, scale taken out). nullopt for any other joint.
 */
LILA_SCENE_API std::optional<OffsetKey> offsetFor(Graph& g, NodeId joint, const glm::dmat3& local);
/** The splineJoint driving a joint, or kNoNode. */
LILA_SCENE_API NodeId splineJointOf(Graph& g, NodeId joint);

LILA_SCENE_API void registerTypes();
LILA_SCENE_API void registerCommands();
} // namespace lila::scene::spline
