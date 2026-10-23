#pragma once

#include "core/Export.h"

#include "gn/NodeLib.h"

#include <memory>

namespace lila::gn
{
/** A group compiled for evaluation: the nodes the output depends on, in dependency order. */
struct Plan
{
    struct Source
    {
        int step = -1;
        int output = -1;
        SocketType type = SocketType::Float;
    };
    struct Step
    {
        size_t node = 0; // index into group.nodes
        const NodeDecl* decl = nullptr;
        Sockets sockets;
        std::vector<std::vector<Source>> inputs; // links per input socket
    };

    Group group;
    std::vector<Step> steps;
    int outputStep = -1;
    /** The active Viewer node's step, run after the output's; -1 when no Viewer is active. */
    int viewerStep = -1;
    /** Set when the group can't run (a cycle, unknown nodes); evaluation then passes the input through. */
    std::string error;
    bool usesTime = false;
};

using PlanPtr = std::shared_ptr<const Plan>;

LILA_SCENE_API PlanPtr compile(const Group& g);
/** Runs the plan on the worker thread. Throws ExecError naming the failing node. */
LILA_SCENE_API GeometryPtr execute(const Plan& plan, const ExecEnv& env);
/** Runs the plan and returns what reached each Group Output socket (geometry first, then the group's outputs). */
LILA_SCENE_API std::vector<Value> executeOutputs(const Plan& plan, const ExecEnv& env);
/** The compiled plan for a definition, shared between evaluations (nested Group nodes use it). */
LILA_SCENE_API PlanPtr cachedPlan(const json& definition);
/** The Viewer node that is switched on in the group, or null. */
LILA_SCENE_API const GNode* activeViewer(const Group& g);
} // namespace lila::gn
