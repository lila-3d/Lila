#pragma once

#include "core/Export.h"

#include "scene/Node.h"
#include "scene/Transform.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>
#include <vector>

/**
 * The Unreal round-trip check: a skeleton as it is exported (deform joints only, each keyed with translate, rotate and
 * scale relative to its nearest deform ancestor) evaluated under Unreal's transform rules, its skins compared with Lila's
 * own deformed meshes every frame. Unreal composes a child's scale with its parent's per axis and has no shear, so a
 * non-uniform scale under a rotated child does not survive; the check finds the joints that move differently.
 */
namespace lila::scene::uecheck
{
/** Unreal's FTransform: scale on the bone's own axes, then rotation, then translation. */
struct UeTransform
{
    glm::dvec3 translation{0.0};
    glm::dquat rotation{1.0, 0.0, 0.0, 0.0};
    glm::dvec3 scale{1.0};
};

/** A local matrix as an exporter writes it (rigel::exportTrs). */
LILA_SCENE_API UeTransform fromLocal(const glm::dmat4& local, RotateOrder order);
/** FTransform child * parent: rotations multiply, scales multiply per axis, the child's translation is scaled and turned. */
LILA_SCENE_API UeTransform compose(const UeTransform& child, const UeTransform& parent);
LILA_SCENE_API glm::dmat4 toMatrix(const UeTransform& t);

struct Options
{
    /** A joint of each skeleton to check; empty checks every skeleton. */
    std::vector<NodeId> roots;
    double start = 0.0, end = 0.0; // seconds
    double fps = 30.0;
    /** cm a skinned point (or a joint's axis tip, for joints nothing is skinned to) may drift. */
    double tolerance = 0.01;
    /** Joints named like an influence plus this suffix (a baked copy) are checked against that influence's skin. */
    std::string suffix = "_bake";
    /** At most this many frames, spread evenly over the range with both ends kept; 0 checks every frame. */
    int maxFrames = 0;
    /**
     * Skips the skins on frames where every joint's Unreal matrix equals Lila's, since they cannot drift then. A baked
     * copy is not compared with its original on those frames.
     */
    bool skipMatchingFrames = false;
};

struct Mismatch
{
    NodeId joint = kNoNode;
    /** The largest drift, cm, and the time it happened at. */
    double error = 0.0;
    double time = 0.0;
    /** The deform ancestor whose non-uniform scale the joint inherits at that time, if any. */
    NodeId scaledAncestor = kNoNode;
};

struct Report
{
    /** Joints past the tolerance, worst first. */
    std::vector<Mismatch> mismatches;
    double worst = 0.0;
    int frames = 0;
    size_t joints = 0;
    size_t skins = 0;
};

/** Evaluates every frame of the range (main thread; restores the time). */
LILA_SCENE_API Report check(Graph& g, const Options& opts);

/** The validation source the check reports under. */
inline constexpr const char* kValidator = "unrealRoundTrip";
/** Reports each mismatch as an error on its joint. Exporters refuse through checkExport rather than the command gate. */
LILA_SCENE_API void reportIssues(Graph& g, const Report& r);

/**
 * Whether a skeleton could come out different in Unreal: a joint with deform joints below it, or a transform above the
 * skeleton, whose scale is non-uniform now or comes through a connection (keys, constraints, drivers). Translation,
 * stretch included, always survives.
 */
LILA_SCENE_API bool mayShear(Graph& g, const std::vector<NodeId>& roots);

/** Frames an export samples at most, spread over its range. */
inline constexpr int kExportFrames = 60;

struct ExportCheck
{
    /** Why the export is refused; empty when it may go ahead. */
    std::string error;
    /** False when nothing could shear, so no frame was evaluated. */
    bool evaluated = false;
    Report report;
};

/**
 * The check an exporter runs on what it writes: the skeletons of nodes (joints among them, and the joints skinning
 * their meshes) over [start, end] seconds, NaN for the playback range. Replaces those joints' round-trip issues with
 * this run's. Skipped while a SkipScope lives on this thread.
 */
LILA_SCENE_API ExportCheck checkExport(Graph& g, const std::vector<NodeId>& nodes, bool children, double start, double end, double fps,
                                       const std::string& command);

/** While one lives on a thread, checkExport passes everything: unrealSync with unrealCheck off. */
class LILA_SCENE_API SkipScope
{
public:
    SkipScope();
    ~SkipScope();
    SkipScope(const SkipScope&) = delete;
    SkipScope& operator=(const SkipScope&) = delete;
};
LILA_SCENE_API bool skipping();

/** unrealCheck and the unrealRoundTrip validator. Idempotent. */
LILA_SCENE_API void registerCommands();
} // namespace lila::scene::uecheck
