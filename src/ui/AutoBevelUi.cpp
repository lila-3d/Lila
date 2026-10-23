#include "ui/AutoBevelUi.h"

#include "ops/Command.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "scene/AutoBevel.h"
#include "scene/Graph.h"
#include "scene/Modifiers.h"
#include "scene/Transform.h"
#include "ui/Actions.h"
#include "ui/AttributeEditor.h"
#include "ui/ViewportHooks.h"
#include "ui/ViewportTool.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>

namespace lila::ui
{
namespace
{
using scene::Graph;
using scene::NodeId;

const glm::vec4 kEdgeColor(0.8f, 0.8f, 0.8f, 1.0f);

void run(const char* name, json call = json::object())
{
    try
    {
        ops::execute(name, call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

void drawBevelEdges(render::FrameState& s, OverlayPainter&, const ViewInfo& view)
{
    LILA_ZONE();

    Graph& g = Graph::current();
    const std::vector<NodeId>& sel = g.selection();
    for (NodeId id : g.nodes())
    {
        scene::Node* n = g.node(id);
        if (!n || !scene::autoBevel::isAutoBevel(*n) || scene::autoBevel::editing(*n))
            continue;
        bool picked = std::find(sel.begin(), sel.end(), id) != sel.end();
        if (!picked && !n->as<bool>(n->type().attrIndex("showEdges")))
            continue;
        NodeId meshId = scene::autoBevel::meshOf(g, id);
        scene::Node* mesh = g.node(meshId);
        if (!mesh || !scene::effectivelyVisible(g, meshId))
            continue;
        auto set = scene::autoBevel::chosen(g, id);
        if (!set || !set->mesh)
            continue;
        const geo::Mesh& m = *set->mesh;
        glm::dmat4 world = mesh->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        for (geo::Index e : set->edges)
        {
            if (!m.edgeValid(e))
                continue;
            glm::dvec3 a = glm::dvec3(world * glm::dvec4(m.position(m.edgePoint(e, 0)), 1.0));
            glm::dvec3 b = glm::dvec3(world * glm::dvec4(m.position(m.edgePoint(e, 1)), 1.0));
            glm::dvec3 mid = (a + b) * 0.5;
            glm::dvec3 side = glm::cross(b - a, view.viewDir(mid));
            double len = glm::length(side);
            // Three lines a pixel apart read as one thick one.
            glm::dvec3 step = len > 0.0 ? side / len * view.unitsPerPixel(mid) : glm::dvec3(0.0);
            for (double k : {-1.0, 0.0, 1.0})
            {
                s.linesOnTop.push_back({glm::vec3(a + step * k), kEdgeColor});
                s.linesOnTop.push_back({glm::vec3(b + step * k), kEdgeColor});
            }
        }
    }
}

QPushButton* button(const QString& text, const QString& tip, std::function<void()> fn)
{
    auto* b = new QPushButton(text);
    b->setToolTip(tip);
    QObject::connect(b, &QPushButton::clicked, b, std::move(fn));
    return b;
}

/** Status line and Edit Edges buttons for one modifier, kept current by a graph listener. */
QWidget* modifierPanel(NodeId mod, bool withSelect)
{
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(8, 2, 4, 2);
    v->setSpacing(3);
    auto* status = new QLabel;
    status->setStyleSheet("color: #9a9a9a;");
    v->addWidget(status);

    auto name = [mod] {
        scene::Node* n = Graph::current().node(mod);
        return n ? n->name() : std::string();
    };
    auto call = [name](const char* cmd) { return [name, cmd] { run(cmd, {{"args", {name()}}}); }; };

    auto* row1 = new QHBoxLayout;
    auto* edit = button("Edit Edges", "Show the input mesh with these edges selected; change them with normal selection.", call("bevelEditEdges"));
    auto* apply = button("Apply", "Store the selected edges and finish editing.", call("bevelApplyEdges"));
    auto* cancel = button("Cancel", "Finish editing without changing the edges.", call("bevelCancelEdges"));
    row1->addWidget(edit);
    row1->addWidget(apply);
    row1->addWidget(cancel);
    v->addLayout(row1);
    auto* row2 = new QHBoxLayout;
    row2->addWidget(button("Measure Thin", "Set thin to 1.5 times the selected edge's length.", call("bevelMeasureThin")));
    auto* clear = button("Clear Overrides", "Drop the hand-picked edges.", call("bevelClearOverrides"));
    row2->addWidget(clear);
    if (withSelect)
        row2->addWidget(button("Select", "Select the modifier to edit its parameters and see its edges.", [name] {
            run("select", {{"args", {name()}}});
        }));
    v->addLayout(row2);

    QPointer<QWidget> guard(w);
    auto refresh = [guard, status, edit, apply, cancel, clear, mod] {
        if (!guard)
            return;
        Graph& g = Graph::current();
        scene::Node* n = g.node(mod);
        if (!n)
            return;
        auto set = scene::autoBevel::chosen(g, mod);
        bool editing = scene::autoBevel::editing(*n);
        QString text = QString("%1 edges").arg(set ? int(set->edges.size()) : 0);
        if (set && set->lost > 0)
            text += QString(", %1 hand-picked edges lost").arg(set->lost);
        if (editing)
            text += ", editing";
        status->setText(text);
        edit->setEnabled(!editing);
        apply->setEnabled(editing);
        cancel->setEnabled(editing);
        clear->setEnabled(!editing);
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

QWidget* meshPanel(NodeId mesh)
{
    Graph& g = Graph::current();
    std::vector<NodeId> mods;
    for (NodeId id : scene::modifiers::stack(g, mesh))
        if (scene::autoBevel::isAutoBevel(*g.node(id)))
            mods.push_back(id);
    if (mods.empty())
        return nullptr;
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    for (NodeId id : mods)
    {
        auto* title = new QLabel(QString::fromStdString(g.node(id)->name()));
        title->setContentsMargins(8, 2, 4, 0);
        v->addWidget(title);
        v->addWidget(modifierPanel(id, true));
    }
    return w;
}

void registerActions()
{
    auto add = [](const char* id, const char* label, const char* cmd, const char* doc) {
        actions::add({.id = id,
                      .label = label,
                      .category = "Modifiers",
                      .run = [cmd] { run(cmd); },
                      .doc = doc,
                      .python = std::string(cmd) + "()"});
    };
    add("modifier.autoBevel", "Auto Bevel", "autoBevel", scene::modifiers::kindDoc("autoBevel").c_str());
    add("select.foldEdges", "Select Fold Edges", "selectFoldEdges", "Select fold edges by the auto bevel rule.");
    add("bevel.editEdges", "Edit Bevel Edges", "bevelEditEdges", "Edit the selected auto bevel's edges with normal selection.");
    add("bevel.applyEdges", "Apply Bevel Edges", "bevelApplyEdges", "Store the selected edges in the auto bevel being edited.");
    add("bevel.cancelEdges", "Cancel Bevel Edges", "bevelCancelEdges", "Stop editing auto bevel edges without changes.");
    add("bevel.measureThin", "Measure Bevel Thin", "bevelMeasureThin", "Set the auto bevel's thin from the selected edge.");
    add("bevel.clearOverrides", "Clear Bevel Overrides", "bevelClearOverrides", "Drop the auto bevel's hand-picked edges.");
    actions::add({.id = "bevel.showEdges",
                  .label = "Show Bevel Edges",
                  .category = "Modifiers",
                  .run =
                      [] {
                          Graph& g = Graph::current();
                          json names = json::array();
                          bool on = true;
                          for (const std::string& name : Args{}.objects())
                          {
                              scene::Node* n = g.find(name);
                              if (n && scene::autoBevel::isAutoBevel(*n))
                              {
                                  names.push_back(name);
                                  on = !n->as<bool>(n->type().attrIndex("showEdges"));
                              }
                          }
                          if (names.empty())
                          {
                              log::error("Show Bevel Edges: select an autoBevel modifier");
                              return;
                          }
                          for (const json& name : names)
                              run("setAttr", {{"args", {name.get<std::string>() + ".showEdges", on}}});
                          viewportTools::requestRedraw();
                      },
                  .doc = "Toggle the viewport highlight of the selected auto bevel modifiers' edges."});
}
} // namespace

void registerAutoBevelUi()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    registerActions();
    viewportHooks::addDrawer(drawBevelEdges);
    attributeEditor::addSection("autoBevelModifier", "Bevel Edges", [](NodeId id) { return modifierPanel(id, false); });
    attributeEditor::addSection("mesh", "Auto Bevel", [](NodeId id) { return meshPanel(id); });
}
} // namespace lila::ui
