#pragma once

#include "core/Export.h"
#include "core/Registry.h"
#include "scene/Plug.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lila::ext
{
class Context;
}

namespace lila::scene::ik
{
/** One solve of a joint chain, everything in world space. */
struct Problem
{
    /** The chain's joints from its start to its end effector, as they are before solving. At least two. */
    std::vector<glm::dmat4> world;
    /** The world matrix of the start joint's parent (identity for a root). */
    glm::dmat4 parentWorld{1.0};
    glm::dvec3 target{0.0};
    /** Where the chain's bend points; unset leaves the solver to keep the current bend plane. */
    std::optional<glm::dvec3> pole;
    /** Roll of the bend plane around the start-to-target axis, radians. A spline solve twists the end this much past the start. */
    double twist = 0.0;
    /** Spline solves: the curve the chain lies along, world space, densely sampled from its start. */
    std::vector<glm::dvec3> curve;
    /** Spline solves: turn of the whole chain about its bones, radians. */
    double roll = 0.0;
    /** Spline solves: spread the joints over the whole curve instead of keeping the bone lengths. */
    bool stretch = false;
    int maxIterations = 20;
    double tolerance = 1e-4; // cm
};

/**
 * The solved local rotation of every joint in Problem::world except the end effector, each relative to its parent's
 * solved world frame (the start joint's to parentWorld). Joints keep their translations and scales.
 */
using Solution = std::vector<glm::dquat>;

/** A curve as world points, the form an ikHandle's inCurve takes. Kind "curvePoints"; only ever connected, never stored. */
struct LILA_SCENE_API CurvePoints final : Data
{
    std::vector<glm::dvec3> points;
    const char* kind() const override { return "curvePoints"; }
    nlohmann::json toJson() const override { return {{"kind", kind()}}; }
    size_t bytes() const override { return points.size() * sizeof(glm::dvec3); }
};

/** An IK solver (Maya's MPxIkSolver): ikRPsolver, ikSCsolver, a spline solver, or an extension's own. */
struct Solver
{
    std::string name; // "ikRPsolver"
    std::string doc;
    /** Chain length it accepts; maxJoints 0 means any. */
    int minJoints = 2;
    int maxJoints = 0;
    /** Called on worker threads during evaluation, so it must only read the problem. */
    std::function<Solution(const Problem&)> solve;
};

LILA_SCENE_API Registry<Solver>& solvers();
/** Throws std::invalid_argument when the name is taken or solve is unset. */
LILA_SCENE_API const Solver& registerSolver(Solver solver);
/** For extensions: owned by ctx and removed when it unloads. Null, with a log message, if the name is taken. */
LILA_SCENE_API const Solver* registerSolver(ext::Context& ctx, Solver solver);
LILA_SCENE_API const Solver* findSolver(std::string_view name);
} // namespace lila::scene::ik
