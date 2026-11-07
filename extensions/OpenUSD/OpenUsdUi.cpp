// The Attribute Editor sections of live USD stages: the stage's edit layer, variants and payloads, and the composed
// prim properties of an object in a stage, editable as overs.

#include "OpenUsd.h"
#include "UsdBackend.h"
#include "lila/FileDialog.h"

#include <lila/Command.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Node.h>
#include <lila/UI.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStyle>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <filesystem>

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
json query(const char* name, const std::string& object, const std::string& property = {})
{
    try
    {
        Args a;
        a.positional = json::array({object});
        if (!property.empty())
            a.positional.push_back(property);
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
    for (const json& layer : info["addedLayers"])
    {
        const std::string file = layer.get<std::string>();
        auto* remove = new QPushButton("Remove", w);
        remove->setToolTip("Take the layer out of the stage; the file stays.");
        QObject::connect(remove, &QPushButton::clicked, w, [stage, file] { runCommand("usdStageRemoveLayer", {{"args", {file, stage}}}); });
        auto* label = new QLabel(qs(std::filesystem::path(file).filename().string()), w);
        label->setToolTip(qs(file));
        auto* line = new QHBoxLayout;
        line->addWidget(label, 1);
        line->addWidget(remove);
        form->addRow("Layer", line);
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
    button("Add Layer...", "Add a layer under the edits and over the source: a new .usda, or a layer you have.", [stage, w] {
        const QString file = ui::fileDialog::getSaveFileName(w, "Add Layer", QString(), "USD (*.usda *.usd *.usdc)");
        if (!file.isEmpty())
            runCommand("usdStageAddLayer", {{"args", {file.toStdString(), stage}}});
    });
    button("Write Shot...", "Write the edits as a .usda that sublayers the source, for Houdini or Maya.", [stage, w] {
        const QString file = ui::fileDialog::getSaveFileName(w, "Write Shot", QString(), "USD (*.usda *.usd)");
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

QString fileName(const std::string& file) { return qs(std::filesystem::path(file).filename().string()); }

QString opinionLayer(const json& o)
{
    const std::string role = o.value("role", std::string());
    QString text = fileName(o["layer"].get<std::string>());
    if (role == "edits")
        text += " (Lila edits)";
    else if (role == "added")
        text += " (added)";
    else if (role == "session")
        text += " (session)";
    return text;
}

QString opinionValue(const json& o)
{
    if (o.contains("targets"))
        return qs(o["targets"].get<std::string>());
    QString v = o.contains("value") ? qs(o["value"].get<std::string>()) : QString();
    if (const int n = o.value("timeSamples", 0); n > 0)
        v += (v.isEmpty() ? "" : ", ") + QString("%1 time samples").arg(n);
    return v;
}

QString specValue(const json& o)
{
    QStringList parts{qs(o["specifier"].get<std::string>())};
    if (o.contains("type"))
        parts << qs(o["type"].get<std::string>());
    if (o.contains("active"))
        parts << (o["active"].get<bool>() ? "active" : "inactive");
    if (o.contains("kind"))
        parts << "kind " + qs(o["kind"].get<std::string>());
    if (o.contains("instanceable"))
        parts << (o["instanceable"].get<bool>() ? "instanceable" : "not instanceable");
    if (o.contains("variantSelections"))
        for (const auto& [set, choice] : o["variantSelections"].items())
            parts << qs(set + " = " + choice.get<std::string>());
    return parts.join(", ");
}

void opinionItem(QTreeWidgetItem* parent, const json& o, const QString& value)
{
    auto* item = new QTreeWidgetItem(parent, {opinionLayer(o), qs(o["arc"].get<std::string>()), value, qs(o["path"].get<std::string>())});
    item->setToolTip(0, qs(o["layer"].get<std::string>()));
    item->setToolTip(2, value);
    if (o.value("wins", false))
    {
        QFont bold = item->font(0);
        bold.setBold(true);
        for (int c = 0; c < 4; ++c)
            item->setFont(c, bold);
    }
}

/** Every opinion on an object's prim, or on one of its properties, strongest first. */
void showOpinions(QWidget* from, const std::string& object, const std::string& property)
{
    const json info = query("usdStageOpinions", object, property);
    if (info.is_null())
        return;
    auto* dialog = new QDialog(from->window());
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(qs("Opinions: " + info["prim"].get<std::string>() + (property.empty() ? "" : "." + property)));
    auto* v = new QVBoxLayout(dialog);
    v->addWidget(wrapped("Strongest first. The value comes from the opinion in bold.", dialog));
    auto* tree = new QTreeWidget(dialog);
    tree->setHeaderLabels({"Layer", "Arc", "Opinion", "Path"});
    tree->setRootIsDecorated(property.empty());
    tree->setUniformRowHeights(true);
    QTreeWidgetItem* root = tree->invisibleRootItem();
    if (property.empty())
    {
        auto* prim = new QTreeWidgetItem(root, {"Prim"});
        for (const json& o : info["specs"])
            opinionItem(prim, o, specValue(o));
        prim->setExpanded(true);
    }
    for (const json& p : info["properties"])
    {
        if (p["opinions"].empty() && property.empty())
            continue;
        QTreeWidgetItem* parent = root;
        if (property.empty())
        {
            parent = new QTreeWidgetItem(root, {qs(p["name"].get<std::string>())});
            parent->setExpanded(p.contains("losesTo"));
        }
        for (const json& o : p["opinions"])
            opinionItem(parent, o, opinionValue(o));
        QString note;
        if (p.value("fallback", false))
            note = "No layer sets a value: the schema's default shows.";
        else if (p.contains("losesTo"))
            note = "Lila's edit is hidden by the stronger opinion in " + fileName(p["losesTo"].get<std::string>()) + ".";
        if (note.isEmpty())
            continue;
        if (property.empty())
            parent->setText(2, note);
        else
            v->addWidget(wrapped(note, dialog));
    }
    for (int c = 0; c < 3; ++c)
        tree->resizeColumnToContents(c);
    v->addWidget(tree, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    v->addWidget(buttons);
    dialog->resize(820, 420);
    dialog->show();
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
    auto* opinions = new QPushButton("Opinions...", w);
    opinions->setToolTip("Every layer's opinion on this prim and its properties, strongest first.");
    QObject::connect(opinions, &QPushButton::clicked, w, [w, object] { showOpinions(w, object, {}); });
    auto* top = new QHBoxLayout;
    top->addWidget(opinions);
    top->addStretch(1);
    v->addLayout(top);

    auto* form = new QFormLayout;
    const scene::Node* n = scene::Graph::current().node(id);
    for (const json& p : info["properties"])
    {
        const std::string name = p["name"].get<std::string>(), type = p.value("type", std::string());
        // Schema properties nobody authored and that hold nothing would only be empty rows.
        if (type == "rel" ? p["targets"].empty() : p["value"].is_null() && !p.value("authored", false))
            continue;
        // Components are the object's own attributes, each schema a section of the Attributes tab.
        if (const int a = n ? n->type().attrIndex(name) : -1; a >= 0 && (n->type().attrs[size_t(a)].flags & scene::AttrUser))
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
        auto* stack = new QToolButton(w);
        stack->setAutoRaise(true);
        stack->setText("...");
        stack->setToolTip("The opinions on " + qs(name) + ", strongest first.");
        if (p.contains("losesTo"))
        {
            const QString why = "Lila's edit doesn't show: " + fileName(p["losesTo"].get<std::string>()) + " has a stronger opinion.";
            stack->setIcon(w->style()->standardIcon(QStyle::SP_MessageBoxWarning));
            stack->setToolTip(why);
            edit->setToolTip(why + "\n" + edit->toolTip());
        }
        QObject::connect(stack, &QToolButton::clicked, w, [w, object, name] { showOpinions(w, object, name); });
        auto* field = new QHBoxLayout;
        field->addWidget(edit, 1);
        field->addWidget(stack);
        auto* label = new QLabel(qs(name), w);
        label->setToolTip(edit->toolTip());
        form->addRow(label, field);
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
