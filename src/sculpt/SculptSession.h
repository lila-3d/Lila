#pragma once

#include "core/Export.h"

#include "geo/Ops.h"
#include "scene/Node.h"
#include "scene/Plug.h"
#include "sculpt/Sculpt.h"
#include "sculpt/SubdLevels.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

/**
 * Sculpt mode on a scene mesh. Entering builds a temporary high-resolution copy of the mesh's cage to sculpt on
 * (subdivision levels, a voxel remesh, or the cage itself for direct sculpting); strokes edit it; exiting moves the cage
 * so its subdivision fits the sculpt. The copy lives outside the scene, so it is never saved, unless exiting keeps it as
 * the hidden child mesh "<mesh>_high" that baking pairs with the mesh and the next Enter Sculpt resumes from.
 *
 * Commands: sculptEnter, sculptExit, sculptCancel, sculptLevel, sculptAddLevel, sculptDeleteHigher, sculptStroke,
 * sculptMask, sculptColor, sculptState, transferShape. Entering, exiting, each level change and each stroke are single
 * undo steps.
 */
namespace lila::sculpt
{
inline constexpr const char* kProxySuffix = "_high";
/** Hidden attrs on a kept high poly: the finest level when it keeps a lower one, and the level a resume opens at. */
inline constexpr const char* kFinestAttr = "sculptFinest";
inline constexpr const char* kResumeLevelAttr = "sculptResumeLevel";
/** Point layers on a kept proxy holding the mask and freeze state. */
inline constexpr const char* kMaskLayer = "sculptMask";
inline constexpr const char* kFreezeLayer = "sculptFreeze";
/** The color set polypaint lives in, on a kept proxy and on the cage after Exit Sculpt. */
inline constexpr const char* kColorSet = "polypaint";

enum class ProxyMode : uint8_t
{
    Subdivide,
    Remesh,
    Direct,
};

/** sculptExit's solver for a session, kept between calls: see exitPositions. */
struct ExitSolver;

struct Session
{
    scene::NodeId node = scene::kNoNode;
    ProxyMode mode = ProxyMode::Subdivide;
    geo::SubdivideOptions subdivide;
    /** The cage at Enter, in object space, with its ids. */
    geo::Mesh cage;
    /** Subdivide mode: the subdivision levels, shared by every session of this sculpt. */
    std::shared_ptr<const SubdStack> stack;
    /** Subdivide mode: the level being sculpted, 0 (the cage) to stack->top(). */
    int level = 0;
    /** Subdivide mode: what each level above `level` adds, [k] for level k; null adds nothing. */
    std::vector<DetailPtr> detail;
    SculptMesh sculpt;
    /** The kept proxy node this session resumed from. */
    scene::NodeId proxyNode = scene::kNoNode;
    /** The symmetry the sculpt mesh was last set up with: its layer name, empty when off. */
    std::string symmetryKey;

    /** A stroke the viewport tool already applied live, adopted by the sculptStroke call carrying this token. */
    uint64_t liveToken = 0;
    StrokeRecord live;

    std::shared_ptr<ExitSolver> exitSolver;
};

/** The high poly a mesh's last Exit Sculpt kept, which the next Enter Sculpt resumes from. */
struct KeptSculpt
{
    scene::NodeId node = scene::kNoNode;
    /** The level it resumes at, -1 for the finest. */
    int level = -1;
    /** Its subdivision levels, 0 for a voxel remesh. */
    int levels = 0;
};

LILA_SCENE_API std::optional<KeptSculpt> keptSculpt(const scene::Node& mesh);

/** The open session, or null. */
LILA_SCENE_API std::shared_ptr<Session> active();

/** Called on the main thread whenever the session opens, closes, or its proxy changes through a command or undo. */
LILA_SCENE_API int addListener(std::function<void()> fn);
LILA_SCENE_API void removeListener(int id);
LILA_SCENE_API void notify();

/** Follows the mesh's symmetry setting: call before each stroke. */
LILA_SCENE_API void syncSymmetry(Session& s);

LILA_SCENE_API uint64_t nextLiveToken();

/** Gets the tumble point a stroke's undo or redo goes back to (StrokeRecord::pivotBefore and pivotAfter). */
LILA_SCENE_API void setPivotHandler(std::function<void(const std::optional<glm::dvec3>&)> handler);

/**
 * The cage positions (per point capacity, object space) sculptExit would leave with this method ("fit", "closest" or
 * "normal") and the mesh's current symmetry: what Exit Sculpt is about to make, for showing it live. The fit's solver
 * is kept on the session, so after the first call each one costs a subdivision and a back substitution.
 */
LILA_SCENE_API std::vector<glm::dvec3> exitPositions(Session& s, const std::string& method, int iterations = 3);

/**
 * The low poly sculptExit would leave with this method, at a subdivision level: 0 is the mesh's own topology (exitPositions),
 * a higher one that level's topology, its points onto the sculpt (closest, normal) or its own subdivision cage (fit).
 */
LILA_SCENE_API geo::Mesh lowPoly(Session& s, const std::string& method, int level = 0, int iterations = 3);

LILA_SCENE_API void registerCommands();
} // namespace lila::sculpt
