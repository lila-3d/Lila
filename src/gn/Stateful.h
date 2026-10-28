#pragma once

#include "core/Export.h"

#include "gn/NodeLib.h"
#include "scene/Simulation.h"

#include <cstdint>
#include <memory>
#include <vector>

/**
 * Simulation nodes: geometry nodes whose output at a frame depends on every frame before it (rigid bodies, solvers).
 *
 * The group stays a pure function of time. The base simulation framework (scene::sim) owns time, stepping and the
 * cache, with the modifier running the group as the simulation node:
 *
 * - When the modifier evaluates, a simulation node publishes what reached its inputs (SimInputs) under the modifier.
 * - After every graph evaluation, a hook on the main thread asks the framework for the current frame. The framework
 *   moves time over the frames it hasn't cached, evaluating the scene at each step, and the node's NodeSim advances
 *   its state from the inputs published at that step. While it samples, the modifier runs only the steps that feed
 *   the simulation node.
 * - The frame's state is published back, the modifier is marked dirty, and its next evaluation hands that state to the
 *   node's output function, which builds the node's outputs on the worker thread like any other node.
 *
 * Steps that don't depend on time are cached by the modifier between evaluations, so sampling a frame doesn't redo an
 * expensive upstream setup (a fracture) every step. One simulation node per group, in the modifier's own group.
 */
namespace lila::gn
{
/** What reached a simulation node's inputs at one time, owned so it outlives the evaluation. */
class LILA_SCENE_API SimInputs
{
public:
    SimInputs(const GNode& node, const NodeDecl& decl, Sockets sockets, std::vector<std::vector<Value>> inputs, double time);

    double time() const { return mTime; }
    const GNode& node() const { return mNode; }
    /** Never null; empty when unconnected. */
    GeometryPtr geometry(std::string_view id) const;
    double f(std::string_view id) const;
    int64_t i(std::string_view id) const;
    bool b(std::string_view id) const;
    glm::dvec3 v(std::string_view id) const;
    std::string s(std::string_view id) const;
    std::string prop(std::string_view id) const { return mDecl->propValue(mNode, id); }
    /** Content hash of every input: geometry by value, single values by value. The framework chains it per step. */
    uint64_t hash() const { return mHash; }

private:
    const Value& in(std::string_view id) const;

    GNode mNode;
    const NodeDecl* mDecl = nullptr;
    Sockets mSockets;
    std::vector<std::vector<Value>> mInputs;
    double mTime = 0.0;
    uint64_t mHash = 0;
};

/** One simulation node's solver. Lives on the main thread next to the framework's cache; every call is from there. */
class NodeSim
{
public:
    virtual ~NodeSim() = default;

    struct Setup
    {
        int64_t startFrame = 1;
        int substeps = 1;
        /** Settings outside the sampled inputs whose change throws the cache away. */
        uint64_t paramsHash = 0;
    };
    /** From the latest published inputs. Throws std::invalid_argument when the node can't run. */
    virtual Setup configure(const SimInputs& latest) = 0;
    /** The state at the start frame. */
    virtual void reset(const SimInputs& in, std::vector<uint8_t>& state) = 0;
    /** One step toward inputs sampled at the step's end. */
    virtual void step(const SimInputs& in, std::vector<uint8_t>& state, const scene::sim::StepInfo& at) = 0;
};

/** What makes a node type a simulation node (NodeDecl::stateful). */
struct StatefulDecl
{
    std::function<std::unique_ptr<NodeSim>()> create;
    /**
     * The node's outputs at a frame from that frame's state; state is null until the simulation has reached the frame,
     * and the outputs should then show the start (the inputs passed through). Runs on a worker thread.
     */
    std::function<void(ExecContext& c, const std::vector<uint8_t>* state)> output;
};

/** Content hash of a geometry, memoised per immutable component. */
LILA_SCENE_API uint64_t hashGeometry(const Geometry& g, uint64_t h = 1469598103934665603ull);

/** Runs a simulation node's step inside a plan: publishes its inputs, then builds its outputs from the published state. */
LILA_SCENE_API void execStateful(ExecContext& c);

/**
 * The state bytes of a modifier's simulation at a frame, simulating up to it first (main thread). Throws like
 * scene::sim::simulate.
 */
LILA_SCENE_API std::vector<uint8_t> simulationState(scene::Graph& g, scene::NodeId modifier, int64_t frame);
/** The state bytes at a frame only if already simulated and still valid; empty otherwise. Never simulates (drawing uses it). */
LILA_SCENE_API std::vector<uint8_t> cachedSimulationState(scene::Graph& g, scene::NodeId modifier, int64_t frame);
/** What most recently reached the modifier's simulation node; null before it has been evaluated. */
LILA_SCENE_API std::shared_ptr<const SimInputs> simulationInputs(scene::NodeId modifier);
/** Whether the modifier's simulation state for the time's frame has been published, so its output shows it. Thread-safe. */
LILA_SCENE_API bool simulationStateReady(scene::NodeId modifier, double time);
/** Republishes inputs simulationInputs returned earlier, for a modifier restoring a cached frame instead of evaluating. */
LILA_SCENE_API void restoreSimulationInputs(scene::NodeId modifier, std::shared_ptr<const SimInputs> inputs);

/** Whether the group has a simulation node at its top level. */
LILA_SCENE_API bool hasStateful(const Group& g);

/** The framework hook and solver type for modifiers running simulation groups. Called by registerTypes. */
void registerSimulationSupport();
/**
 * The modifier ran a plan with a simulation node (or stopped running one); inputsUseTime when a step feeding the node
 * depends on time. Thread-safe.
 */
void noteSimulationModifier(scene::NodeId modifier, bool simulating, bool inputsUseTime);
/** Drops what a deleted or retyped modifier published. Thread-safe. */
void forgetSimulationModifier(scene::NodeId modifier);
} // namespace lila::gn
