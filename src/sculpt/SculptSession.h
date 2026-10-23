#pragma once

#include "core/Export.h"

#include "geo/Ops.h"
#include "scene/Plug.h"
#include "sculpt/Sculpt.h"

#include <functional>
#include <memory>

/**
 * Sculpt mode on a scene mesh. Entering builds a temporary high-resolution proxy of the mesh's cage (subdivided,
 * voxel remeshed, or the cage itself for direct sculpting); strokes edit the proxy; exiting moves the cage so its
 * subdivision fits the sculpt. The proxy lives outside the scene, so it is never saved, unless exiting keeps it as a
 * hidden child mesh "<mesh>_sculptProxy" that the next Enter Sculpt resumes from.
 *
 * Commands: sculptEnter, sculptExit, sculptCancel, sculptStroke, sculptMask, sculptState, transferShape. Entering,
 * exiting and each stroke are single undo steps.
 */
namespace lila::sculpt
{
inline constexpr const char* kProxySuffix = "_sculptProxy";
/** Point layers on a kept proxy holding the mask and freeze state. */
inline constexpr const char* kMaskLayer = "sculptMask";
inline constexpr const char* kFreezeLayer = "sculptFreeze";

enum class ProxyMode : uint8_t
{
    Subdivide,
    Remesh,
    Direct,
};

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
    int symmetryAxis = -1;

    /** A stroke the viewport tool already applied live, adopted by the sculptStroke call carrying this token. */
    uint64_t liveToken = 0;
    StrokeRecord live;
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

LILA_SCENE_API void registerCommands();
} // namespace lila::sculpt
