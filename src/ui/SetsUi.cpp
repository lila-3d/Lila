#include "ui/SetsUi.h"

#include "ops/Command.h"
#include "core/Log.h"
#include "scene/Graph.h"
#include "scene/ObjectSets.h"
#include "ui/Actions.h"
#include "ui/Icons.h"

#include <QInputDialog>
#include <QMenu>
#include <QPointer>

namespace lila::ui::sets
{
namespace
{
json run(const char* command, const json& call)
{
    try
    {
        return ops::execute(command, call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::warn("{}", e.what());
        return nullptr;
    }
}

void createQuickSelect(QWidget* parent)
{
    bool ok = false;
    QString name = QInputDialog::getText(parent, "Create Quick Select Set", "Name", QLineEdit::Normal, "quickSelectSet1", &ok);
    if (!ok)
        return;
    run("createSet", {{"name", name.trimmed().toStdString()}, {"quickSelect", true}});
}
} // namespace

void registerActions(QWidget* dialogParent)
{
    if (actions::find("sets.create"))
        return;
    actions::add({.id = "sets.create", .label = "Set", .category = "Create",
                  .doc = "A new object set holding the selected objects, or the selected components in component mode.",
                  .python = "lila.ops.createSet(name=\"set1\")", .command = "createSet", .call = json::object()});
    QPointer<QWidget> parent(dialogParent);
    actions::add({.id = "sets.createQuickSelect", .label = "Quick Select Set...", .category = "Create",
                  .run = [parent] { createQuickSelect(parent); },
                  .doc = "Names a quick select set holding the selection (objects or components); Edit > Quick Select Sets "
                         "selects it again in one click.",
                  .python = "lila.ops.createSet(name=\"pickMe\", quickSelect=True)"});
}

void attachQuickSelectMenu(QMenu* menu)
{
    menu->setToolTipsVisible(true);
    QObject::connect(menu, &QMenu::aboutToShow, menu, [menu] {
        menu->clear();
        scene::Graph& g = scene::Graph::current();
        for (const scene::Node* s : scene::objectSets::all(g, true))
        {
            const std::string name = s->name();
            QAction* a = menu->addAction(icon("list-details"), QString::fromStdString(name),
                                         [name] { run("selectSetMembers", {{"args", {name}}}); });
            a->setToolTip(QString::fromStdString("Select the members of " + name));
        }
        if (menu->isEmpty())
            menu->addAction("(no quick select sets)")->setEnabled(false);
        menu->addSeparator();
        if (const Action* create = actions::find("sets.createQuickSelect"))
            menu->addAction(QString::fromStdString(create->label), [] { actions::run(json("sets.createQuickSelect")); });
    });
}
} // namespace lila::ui::sets
