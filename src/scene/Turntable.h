#pragma once

#include "core/Export.h"

#include "scene/Transform.h"

#include <glm/glm.hpp>

#include <optional>
#include <vector>

namespace lila::scene
{
class Graph;
}

/**
 * Turntables, for presenting: the model turns in front of a still camera. A `turntable` node is a transform whose
 * position is the pivot and whose up axis (scene up in its own frame) is the axis. Its members are a list, never its
 * children, so the hierarchy and exports are untouched: each top-most member gets a view-only turn above its parent
 * (Graph::setDisplayTurns), so everything that reads world matrices sees it turned.
 *
 * The turn is the node's live angle, which is not an attribute: turning never records undo, never marks the scene
 * modified, and scenes open paused at restAngle. Exports and the Unreal link never see the turn; renders do when
 * includeInRenders is on.
 */
namespace lila::scene::turntable
{
inline constexpr const char* kTypeName = "turntable";
inline constexpr const char* kMembersKind = "turntableMembers";

class LILA_SCENE_API Turntable : public Transform
{
public:
    enum Attr
    {
        kSecondsPerTurn = Transform::kCount,
        kDirection,
        kEase,
        kRestAngle,
        kStepAngle,
        kLightsFollow,
        kHoldWhileEditing,
        kResumeAfter,
        kIncludeInRenders,
        kMembers,
        kCount
    };

    Turntable();
    ~Turntable() override;

    /** Live state, main thread only. Degrees and seconds. */
    double angle = 0.0;
    double velocity = 0.0;
    bool playing = false;
    double speedScale = 1.0;
    /** Paused by an edit to a member until this many seconds of quiet (clock time), when holdWhileEditing. */
    bool held = false;
    double lastEdit = 0.0;
    bool dragging = false;
    /** After a flick: velocity decays slowly toward the cruise speed (or rest) instead of easing. */
    bool coasting = false;
    /**
     * Tilt toward or away from the camera, degrees, about pitchAxis through pitchCentre (world, set when a pitch drag
     * starts). Applied after the spin, so a tilted model spins about its own tilted axis.
     */
    double pitch = 0.0;
    glm::dvec3 pitchAxis{1.0, 0.0, 0.0};
    glm::dvec3 pitchCentre{0.0};
    struct Seek
    {
        double from = 0.0, to = 0.0, start = 0.0, duration = 0.5;
        bool pauseAfter = false;
        double pitchFrom = 0.0, pitchTo = 0.0;
    };
    std::optional<Seek> seek;
};

LILA_SCENE_API bool isTurntable(const Node& n);
/** Every turntable in g, in creation order. */
LILA_SCENE_API std::vector<Turntable*> all(const Graph& g);
LILA_SCENE_API Turntable* get(const Graph& g, NodeId id);

/** The live members (deleted ones drop out). */
LILA_SCENE_API std::vector<NodeId> members(const Graph& g, NodeId turntable);
/** Writes the member list, undoably. */
LILA_SCENE_API void setMembers(Graph& g, NodeId turntable, const std::vector<NodeId>& list);
/** The turntable that node, or an ancestor of it, is on; kNoNode when none. */
LILA_SCENE_API NodeId turntableOf(const Graph& g, NodeId node);

/** World axis through the pivot (unit) and the pivot. */
LILA_SCENE_API glm::dvec3 axis(const Graph& g, NodeId turntable);
LILA_SCENE_API glm::dvec3 pivot(const Graph& g, NodeId turntable);
/** The world turn by degrees about the axis through the pivot. */
LILA_SCENE_API glm::dmat4 turnBy(const Graph& g, NodeId turntable, double degrees);
/** The whole live turn: the spin by angle, then the pitch. */
LILA_SCENE_API glm::dmat4 liveTurn(const Graph& g, const Turntable& t);
/** Cruise speed in degrees per second, signed: positive turns counter-clockwise seen from above. */
LILA_SCENE_API double cruiseSpeed(const Turntable& t);

/** How far its members reach from its axis, cm: the radius they sweep through a turn. 0 with no mesh on it. */
LILA_SCENE_API double reach(const Graph& g, NodeId turntable);

/** Pushes every turntable's current turn into the graph as display turns (or none while exporting). */
LILA_SCENE_API void sync(Graph& g);

/** Monotonic clock seconds, the `now` the live controls take. */
LILA_SCENE_API double now();

// Live control. All ease unless said otherwise.
LILA_SCENE_API void setPlaying(Graph& g, NodeId turntable, bool on);
/** Jumps, no easing. */
LILA_SCENE_API void setAngle(Graph& g, NodeId turntable, double degrees);
LILA_SCENE_API void turnTo(Graph& g, NodeId turntable, double degrees, double now, bool pauseAfter = false);
/** Eases to the n-th next stop (stepAngle apart from restAngle) in the direction of n. */
LILA_SCENE_API void step(Graph& g, NodeId turntable, int n, double now);
/** Eases back to restAngle with no pitch, and pauses. */
LILA_SCENE_API void front(Graph& g, NodeId turntable, double now);
/** Scales the cruise speed (the change eases in). Clamped to 0.05..20. */
LILA_SCENE_API void scaleSpeed(Graph& g, NodeId turntable, double factor);
/** Hand turning: grab stops it under the hand, drag turns it by degrees over dt seconds, release lets it coast. */
LILA_SCENE_API void grab(Graph& g, NodeId turntable);
LILA_SCENE_API void drag(Graph& g, NodeId turntable, double degrees, double dt);
/** Tilts by degrees about axis through centre (world), clamped to +-89. */
LILA_SCENE_API void dragPitch(Graph& g, NodeId turntable, double degrees, const glm::dvec3& axis, const glm::dvec3& centre);
LILA_SCENE_API void release(Graph& g, NodeId turntable);
/** A member was edited: a spinning turntable with holdWhileEditing stops quickly, and resumes after resumeAfter. */
LILA_SCENE_API void memberEdited(Graph& g, NodeId turntable, double now);

/** Advances every turntable by dt seconds of clock time; syncs the graph. True while any of them still moves. */
LILA_SCENE_API bool advance(Graph& g, double dt, double now);
/** True when a turntable's angle changes on its own (playing, easing, coasting). */
LILA_SCENE_API bool moving(const Turntable& t);

/** Degrees to add to the environment's rotation for turntables whose lights follow them. */
LILA_SCENE_API double environmentTurn(const Graph& g);

/**
 * Grows the world box lo..hi to what framed sweeps through a full turn on its turntable: framed nodes that are, or are
 * under, a turntable member, and a framed turntable's own members. False when none of them is on a turntable.
 */
LILA_SCENE_API bool sweepBounds(const Graph& g, const std::vector<NodeId>& framed, glm::dvec3& lo, glm::dvec3& hi);

/** The footprint of nodes on the ground: the centre of their world bounds dropped to the lowest point along scene up. */
LILA_SCENE_API std::optional<glm::dvec3> footprintCentre(const Graph& g, const std::vector<NodeId>& nodes);

LILA_SCENE_API void registerTypes();
LILA_SCENE_API void registerCommands();
} // namespace lila::scene::turntable
