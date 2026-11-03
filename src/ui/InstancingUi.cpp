#include "ui/InstancingUi.h"

#include "ops/Command.h"
#include "core/Log.h"
#include "scene/ArrayGroup.h"
#include "scene/Graph.h"
#include "scene/Instancing.h"
#include "ui/AttributeEditor.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace lila::ui
{
namespace
{
using scene::Graph;
using scene::NodeId;
namespace inst = scene::instancing;

void runOnNode(const char* name, const std::string& node, json flags = json::object())
{
    try
    {
        flags["args"] = {node};
        ops::execute(name, flags, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

QWidget* instancingPanel(NodeId mesh)
{
    Graph& g = Graph::current();
    const inst::Kind kind = inst::kind(g, mesh);
    if (kind == inst::Kind::None)
        return nullptr;
    const NodeId src = inst::source(g, mesh);
    const auto count = qsizetype(inst::members(g, mesh).size());
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(8, 2, 4, 2);
    QString what = kind == inst::Kind::Source ? QString("Source shared by %1 other meshes").arg(count - 1)
                                              : QString("%1 of %2, %3 meshes share it")
                                                    .arg(kind == inst::Kind::Instance ? "Instance" : "Reference")
                                                    .arg(QString::fromStdString(g.node(src)->name()))
                                                    .arg(count);
    auto* label = new QLabel(what);
    QFont f = label->font();
    f.setItalic(kind != inst::Kind::Source);
    label->setFont(f);
    label->setWordWrap(true);
    if (src)
        label->setToolTip(QString::fromStdString(g.fullPath(src)));
    v->addWidget(label);
    auto* row = new QHBoxLayout;
    const std::string path = g.node(mesh)->path();
    auto button = [&](const char* text, const char* cmd, const char* tip) {
        auto* b = new QPushButton(text);
        b->setToolTip(tip);
        QObject::connect(b, &QPushButton::clicked, b, [cmd, path] { runOnNode(cmd, path); });
        row->addWidget(b);
    };
    if (kind != inst::Kind::Source)
        button("Select Source", "selectInstanceSource", "Select the mesh this one shares from.");
    button("Select All", "selectInstances", "Select every mesh sharing this data.");
    button("Make Unique", "makeUnique", "Give this mesh data of its own, so edits to it stop reaching the others.");
    v->addLayout(row);
    return w;
}
QWidget* arrayPanel(NodeId id)
{
    Graph& g = Graph::current();
    const scene::Node* n = g.node(id);
    if (!n || !scene::arrays::isArray(*n))
        return nullptr;
    g.evaluate();
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(8, 2, 4, 2);
    const size_t copies = scene::arrays::copyTransforms(*n).size();
    const size_t sources = scene::arrays::sources(g, id).size();
    auto* label = new QLabel(QString("%1 cop%2 of %3 source%4, generated").arg(copies).arg(copies == 1 ? "y" : "ies").arg(sources).arg(sources == 1 ? "" : "s"));
    label->setWordWrap(true);
    v->addWidget(label);
    auto* row = new QHBoxLayout;
    const std::string path = n->path();
    auto button = [&](const char* text, const char* mode, const char* tip) {
        auto* b = new QPushButton(text);
        b->setToolTip(tip);
        QObject::connect(b, &QPushButton::clicked, b, [mode, path] { runOnNode("bakeArray", path, {{"mode", mode}}); });
        row->addWidget(b);
    };
    button("Bake to Instances", "instance", "Replace the array with a group of real objects that share the sources' meshes.");
    button("Bake to Copies", "copy", "Replace the array with a group of real, independent copies.");
    v->addLayout(row);
    return w;
}
} // namespace

void registerInstancingUi()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    attributeEditor::addSection("mesh", "Instancing", [](NodeId id) { return instancingPanel(id); });
    attributeEditor::addSection("array", "Array Copies", [](NodeId id) { return arrayPanel(id); });
}
} // namespace lila::ui
