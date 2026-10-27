#include "ui/ConformTool.h"

#include "core/Log.h"
#include "curve/CurveNodes.h"
#include "curve/Tessellate.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/LastOp.h"
#include "scene/SphereState.h"
#include "scene/Transform.h"
#include "ui/Actions.h"
#include "ui/GizmoMath.h"
#include "ui/SurfaceSnap.h"
#include "ui/ViewportDisplay.h"
#include "ui/ViewportTool.h"

#include <QTimer>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <map>

namespace lila::ui
{
namespace
{
constexpr const char* kConformCommand = "polyConformToObject";
constexpr const char* kSpherizeCommand = "polySpherize";

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

namespace lo = scene::lastOp;

bool spherizeLive()
{
    const lo::Op* op = lo::current();
    return op && op->command == kSpherizeCommand && lo::reachable();
}

double distanceToPoint(QPointF a, QPointF b) { return std::hypot(a.x() - b.x(), a.y() - b.y()); }

/**
 * After Spherize ran, shows its sphere in the viewport: wire circles on the ellipsoid's three planes, move arrows and a disc
 * (view plane) at the centre, a ring around it for the radius and a square on each axis for the ellipsoid's scale. Dragging
 * a handle re-runs the op through scene::lastOp, so the whole drag is one undo step and the adjust-last-op popup follows.
 * Enter accepts, Esc undoes the spherize.
 */
class SpherizeTool final : public ViewportTool
{
public:
    explicit SpherizeTool(size_t undoStart) : mUndoStart(undoStart), mSphere(scene::lastSphere())
    {
        remember();
        // Undo and redo between adjustments put back an earlier call, whose sphere was remembered when it ran.
        mListener = lo::addListener([this](lo::Change c) {
            if (c == lo::Change::Recorded || c == lo::Change::Rerun)
                remember();
            if (mDrag == Handle::None)
                mSphere = current();
            viewportTools::requestRedraw();
        });
    }

    ~SpherizeTool() override { lo::removeListener(mListener); }

    const char* name() const override { return "Spherize"; }

    void deactivate() override
    {
        if (mDrag != Handle::None)
            lo::endDrag();
    }

    std::optional<QCursor> cursor() const override
    {
        return mHover == Handle::None ? std::nullopt : std::optional<QCursor>(QCursor(Qt::PointingHandCursor));
    }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton)
            return false;
        if (!spherizeLive())
            return true;
        mSphere = current();
        mDrag = pick(e->position(), view);
        if (mDrag == Handle::None)
            return true;
        mStart = mSphere;
        glm::dvec3 origin, dir;
        view.ray(e->position(), origin, dir);
        if (mDrag == Handle::Center)
        {
            const glm::dvec3 hit = onViewPlane(origin, dir, mStart.center, view);
            mGrab = mStart.center - hit;
        }
        else if (mDrag == Handle::Ring)
        {
            const auto c = view.project(mStart.center);
            mStartRing = c ? distanceToPoint(*c, e->position()) : 0.0;
        }
        else if (isMove(mDrag))
            mStartAxis = gizmo::lineParam(mStart.center, worldAxis(mDrag), origin, dir);
        else
            mStartAxis = alongAxis(origin, dir, int(mDrag) - int(Handle::AxisX));
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        if (mDrag == Handle::None)
        {
            const Handle h = pick(e->position(), view);
            if (h != mHover)
            {
                mHover = h;
                viewportTools::requestRedraw();
            }
            return false;
        }
        glm::dvec3 origin, dir;
        view.ray(e->position(), origin, dir);
        scene::SphereState next = mStart;
        if (mDrag == Handle::Center)
            next.center = onViewPlane(origin, dir, mStart.center, view) + mGrab;
        else if (isMove(mDrag))
        {
            const glm::dvec3 a = worldAxis(mDrag);
            next.center = mStart.center + a * (gizmo::lineParam(mStart.center, a, origin, dir) - mStartAxis);
        }
        else if (mDrag == Handle::Ring)
        {
            const auto c = view.project(mStart.center);
            if (!c || mStartRing < 1.0)
                return true;
            next.radius = std::max(1e-3, mStart.radius * distanceToPoint(*c, e->position()) / mStartRing);
        }
        else
        {
            const int axis = int(mDrag) - int(Handle::AxisX);
            const double t = alongAxis(origin, dir, axis);
            if (std::abs(mStartAxis) < 1e-9 || t * mStartAxis <= 0.0)
                return true;
            next.scale[axis] = std::clamp(mStart.scale[axis] * t / mStartAxis, 0.02, 50.0);
        }
        apply(next);
        return true;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo&) override
    {
        if (e->button() != Qt::LeftButton || mDrag == Handle::None)
            return false;
        mDrag = Handle::None;
        lo::endDrag();
        mSphere = current();
        viewportTools::requestRedraw();
        return true;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)
        {
            finish(false);
            return true;
        }
        if (e->key() == Qt::Key_Escape)
        {
            finish(true);
            return true;
        }
        return false;
    }

    void draw(render::FrameState&, OverlayPainter& p, const ViewInfo& view) override
    {
        if (!spherizeLive())
        {
            end();
            return;
        }
        if (mDrag == Handle::None)
            mSphere = current();
        if (mSphere.radius <= 0.0)
            return;
        const glm::vec4 wire(0.35f, 0.85f, 1.0f, 0.9f), hot(1.0f, 1.0f, 1.0f, 1.0f);
        const glm::dmat3 axes = frame();
        for (int i = 0; i < 3; ++i)
        {
            const int j = (i + 1) % 3, k = (i + 2) % 3;
            std::vector<QPointF> run;
            bool broken = false;
            for (int n = 0; n < 72; ++n)
            {
                const double a = glm::two_pi<double>() * n / 72.0;
                const glm::dvec3 w = mSphere.center + axes[j] * (mSphere.scale[j] * mSphere.radius * std::cos(a)) +
                                     axes[k] * (mSphere.scale[k] * mSphere.radius * std::sin(a));
                if (auto px = view.project(w))
                    run.push_back(*px);
                else
                    broken = true;
            }
            if (run.size() > 1)
                p.polyline(run, 1.5f, wire, !broken);
        }
        const auto lit = [&](Handle h) { return mHover == h || mDrag == h; };
        if (const auto c = view.project(mSphere.center))
        {
            p.polyline(ringPoints(*c, view), lit(Handle::Ring) ? 3.0f : 2.0f, lit(Handle::Ring) ? hot : wire, true);
            for (int i = 0; i < 3; ++i)
            {
                const Handle h = Handle(int(Handle::MoveX) + i);
                const auto tip = moveTip(i, view);
                if (!tip || distanceToPoint(*c, *tip) < 4.0)
                    continue;
                const glm::vec4 col = lit(h) ? hot : glm::vec4(viewportDisplay::axisColor(i), 1.0f);
                const QPointF d = (*tip - *c) / distanceToPoint(*c, *tip), side(-d.y(), d.x());
                p.line(*c, *tip - d * 10.0, lit(h) ? 3.0f : 2.0f, col);
                p.triangle(*tip, *tip - d * 12.0 + side * 5.0, *tip - d * 12.0 - side * 5.0, col);
            }
            p.disc(*c, lit(Handle::Center) ? 7.0f : 5.5f, lit(Handle::Center) ? hot : wire);
            p.label(*c + QPointF(10, -10), QString("R %1 cm").arg(mSphere.radius, 0, 'f', 2));
        }
        for (int i = 0; i < 3; ++i)
            if (const auto t = view.project(axisTip(i)))
                p.rect(QRectF(t->x() - 5, t->y() - 5, 10, 10), lit(Handle(int(Handle::AxisX) + i)) ? hot : glm::vec4(viewportDisplay::axisColor(i), 1.0f));
        drawToolMessage(p, view,
                        "Spherize: drag an arrow or the centre to move, the ring for the radius, an axis square to stretch. Enter: accept, Esc: undo");
    }

private:
    enum class Handle
    {
        None,
        Center,
        Ring,
        AxisX,
        AxisY,
        AxisZ,
        MoveX,
        MoveY,
        MoveZ,
    };

    static constexpr double kArrowPx = 80.0;

    static bool isMove(Handle h) { return h == Handle::MoveX || h == Handle::MoveY || h == Handle::MoveZ; }

    static glm::dvec3 worldAxis(Handle h)
    {
        const int i = int(h) - int(Handle::MoveX);
        return {i == 0, i == 1, i == 2};
    }

    /** The sphere the op's current call ran with, else the latest run's. */
    scene::SphereState current() const
    {
        if (const lo::Op* op = lo::current())
            if (auto it = mByCall.find(op->call.dump()); it != mByCall.end())
                return it->second;
        return scene::lastSphere();
    }

    void remember()
    {
        if (const lo::Op* op = lo::current(); op && op->command == kSpherizeCommand)
            mByCall[op->call.dump()] = scene::lastSphere();
    }

    glm::dmat3 frame() const
    {
        const glm::dquat q = glm::angleAxis(glm::radians(mSphere.rotation.z), glm::dvec3(0, 0, 1)) *
                             glm::angleAxis(glm::radians(mSphere.rotation.y), glm::dvec3(0, 1, 0)) *
                             glm::angleAxis(glm::radians(mSphere.rotation.x), glm::dvec3(1, 0, 0));
        return glm::mat3_cast(q);
    }

    glm::dvec3 axisTip(int i) const { return mSphere.center + frame()[i] * (mSphere.scale[i] * mSphere.radius); }

    static glm::dvec3 camRight(const ViewInfo& v) { return {v.view[0][0], v.view[1][0], v.view[2][0]}; }

    /** Screen end of a move arrow: a fixed pixel length along the world axis at any zoom. */
    std::optional<QPointF> moveTip(int i, const ViewInfo& view) const
    {
        const auto c = view.project(mSphere.center), unit = view.project(mSphere.center + camRight(view));
        if (!c || !unit)
            return std::nullopt;
        const double perCm = distanceToPoint(*c, *unit);
        if (perCm < 1e-9)
            return std::nullopt;
        return view.project(mSphere.center + glm::dvec3(i == 0, i == 1, i == 2) * (kArrowPx / perCm));
    }

    static double distanceToSegment(QPointF px, QPointF a, QPointF b)
    {
        const QPointF d = b - a;
        const double len2 = QPointF::dotProduct(d, d);
        const double t = len2 > 0 ? std::clamp(QPointF::dotProduct(px - a, d) / len2, 0.0, 1.0) : 0.0;
        return distanceToPoint(a + d * t, px);
    }

    /** Screen radius of the silhouette ring, which tracks the sphere's mean size. */
    double ringRadius(QPointF centerPx, const ViewInfo& view) const
    {
        const double mean = (mSphere.scale.x + mSphere.scale.y + mSphere.scale.z) / 3.0;
        const auto edge = view.project(mSphere.center + camRight(view) * (mSphere.radius * mean));
        return edge ? distanceToPoint(centerPx, *edge) : 0.0;
    }

    std::vector<QPointF> ringPoints(QPointF centerPx, const ViewInfo& view) const
    {
        const double r = ringRadius(centerPx, view);
        std::vector<QPointF> out;
        for (int n = 0; n < 64; ++n)
        {
            const double a = glm::two_pi<double>() * n / 64.0;
            out.push_back(centerPx + QPointF(std::cos(a), std::sin(a)) * r);
        }
        return out;
    }

    Handle pick(QPointF px, const ViewInfo& view) const
    {
        if (!spherizeLive() || mSphere.radius <= 0.0)
            return Handle::None;
        const auto c = view.project(mSphere.center);
        if (c && distanceToPoint(*c, px) < 9.0)
            return Handle::Center;
        for (int i = 0; i < 3; ++i)
            if (const auto t = view.project(axisTip(i)); t && distanceToPoint(*t, px) < 9.0)
                return Handle(int(Handle::AxisX) + i);
        if (!c)
            return Handle::None;
        for (int i = 0; i < 3; ++i)
            if (const auto t = moveTip(i, view); t && distanceToSegment(px, *c, *t) < 7.0)
                return Handle(int(Handle::MoveX) + i);
        if (std::abs(distanceToPoint(*c, px) - ringRadius(*c, view)) < 6.0)
            return Handle::Ring;
        return Handle::None;
    }

    static glm::dvec3 onViewPlane(const glm::dvec3& origin, const glm::dvec3& dir, const glm::dvec3& through, const ViewInfo& view)
    {
        const glm::dvec3 n = view.ortho ? glm::normalize(glm::dvec3(-view.view[0][2], -view.view[1][2], -view.view[2][2])) : view.viewDir(through);
        const double denom = glm::dot(dir, n);
        return std::abs(denom) < 1e-9 ? through : origin + dir * (glm::dot(through - origin, n) / denom);
    }

    /** Where along the axis (from the centre, world cm) the ray passes closest to it. */
    double alongAxis(const glm::dvec3& origin, const glm::dvec3& dir, int axis) const
    {
        const glm::dvec3 a = frame()[axis], w = mStart.center - origin;
        const double b = glm::dot(a, dir), denom = 1.0 - b * b;
        return denom < 1e-9 ? 0.0 : (b * glm::dot(dir, w) - glm::dot(a, w)) / denom;
    }

    void apply(const scene::SphereState& s)
    {
        json flags = {{"center", {s.center.x, s.center.y, s.center.z}},
                      {"radius", s.radius},
                      {"scale", {s.scale.x, s.scale.y, s.scale.z}},
                      {"rotation", {s.rotation.x, s.rotation.y, s.rotation.z}}};
        try
        {
            lo::rerun(flags, CallSource::UI, true);
            mSphere = s;
        }
        catch (const CommandError& e)
        {
            log::error("{}", e.what());
        }
        viewportTools::requestRedraw();
    }

    void finish(bool undo)
    {
        if (mDrag != Handle::None)
        {
            lo::endDrag(!undo);
            mDrag = Handle::None;
        }
        if (undo)
            UndoStack::get().jumpTo(std::min(mUndoStart, UndoStack::get().undoCount()));
        end();
    }

    static void end()
    {
        // Not from inside this tool's own handler: set() destroys it.
        QTimer::singleShot(0, [] {
            if (viewportTools::active() && std::string_view(viewportTools::active()->name()) == "Spherize")
                viewportTools::set(nullptr);
        });
        viewportTools::requestRedraw();
    }

    size_t mUndoStart;
    scene::SphereState mSphere, mStart;
    std::map<std::string, scene::SphereState> mByCall;
    int mListener = 0;
    Handle mHover = Handle::None, mDrag = Handle::None;
    glm::dvec3 mGrab{0.0};
    double mStartRing = 0.0, mStartAxis = 0.0;
};

/** Runs Spherize with the saved options, then hands the result to the tool for editing. */
void startSpherize()
{
    const size_t start = UndoStack::get().undoCount();
    actions::runWithOptions(kSpherizeCommand, json::object());
    if (spherizeLive() && UndoStack::get().undoCount() > start && scene::lastSphere().radius > 0.0)
        viewportTools::set(std::make_unique<SpherizeTool>(start));
}

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
    actions::add({
        .id = "mesh.spherize",
        .label = "Spherize",
        .category = "Mesh",
        .run = startSpherize,
        .doc = "Pushes the selected components (whole meshes in object mode) onto a sphere fitted to them. Drag the move arrows or "
               "centre, the radius ring or the axis squares (an ellipsoid) in the viewport; Enter accepts, Esc undoes.",
        .python = "lila.ui.run_action(\"mesh.spherize\")",
        .command = kSpherizeCommand,
        .call = json::object(),
    });
    add("mesh.editEdgeFlow", "Edit Edge Flow", "polyEditEdgeFlow", json::object(),
        "Moves the selected edge loops so they follow the curvature of the surface around them, rounding a flat loop into a curved "
        "surface. Vertex and face selections fit both directions.");
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
