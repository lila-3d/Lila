#pragma once

#include "core/Export.h"

#include "scene/Node.h"
#include "scene/RigMath.h"
#include "scene/Transform.h"

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace lila::scene
{
class Graph;
}

/**
 * Rig elements: IK chains, reverse feet, hand and foot locks, lookers and twist drivers.
 *
 * The joints' keyed rotate is the truth (FK). A chain's controller (its reverse foot, else its ikHandle) is a way to pose
 * those rotations: grabbing it puts it on the FK pose, the edit is solved by IK, and the solved rotations go back into
 * the joints' rotate (keyed when the chain is animated or auto key is on). Nothing switches between IK and FK. The one
 * time the solver drives the joints during playback is a lock: a key whose lock channel is on holds the controller
 * where it was (its keys step) until the chain's next key, and the IK holds the hand or foot there.
 *
 * Everything drives joints through their solvedRotate and ikBlend inputs, never their keyable channels.
 */
namespace lila::scene::rigel
{
inline constexpr const char* kIkChainType = "ikChain";
inline constexpr const char* kReverseFootType = "reverseFoot";
inline constexpr const char* kAlignType = "rigAlign";
inline constexpr const char* kLookerType = "looker";
inline constexpr const char* kTwistType = "twistDriver";

/** Whether the node is one of the rig element types above (or derived from one). */
LILA_SCENE_API bool isElement(const Node& n);

struct ChainOptions
{
    std::string name;   // default <end>_ik
    std::string solver; // default 2-bone for three joints, else FABRIK
    NodeId parent = kNoNode;
    /** Bend direction from the start joint; default the chain's current bend. */
    std::optional<glm::dvec3> poleVector;
};
/** An IK chain from start to end (an ikHandle plus the node turning the end with it). Throws std::invalid_argument. */
LILA_SCENE_API NodeId createIkChain(Graph& g, NodeId start, NodeId end, const ChainOptions& opts = {});

struct FootOptions
{
    std::string name; // default <ankle>_reverseFoot
    NodeId ball = kNoNode; // default the ankle's first child joint
    NodeId toe = kNoNode;  // default the ball's first child joint
    NodeId parent = kNoNode;
    /** Pivots in world space (heel, ball, toe, inner, outer); default from the skinned sole, else from the joints. */
    std::optional<std::array<glm::dvec3, 5>> pivots;
};
/** A reverse foot on the chain's ankle. Throws std::invalid_argument. */
LILA_SCENE_API NodeId createReverseFoot(Graph& g, NodeId chain, const FootOptions& opts = {});

struct LookerOptions
{
    std::string name; // default <joint>_looker
    std::string aimAxis = "x";
    std::string upAxis = "y";
    glm::dvec3 upVector{0.0, 1.0, 0.0};
    double distance = 50.0;
    double weight = 1.0;
    NodeId parent = kNoNode;
};
LILA_SCENE_API NodeId createLooker(Graph& g, NodeId joint, const LookerOptions& opts = {});

struct TwistOptions
{
    /** The joint whose twist is read: the twist joint's parent (counter it) or a joint below that parent (follow it). */
    NodeId source = kNoNode;
    /** Share of the source's twist the twist joint takes; negative counters it. Default -1 for the parent, else 0.5. */
    std::optional<double> weight;
    std::string name; // default <joint>_twist
};
/**
 * Drives a twist joint from another joint's twist about the bone they share, as UE's control rig does for Manny:
 * upperarm and thigh twist joints counter their parent's twist, lowerarm and calf twist joints take the hand's and
 * foot's. The twist joint's own rotate is replaced (its rest rotation stays the base). Throws std::invalid_argument.
 */
LILA_SCENE_API NodeId createTwistDriver(Graph& g, NodeId twistJoint, const TwistOptions& opts = {});

// Finding elements ------------------------------------------------------------------------------------------------

/** The chain's joints from its start to its end effector. */
LILA_SCENE_API std::vector<NodeId> chainJoints(Graph& g, NodeId chain);
/** chainJoints plus the reverse foot's ball: everything posing the chain writes. */
LILA_SCENE_API std::vector<NodeId> posedJoints(Graph& g, NodeId chain);
LILA_SCENE_API NodeId handleOf(Graph& g, NodeId chain);
/** The IK chain whose end effector is joint, or kNoNode. */
LILA_SCENE_API NodeId chainForEffector(Graph& g, NodeId joint);
/** The IK chain whose joints include joint, or kNoNode. */
LILA_SCENE_API NodeId chainContaining(Graph& g, NodeId joint);
/** The reverse foot on the chain, or kNoNode. */
LILA_SCENE_API NodeId footForChain(Graph& g, NodeId chain);
/** A chain from a chain, its ikHandle, its reverse foot, or any joint in it. */
LILA_SCENE_API NodeId chainFrom(Graph& g, NodeId node);
/** The chain whose controller node is: its reverse foot, or its ikHandle when it has none. kNoNode otherwise. */
LILA_SCENE_API NodeId chainOfController(Graph& g, NodeId node);
/** The node an animator grabs for the chain: its reverse foot, else its ikHandle. */
LILA_SCENE_API NodeId controllerOf(Graph& g, NodeId chain);
/** Every chain in the scene, in id order. */
LILA_SCENE_API std::vector<NodeId> chains(Graph& g);
/** Elements of a type whose string attr names the node. */
LILA_SCENE_API std::vector<NodeId> elementsNaming(Graph& g, const char* type, const char* attr, const std::string& name);

// Posing ----------------------------------------------------------------------------------------------------------

/** Whether the chain's key at time is locked (its lock channel at or above one half). */
LILA_SCENE_API bool locked(Graph& g, NodeId chain, double time);
/** Whether the solver holds the chain now: locked, or grabbed. */
LILA_SCENE_API bool held(Graph& g, NodeId chain);

/**
 * Where the controller sits for the current FK pose: its own world matrix while held, else the matrix that puts it on
 * the end effector (and the sole) as the joints are posed. What the viewport draws and grabbing starts from.
 */
LILA_SCENE_API glm::dmat4 controllerWorld(Graph& g, NodeId chain);
/**
 * Puts the controller (and the pole) onto the FK pose, so solving changes nothing. Skipped while held, when the
 * controller already is the pose. Records undo.
 */
LILA_SCENE_API void matchController(Graph& g, NodeId chain);
/**
 * Solves the chain at its controller and writes the solved rotations into the joints' rotate. With key, keys them,
 * the controller (translate, rotate, its channels and pole) and the lock channel at the current time. Records undo.
 */
LILA_SCENE_API void solveToFk(Graph& g, NodeId chain, bool key);
/**
 * Whether posing the chain keys: auto key is on or the chain is animated (its joints' rotate, its controller or its
 * lock channel are driven), and undo is recording (interactive previews only hold).
 */
LILA_SCENE_API bool keysWhenPosed(Graph& g, NodeId chain);

/**
 * Edits controllers the FK way: matches each chain's controller to the pose, runs edit (which moves, turns, rolls or
 * swings them), then solves each chain into its joints' rotate and keys as keysWhenPosed says. nodes may be
 * controllers, chains or joints in chains. Returns the chains posed.
 */
LILA_SCENE_API std::vector<NodeId> poseControllers(Graph& g, const std::vector<NodeId>& nodes, const std::function<void()>& edit);

/** Keys lock on (planted) or off at time, keying the controller there stepped while locked. Records undo. */
LILA_SCENE_API void setLock(Graph& g, NodeId chain, double time, bool on);

/** Swings the chain's pole by degrees; turn also turns the controller with the knee or elbow. Records undo. */
LILA_SCENE_API void swingPole(Graph& g, NodeId chain, double degrees, bool turn);

/** Reverse foot: puts the control on the sole (align: levels it too) keeping the pose. Records undo. */
LILA_SCENE_API void snapReverseFoot(Graph& g, NodeId foot, bool align);

/** Reverse foot channels by the given amounts (or to them, absolute), within its limits. Records undo. */
LILA_SCENE_API void addFootChannels(Graph& g, NodeId foot, std::optional<double> roll, std::optional<double> twist, std::optional<double> bank,
                                    bool absolute);

// Deform joints and the clean bake ---------------------------------------------------------------------------------

/** Whether the joint deforms (its deform flag): exporters write deform joints only. True for non-joints. */
LILA_SCENE_API bool isDeform(const Node& n);

struct BakeOptions
{
    double start = 0.0, end = 0.0; // seconds
    double fps = 30.0;
    std::string suffix = "_bake";
    /**
     * A joint with deform children and non-uniform scale is written as an unscaled driver (name_drv plus suffix) that
     * its children follow, with a leaf under it carrying the scale, so Unreal (which hands a parent's scale to its
     * children without shear) matches.
     */
    bool scaleSafe = true;
};

/** A local matrix as the translate, rotate (degrees, nearest nearRotate) and scale a bake keys; any shear is lost. */
struct ExportTrs
{
    glm::dvec3 translate{0.0}, rotate{0.0}, scale{1.0};
};
LILA_SCENE_API ExportTrs exportTrs(const glm::dmat4& local, RotateOrder order, const glm::dvec3& nearRotate);
/**
 * A clean copy of the deform joints under the given roots: the same names plus suffix, hierarchy closed over the
 * non-deform joints, keyed every frame with the evaluated world transform of each original (so whatever drives the rig,
 * the copy has plain TRS keys). Undoable. Returns the copy's top joints.
 */
LILA_SCENE_API std::vector<NodeId> bakeToDeformSkeleton(Graph& g, const std::vector<NodeId>& roots, const BakeOptions& opts);

LILA_SCENE_API void registerTypes();
LILA_SCENE_API void registerCommands();
} // namespace lila::scene::rigel
