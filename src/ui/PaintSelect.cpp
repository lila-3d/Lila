#include "ui/PaintSelect.h"

#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "geo/RenderMesh.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/ComponentPick.h"
#include "scene/DisplayLayers.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/Transform.h"
#include "ui/Actions.h"
#include "ui/DrawOverrides.h"
#include "ui/Isolate.h"
#include "ui/MeshCache.h"
#include "ui/Tools.h"
#include "ui/ViewportTool.h"
#include "uv/UvCommands.h"

#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace lila::ui::paintSelect
{
using scene::ComponentMode;
using scene::NodeId;
namespace cp = scene::componentPick;

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr const char* kRadiusKey = "/paintSelect/radius";

/** Brushes paint in the active view, so its isolation decides what they can reach. */
bool pickableMesh(const scene::Graph& g, NodeId id, bool inherit)
{
    const scene::Node* n = g.node(id);
    return n && n->type().isA("mesh") && !drawOverrides::forType(n->type()) && scene::effectivelyVisible(g, id, inherit) &&
           scene::displayLayers::pickable(g, id) && scene::selectionMask::pickable(*n) && isolate::shows(scene::isolation::active(), id);
}

/** The meshes components are picked on, as the viewport has them: the selected ones, or every pickable mesh. */
std::vector<NodeId> hilited()
{
    scene::Graph& g = scene::Graph::current();
    std::vector<NodeId> out;
    const bool inherit = scene::inheritVisibility();
    // Meshes under a selected group count too; hidden ones only when selected themselves.
    for (NodeId id : uv::meshesUnder(g, g.selection()))
        if ((std::ranges::find(g.selection(), id) != g.selection().end() || pickableMesh(g, id, inherit)) &&
            isolate::shows(scene::isolation::active(), id))
            out.push_back(id);
    if (out.empty())
    {
        for (NodeId id : g.nodes())
            if (pickableMesh(g, id, inherit))
                out.push_back(id);
    }
    return out;
}

glm::dvec3 forward(const ViewInfo& v) { return -glm::dvec3(v.view[0][2], v.view[1][2], v.view[2][2]); }

/** Toward the camera from a point; parallel in orthographic views. */
glm::dvec3 toEye(const ViewInfo& v, const glm::dvec3& at) { return v.ortho ? -forward(v) : v.eye - at; }

bool faceFront(const geo::Mesh& m, const glm::dmat4& world, const glm::dmat3& normalMat, uint32_t f, const ViewInfo& view)
{
    if (!m.faceValid(f))
        return false;
    glm::dvec3 n = normalMat * m.faceNormal(f);
    glm::dvec3 c(world * glm::dvec4(m.faceCentroid(f), 1.0));
    return glm::dot(n, toEye(view, c)) > 0.0;
}

/** Nothing on the given meshes is closer along the view ray to world. */
bool unoccluded(const scene::Graph& g, const std::vector<NodeId>& meshes, const ViewInfo& view, const glm::dvec3& world)
{
    glm::dvec3 eye = view.ortho ? world - forward(view) * 1e5 : view.eye;
    glm::dvec3 dir = world - eye;
    double dist = glm::length(dir);
    if (dist <= 0.0)
        return true;
    dir /= dist;
    for (NodeId id : meshes)
    {
        const scene::Node* n = g.node(id);
        if (auto hit = isolate::raycast(scene::isolation::active(), id, n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh),
                                        n->as<glm::dmat4>(scene::Transform::kWorldMatrix), eye, dir);
            hit && hit->distance < dist * (1.0 - 1e-4) - 1e-3)
            return false;
    }
    return true;
}

const char* flagFor(ComponentMode m) { return m == ComponentMode::Vertex ? "vertices" : m == ComponentMode::Edge ? "edges" : "faces"; }

int slot(ComponentMode m) { return m == ComponentMode::Vertex ? 0 : m == ComponentMode::Edge ? 1 : 2; }

/** Runs body with undo recording off. */
template <class F> void unrecorded(F&& body)
{
    UndoStack& u = UndoStack::get();
    bool was = u.enabled();
    u.setEnabled(false);
    body();
    u.setEnabled(was);
}

} // namespace

bool frontFacing(const geo::Mesh& m, const glm::dmat4& world, ComponentMode kind, uint32_t id, const ViewInfo& view)
{
    glm::dmat3 nm = glm::transpose(glm::inverse(glm::dmat3(world)));
    switch (kind)
    {
    case ComponentMode::Face: return faceFront(m, world, nm, id, view);
    case ComponentMode::Edge:
    {
        if (!m.edgeValid(id))
            return false;
        bool any = false, faces = false;
        m.forEachEdgeCorner(id, [&](geo::Index c) {
            faces = true;
            any = any || faceFront(m, world, nm, m.cornerFace(c), view);
        });
        return any || !faces;
    }
    case ComponentMode::Vertex:
    {
        if (!m.pointValid(id))
            return false;
        std::vector<geo::Index> faces = m.pointFaces(id);
        return faces.empty() || std::ranges::any_of(faces, [&](geo::Index f) { return faceFront(m, world, nm, f, view); });
    }
    default: return true;
    }
}

bool backfaceCulling() { return PrefStore::get().value<bool>("general", "/select/backfaceCulling", false); }

std::map<NodeId, scene::components::Ids> componentsUnder(const ViewInfo& view, QPointF at, double radiusPx)
{
    LILA_ZONE();

    std::map<NodeId, scene::components::Ids> out;
    ComponentMode mode = scene::components::mode();
    if (mode == ComponentMode::Object)
        return out;
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    std::vector<NodeId> live = hilited();
    cp::Screen screen{view.viewProj(), view.width, view.height};
    glm::dvec2 cursor(at.x(), at.y());
    glm::dvec3 rayO, rayD;
    view.ray(at, rayO, rayD);
    for (NodeId id : live)
    {
        const scene::Node* n = g.node(id);
        scene::MeshPtr mesh = scene::modifiers::cage(g, id);
        if (!mesh)
            continue;
        const geo::Mesh& m = *mesh;
        glm::dmat4 world = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        const auto vis = isolate::visibility(scene::isolation::active(), id, mesh);
        auto& ids = out[id];
        if (mode == ComponentMode::Face)
        {
            // The face under the brush centre, then every face whose centre falls inside it.
            if (auto pd = pickDataFor(mesh); pd)
                if (auto hit = isolate::raycast(scene::isolation::active(), id, mesh, world, rayO, rayD); hit && hit->triangle < pd->triangleFaces.size())
                    {
                        uint32_t f = pd->triangleFaces[hit->triangle];
                        if (frontFacing(m, world, mode, f, view) && unoccluded(g, live, view, rayO + rayD * hit->distance))
                            ids.push_back(f);
                    }
            for (uint32_t f = 0; f < m.faceCapacity(); ++f)
            {
                if (!m.faceValid(f) || !vis->face(f))
                    continue;
                glm::dvec3 c(world * glm::dvec4(m.faceCentroid(f), 1.0));
                auto s = screen.project(c);
                if (s && glm::length(*s - cursor) < radiusPx && frontFacing(m, world, mode, f, view) && unoccluded(g, live, view, c))
                    ids.push_back(f);
            }
        }
        else
            for (const cp::Candidate& c : cp::nearby(m, world, mode, screen, cursor, radiusPx))
                if (vis->shows(mode, c.id) && frontFacing(m, world, mode, c.id, view) && unoccluded(g, live, view, cp::anchor(m, world, mode, c.id)))
                    ids.push_back(c.id);
        if (ids.empty())
            out.erase(id);
    }
    return out;
}

std::vector<NodeId> objectsUnder(const ViewInfo& view, QPointF at, double radiusPx)
{
    LILA_ZONE();

    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    const bool inherit = scene::inheritVisibility();
    std::vector<NodeId> meshes;
    for (NodeId id : g.nodes())
        if (pickableMesh(g, id, inherit))
            meshes.push_back(id);

    // Rays through the centre and two rings of the brush; each takes the nearest surface it meets if that faces the camera.
    std::vector<QPointF> samples{at};
    for (double ring : {0.5, 1.0})
        for (int k = 0; k < 8; ++k)
        {
            double a = 2.0 * kPi * (k + ring) / 8.0;
            samples.push_back(at + QPointF(std::cos(a), std::sin(a)) * radiusPx * ring);
        }
    std::vector<NodeId> out;
    for (QPointF s : samples)
    {
        glm::dvec3 o, d;
        view.ray(s, o, d);
        double best = 1e300;
        NodeId bestId = scene::kNoNode;
        bool bestFront = false;
        for (NodeId id : meshes)
        {
            const scene::Node* n = g.node(id);
            scene::MeshPtr mesh = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
            auto pd = pickDataFor(mesh);
            if (!pd || !mesh)
                continue;
            glm::dmat4 world = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
            if (auto hit = isolate::raycast(scene::isolation::active(), id, mesh, world, o, d); hit && hit->distance < best)
            {
                best = hit->distance;
                bestId = id;
                bestFront = hit->triangle < pd->triangleFaces.size() &&
                            frontFacing(*mesh, world, ComponentMode::Face, pd->triangleFaces[hit->triangle], view);
            }
        }
        if (bestId != scene::kNoNode && bestFront && std::find(out.begin(), out.end(), bestId) == out.end())
            out.push_back(bestId);
    }
    return out;
}

Stroke::Op Stroke::opFor(Qt::KeyboardModifiers mods, Op plain)
{
    bool shift = mods & Qt::ShiftModifier, ctrl = mods & Qt::ControlModifier;
    return shift && ctrl ? Op::Add : ctrl ? Op::Remove : shift ? Op::Toggle : plain;
}

void Stroke::begin(Op op)
{
    mActive = true;
    mOp = op;
    mMode = scene::components::mode();
    mTouched.clear();
    mTouchedObjects.clear();
    for (ComponentMode k : {ComponentMode::Vertex, ComponentMode::Edge, ComponentMode::Face})
        mStart[slot(k)] = scene::components::selected(k);
    mStartObjects = scene::Graph::current().selection();
    if (op == Op::Replace)
        preview();
}

void Stroke::paint(const ViewInfo& view, QPointF a, QPointF b, double radiusPx)
{
    LILA_ZONE();

    if (!mActive)
        return;
    QPointF d = b - a;
    double len = std::hypot(d.x(), d.y());
    int steps = std::max(1, int(std::ceil(len / std::max(1.0, radiusPx * 0.5))));
    bool changed = false;
    for (int i = len > 0.0 ? 1 : 0; i <= steps; ++i)
    {
        QPointF at = a + d * (double(i) / steps);
        if (mMode == ComponentMode::Object)
        {
            for (NodeId id : objectsUnder(view, at, radiusPx))
                changed |= mTouchedObjects.insert(id).second;
        }
        else
            for (auto& [node, ids] : componentsUnder(view, at, radiusPx))
                for (uint32_t id : ids)
                    changed |= mTouched[node].insert(id).second;
    }
    if (changed)
        preview();
}

void Stroke::restoreStart()
{
    unrecorded([&] {
        if (mMode == ComponentMode::Object)
            scene::Graph::current().select(mStartObjects);
        else
            scene::components::setAll(mStart[0], mStart[1], mStart[2]);
    });
}

void Stroke::preview()
{
    restoreStart();
    unrecorded([&] {
        if (mMode == ComponentMode::Object)
        {
            std::vector<NodeId> ids(mTouchedObjects.begin(), mTouchedObjects.end());
            using SM = scene::SelectMode;
            SM sm = mOp == Op::Replace ? SM::Replace : mOp == Op::Add ? SM::Add : mOp == Op::Remove ? SM::Remove : SM::Toggle;
            scene::Graph::current().select(ids, sm);
            return;
        }
        using CO = scene::components::Op;
        if (mOp == Op::Replace && mTouched.empty())
        {
            std::map<NodeId, scene::components::Ids> sets[3] = {mStart[0], mStart[1], mStart[2]};
            sets[slot(mMode)].clear();
            scene::components::setAll(sets[0], sets[1], sets[2]);
            return;
        }
        bool first = true;
        for (auto& [node, ids] : mTouched)
        {
            CO op = mOp == Op::Replace ? (first ? CO::Replace : CO::Add) : mOp == Op::Add ? CO::Add : mOp == Op::Remove ? CO::Remove : CO::Toggle;
            scene::components::select(node, mMode, scene::components::Ids(ids.begin(), ids.end()), op);
            first = false;
        }
    });
}

void Stroke::cancel()
{
    if (!mActive)
        return;
    mActive = false;
    restoreStart();
}

void Stroke::end()
{
    if (!mActive)
        return;
    mActive = false;
    restoreStart();
    scene::Graph& g = scene::Graph::current();
    auto flags = [&](json& call) {
        if (mOp == Op::Add)
            call["add"] = true;
        else if (mOp == Op::Remove)
            call["deselect"] = true;
        else if (mOp == Op::Toggle)
            call["toggle"] = true;
    };
    try
    {
        if (mMode == ComponentMode::Object)
        {
            json names = json::array();
            for (NodeId id : mTouchedObjects)
                if (const scene::Node* n = g.node(id))
                    names.push_back(n->path());
            if (names.empty())
            {
                if (mOp == Op::Replace && !mStartObjects.empty())
                    ops::execute("select", {{"clear", true}}, CallSource::UI);
                return;
            }
            json call = {{"args", names}};
            flags(call);
            ops::execute("select", call, CallSource::UI);
            return;
        }
        const char* flag = flagFor(mMode);
        if (mTouched.empty())
        {
            if (mOp == Op::Replace && !mStart[slot(mMode)].empty())
                if (const scene::Node* n = g.node(mStart[slot(mMode)].begin()->first))
                    ops::execute("selectComponents", {{"args", {n->path()}}, {flag, json::array()}}, CallSource::UI);
            return;
        }
        UndoChunk chunk("Paint Select");
        bool first = true;
        for (auto& [node, ids] : mTouched)
        {
            const scene::Node* n = g.node(node);
            if (!n)
                continue;
            json call = {{"args", {n->path()}}, {flag, scene::components::Ids(ids.begin(), ids.end())}};
            flags(call);
            if (mOp == Op::Replace && !first)
                call["add"] = true;
            first = false;
            ops::execute("selectComponents", call, CallSource::UI);
        }
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

double tabRadius(ComponentMode mode) { return mode == ComponentMode::Vertex ? 10.0 : mode == ComponentMode::Edge ? 6.0 : 4.0; }

namespace
{
bool gLassoHeld = false;
} // namespace

bool lassoHeld() { return gLassoHeld; }

bool lassoActive() { return gLassoHeld || PrefStore::get().value<bool>("general", kLassoPref, false); }

namespace
{
double brushRadius() { return std::clamp(PrefStore::get().value<double>("general", kRadiusKey, 30.0), 2.0, 1000.0); }

void setBrushRadius(double r) { PrefStore::get().set("general", kRadiusKey, std::clamp(r, 2.0, 1000.0)); }

class PaintSelectTool final : public ViewportTool
{
public:
    const char* name() const override { return "Paint Select"; }

    void deactivate() override { mStroke.cancel(); }

    bool ownsBrushRadius() const override { return true; }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        mHover = e->position();
        if (tools::brushRadiusDrag(e, false))
        {
            mAdjusting = true;
            mAdjustStart = e->position();
            if (e->button() == Qt::MiddleButton)
                setBrushRadius(0.0);
            mAdjustFrom = brushRadius();
            return true;
        }
        if (e->button() != Qt::LeftButton)
            return false;
        mStroke.begin(Stroke::opFor(e->modifiers(), Stroke::Op::Add));
        mLast = e->position();
        mStroke.paint(view, mLast, mLast, brushRadius());
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        mHover = e->position();
        if (mAdjusting)
        {
            setBrushRadius(mAdjustFrom + (e->position().x() - mAdjustStart.x()));
            viewportTools::requestRedraw();
            return true;
        }
        if (mStroke.active())
        {
            mStroke.paint(view, mLast, e->position(), brushRadius());
            mLast = e->position();
            return true;
        }
        viewportTools::requestRedraw();
        return false;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo&) override
    {
        if (e->button() != Qt::LeftButton && !(mAdjusting && e->button() == Qt::MiddleButton))
            return false;
        mAdjusting = false;
        mStroke.end();
        return true;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        switch (e->key())
        {
        case Qt::Key_BracketLeft:
        case Qt::Key_BracketRight:
            setBrushRadius(brushRadius() * (e->key() == Qt::Key_BracketLeft ? 0.8 : 1.25));
            viewportTools::requestRedraw();
            return true;
        case Qt::Key_Escape:
            if (mStroke.active())
                mStroke.cancel();
            else
                QTimer::singleShot(0, [] { viewportTools::set(nullptr); });
            return true;
        default: return false;
        }
    }

    void draw(render::FrameState&, OverlayPainter& p, const ViewInfo& view) override
    {
        double r = brushRadius();
        bool objects = scene::components::mode() == ComponentMode::Object;
        drawToolMessage(p, view, QString("Paint select %1: radius %2 px   Ctrl removes, Shift toggles, B drag or [ ] resizes")
                                     .arg(objects ? "objects" : scene::components::modeName(scene::components::mode()))
                                     .arg(int(std::lround(r))));
        if (!mHover)
            return;
        QPointF at = mAdjusting ? mAdjustStart : *mHover;
        std::vector<QPointF> pts;
        for (int k = 0; k < 64; ++k)
        {
            double a = 2.0 * kPi * k / 64;
            pts.push_back(at + QPointF(std::cos(a), std::sin(a)) * r);
        }
        p.polyline(pts, 2.5f, {0.05f, 0.05f, 0.05f, 0.6f}, true);
        p.polyline(pts, 1.5f, {0.95f, 0.95f, 0.95f, 1.0f}, true);
    }

    std::optional<QCursor> cursor() const override { return QCursor(Qt::CrossCursor); }

private:
    Stroke mStroke;
    std::optional<QPointF> mHover;
    QPointF mLast;
    bool mAdjusting = false;
    QPointF mAdjustStart;
    double mAdjustFrom = 0.0;
};
} // namespace

void registerPaintSelectUi()
{
    viewportTools::add({.id = kToolId, .label = "Paint Select", .icon = "brush", .create = [] { return std::make_unique<PaintSelectTool>(); }});
    actions::add({.id = "select.paintTool",
                  .label = "Paint Select Tool",
                  .category = "Select",
                  .run = [] { viewportTools::activate(kToolId); },
                  .doc = "Brush components (or objects in object mode) into the selection; Ctrl removes, Shift toggles. Only "
                         "what faces the camera is picked. B + drag or [ ] sets the radius.",
                  .python = "lila.ui.run_action(\"select.paintTool\")"});
    actions::add({.id = "select.paintRadiusSmaller", .label = "Paint Select Radius Smaller", .category = "Select",
                  .run = [] { setBrushRadius(brushRadius() * 0.8); },
                  .doc = "Shrinks the Paint Select brush.",
                  .python = "lila.prefs.set(\"general\", \"/paintSelect/radius\", 20)"});
    actions::add({.id = "select.paintRadiusLarger", .label = "Paint Select Radius Larger", .category = "Select",
                  .run = [] { setBrushRadius(brushRadius() * 1.25); },
                  .doc = "Grows the Paint Select brush.",
                  .python = "lila.prefs.set(\"general\", \"/paintSelect/radius\", 40)"});
    actions::add({.id = "select.toggleLasso", .label = "Lasso Select", .category = "Select",
                  .run = [] { PrefStore::get().set("general", kLassoPref, !PrefStore::get().value<bool>("general", kLassoPref, false)); },
                  .doc = "Drag selection draws a freehand lasso instead of a marquee and picks what falls inside it. Shift "
                         "toggles, Ctrl removes, Ctrl+Shift adds, as with the marquee.",
                  .python = "lila.prefs.set(\"general\", \"/select/lasso\", True)"});
    actions::add({.id = "select.lassoHoldOn", .label = "Lasso Select (hold)", .category = "Select",
                  .run = [] { gLassoHeld = true; },
                  .doc = "While held, drag selection draws a lasso instead of a marquee.",
                  .python = "lila.ui.run_action(\"select.lassoHoldOn\")"});
    actions::add({.id = "select.lassoHoldOff", .label = "End Lasso Select Hold", .category = "Select",
                  .run = [] { gLassoHeld = false; },
                  .doc = "Releases the lasso hold.",
                  .python = "lila.ui.run_action(\"select.lassoHoldOff\")"});
}
} // namespace lila::ui::paintSelect
