#include "ui/SplitRingsUi.h"

#include "ops/Command.h"
#include "core/Log.h"
#include "scene/Graph.h"
#include "scene/SplitRingsModifier.h"
#include "ui/Actions.h"
#include "ui/AttributeEditor.h"
#include "ui/UiCommands.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <memory>

namespace lila::ui
{
namespace
{
using scene::Graph;
using scene::NodeId;

QPushButton* panelButton(const QString& text, const QString& tip, std::function<void()> fn)
{
    auto* b = new QPushButton(text);
    b->setToolTip(tip);
    QObject::connect(b, &QPushButton::clicked, b, std::move(fn));
    return b;
}

/** Status line and Edit Selection buttons for one modifier, kept current by a graph listener. */
QWidget* selectionPanel(NodeId mod)
{
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(8, 2, 4, 2);
    v->setSpacing(3);
    auto* status = new QLabel;
    status->setStyleSheet("color: #9a9a9a;");
    v->addWidget(status);

    auto call = [mod](const char* cmd) {
        return [mod, cmd] {
            if (scene::Node* n = Graph::current().node(mod))
                runUi(cmd, {{"args", {n->path()}}});
        };
    };
    auto* row = new QHBoxLayout;
    auto* edit = panelButton("Edit Selection", "Show the input mesh with these edges or faces selected; change them with normal selection.",
                             call("splitRingsEditSelection"));
    auto* apply = panelButton("Apply", "Store the selected edges or faces and finish editing.", call("splitRingsApplySelection"));
    auto* cancel = panelButton("Cancel", "Finish editing without changing what is split.", call("splitRingsCancelSelection"));
    row->addWidget(edit);
    row->addWidget(apply);
    row->addWidget(cancel);
    v->addLayout(row);

    QPointer<QWidget> guard(w);
    auto refresh = [guard, status, edit, apply, cancel, mod] {
        if (!guard)
            return;
        Graph& g = Graph::current();
        scene::Node* n = g.node(mod);
        if (!n)
            return;
        const bool editing = scene::splitRingsModifier::editing(*n);
        const scene::splitRingsModifier::Selection sel = scene::splitRingsModifier::selection(g, mod);
        QString text = sel.whole ? QString("Whole mesh")
                                 : QString("%1 %2").arg(sel.ids.size()).arg(sel.kind == scene::ComponentMode::Edge ? "edges" : "faces");
        if (sel.lost > 0)
            text += QString(", %1 lost").arg(sel.lost);
        if (editing)
            text += ", editing";
        status->setText(text);
        edit->setEnabled(!editing);
        apply->setEnabled(editing);
        cancel->setEnabled(editing);
    };
    refresh();
    // Deferred: listeners fire mid-edit, when the graph must not be evaluated.
    auto queued = std::make_shared<bool>(false);
    int listener = Graph::current().addListener([w, refresh, queued](scene::Change, NodeId) {
        if (*queued)
            return;
        *queued = true;
        QTimer::singleShot(0, w, [refresh, queued] {
            *queued = false;
            refresh();
        });
    });
    QObject::connect(w, &QObject::destroyed, [listener] { Graph::current().removeListener(listener); });
    return w;
}
} // namespace

void registerSplitRingsUi()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    auto add = [](const char* id, const char* label, const char* cmd, const char* doc) {
        actions::add({.id = id, .label = label, .category = "Modifiers", .run = [cmd] { runUi(cmd); }, .doc = doc,
                      .python = std::string(cmd) + "()"});
    };
    add("splitRings.editSelection", "Edit Split Rings Selection", "splitRingsEditSelection",
        "Edit the selected split rings modifier's edges or faces with normal selection.");
    add("splitRings.applySelection", "Apply Split Rings Selection", "splitRingsApplySelection",
        "Store the selected edges or faces in the split rings modifier being edited.");
    add("splitRings.cancelSelection", "Cancel Split Rings Selection", "splitRingsCancelSelection",
        "Stop editing the split rings selection without changes.");
    attributeEditor::addSection("splitRingsModifier", "Split Selection", [](NodeId id) { return selectionPanel(id); });
}
} // namespace lila::ui
