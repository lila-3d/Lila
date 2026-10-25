#include "ui/LodDisplay.h"

#include "ops/Command.h"
#include "core/PrefStore.h"
#include "ui/Actions.h"
#include "ui/ViewInfo.h"
#include "ui/ViewportWidget.h"

#include <QActionGroup>
#include <QMenu>

#include <algorithm>
#include <format>
#include <string>

namespace lila::ui::lodDisplay
{
namespace
{
using scene::lod::Mode;

std::string panelKey(int panel, const char* item) { return std::format("/viewport/lod/panel{}/{}", panel, item); }

std::string modeAction(Mode m) { return std::string("viewport.lod.") + scene::lod::modeName(m); }
std::string levelAction(int level) { return std::format("viewport.lod.level{}", level); }

bool isChoice(int panel, Mode m, int lvl) { return mode(panel) == m && (m != Mode::Level || level(panel) == lvl); }
} // namespace

Mode mode(int panel)
{
    PrefStore& p = PrefStore::get();
    json own = p.value("general", panelKey(panel, "mode"), json(nullptr));
    const std::string name = own.is_string() ? own.get<std::string>() : p.value<std::string>("general", kMode, "screenSize");
    Mode m = Mode::ScreenSize;
    scene::lod::parseMode(name, &m);
    return m;
}

int level(int panel)
{
    PrefStore& p = PrefStore::get();
    json own = p.value("general", panelKey(panel, "level"), json(nullptr));
    const int l = own.is_number() ? own.get<int>() : p.value<int>("general", kLevel, 0);
    return std::clamp(l, 0, scene::lod::kMaxLevels - 1);
}

void set(int panel, Mode m, int lvl)
{
    PrefStore::get().set("general", panelKey(panel, "mode"), scene::lod::modeName(m));
    if (m == Mode::Level)
        PrefStore::get().set("general", panelKey(panel, "level"), std::clamp(lvl, 0, scene::lod::kMaxLevels - 1));
}

scene::lod::View view(int panel, const ViewInfo& info)
{
    return {.mode = mode(panel), .level = level(panel), .eye = info.eye, .proj = info.proj, .ortho = info.ortho};
}

void registerActions(std::function<ViewportWidget*()> active)
{
    auto add = [active](std::string id, std::string label, Mode m, int lvl, std::string doc) {
        const std::string python = "lila.ui.run_action(\"" + id + "\")";
        actions::add({.id = std::move(id), .label = std::move(label), .category = "Display",
                      .run = [active, m, lvl] {
                          if (ViewportWidget* v = active())
                              set(v->hudPanel(), m, lvl);
                      },
                      .doc = std::move(doc),
                      .python = python,
                      .checked = [active, m, lvl] {
                          ViewportWidget* v = active();
                          return v && isChoice(v->hudPanel(), m, lvl);
                      }});
    };
    add(modeAction(Mode::ScreenSize), "LOD by Screen Size", Mode::ScreenSize, 0,
        "LOD groups in the active view show the level Unreal would draw at their size on screen.");
    add(modeAction(Mode::All), "All LOD Levels", Mode::All, 0, "LOD groups in the active view show every level at once.");
    for (int i = 0; i < scene::lod::kMaxLevels; ++i)
        add(levelAction(i), std::format("Show LOD {}", i), Mode::Level, i,
            std::format("LOD groups in the active view show level {} (their last level when they have fewer).", i));

    actions::add({.id = "edit.lodGroup", .label = "LOD Group", .category = "Edit",
                  .doc = "Groups the selected objects as levels of detail, LOD0 first in selection order. Unreal imports the group as one "
                         "mesh with those LODs.",
                  .python = ops::toPython("lodGroup", json::object()), .command = "lodGroup", .call = json::object()});
    actions::add({.id = "edit.generateLods", .label = "Generate LODs", .category = "Edit",
                  .doc = "Makes an LOD group from the selected mesh with three reduced copies as LOD1 to LOD3, each half the triangles of the "
                         "one before.",
                  .python = ops::toPython("lodGroup", {{"generate", 3}}), .command = "lodGroup", .call = {{"generate", 3}}});
}

void fillMenu(QMenu* menu, ViewportWidget* view)
{
    const int panel = view->hudPanel();
    auto* group = new QActionGroup(menu);
    auto item = [&](const QString& label, Mode m, int lvl) {
        QAction* a = menu->addAction(label);
        a->setCheckable(true);
        a->setChecked(isChoice(panel, m, lvl));
        group->addAction(a);
        QObject::connect(a, &QAction::triggered, menu, [panel, m, lvl] { set(panel, m, lvl); });
    };
    item("By Screen Size", Mode::ScreenSize, 0);
    item("All Levels", Mode::All, 0);
    menu->addSeparator();
    for (int i = 0; i < scene::lod::kMaxLevels; ++i)
        item(QString("LOD %1").arg(i), Mode::Level, i);
    menu->addSeparator();
    for (const char* id : {"edit.lodGroup", "edit.generateLods"})
        if (const Action* a = actions::find(id))
            QObject::connect(menu->addAction(QString::fromStdString(a->label)), &QAction::triggered, menu, [id] { actions::run(id); });
}
} // namespace lila::ui::lodDisplay
