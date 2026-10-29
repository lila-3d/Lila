#pragma once

#include "core/Export.h"

#include "scene/Node.h"
#include "uv/LiveSolver.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <vector>

/**
 * The UI side of live uv solving: settings shared by the UV editor and the 3D brush (kept in the "general" prefs under
 * /uv/live), the 60 Hz driver that ticks the live sessions, the 3D uv brush tool, and the 3D checker and seam display.
 */
namespace lila::ui::uvLive
{
struct BrushSettings
{
    uv::LiveBrush brush = uv::LiveBrush::Relax;
    double radius = 50.0; // screen px
    double strength = 0.5;
};

LILA_UI_API BrushSettings& brush();
/** Saves the settings and tells listeners. */
LILA_UI_API void settingsChanged();

/** Live mode: dragging uvs steers the solver, and cuts, sews and pins re-solve their islands as they happen. */
LILA_UI_API bool liveMode();
LILA_UI_API void setLiveMode(bool on);
/** Stretch against compression, -1..1 (see uv::LiveOptions::balance). */
LILA_UI_API double balance();
LILA_UI_API void setBalance(double b);
LILA_UI_API bool checker3d();
LILA_UI_API void setChecker3d(bool on);
/** Checks across one 0-1 tile, for the 3D checker and the editor's checker backdrop. */
LILA_UI_API int checkerTiling();
LILA_UI_API void setCheckerTiling(int checks);
/** Island select: face mode where clicks and marquees, in the UV editor and the viewports, select whole uv islands. */
LILA_UI_API bool islandSelect();
LILA_UI_API void setIslandSelect(bool on);
/** The 3D checker only draws while a UV editor is on screen; each editor reports its visibility here. */
LILA_UI_API void setEditorShown(const void* editor, bool shown);
LILA_UI_API bool seams3d();
LILA_UI_API void setSeams3d(bool on);
/** Stretch heatmap in two colours instead of grayscale (still ordered light = stretched). */
LILA_UI_API bool twoColourHeatmap();
LILA_UI_API void setTwoColourHeatmap(bool on);

/** Called on settings changes and whenever a live session publishes a new frame. Main thread. */
LILA_UI_API int addListener(std::function<void()> fn);
LILA_UI_API void removeListener(int id);

/** Meshes the uv tools show (see uv::shownMeshes). */
LILA_UI_API std::vector<scene::NodeId> shownMeshes();
/** Faces selected on the node (any component mode, converted), empty when none. */
LILA_UI_API std::vector<geo::Index> selectedFaces(scene::NodeId node);

/** Starts the driver that ticks live sessions (idempotent). */
LILA_UI_API void ensureDriver();

enum class Op
{
    Relax,   // relax the islands live until they settle
    Flatten, // solve from scratch, then relax live
    Settle,  // relax to convergence, then a SLIM pass for a fold-free finish
};
/** Runs op on the selected islands (or every island) of the shown meshes, without blocking; one undo step each. */
LILA_UI_API void run(Op op);

/** After a structural edit (cut, sew, pin) in live mode, the islands holding the faces settle live. */
LILA_UI_API void settleAfter(scene::NodeId node, const std::vector<geo::Index>& faces, const char* label);

/** Runs uvCut or uvSew on the selected edges from the UI; in live mode the touched islands then settle live. Returns the result. */
LILA_UI_API nlohmann::json cutOrSew(const char* command, nlohmann::json call);

/** The uv brush on the 3D mesh (action "uv.brush3d"). */
LILA_UI_API void startBrushTool3d();

/** Actions, prefs and the 3D drawer. Called from registerUvActions. */
LILA_UI_API void install();
} // namespace lila::ui::uvLive
