#pragma once

#include "core/Export.h"

#include "scene/Node.h"
#include "scene/Skin.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

/**
 * Routes to Unreal for skins whose result linear skinning can't show (centres of rotation, direct delta mush, delta
 * mush, wrap and other deformers after the skin): linear weights fitted over a range of motion (SSDR on Dem Bones),
 * corrective morph targets driven by pose drivers, and range-of-motion takes for ML Deformer training data.
 */
namespace lila::scene::skinbake
{
/** Joint rotations (Euler degrees, the joint's rotate) making up one pose; joints not listed keep theirs. */
using Pose = std::vector<std::pair<NodeId, glm::dvec3>>;

struct RomOptions
{
    /** Degrees each joint turns either way about its local axes. */
    double angle = 45.0;
    /** One joint about one axis at a time, each way. */
    bool sweep = true;
    /** Poses turning every joint at once by a seeded random amount within angle. */
    int randomPoses = 20;
    uint64_t seed = 1;
    /** Local axes to turn about, any of "xyz". */
    std::string axes = "xyz";
};

/** A sweep pose's joint, axis (0 x, 1 y, 2 z) and sign; random poses have no joint. */
struct PoseInfo
{
    NodeId joint = kNoNode;
    int axis = -1;
    int sign = 0;
};

/** Range-of-motion poses from the joints' current rotations (the rest), sweeps first. */
LILA_SCENE_API std::vector<Pose> romPoses(Graph& g, const std::vector<NodeId>& joints, const RomOptions& o, std::vector<PoseInfo>* info = nullptr);

/** The skin's influence joints whose rotate is set directly (not keyed, constrained or driven), so poses can turn them. */
LILA_SCENE_API std::vector<NodeId> poseableJoints(Graph& g, NodeId skin);

struct FitOptions
{
    /** Range of motion (posing the joints) or the animation over [start, end]. */
    bool animation = false;
    RomOptions rom;
    std::optional<int64_t> start, end; // frames; the playback range by default
    int step = 1;
    /** Extra bones solved with the weights; needs animation (they are keyed over it). */
    int helpers = 0;
    /** 0: the skin's maxInfluences. */
    int maxInfluences = 0;
    int iterations = 3;
    double smoothness = 1e-4;
    /** Leave simulated deformers (jiggle) on while sampling; off for a range of motion, which isn't played. */
    bool simulated = false;
};

struct FitResult
{
    WeightTable weights;
    double rmse = 0.0, maxError = 0.0;
    /** Error of the skin's own weights under linear skinning against the same samples, for comparison. */
    double linearRmse = 0.0;
    int frames = 0;
    /** Helper bones: rest pivot (object space) and rest-to-frame matrices per sample (object space), plus sample times. */
    std::vector<glm::dvec3> helperPivots;
    std::vector<std::vector<glm::dmat4>> helperTransforms; // [helper][sample]
    std::vector<double> times;
};

/** Samples the mesh's whole stack and fits linear weights to it. Leaves the scene as it was. Throws CommandError. */
LILA_SCENE_API FitResult fitSkinWeights(Graph& g, NodeId skin, const FitOptions& o);

/** Whether linear skinning alone would lose something on export: a non-linear method or deformers after the skin. */
LILA_SCENE_API bool needsRoute(Graph& g, NodeId skin);

/**
 * Weights to export for a skin that needsRoute: the stored exportWeights when they still fit the mesh, else a fit
 * over the default range of motion. Empty when no route is needed. note says what was done, for export warnings.
 */
LILA_SCENE_API std::optional<WeightTable> exportWeights(Graph& g, NodeId skin, std::string* note = nullptr);

/** bakeSkinWeights, bakeCorrectiveMorphs, rangeOfMotion. */
LILA_SCENE_API void registerCommands();
} // namespace lila::scene::skinbake
