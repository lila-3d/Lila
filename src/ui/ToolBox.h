#pragma once

#include "core/Export.h"

#include "ui/IconBars.h"

#include <functional>

class QMainWindow;

namespace lila::ui
{
class ViewportWidget;
}

/**
 * The tool box: a vertical icon bar (iconBars::kToolBox) down the main window's left edge with the selection tools,
 * selection modes and the active view's display aids. /ui/showToolBox shows or hides it (window.toggleToolBox).
 */
namespace lila::ui::toolBox
{
inline constexpr const char* kShownPref = "/ui/showToolBox";
/** Icon size in logical pixels. */
inline constexpr int kIconSize = 26;

/** Puts the bar at window's left edge. state answers the buttons' checks. */
LILA_UI_API void install(QMainWindow* window, iconBars::StateFn state);

/** Moves the bar back into a column of its own left of the collapsed-panel strip; called after each layout restore. */
LILA_UI_API void place(QMainWindow* window);

/** The tool box's actions (window.toggleToolBox, select.marqueeTool, select.lassoTool) and Lila's items in it. */
LILA_UI_API void registerItems(std::function<ViewportWidget*()> active);
} // namespace lila::ui::toolBox
