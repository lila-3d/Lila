#pragma once

#include <functional>

namespace lila::ui
{
/** Dock object names for the API reference and terminal panels. */
inline constexpr const char* kApiReferenceDock = "apiReference";
inline constexpr const char* kTerminalDock = "terminal";
inline constexpr const char* kReferenceEditorDock = "referenceEditor";
inline constexpr const char* kScriptEditorDock = "scriptEditor";

/** Registers "window.apiReference", "window.scriptEditor", "window.terminal", "window.referenceEditor", "window.displayLayers" (plus the layers.* actions) and "window.undoHistory". They do nothing until a dock hook is set. */
void registerPanelActions();

/** How a dock is shown: the main window raises the QDockWidget with that object name. */
void setShowDockHook(std::function<void(const char* dockName)> showDock);

/** How the window.* panel actions show a dock, or tuck it away when it is already showing. */
void setToggleDockHook(std::function<void(const char* dockName)> toggleDock);
} // namespace lila::ui
