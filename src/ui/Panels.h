#pragma once

#include "core/Export.h"
#include "core/Registry.h"
#include "ui/Owner.h"

#include <qnamespace.h>

#include <cstdint>
#include <functional>
#include <string>

class QDockWidget;
class QWidget;

namespace lila::ui
{
/** Dock object names for the API reference and terminal panels. */
inline constexpr const char* kApiReferenceDock = "apiReference";
inline constexpr const char* kTerminalDock = "terminal";
inline constexpr const char* kReferenceEditorDock = "referenceEditor";
inline constexpr const char* kScriptEditorDock = "scriptEditor";

/** Registers "window.apiReference", "window.scriptEditor", "window.terminal", "window.referenceEditor", "window.displayLayers" (plus the layers.* actions) and "window.undoHistory". They do nothing until a dock hook is set. */
LILA_UI_API void registerPanelActions();

/** How a dock is shown: the main window raises the QDockWidget with that object name. */
LILA_UI_API void setShowDockHook(std::function<void(const char* dockName)> showDock);

/** How the window.* panel actions show a dock, or tuck it away when it is already showing. */
LILA_UI_API void setToggleDockHook(std::function<void(const char* dockName)> toggleDock);

/**
 * Dockable panels (Maya's workspaceControl). The main window makes a dock for every registered panel, Lila's own
 * included, and adds or removes docks as panels come and go, so an extension's panel docks, tabs, collapses and saves
 * with layouts like any other.
 */
namespace panels
{
struct Panel
{
    std::string id;    // the dock's object name, which layouts and the window state save it under
    std::string title;
    /** Makes the content; parent is the main window. Called once per dock. */
    std::function<QWidget*(QWidget* parent)> create;
    Qt::DockWidgetArea area = Qt::RightDockWidgetArea;
    std::string icon; // Tabler name, for the collapsed sidebar tab
    bool open = true; // shown when the dock is first made
    /** Runs on the new dock: split or tabify it with docks made before it, give it a title bar. */
    std::function<void(QDockWidget*)> place;
    /** An action that shows or tucks away the panel, registered with it under the same owner; empty for none. */
    std::string toggleAction;
};

LILA_UI_API Registry<Panel>& registry();
/** Adds or replaces. */
LILA_UI_API void add(Panel p, std::string owner = kLilaOwner);
/** Owned by the extension and removed with its dock when it unloads. toggleAction defaults to "window.<id>". */
LILA_UI_API uint64_t add(ext::Context& ctx, Panel p);
LILA_UI_API bool remove(const std::string& id);

/** Shows and raises the panel's dock, through the window's dock hook. */
LILA_UI_API void show(const std::string& id);
/** Shows the panel, or tucks it away when it is showing. */
LILA_UI_API void toggle(const std::string& id);
} // namespace panels
} // namespace lila::ui
