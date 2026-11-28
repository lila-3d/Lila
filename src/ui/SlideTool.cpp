#include "ui/SlideTool.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "geo/Topology.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/Modifiers.h"
#include "scene/Transform.h"
#include "ui/Actions.h"
#include "ui/ViewportTool.h"
#include "ui/ViewportToolMath.h"

#include <QTimer>

#include <cmath>
#include <format>

namespace lila::ui
{
namespace
{
using geo::Index;
using geo::kInvalid;
using scene::ComponentMode;

constexpr double kGrabRadius = 10.0; // px
constexpr double kDirectionPx = 4.0; // drag before a vertex slide picks its edges

const glm::vec4 kRail(0.6f, 0.6f, 0.6f, 0.8f);

class SlideTool final : public ViewportTool
{
public:
    const char* name() const override { return "Slide"; }

    void activate() override { log::info("Slide: middle-drag, or drag a selected edge or vertex, to slide it along the surface; Shift slides evenly, Esc exits"); }

    void deactivate() override
    {
        if (mDrag)
            cancel();
    }

    std::optional<QCursor> cursor() const override { return mDrag ? std::optional(QCursor(Qt::SizeAllCursor)) : std::nullopt; }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (mDrag || !slidable())
            return false;
        // Left presses select as usual unless they land on the selection itself.
        if (e->button() == Qt::LeftButton ? !overSelection(e->position(), view) : e->button() != Qt::MiddleButton)
            return false;
        begin(e->position());
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        if (!mDrag)
            return false;
        mCursor = e->position();
        update(view, e->modifiers() & Qt::ShiftModifier);
        return true;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo&) override
    {
        if (!mDrag || (e->button() != Qt::LeftButton && e->button() != Qt::MiddleButton))
            return false;
        commit();
        return true;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo& view) override
    {
        if (e->key() == Qt::Key_Shift && mDrag)
        {
            update(view, true);
            return true;
        }
        if (e->key() != Qt::Key_Escape)
            return false;
        if (mDrag)
            cancel();
        else
            // Not from inside our own handler: set() destroys this tool.
            QTimer::singleShot(0, [] { viewportTools::set(nullptr); });
        viewportTools::requestRedraw();
        return true;
    }

    void draw(render::FrameState&, OverlayPainter& p, const ViewInfo& view) override
    {
        LILA_ZONE();

        const bool vertex = scene::components::mode() == ComponentMode::Vertex;
        if (mDrag && mDrag->hasRails)
            for (const Piece& piece : mDrag->pieces)
            {
                const geo::Mesh& m = *piece.base;
                for (size_t k = 0; k < piece.rails.points.size(); ++k)
                {
                    const glm::dvec3 at = piece.worldOf(m.position(piece.rails.points[k]));
                    for (Index to : piece.rails.ends[k])
                        if (to != kInvalid)
                            if (auto a = view.project(at), b = view.project(piece.worldOf(m.position(to))); a && b)
                                p.line(*a, *b, 1.5f, kRail);
                }
            }
        QString prompt = !slidable() ? QString("select edges, faces or vertices") : mDrag ? QString("release to apply") : QString("middle-drag or drag the selection");
        std::vector<ToolField> fields;
        if (mDrag)
            fields.push_back({"amount", QString::fromStdString(std::format("{:.2f}", mDrag->amount))});
        drawToolStatus(p, view,
                       {.tool = "Slide",
                        .mode = vertex ? "vertex" : "edge",
                        .fields = fields,
                        .flags = {{"even", mDrag ? mDrag->even : bool(QGuiApplication::queryKeyboardModifiers() & Qt::ShiftModifier)}},
                        .prompt = prompt,
                        .keys = {{"MMB drag", "slide"}, {"Shift", "even"}, {"Esc", mDrag ? "cancel" : "exit"}}});
    }

private:
    /** One mesh's part of the slide: its base mesh at the press and the rails the command will follow. */
    struct Piece
    {
        scene::NodeId id = scene::kNoNode;
        scene::PlugRef plug;
        scene::PlugValue saved;
        scene::MeshPtr base;
        glm::dmat4 world{1.0};
        ComponentMode kind = ComponentMode::Edge;
        scene::components::Ids ids;
        geo::SlideRails rails;

        glm::dvec3 worldOf(const glm::dvec3& local) const { return glm::dvec3(world * glm::dvec4(local, 1.0)); }
    };

    struct Drag
    {
        QPointF press;
        std::vector<Piece> pieces;
        std::optional<glm::dvec3> direction;
        bool hasRails = false;
        double amount = 0.0;
        bool even = false;
    };

    static bool slidable()
    {
        const ComponentMode kind = scene::components::mode();
        if (kind == ComponentMode::Object)
            return false;
        for (auto& [node, ids] : scene::components::selected(kind))
            if (!ids.empty())
                return true;
        return false;
    }

    /** Whether px is on a selected vertex or edge (a face selection's border) of a mesh. */
    bool overSelection(QPointF px, const ViewInfo& view) const
    {
        scene::Graph& g = scene::Graph::current();
        const ComponentMode kind = scene::components::mode();
        for (auto& [node, ids] : scene::components::selected(kind))
        {
            scene::Node* n = g.node(node);
            scene::MeshPtr cage = n && n->type().isA("mesh") ? scene::modifiers::cage(g, node) : nullptr;
            if (!cage)
                continue;
            const geo::Mesh& m = *cage;
            const glm::dmat4 world = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
            auto at = [&](Index p) { return view.project(glm::dvec3(world * glm::dvec4(m.position(p), 1.0))); };
            if (kind == ComponentMode::Vertex)
            {
                for (Index p : ids)
                    if (m.pointValid(p))
                        if (auto s = at(p); s && distPx(*s, px) < kGrabRadius)
                            return true;
                continue;
            }
            const scene::components::Ids edges = kind == ComponentMode::Face ? geo::regionBoundaryEdges(m, ids) : ids;
            for (Index e : edges)
                if (m.edgeValid(e))
                    if (auto a = at(m.edgePoint(e, 0)), b = at(m.edgePoint(e, 1)); a && b && segmentDistPx(px, *a, *b) < kGrabRadius)
                        return true;
        }
        return false;
    }

    void begin(QPointF px)
    {
        scene::Graph& g = scene::Graph::current();
        g.evaluate();
        Drag d;
        d.press = px;
        const ComponentMode kind = scene::components::mode();
        for (auto& [node, ids] : scene::components::selected(kind))
        {
            scene::Node* n = g.node(node);
            if (!n || !n->type().isA("mesh") || ids.empty())
                continue;
            Piece piece;
            piece.id = node;
            piece.plug = scene::modifiers::editPlug(g, node);
            piece.saved = g.storedValue(piece.plug);
            const scene::MeshPtr* base = std::get_if<scene::MeshPtr>(&piece.saved);
            if (!base || !*base)
                continue;
            piece.base = *base;
            piece.world = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
            piece.kind = kind;
            piece.ids = ids;
            d.pieces.push_back(std::move(piece));
        }
        if (d.pieces.empty())
            return;
        // Edge slides orient every chain alike from the start; vertex slides wait for the drag to say which edges.
        if (kind != ComponentMode::Vertex)
        {
            glm::dvec3 lean(0.0);
            for (Piece& piece : d.pieces)
            {
                piece.rails = scene::slideRails(*piece.base, piece.world, kind, piece.ids, std::nullopt);
                const geo::Mesh& m = *piece.base;
                for (size_t k = 0; k < piece.rails.points.size(); ++k)
                {
                    const glm::dvec3 at = piece.worldOf(m.position(piece.rails.points[k]));
                    if (Index to = piece.rails.ends[k][0]; to != kInvalid)
                        lean += piece.worldOf(m.position(to)) - at;
                    if (Index to = piece.rails.ends[k][1]; to != kInvalid)
                        lean -= piece.worldOf(m.position(to)) - at;
                }
            }
            if (glm::length(lean) > 1e-12)
                setDirection(d, glm::normalize(lean));
        }
        mDrag = std::move(d);
        mCursor = px;
        viewportTools::requestRedraw();
    }

    static void setDirection(Drag& d, const glm::dvec3& dir)
    {
        d.direction = dir;
        d.hasRails = false;
        for (Piece& piece : d.pieces)
        {
            piece.rails = scene::slideRails(*piece.base, piece.world, piece.kind, piece.ids, dir);
            d.hasRails = d.hasRails || !piece.rails.points.empty();
        }
    }

    /** The world direction a screen drag from the press points along: each selected vertex's edge most along it, summed. */
    glm::dvec3 vertexDirection(QPointF delta, const ViewInfo& view) const
    {
        const double len = std::hypot(delta.x(), delta.y());
        glm::dvec3 sum(0.0);
        for (const Piece& piece : mDrag->pieces)
        {
            const geo::Mesh& m = *piece.base;
            for (Index v : piece.ids)
            {
                if (!m.pointValid(v))
                    continue;
                const glm::dvec3 at = piece.worldOf(m.position(v));
                const std::optional<QPointF> sv = view.project(at);
                if (!sv)
                    continue;
                double best = -2.0;
                glm::dvec3 pick(0.0);
                m.forEachPointEdge(v, [&](Index e) {
                    const glm::dvec3 to = piece.worldOf(m.position(m.edgeOtherPoint(e, v)));
                    const std::optional<QPointF> st = view.project(to);
                    if (!st)
                        return;
                    const QPointF s = *st - *sv;
                    const double sl = std::hypot(s.x(), s.y());
                    if (sl < 1e-6)
                        return;
                    if (const double c = (s.x() * delta.x() + s.y() * delta.y()) / (sl * len); c > best)
                    {
                        best = c;
                        pick = to - at;
                    }
                });
                if (glm::length(pick) > 1e-12)
                    sum += glm::normalize(pick);
            }
        }
        return glm::length(sum) > 1e-12 ? glm::normalize(sum) : glm::dvec3(0.0);
    }

    /** Average screen offset to each side's rail ends. */
    std::array<QPointF, 2> sideVectors(const ViewInfo& view) const
    {
        std::array<QPointF, 2> sum{};
        std::array<int, 2> count{};
        for (const Piece& piece : mDrag->pieces)
        {
            const geo::Mesh& m = *piece.base;
            for (size_t k = 0; k < piece.rails.points.size(); ++k)
            {
                const std::optional<QPointF> a = view.project(piece.worldOf(m.position(piece.rails.points[k])));
                for (int s = 0; s < 2; ++s)
                    if (Index to = piece.rails.ends[k][size_t(s)]; a && to != kInvalid)
                        if (const std::optional<QPointF> b = view.project(piece.worldOf(m.position(to))))
                        {
                            sum[size_t(s)] += *b - *a;
                            ++count[size_t(s)];
                        }
            }
        }
        for (int s = 0; s < 2; ++s)
            if (count[size_t(s)])
                sum[size_t(s)] /= double(count[size_t(s)]);
        return sum;
    }

    void update(const ViewInfo& view, bool even)
    {
        Drag& d = *mDrag;
        d.even = even;
        const QPointF delta = mCursor - d.press;
        if (scene::components::mode() == ComponentMode::Vertex && !d.direction)
        {
            if (std::hypot(delta.x(), delta.y()) < kDirectionPx)
                return;
            if (const glm::dvec3 dir = vertexDirection(delta, view); glm::length(dir) > 0.0)
                setDirection(d, dir);
        }
        if (!d.hasRails)
            return;
        const auto [a, b] = sideVectors(view);
        auto dot = [](QPointF u, QPointF v) { return u.x() * v.x() + u.y() * v.y(); };
        const QPointF axis = a - b;
        double t = 0.0;
        if (dot(delta, axis) >= 0.0)
            t = dot(a, a) > 1e-6 ? dot(delta, a) / dot(a, a) : 0.0;
        else
            t = dot(b, b) > 1e-6 ? -dot(delta, b) / dot(b, b) : 0.0;
        d.amount = std::clamp(t, -1.0, 1.0);
        preview();
    }

    void restore()
    {
        scene::Graph& g = scene::Graph::current();
        UndoStack& u = UndoStack::get();
        const bool was = u.enabled();
        u.setEnabled(false);
        for (const Piece& piece : mDrag->pieces)
            if (g.node(piece.id))
                g.setValue(piece.plug, piece.saved);
        u.setEnabled(was);
    }

    void preview()
    {
        restore();
        UndoStack& u = UndoStack::get();
        const bool was = u.enabled();
        u.setEnabled(false);
        try
        {
            scene::slideSelectedComponents("Slide", {.amount = mDrag->amount, .direction = mDrag->direction, .even = mDrag->even});
        }
        catch (const CommandError& e)
        {
            log::error("{}", e.what());
        }
        u.setEnabled(was);
        viewportTools::requestRedraw();
    }

    void commit()
    {
        restore();
        const Drag d = std::move(*mDrag);
        mDrag.reset();
        if (d.hasRails && d.direction && std::abs(d.amount) > 1e-6)
        {
            try
            {
                const glm::dvec3& dir = *d.direction;
                ops::execute("polySlide", {{"amount", std::round(d.amount * 1e4) / 1e4}, {"direction", {dir.x, dir.y, dir.z}}, {"even", d.even}},
                             CallSource::UI);
            }
            catch (const CommandError& e)
            {
                log::error("{}", e.what());
            }
        }
        viewportTools::requestRedraw();
    }

    void cancel()
    {
        restore();
        mDrag.reset();
        viewportTools::requestRedraw();
    }

    std::optional<Drag> mDrag;
    QPointF mCursor;
};
} // namespace

void registerSlideActions()
{
    actions::add({
        .id = "mesh.slide",
        .label = "Slide",
        .category = "Mesh",
        .run = [] { viewportTools::set(std::make_unique<SlideTool>()); },
        .doc = "Slide the selected edges along the edges beside them, or vertices along their own edges, without changing the surface. "
               "Middle-drag anywhere or drag the selection; Shift slides evenly, Esc exits.",
        .python = "polySlide(amount=0.5)",
        .keywords = {"slide edge", "edge slide", "vertex slide", "slide vertex", "slide edge tool"},
    });
}
} // namespace lila::ui
