#pragma once

#include "core/Export.h"

#include <functional>
#include <string>
#include <vector>

class QMainWindow;

/**
 * Which menu set the menu bar shows, worked out from what the user is doing: the active tool, then the focused
 * editor, then the selection, then the layout's own set. A pinned set (F3 to F7, or Windows > Menu Set) wins until it is
 * unpinned. Nothing here gates a command: the hotbox and Ctrl+T always reach every menu.
 */
namespace lila::ui::menuContext
{
/** Every set a menu can belong to, in menu order. */
LILA_UI_API const std::vector<std::string>& sets();

/** The set the menu bar shows now. */
LILA_UI_API std::string current();

/** The set a layout starts with; applying a layout also unpins. */
LILA_UI_API void setLayoutSet(const std::string& set);
LILA_UI_API std::string layoutSet();

LILA_UI_API void pin(const std::string& set);
LILA_UI_API void unpin();
/** Empty when following the context. */
LILA_UI_API std::string pinned();

/** Runs fn whenever current() may have changed. */
LILA_UI_API void onChanged(std::function<void()> fn);

/** Starts listening to tools, focus and selection, and registers the menuSet.* actions. */
LILA_UI_API void install(QMainWindow* window);

/** The set a viewport tool belongs to, or empty for a tool every set uses (selection, transform, curve drawing). */
LILA_UI_API std::string setForTool(const std::string& toolName);
/** The set a dock (by object name) belongs to, or empty. */
LILA_UI_API std::string setForDock(const std::string& dockName);
} // namespace lila::ui::menuContext
