#pragma once

#include "core/Export.h"

#include "geo/Ops.h"
#include "scene/Plug.h"
#include "sculpt/Sculpt.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

/**
 * Sculpt mode on a scene mesh. Entering builds a temporary high-resolution proxy of the mesh's cage (subdivided,
 * voxel remeshed, or the cage itself for direct sculpting); strokes edit the proxy; exiting moves the cage so its
 * subdivision fits the sculpt. The proxy lives outside the scene, so it is never saved, unless exiting keeps it as a
 * hidden child mesh "<mesh>_sculptProxy" that the next Enter Sculpt resumes from.
 *
 * Commands: sculptEnter, sculptExit, sculptCancel, sculptStroke, sculptMask, sculptColor, sculptState, transferShape. Entering,
 * exiting and each stroke are single undo steps.
 */
namespace lila::sculpt
{
inline constexpr const char* kProxySuffix = "_sculptProxy";
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
    /** Subdivide mode: the proxy's points are these stencils applied to the cage. */
    std::vector<geo::PointStencil> stencils;
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

LILA_SCENE_API void registerCommands();
} // namespace lila::sculpt
