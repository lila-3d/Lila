#include "ui/UnrealMappingUi.h"

#include "core/Log.h"
#include "link/UnrealLink.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/ObjectSets.h"
#include "ui/AttributeEditor.h"

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace lila::ui
{
namespace
{
using scene::NodeId;

void runMappingCommand(const char* name, json flags)
{
    try
    {
        ops::execute(name, flags, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

/** Maps the node again with one field changed, keeping every other option the mapping has. */
void remap(const std::string& node, const char* key, const json& value)
{
    json m = link::mappingOf(node);
    const CommandDef* def = ops::find("unrealMap");
    if (!m.is_object() || !def)
        return;
    m[key] = value;
    json flags = {{"args", {node}}};
    for (const FlagDef& f : def->flags)
        if (m.contains(f.name) && !m[f.name].is_null())
            flags[f.name] = m[f.name];
    runMappingCommand("unrealMap", flags);
}

QWidget* mappingPanel(NodeId id)
{
    scene::Graph& g = scene::Graph::current();
    const scene::Node* n = g.node(id);
    if (!n)
        return nullptr;
    const std::string node = n->path();
    const json m = link::mappingOf(node);
    if (!m.is_object())
        return nullptr;
    const std::string kind = m.value("kind", std::string("static"));

    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(8, 2, 4, 2);
    auto* form = new QFormLayout;
    v->addLayout(form);

    if (kind != "spline")
    {
        auto* path = new QLineEdit(QString::fromStdString(m.value("contentPath", std::string())));
        path->setToolTip("The Unreal asset this node is imported as.");
        QObject::connect(path, &QLineEdit::editingFinished, path, [path, node] {
            if (!path->text().isEmpty() && path->isModified())
                remap(node, "contentPath", path->text().toStdString());
            path->setModified(false);
        });
        form->addRow("Content path", path);
    }

    if (kind == "static" || kind == "skeletal" || kind == "animation")
    {
        auto* kinds = new QComboBox;
        for (const char* k : {"static", "skeletal", "animation"})
            kinds->addItem(QString(k).replace(0, 1, QString(k).at(0).toUpper()), k);
        kinds->setCurrentIndex(kinds->findData(QString::fromStdString(kind)));
        QObject::connect(kinds, &QComboBox::activated, kinds, [kinds, node] { remap(node, "kind", kinds->currentData().toString().toStdString()); });
        form->addRow("Kind", kinds);
    }
    else
        form->addRow("Kind", new QLabel(QString::fromStdString(kind)));

    if (kind == "skeletal" || kind == "animation")
    {
        auto* facing = new QComboBox;
        facing->addItem("Preference default", QString());
        facing->addItem("+X (as modelled)", "x");
        facing->addItem("+Y (Epic mannequin)", "y");
        facing->setCurrentIndex(std::max(0, facing->findData(QString::fromStdString(m.value("facing", std::string())))));
        facing->setToolTip("The way the character faces in Unreal. A mesh and its animations need the same facing.");
        QObject::connect(facing, &QComboBox::activated, facing, [facing, node] {
            const std::string f = facing->currentData().toString().toStdString();
            remap(node, "facing", f.empty() ? json() : json(f));
        });
        form->addRow("Facing", facing);
    }

    auto* row = new QHBoxLayout;
    auto* send = new QPushButton("Send");
    send->setToolTip("Export this node and reimport it in Unreal now.");
    QObject::connect(send, &QPushButton::clicked, send, [node] { runMappingCommand("unrealSync", {{"args", {node}}}); });
    auto* remove = new QPushButton("Remove");
    remove->setToolTip("Stop sending this node to Unreal. The asset in Unreal stays.");
    QObject::connect(remove, &QPushButton::clicked, remove, [node] { runMappingCommand("unrealUnmap", {{"args", {node}}}); });
    row->addWidget(send);
    row->addWidget(remove);
    row->addStretch();
    v->addLayout(row);
    return w;
}
} // namespace

void registerUnrealMappingUi()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    attributeEditor::addSection("transform", "Unreal", [](NodeId id) { return mappingPanel(id); });
    attributeEditor::addSection(scene::objectSets::kTypeName, "Unreal", [](NodeId id) { return mappingPanel(id); });
}
} // namespace lila::ui
