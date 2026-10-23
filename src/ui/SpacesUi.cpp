#include "ui/SpacesUi.h"

#include "cmds/Command.h"
#include "core/Log.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/Spaces.h"
#include "ui/Actions.h"
#include "ui/AttributeEditor.h"
#include "ui/Icons.h"
#include "ui/Menus.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

namespace lila::ui
{
namespace
{
using scene::Graph;
using scene::NodeId;

void run(const char* name, json call = json::object())
{
    try
    {
        cmds::execute(name, call, CallSource::UI);
    }
    catch (const std::exception& e)
    {
        log::error("{}", e.what());
    }
}

std::vector<std::string> selectedWithSpaces()
{
    Graph& g = Graph::current();
    std::vector<std::string> out;
    for (NodeId id : g.selection())
        if (scene::spaces::hasSpaces(g, id))
            out.push_back(g.node(id)->name());
    return out;
}

void switchSelected(const json& space, bool range)
{
    std::vector<std::string> nodes = selectedWithSpaces();
    if (nodes.empty())
    {
        log::warn("Switch Space: select nodes with spaces (Rig > Spaces > Add Space)");
        return;
    }
    json call = {{"args", nodes}, {"space", space}};
    if (range)
        call["range"] = {scene::anim::playback().minTime, scene::anim::playback().maxTime};
    run("switchSpace", call);
}

/** The lead selection's spaces, to switch to now or over the playback range. */
void fillSwitchMenu(QMenu* menu, bool range)
{
    QObject::connect(menu, &QMenu::aboutToShow, menu, [menu, range] {
        menu->clear();
        Graph& g = Graph::current();
        const NodeId lead = g.selection().empty() ? scene::kNoNode : g.selection().back();
        if (!lead || !scene::spaces::hasSpaces(g, lead))
        {
            menu->addAction("(select a node with spaces)")->setEnabled(false);
            return;
        }
        const std::vector<std::string> names = scene::spaces::names(g, lead);
        const int now = scene::spaces::active(g, lead);
        for (int i = 0; i < int(names.size()); ++i)
        {
            QAction* a = menu->addAction(QString::fromStdString(names[size_t(i)]), [i, range] { switchSelected(i, range); });
            a->setCheckable(true);
            a->setChecked(!range && i == now);
        }
    });
}

QWidget* spacesPanel(NodeId id)
{
    Graph& g = Graph::current();
    if (!g.node(id))
        return nullptr;
    const std::string node = g.node(id)->name();
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(8, 2, 4, 4);
    v->setSpacing(2);
    const int now = scene::spaces::active(g, id);
    for (const scene::spaces::Space& s : scene::spaces::list(g, id))
    {
        auto* row = new QHBoxLayout;
        auto* name = new QLabel(QString::fromStdString(s.name.empty() ? "(missing)" : s.name) + (s.index == now ? "  (active)" : ""));
        QFont f = name->font();
        f.setBold(s.index == now);
        name->setFont(f);
        row->addWidget(name, 1);
        auto* sw = new QToolButton;
        sw->setIcon(icon("arrows-exchange"));
        sw->setToolTip("Switch to this space, keeping the pose");
        sw->setEnabled(s.index != now);
        QObject::connect(sw, &QToolButton::clicked, sw, [node, i = s.index] { run("switchSpace", {{"args", {node}}, {"space", i}}); });
        row->addWidget(sw);
        auto* rm = new QToolButton;
        rm->setIcon(icon("trash"));
        rm->setToolTip("Remove this space; times spent in it go back to the parent, keeping the pose");
        rm->setEnabled(s.index > 0);
        QObject::connect(rm, &QToolButton::clicked, rm, [node, i = s.index] { run("removeSpace", {{"args", {node}}, {"space", i}}); });
        row->addWidget(rm);
        v->addLayout(row);
    }
    auto* add = new QHBoxLayout;
    auto* target = new QLineEdit;
    target->setPlaceholderText("world or a node name");
    add->addWidget(target, 1);
    auto* addButton = new QPushButton("Add Space");
    addButton->setToolTip("Add a space: world, or another node to follow");
    QObject::connect(addButton, &QPushButton::clicked, addButton, [node, target] {
        const std::string t = target->text().trimmed().toStdString();
        run("addSpace", {{"args", {node}}, {"space", t.empty() ? "world" : t}});
    });
    add->addWidget(addButton);
    v->addLayout(add);
    return w;
}

void registerActions()
{
    auto add = [](const char* id, const char* label, std::function<void()> fn, const char* python, const char* doc) {
        actions::add({.id = id, .label = label, .category = "Rig", .run = std::move(fn), .doc = doc, .python = python});
    };
    add("rig.addWorldSpace", "Add World Space", [] { run("addSpace", {{"space", "world"}}); }, "addSpace(space=\"world\")",
        "Give the selected nodes a world space to animate in; switching to it keeps the pose.");
    add("rig.addSpace", "Add Space", [] { run("addSpace"); }, "addSpace()",
        "Select the spaces, then the node last: the node can then be animated in any of them, switched from the channel box.");
    add("rig.removeSpace", "Remove Active Space", [] {
        Graph& g = Graph::current();
        for (const std::string& n : selectedWithSpaces())
            if (int i = scene::spaces::active(g, g.find(n)->id()); i > 0)
                run("removeSpace", {{"args", {n}}, {"space", i}});
    }, "", "Remove the selected nodes' active space, switching them back to their parent with the pose kept.");
    add("rig.switchSpaceParent", "Switch to Parent Space", [] { switchSelected("parent", false); }, "switchSpace(space=\"parent\")",
        "Switch the selected nodes to their parent space at this frame, keeping the pose.");
    add("rig.switchSpaceWorld", "Switch to World Space", [] { switchSelected("world", false); }, "switchSpace(space=\"world\")",
        "Switch the selected nodes to their world space at this frame, keeping the pose.");
}
} // namespace

void registerSpacesUi()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    registerActions();
    attributeEditor::addSection("transform", "Spaces", [](NodeId id) { return spacesPanel(id); });
}

void setupSpacesMenu()
{
    menus::addActions("Rig/Spaces", {"rig.addWorldSpace", "rig.addSpace", "rig.removeSpace", "", "rig.switchSpaceParent", "rig.switchSpaceWorld"});
    menus::addSubmenu("Rig/Spaces", "Switch To", [](QMenu* m) { fillSwitchMenu(m, false); });
    menus::addSubmenu("Rig/Spaces", "Switch Over Playback Range", [](QMenu* m) { fillSwitchMenu(m, true); });
}
} // namespace lila::ui
