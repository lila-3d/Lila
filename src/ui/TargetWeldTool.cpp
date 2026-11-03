#include "ui/TargetWeldTool.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "geo/Bvh.h"
#include "ops/Command.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/Transform.h"
#include "ui/Actions.h"
#include "ui/ToolColors.h"
#include "ui/ViewportTool.h"
#include "ui/ViewportToolMath.h"

#include <QTimer>

#include <cmath>
#include <map>
#include <set>

namespace lila::ui
{
namespace
{
using geo::Index;
using geo::kInvalid;

constexpr double kWeldPickRadius = 14.0; // px

const glm::vec4 kSource(0.75f, 0.75f, 0.75f, 1.0f);

class TargetWeldTool final : public ViewportTool
{
public:
    const char* name() const override { return "Target Weld"; }

    void activate() override
    {
        refresh();
        log::info("Target Weld: drag from a vertex (an edge in edge mode) onto another to weld it there, Shift meets halfway, Esc exits");
    }

    std::optional<QCursor> cursor() const override { return QCursor(Qt::CrossCursor); }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        // The right button keeps its marking menus.
        if (e->button() != Qt::LeftButton)
            return false;
        refresh();
        mSource = pick(e->position(), view, std::nullopt);
        mHover.reset();
        mCursor = e->position();
        viewportTools::requestRedraw();
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        refresh();
        mCursor = e->position();
        mCursorView = view.camera;
        mHover = pick(e->position(), view, mSource);
        viewportTools::requestRedraw();
        return true;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton)
            return false;
        if (mSource)
            if (std::optional<Pick> target = pick(e->position(), view, mSource))
                weld(*mSource, *target, e->modifiers() & Qt::ShiftModifier);
        mSource.reset();
        mHover = pick(e->position(), view, std::nullopt);
        viewportTools::requestRedraw();
        return true;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        if (e->key() == Qt::Key_Shift)
            viewportTools::requestRedraw();
        if (e->key() != Qt::Key_Escape)
            return false;
        if (mSource)
            mSource.reset();
        else
            // Not from inside our own handler: set() destroys this tool.
            QTimer::singleShot(0, [] { viewportTools::set(nullptr); });
        viewportTools::requestRedraw();
        return true;
    }

    void draw(render::FrameState&, OverlayPainter& p, const ViewInfo& view) override
    {
        LILA_ZONE();

        refresh();
        const bool center = QGuiApplication::queryKeyboardModifiers() & Qt::ShiftModifier;
        auto ends = [&](const Pick& k) {
            const Target& t = mTargets.at(k.node);
            const geo::Mesh& m = *t.mesh;
            if (!edgeMode())
                return std::pair{t.world(m.position(k.id)), t.world(m.position(k.id))};
            return std::pair{t.world(m.position(m.edgePoint(k.id, 0))), t.world(m.position(m.edgePoint(k.id, 1)))};
        };
        auto mark = [&](const Pick& k, const glm::vec4& color) {
            auto [a, b] = ends(k);
            auto sa = view.project(a), sb = view.project(b);
            if (!sa || !sb)
                return;
            if (edgeMode())
            {
                p.line(*sa, *sb, 4.5f, kHandleRim);
                p.line(*sa, *sb, 2.5f, color);
            }
            else
            {
                p.disc(*sa, 6.0f, kHandleRim, 16);
                p.disc(*sa, 4.0f, color, 16);
            }
        };
        const bool valid = [&] {
            for (const std::optional<Pick>* k : {&mSource, &mHover})
                if (*k && (!mTargets.contains((*k)->node) || !mTargets.at((*k)->node).mesh))
                    return false;
            return true;
        }();
        if (valid && mSource)
        {
            mark(*mSource, kSource);
            auto [a, b] = ends(*mSource);
            const std::optional<QPointF> from = view.project((a + b) * 0.5);
            std::optional<QPointF> to = view.camera == mCursorView ? std::optional(mCursor) : std::nullopt;
            if (mHover)
            {
                auto [c, d] = ends(*mHover);
                to = view.project(center ? (a + b + c + d) * 0.25 : (c + d) * 0.5);
            }
            if (from && to)
                p.dashedLine(*from, *to, 1.5f, 6.0f, kHotColor);
        }
        if (valid && mHover && view.camera == mCursorView)
            mark(*mHover, kHotColor);
        drawToolStatus(p, view,
                       {.tool = "Target Weld",
                        .mode = edgeMode() ? "edge" : "vertex",
                        .prompt = edgeMode() ? "drag an edge onto another" : "drag a vertex onto another",
                        .keys = {{"Shift", "meet halfway"}, {"Esc", "exit"}}});
    }

private:
    struct Target
    {
        scene::MeshPtr mesh;
        glm::dmat4 matrix{1.0};
        geo::MeshBvh bvh; // world space

        glm::dvec3 world(const glm::dvec3& local) const { return glm::dvec3(matrix * glm::dvec4(local, 1.0)); }
    };

    struct Pick
    {
        scene::NodeId node = scene::kNoNode;
        Index id = kInvalid;
    };

    static bool edgeMode() { return scene::components::mode() == scene::ComponentMode::Edge; }

    void refresh()
    {
        LILA_ZONE();

        scene::Graph& g = scene::Graph::current();
        g.evaluate();
        std::set<scene::NodeId> ids(g.selection().begin(), g.selection().end());
        if (scene::components::mode() != scene::ComponentMode::Object)
            for (auto& [node, sel] : scene::components::selected(scene::components::mode()))
                ids.insert(node);
        std::erase_if(ids, [&](scene::NodeId id) {
            scene::Node* n = g.node(id);
            return !n || !n->type().isA("mesh");
        });
        if (ids.empty())
            for (scene::NodeId id : g.nodes())
                if (scene::Node* n = g.node(id); n && n->type().isA("mesh") && scene::effectivelyVisible(g, id))
                    ids.insert(id);
        std::erase_if(mTargets, [&](const auto& kv) { return !ids.contains(kv.first); });
        for (scene::NodeId id : ids)
        {
            scene::MeshPtr cage = scene::modifiers::cage(g, id);
            const glm::dmat4 matrix = g.node(id)->as<glm::dmat4>(scene::Transform::kWorldMatrix);
            Target& t = mTargets[id];
            if (cage == t.mesh && matrix == t.matrix)
                continue;
            // An undo or another edit changed the mesh: the held ids may name other components now.
            if (mSource && mSource->node == id)
                mSource.reset();
            mHover.reset();
            t.mesh = cage;
            t.matrix = matrix;
            t.bvh = geo::MeshBvh();
            if (!cage)
                continue;
            geo::Mesh w = *cage;
            for (glm::dvec3& p : w.positions())
                p = t.world(p);
            t.bvh.build(w);
        }
    }

    bool visible(const Target& t, const glm::dvec3& world, const ViewInfo& view) const
    {
        if (t.bvh.empty())
            return true;
        const glm::dvec3 d = world - view.eye;
        const double len = glm::length(d);
        // A little slack so points lying on the surface aren't hidden by it.
        const double slack = view.unitsPerPixel(world) * 3.0;
        return len <= slack || !t.bvh.occluded(view.eye, d / len, len - slack);
    }

    /** The visible vertex (or edge) nearest px; with a source, only another of the source's mesh. */
    std::optional<Pick> pick(QPointF px, const ViewInfo& view, const std::optional<Pick>& source) const
    {
        LILA_ZONE();

        std::optional<Pick> out;
        double best = kWeldPickRadius;
        for (const auto& [id, t] : mTargets)
        {
            if (!t.mesh || (source && source->node != id))
                continue;
            const geo::Mesh& m = *t.mesh;
            if (!edgeMode())
            {
                for (Index p = 0; p < m.pointCapacity(); ++p)
                {
                    if (!m.pointValid(p) || (source && source->id == p))
                        continue;
                    const glm::dvec3 w = t.world(m.position(p));
                    if (std::optional<QPointF> s = view.project(w); s && distPx(*s, px) < best && visible(t, w, view))
                    {
                        best = distPx(*s, px);
                        out = Pick{id, p};
                    }
                }
                continue;
            }
            for (Index e = 0; e < m.edgeCapacity(); ++e)
            {
                if (!m.edgeValid(e) || (source && source->id == e))
                    continue;
                const glm::dvec3 a = t.world(m.position(m.edgePoint(e, 0))), b = t.world(m.position(m.edgePoint(e, 1)));
                std::optional<QPointF> sa = view.project(a), sb = view.project(b);
                if (!sa || !sb)
                    continue;
                const double d = segmentDistPx(px, *sa, *sb);
                if (d < best && visible(t, (a + b) * 0.5, view))
                {
                    best = d;
                    out = Pick{id, e};
                }
            }
        }
        return out;
    }

    void weld(const Pick& source, const Pick& target, bool center)
    {
        scene::Node* n = scene::Graph::current().node(source.node);
        if (!n)
            return;
        try
        {
            ops::execute("polyTargetWeld",
                         {{"mesh", n->path()}, {"source", source.id}, {"target", target.id}, {"edge", edgeMode()}, {"center", center}},
                         CallSource::UI);
        }
        catch (const CommandError& e)
        {
            log::error("{}", e.what());
        }
        refresh();
    }

    std::map<scene::NodeId, Target> mTargets;
    std::optional<Pick> mSource;
    std::optional<Pick> mHover;
    QPointF mCursor;
    std::string mCursorView;
};
} // namespace

void registerTargetWeldActions()
{
    actions::add({
        .id = "mesh.targetWeld",
        .label = "Target Weld",
        .category = "Mesh",
        .run = [] { viewportTools::set(std::make_unique<TargetWeldTool>()); },
        .doc = "Drag from a vertex onto another to weld it there (edges in edge mode, end to nearest end). Shift meets halfway, Esc exits.",
        .python = "lila.ui.run_action(\"mesh.targetWeld\")",
    });
}
} // namespace lila::ui
