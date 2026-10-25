#include "ui/ConformTool.h"

#include "core/Log.h"
#include "curve/CurveNodes.h"
#include "curve/Tessellate.h"
#include "ops/Command.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/Transform.h"
#include "ui/Actions.h"
#include "ui/SurfaceSnap.h"
#include "ui/ViewportTool.h"

#include <QTimer>

#include <algorithm>
#include <cmath>

namespace lila::ui
{
namespace
{
constexpr const char* kConformCommand = "polyConformToObject";

/** Meshes whose components are being conformed, which a click must not pick as the target. */
std::vector<scene::NodeId> editedMeshes()
{
    std::vector<scene::NodeId> out;
    for (auto& [node, ids] : scene::components::selected(scene::components::mode()))
        out.push_back(node);
    return out;
}

bool hasImplicitTarget()
{
    const auto& edited = scene::components::selected(scene::components::mode());
    scene::Graph& g = scene::Graph::current();
    for (scene::NodeId id : g.selection())
        if (scene::Node* n = g.node(id); n && (n->type().isA("mesh") || n->type().isA("curve")) && !edited.contains(id))
            return true;
    return false;
}

/** The visible curve drawn within a few pixels of the cursor. */
scene::Node* curveUnder(QPointF px, const ViewInfo& view)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::Node* best = nullptr;
    double bestD = 8.0;
    for (scene::NodeId id : g.nodes())
    {
        scene::Node* n = g.node(id);
        if (!n || !n->type().isA("curve") || !scene::effectivelyVisible(g, id))
            continue;
        for (const curve::Spline& s : curve::splinesOf(n->value(curve::CurveShape::kWorldSpace)))
        {
            curve::Polyline pl = curve::tessellate(s);
            size_t segs = pl.closed ? pl.size() : (pl.size() ? pl.size() - 1 : 0);
            for (size_t i = 0; i < segs; ++i)
            {
                auto a = view.project(pl.points[i]), b = view.project(pl.points[(i + 1) % pl.size()]);
                if (!a || !b)
                    continue;
                QPointF d = *b - *a;
                double len2 = QPointF::dotProduct(d, d);
                double t = len2 > 0 ? std::clamp(QPointF::dotProduct(px - *a, d) / len2, 0.0, 1.0) : 0.0;
                QPointF q = *a + d * t - px;
                if (double dist = std::hypot(q.x(), q.y()); dist < bestD)
                {
                    bestD = dist;
                    best = n;
                }
            }
        }
    }
    return best;
}

scene::Node* targetUnder(QPointF px, const ViewInfo& view)
{
    if (scene::Node* c = curveUnder(px, view))
        return c;
    std::optional<surfaceSnap::Hit> hit = surfaceSnap::underCursor(px, view, editedMeshes());
    return hit ? scene::Graph::current().node(hit->node) : nullptr;
}

class ConformPickTool final : public ViewportTool
{
public:
    const char* name() const override { return "Conform to Object"; }

    void activate() override { log::info("Conform to Object: click the mesh or curve to conform the selection to, Esc cancels"); }

    std::optional<QCursor> cursor() const override { return QCursor(Qt::PointingHandCursor); }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton)
            return false;
        scene::Node* n = targetUnder(e->position(), view);
        if (!n)
        {
            log::warn("Conform to Object: no mesh or curve there to conform to");
            return true;
        }
        actions::runWithOptions(kConformCommand, {{"target", n->path()}});
        // Not from inside our own handler: set() destroys this tool.
        QTimer::singleShot(0, [] { viewportTools::set(nullptr); });
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        scene::Node* n = targetUnder(e->position(), view);
        QString hover = n ? QString::fromStdString(n->path()) : QString();
        if (hover != mHover)
        {
            mHover = hover;
            viewportTools::requestRedraw();
        }
        return false;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        if (e->key() != Qt::Key_Escape)
            return false;
        QTimer::singleShot(0, [] { viewportTools::set(nullptr); });
        return true;
    }

    void draw(render::FrameState&, OverlayPainter& p, const ViewInfo& view) override
    {
        drawToolMessage(p, view, mHover.isEmpty() ? QString("Conform to Object: click the mesh or curve to conform the selection to, Esc: cancel")
                                                  : QString("Conform to %1").arg(mHover));
    }

private:
    QString mHover;
};

void add(std::string id, std::string label, const char* command, json call, std::string doc)
{
    std::string py = ops::toPython(command, call);
    actions::add({.id = std::move(id), .label = std::move(label), .category = "Mesh", .doc = std::move(doc), .python = py, .command = command,
                  .call = std::move(call)});
}
} // namespace

void registerConformActions()
{
    add("mesh.straighten", "Straighten", "polyStraighten", json::object(),
        "Lines up the selected edge chains (or chains of vertices) between their ends; separate chains straighten separately.");
    add("mesh.circularize", "Circularize", "polyCircularize", json::object(),
        "Makes the selected edge loops, face borders or vertices round, in their best-fit plane. A rectangle of faces becomes a disc, "
        "ready to extrude into a pipe.");
    add("mesh.spherize", "Spherize", "polySpherize", json::object(), "Pushes the selected components onto a sphere fitted to them.");
    add("mesh.flatten", "Flatten", "polyConformShape", {{"shape", "plane"}}, "Moves the selected components onto their best-fit plane.");
    add("mesh.conformShape", "Conform to Shape", "polyConformShape", json::object(),
        "Fits the selected components to a line, circle, square, rectangle, sphere, cylinder or plane; pick the shape in the option box.");
    actions::add({
        .id = "mesh.conformToObject",
        .label = "Conform to Object",
        .category = "Mesh",
        .run =
            [] {
                if (hasImplicitTarget())
                    actions::runWithOptions(kConformCommand, json::object());
                else
                    viewportTools::set(std::make_unique<ConformPickTool>());
            },
        .doc = "Snaps the selected loop onto the nearest edge loop of another mesh, one vertex to each (or onto its surface, in the option "
               "box), or onto a curve. Uses the other selected mesh or curve, or click one.",
        .python = "lila.ui.run_action(\"mesh.conformToObject\")",
        .command = kConformCommand,
        .call = json::object(),
    });
}
} // namespace lila::ui
