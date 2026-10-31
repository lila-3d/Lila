#pragma once

#include "core/Export.h"
#include "ui/ViewInfo.h"

#include <nlohmann/json.hpp>

#include <QPointF>

#include <optional>

/**
 * What a command would do, drawn in the viewports before it runs: the result replaces the meshes it changes, with the
 * edges it adds or moves in a highlight color (or the ones it removes, for a delete), and a readout of the command, its targets and main settings at the result. The
 * marking menu shows it for the item under the cursor.
 *
 * Only adjustable commands are previewed (CommandDef::adjustable), since those are safe to run and undo at will. The
 * preview runs the command for real inside an undo chunk, keeps the meshes it made and cancels the chunk. A command
 * that took too long is skipped for a minute, except on a selection under half the size of the slow one.
 */
namespace lila::ui::commandPreview
{
/** Pref (general) that turns previews off; default on. */
inline constexpr const char* kEnabledPref = "/markingMenu/preview";
/** Pref (general): no preview while the selected meshes hold more faces than this; 0 is no limit. */
inline constexpr const char* kMaxFacesPref = "/markingMenu/previewMaxFaces";
inline constexpr double kMaxFacesDefault = 500000.0;
/** Pref (general): a run slower than this many ms counts as slow. */
inline constexpr const char* kSlowMsPref = "/markingMenu/previewSlowMs";
inline constexpr double kSlowMsDefault = 250.0;
/** Pref (general): seconds a slow command is skipped before it is tried again. */
inline constexpr const char* kRetrySecondsPref = "/markingMenu/previewRetrySeconds";
inline constexpr double kRetrySecondsDefault = 60.0;
/** Pref (general): ms the cursor rests on a marking menu item before it is previewed. */
inline constexpr const char* kDelayMsPref = "/markingMenu/previewDelayMs";
inline constexpr double kDelayMsDefault = 120.0;
/** Pref (general): edges fade out as their faces turn away, reaching full where the face's normal is this far toward the camera (0 to 1). */
inline constexpr const char* kEdgeFadePref = "/markingMenu/previewEdgeFade";
inline constexpr double kEdgeFadeDefault = 1.0;
/** Pref (general): how far the preview's edges are pulled toward the camera past the surface, in % of the changed meshes' size. */
inline constexpr const char* kPullPref = "/markingMenu/previewPull";
inline constexpr double kPullDefault = 0.5;
/** Pref (general): edges up to this far behind the surface (% of the changed meshes' size) still show, faintly; 0 cuts them off. */
inline constexpr const char* kBehindPref = "/markingMenu/previewBehind";
inline constexpr double kBehindDefault = 3.0;
/**
 * Pref (general): where the result shows, within the radius: "cursor" around where the menu opened, "center" around the
 * view's center when it opened, "both" around either, "all" everywhere.
 */
inline constexpr const char* kCoveragePref = "/markingMenu/previewCoverage";
inline constexpr const char* kCoverageDefault = "all";
/** Pref (general): the radius in pixels the result shows within, fading toward its edge. */
inline constexpr const char* kRadiusPxPref = "/markingMenu/previewRadiusPx";
inline constexpr double kRadiusPxDefault = 200.0;
/** Pref (general): with a radius, the result mesh still replaces the original everywhere (as "all" always does); default off. */
inline constexpr const char* kResultMeshPref = "/markingMenu/previewResultMesh";

/** Where the user is looking: a view and a pixel in it, such as where a marking menu opened over a viewport. */
struct Focus
{
    ViewInfo view;
    QPointF px;
};

/**
 * Previews running a binding target (an action id or {"command", "args"}), replacing any preview shown. False, with
 * nothing shown, when it can't be previewed or would change no mesh.
 */
LILA_UI_API bool show(const nlohmann::json& target, const std::optional<Focus>& focus = std::nullopt);
LILA_UI_API void clear();
LILA_UI_API bool showing();

/** Adds the viewport drawer. Idempotent. */
LILA_UI_API void install();
} // namespace lila::ui::commandPreview
