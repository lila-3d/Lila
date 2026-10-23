#include "ui/Panels.h"

#include "core/ExtensionContext.h"
#include "core/Log.h"
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

Registry<panels::Panel>& panels::registry()
{
    static Registry<Panel> r;
    return r;
}

void panels::add(Panel p, std::string owner)
{
    std::string id = p.id;
    if (!p.toggleAction.empty() && !actions::find(p.toggleAction))
        actions::add({.id = p.toggleAction, .label = p.title, .category = "Windows", .run = [id] { panels::toggle(id); },
                      .doc = "Shows the " + p.title + " panel, or tucks it away when it is showing.",
                      .python = "lila.ui.show_panel(\"" + id + "\")"},
                     owner);
    registry().put(id, std::move(p), std::move(owner));
}

uint64_t panels::add(ext::Context& ctx, Panel p)
{
    if (const std::string* owner = registry().ownerOf(p.id); owner && *owner != ctx.owner())
    {
        ctx.log(LogLevel::Error, "panel '" + p.id + "' already belongs to " + *owner);
        return 0;
    }
    if (p.toggleAction.empty())
        p.toggleAction = "window." + p.id;
    std::string id = p.id, action = p.toggleAction;
    bool ownsAction = !actions::find(action);
    add(std::move(p), ctx.owner());
    return ctx.track("panel:" + id, [id, action, ownsAction, own = ctx.owner()] {
        if (const std::string* o = registry().ownerOf(id); o && *o == own)
            registry().remove(id);
        if (const std::string* o = actions::registry().ownerOf(action); ownsAction && o && *o == own)
            actions::remove(action);
    });
}

bool panels::remove(const std::string& id)
{
    return registry().remove(id);
}

void panels::show(const std::string& id)
{
    if (showDockHook())
        showDockHook()(id.c_str());
}

void panels::toggle(const std::string& id)
{
    lila::ui::toggle(id.c_str());
}

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
