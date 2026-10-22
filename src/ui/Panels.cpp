#include "ui/Panels.h"

#include "ui/Actions.h"
#include "ui/DisplayLayersPanel.h"
#include "ui/UndoHistory.h"

namespace lila::ui
{
namespace
{
std::function<void(const char*)>& showDockHook()
{
    static std::function<void(const char*)> hook;
    return hook;
}

std::function<void(const char*)>& toggleDockHook()
{
    static std::function<void(const char*)> hook;
    return hook;
}

void toggle(const char* dock)
{
    if (toggleDockHook())
        toggleDockHook()(dock);
}
} // namespace

void setShowDockHook(std::function<void(const char* dockName)> showDock)
{
    showDockHook() = std::move(showDock);
}

void setToggleDockHook(std::function<void(const char* dockName)> toggleDock)
{
    toggleDockHook() = std::move(toggleDock);
}

void registerPanelActions()
{
    actions::add({.id = "window.apiReference", .label = "API Reference", .category = "Windows", .run = [] { toggle(kApiReferenceDock); },
                  .doc = "Searchable reference for every command and the lila Python package.",
                  .python = "lila.ui.run_action(\"window.apiReference\")"});
    actions::add({.id = "window.scriptEditor", .label = "Script Editor", .category = "Windows", .run = [] { toggle(kScriptEditorDock); },
                  .doc = "Python editor with tabs, run selection and the API reference a key away.",
                  .python = "lila.ui.run_action(\"window.scriptEditor\")"});
    actions::add({.id = "window.terminal", .label = "Terminal", .category = "Windows", .run = [] { toggle(kTerminalDock); },
                  .doc = "Shell in the workspace folder, with Lila's Python on PATH.",
                  .python = "lila.ui.run_action(\"window.terminal\")"});
    actions::add({.id = "window.referenceEditor", .label = "Reference Editor", .category = "Windows", .run = [] { toggle(kReferenceEditorDock); },
                  .doc = "Lists the scene's references: load, unload, reload, import or remove them. Again to tuck it away.",
                  .python = "lila.ui.run_action(\"window.referenceEditor\")"});
    actions::add({.id = "window.displayLayers", .label = "Display Layers", .category = "Windows", .run = [] { toggle(kDisplayLayersDock); },
                  .doc = "Maya's display layer editor: show, hide, template or reference groups of objects together.",
                  .python = "lila.ui.run_action(\"window.displayLayers\")"});
    registerDisplayLayerActions();
    actions::add({.id = "window.undoHistory", .label = "Undo History", .category = "Windows", .run = [] { toggle(kUndoHistoryDock); },
                  .doc = "Lists the undo steps; click one to undo or redo to it.",
                  .python = "lila.ui.run_action(\"window.undoHistory\")"});
}
} // namespace lila::ui
