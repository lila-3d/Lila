#pragma once

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

BrushSettings& brush();
/** Saves the settings and tells listeners. */
void settingsChanged();

/** Live mode: dragging uvs steers the solver, and cuts, sews and pins re-solve their islands as they happen. */
bool liveMode();
void setLiveMode(bool on);
/** Stretch against compression, -1..1 (see uv::LiveOptions::balance). */
double balance();
void setBalance(double b);
bool checker3d();
void setChecker3d(bool on);
bool seams3d();
void setSeams3d(bool on);
/** Stretch heatmap in two colours instead of grayscale (still ordered light = stretched). */
bool twoColourHeatmap();
void setTwoColourHeatmap(bool on);

/** Called on settings changes and whenever a live session publishes a new frame. Main thread. */
int addListener(std::function<void()> fn);
void removeListener(int id);

/** Meshes the uv tools show: the selected ones plus any with a component selection. */
std::vector<scene::NodeId> shownMeshes();
/** Faces selected on the node (any component mode, converted), empty when none. */
std::vector<geo::Index> selectedFaces(scene::NodeId node);

/** Starts the driver that ticks live sessions (idempotent). */
void ensureDriver();

enum class Op
{
    Relax,   // relax the islands live until they settle
    Flatten, // solve from scratch, then relax live
    Settle,  // relax to convergence, then a SLIM pass for a fold-free finish
};
/** Runs op on the selected islands (or every island) of the shown meshes, without blocking; one undo step each. */
void run(Op op);

/** After a structural edit (cut, sew, pin) in live mode, the islands holding the faces settle live. */
void settleAfter(scene::NodeId node, const std::vector<geo::Index>& faces, const char* label);

/** Runs uvCut or uvSew on the selected edges from the UI; in live mode the touched islands then settle live. Returns the result. */
nlohmann::json cutOrSew(const char* command, nlohmann::json call);

/** The uv brush on the 3D mesh (action "uv.brush3d"). */
void startBrushTool3d();

/** Actions, prefs and the 3D drawer. Called from registerUvActions. */
void install();
} // namespace lila::ui::uvLive
