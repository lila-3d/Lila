// The Attribute Editor sections of live USD stages: the stage's edit layer, variants and payloads, and the composed
// prim properties of an object in a stage, editable as overs.

#include "OpenUsd.h"
#include "UsdBackend.h"

#include <lila/Command.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Node.h>
#include <lila/UI.h>

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QWidget>

namespace lila::usd
{
namespace
{
using json = nlohmann::json;
using scene::Graph;
using scene::NodeId;

QString qs(const std::string& s) { return QString::fromStdString(s); }

std::string pathOf(NodeId id)
{
    const scene::Node* n = Graph::current().node(id);
    return n ? n->path() : std::string();
}

/** Runs a command from the UI (echoed, undoable); failures go to the log rather than out of a Qt slot. */
json runCommand(const char* name, const json& call)
{
    try
    {
        return ops::execute(name, call, CallSource::UI);
    }
    catch (const std::exception& e)
    {
        log::write(LogLevel::Error, e.what());
        return json();
    }
}

/** A query the panel makes for itself, not echoed. Null when it fails. */
json query(const char* name, const std::string& object)
{
    try
    {
        Args a;
        a.positional = json::array({object});
        return runBackend(name, a);
    }
    catch (const std::exception&)
    {
        return json();
    }
}

QLabel* wrapped(const QString& text, QWidget* parent)
{
    auto* l = new QLabel(text, parent);
    l->setWordWrap(true);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return l;
}

QWidget* stagePanel(NodeId id)
{
    const std::string stage = pathOf(id);
    const json info = query("usdStageInfo", stage);
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    if (info.is_null())
    {
        v->addWidget(wrapped("The stage could not be opened; see the log.", w));
        return w;
    }
    const json& edit = info["editLayer"];
    const std::string mode = edit.value("mode", "embedded");
    v->addWidget(wrapped(mode == "embedded" ? QString("Edits are kept inside the scene.")
                                            : qs((mode == "sublayer" ? "Edits go straight into its sublayer " : "Edits go to ") +
                                                 edit.value("path", std::string())),
                         w));

    auto* form = new QFormLayout;
    for (const json& vs : info["variants"])
    {
        const std::string prim = vs["prim"].get<std::string>(), set = vs["set"].get<std::string>();
        auto* box = new QComboBox(w);
        for (const json& name : vs["variants"])
            box->addItem(qs(name.get<std::string>()));
        box->setCurrentText(qs(vs["selection"].get<std::string>()));
        box->setToolTip(qs(prim));
        QObject::connect(box, &QComboBox::textActivated, w, [stage, prim, set](const QString& variant) {
            runCommand("usdStageVariant", {{"args", {stage}}, {"prim", prim}, {"set", set}, {"variant", variant.toStdString()}});
        });
        const std::string leaf = prim.substr(prim.find_last_of("/}") + 1);
        const std::string level = vs.value("level", std::string());
        form->addRow(qs(leaf + (level.empty() ? "" : " (" + level + ")") + " " + set), box);
    }
    for (const json& p : info["payloads"])
    {
        const std::string prim = p["prim"].get<std::string>();
        auto* check = new QCheckBox(qs(prim), w);
        check->setChecked(p["loaded"].get<bool>());
        QObject::connect(check, &QCheckBox::toggled, w, [stage, prim](bool on) {
            runCommand("usdStageLoad", {{"args", {stage}}, {"prim", prim}, {"load", on}});
        });
        form->addRow("Payload", check);
    }
    v->addLayout(form);

    auto* row = new QHBoxLayout;
    auto button = [&](const char* label, const char* tip, std::function<void()> fn) {
        auto* b = new QPushButton(label, w);
        b->setToolTip(tip);
        QObject::connect(b, &QPushButton::clicked, w, std::move(fn));
        row->addWidget(b);
    };
    button("Reload", "Read the stage's files again, keeping its edits.", [stage] { runCommand("usdStageReload", {{"args", {stage}}}); });
    button("Write Shot...", "Write the edits as a .usda that sublayers the source, for Houdini or Maya.", [stage, w] {
        const QString file = QFileDialog::getSaveFileName(w, "Write Shot", QString(), "USD (*.usda *.usd)");
        if (!file.isEmpty())
            runCommand("usdStageWriteShot", {{"args", {file.toStdString(), stage}}});
    });
    row->addStretch(1);
    v->addLayout(row);

    std::string warnings;
    for (const json& s : info["warnings"])
        warnings += (warnings.empty() ? "" : "\n") + s.get<std::string>();
    if (!warnings.empty())
        v->addWidget(wrapped(qs("At the last save:\n" + warnings), w));
    return w;
}

QWidget* primPanel(NodeId id)
{
    if (isStageNode(id) || !stageAbove(id))
        return nullptr;
    const std::string object = pathOf(id);
    const json info = query("usdStagePrim", object);
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    if (info.is_null())
    {
        v->addWidget(wrapped("Not linked to a prim. New objects join the stage when the scene saves.", w));
        return w;
    }
    std::string schemas;
    for (const json& s : info["schemas"])
        schemas += (schemas.empty() ? "" : ", ") + s.get<std::string>();
    const std::string level = info.value("level", std::string());
    v->addWidget(wrapped(qs(info["prim"].get<std::string>() + (level.empty() ? "" : " in " + level) + "  (" + info["type"].get<std::string>() +
                            (schemas.empty() ? "" : "; " + schemas) + ")"),
                         w));
    const bool locked = info.value("instanceProxy", false);
    if (locked)
        v->addWidget(wrapped("Inside an instance: its properties can't be edited here.", w));

    auto* form = new QFormLayout;
    for (const json& p : info["properties"])
    {
        const std::string name = p["name"].get<std::string>(), type = p.value("type", std::string());
        // Schema properties nobody authored and that hold nothing would only be empty rows.
        if (type == "rel" ? p["targets"].empty() : p["value"].is_null() && !p.value("authored", false))
            continue;
        auto* edit = new QLineEdit(w);
        std::string value;
        if (type == "rel")
        {
            for (const json& t : p["targets"])
                value += (value.empty() ? "" : ", ") + t.get<std::string>();
        }
        else if (p["value"].is_string())
            value = p["value"].get<std::string>();
        edit->setText(qs(value));
        edit->setCursorPosition(0);
        edit->setToolTip(qs(type + (p.value("custom", false) ? " (custom)" : "") +
                            (p.value("timeSamples", 0) > 0 ? ", time sampled: edits set a sample at the current time" : "")));
        edit->setReadOnly(locked || p.value("truncated", false));
        QObject::connect(edit, &QLineEdit::editingFinished, w, [edit, object, name, value] {
            const std::string now = edit->text().toStdString();
            if (now == value || edit->isReadOnly())
                return;
            const json r = runCommand("usdStageSetProperty", {{"args", {object, name, now}}});
            if (r.is_object() && r.contains("value") && r["value"].is_string())
            {
                const QSignalBlocker block(edit);
                edit->setText(qs(r["value"].get<std::string>()));
            }
        });
        auto* label = new QLabel(qs(name), w);
        label->setToolTip(edit->toolTip());
        form->addRow(label, edit);
    }
    v->addLayout(form);
    return w;
}
} // namespace

void registerUi(ext::Context& ctx)
{
    ui::attributeEditor::addSection(ctx, kStageType, "USD Stage", [](NodeId id) { return stagePanel(id); });
    ui::attributeEditor::addSection(ctx, "transform", "USD Prim", [](NodeId id) { return primPanel(id); });
}
} // namespace lila::usd
