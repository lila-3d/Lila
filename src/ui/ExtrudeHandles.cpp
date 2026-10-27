#include "ui/ExtrudeHandles.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "scene/Components.h"
#include "scene/ExtrudeModifier.h"
#include "scene/Graph.h"
#include "scene/LastOp.h"
#include "scene/Modifiers.h"
#include "scene/Transform.h"
#include "ui/GizmoMath.h"
#include "ui/Manipulator.h"
#include "ui/Overlay.h"
#include "ui/Tools.h"

#include <cmath>
#include <map>
#include <utility>

namespace lila::ui
{
namespace
{
namespace lo = scene::lastOp;
using H = ExtrudeHandles::Handle;

constexpr const char* kCommand = "polyExtrude";
const glm::vec4 kGray(0.70f, 0.70f, 0.70f, 1.0f);
const glm::vec4 kHot(1.0f, 1.0f, 1.0f, 1.0f);

/** The selected extrudeModifier the handles work on when no polyExtrude is live, else 0. */
scene::NodeId selectedModifier()
{
    scene::Graph& g = scene::Graph::current();
    const auto& sel = g.selection();
    for (auto it = sel.rbegin(); it != sel.rend(); ++it)
        if (scene::Node* n = g.node(*it); n && scene::extrudeModifier::isExtrude(*n) && scene::modifiers::meshOf(g, *it) != scene::kNoNode)
            return *it;
    return scene::kNoNode;
}

bool extrudeLive()
{
    const lo::Op* op = lo::current();
    return op && op->command == kCommand && lo::reachable();
}

/** The components the live extrude made: the handles stay on them whatever is selected since. */
const std::map<scene::NodeId, scene::components::Ids>& extruded()
{
    static const std::map<scene::NodeId, scene::components::Ids> none;
    const lo::Op* op = lo::current();
    return op && extrudeLive() ? op->made : none;
}

scene::PlugRef modifierPlug(scene::NodeId mod, const char* name)
{
    scene::Node* n = scene::Graph::current().node(mod);
    return {mod, n ? n->type().attrIndex(name) : -1, -1};
}

double modifierValue(scene::NodeId mod, const char* name)
{
    scene::Node* n = scene::Graph::current().node(mod);
    return n ? n->as<double>(n->type().attrIndex(name)) : 0.0;
}

/** Sets a modifier plug without an undo step, for the ticks of a drag. */
void setQuietly(scene::NodeId mod, const char* name, double value)
{
    UndoStack& undo = UndoStack::get();
    const bool was = undo.enabled();
    undo.setEnabled(false);
    scene::Graph::current().setValue(modifierPlug(mod, name), value);
    undo.setEnabled(was);
}

double flag(const char* name)
{
    if (!extrudeLive())
        if (scene::NodeId mod = selectedModifier())
            return modifierValue(mod, name);
    const lo::Op* op = lo::current();
    const CommandDef* def = ops::find(kCommand);
    if (!op || !def)
        return 0.0;
    for (const FlagDef& f : def->flags)
        if (f.name == name)
        {
            json v = lo::flagValue(*op, f);
            return v.is_number() ? v.get<double>() : 0.0;
        }
    return 0.0;
}

/** Face or edge, whichever extrude is live in. */
scene::ComponentMode extrudeKind(scene::NodeId modifier = scene::kNoNode)
{
    if (modifier)
        if (auto r = scene::extrudeModifier::resolve(scene::Graph::current(), modifier))
            return r->kind;
    const lo::Op* op = lo::current();
    const scene::ComponentMode mode = op && extrudeLive() ? op->modeAfter : scene::components::mode();
    return mode == scene::ComponentMode::Edge ? scene::ComponentMode::Edge : scene::ComponentMode::Face;
}

double handlePixels() { return PrefStore::get().value<double>("general", "/manipulator/size", 100.0); }

double segmentDistance(QPointF p, QPointF a, QPointF b)
{
    QPointF d = b - a;
    double len2 = QPointF::dotProduct(d, d);
    double t = len2 > 0 ? std::clamp(QPointF::dotProduct(p - a, d) / len2, 0.0, 1.0) : 0.0;
    QPointF q = a + d * t;
    return std::hypot(p.x() - q.x(), p.y() - q.y());
}
} // namespace

bool ExtrudeHandles::active() const
{
    if (dragging())
        return true;
    if (!extrudeLive())
        return selectedModifier() != scene::kNoNode;
    return (scene::components::mode() == scene::ComponentMode::Face || scene::components::mode() == scene::ComponentMode::Edge) &&
           !extruded().empty();
}

bool ExtrudeHandles::modifierFrame(uint64_t modifier, Frame& f) const
{
    LILA_ZONE();

    scene::Graph& g = scene::Graph::current();
    auto r = scene::extrudeModifier::resolve(g, modifier);
    scene::Node* mesh = g.node(scene::modifiers::meshOf(g, modifier));
    if (!r || r->ids.empty() || !mesh)
        return false;
    const geo::Mesh& m = *r->input;
    const bool edges = r->kind == scene::ComponentMode::Edge;
    const glm::dmat4 world = mesh->as<glm::dmat4>(scene::Transform::kWorldMatrix);
    const glm::dmat3 w(world);
    const glm::dmat3 normalXform = glm::transpose(glm::inverse(w));
    glm::dvec3 normalSum(0.0), outSum(0.0), centre(0.0);
    for (geo::Index id : r->ids)
    {
        geo::Index face = id;
        if (edges)
        {
            const geo::Index c = m.edgeCorner(id);
            face = m.cornerFace(c);
            const glm::dvec3 a = m.position(m.cornerPoint(c)), b = m.position(m.cornerNextPoint(c));
            outSum += glm::cross(b - a, m.faceNormal(face)) / std::max(1e-12, glm::length(b - a));
            centre += (a + b) * 0.5;
        }
        else
            centre += m.faceCentroid(face);
        normalSum += m.faceNormal(face);
    }
    if (glm::length(normalSum) < 1e-12)
        return false;
    const glm::dvec3 localNormal = glm::normalize(normalSum);
    const glm::dvec3 nw = normalXform * localNormal;
    if (glm::length(nw) < 1e-12)
        return false;
    f.normal = glm::normalize(nw);
    f.worldPerLocal = std::max(1e-9, std::abs(glm::dot(w * localNormal, f.normal)));
    // The handles sit on the cap, where the extrusion put the faces.
    glm::dvec3 local = centre / double(r->ids.size()) + localNormal * modifierValue(modifier, "thickness");
    const glm::dvec3 out = outSum - localNormal * glm::dot(outSum, localNormal);
    if (edges && glm::length(out) > 1e-9)
        local += glm::normalize(out) * modifierValue(modifier, "offset");
    f.center = glm::dvec3(world * glm::dvec4(local, 1.0));
    const glm::dvec3 ref = std::abs(f.normal.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
    const glm::dvec3 t1 = glm::normalize(glm::cross(f.normal, ref));
    f.side = glm::normalize(t1 + glm::cross(f.normal, t1));
    if (edges && glm::length(w * out) > 1e-9)
        f.side = glm::normalize(w * out);
    return true;
}

bool ExtrudeHandles::frame(Frame& f) const
{
    LILA_ZONE();

    if (const uint64_t mod = dragging() ? mDrag.modifier : extrudeLive() ? scene::kNoNode : selectedModifier())
        return modifierFrame(mod, f);

    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    const bool edges = extrudeKind() == scene::ComponentMode::Edge;
    glm::dvec3 normalSum(0.0), outSum(0.0), pointSum(0.0);
    size_t points = 0;
    bool scaled = false;
    for (auto& [node, ids] : extruded())
    {
        scene::Node* n = g.node(node);
        scene::MeshPtr cage = n && n->type().isA("mesh") ? scene::modifiers::cage(g, node) : nullptr;
        if (!cage)
            continue;
        const geo::Mesh& m = *cage;
        const glm::dmat4 world = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        glm::dmat3 w(world);
        glm::dmat3 normalXform = glm::transpose(glm::inverse(w));
        glm::dvec3 localSum(0.0);
        for (geo::Index p : scene::components::convert(m, extrudeKind(), ids, scene::ComponentMode::Vertex))
            if (m.pointValid(p))
            {
                pointSum += glm::dvec3(world * glm::dvec4(m.position(p), 1.0));
                ++points;
            }
        for (geo::Index id : ids)
        {
            geo::Index face = id;
            if (edges)
            {
                // A selected edge is a strip's outer edge; the strip starts flat, so the face it grew from sets the frame.
                if (id >= m.edgeCapacity() || !m.edgeValid(id) || m.edgeCorner(id) == geo::kInvalid)
                    continue;
                geo::Index from = geo::kInvalid;
                m.forEachFaceCorner(m.cornerFace(m.edgeCorner(id)), [&](geo::Index c) {
                    if (const geo::Index r = m.cornerRadial(c); r != c && from == geo::kInvalid)
                        from = r;
                });
                if (from == geo::kInvalid)
                    continue;
                face = m.cornerFace(from);
                const glm::dvec3 d = m.position(m.cornerNextPoint(from)) - m.position(m.cornerPoint(from));
                outSum += w * glm::cross(d, m.faceNormal(face)) / std::max(1e-12, glm::length(d));
            }
            if (face < m.faceCapacity() && m.faceValid(face))
            {
                glm::dvec3 nl = m.faceNormal(face);
                localSum += nl;
                normalSum += normalXform * nl;
            }
        }
        if (!scaled && glm::length(localSum) > 1e-12)
        {
            // Thickness is in the mesh's own units along its own normals.
            glm::dvec3 worldStep = w * glm::normalize(localSum);
            glm::dvec3 nw = normalXform * glm::normalize(localSum);
            if (glm::length(nw) > 1e-12)
                f.worldPerLocal = std::max(1e-9, std::abs(glm::dot(worldStep, glm::normalize(nw))));
            scaled = true;
        }
    }
    if (glm::length(normalSum) < 1e-12 || points == 0)
        return false;
    f.normal = glm::normalize(normalSum);
    f.center = pointSum / double(points);
    // The offset handle sits on a diagonal of the cap's plane, clear of the move manipulator's axis arrows.
    glm::dvec3 ref = std::abs(f.normal.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
    glm::dvec3 t1 = glm::normalize(glm::cross(f.normal, ref));
    glm::dvec3 t2 = glm::cross(f.normal, t1);
    f.side = glm::normalize(t1 + t2);
    if (edges)
        if (glm::dvec3 o = outSum - f.normal * glm::dot(outSum, f.normal); glm::length(o) > 1e-9)
            f.side = glm::normalize(o);
    return true;
}

bool ExtrudeHandles::layout(const Frame& f, const ViewInfo& view, QPointF& base, QPointF& tip, QPointF& offset) const
{
    double L = handlePixels() * view.unitsPerPixel(f.center);
    auto b = view.project(f.center + f.normal * L * 1.05), t = view.project(f.center + f.normal * L * 1.45),
         o = view.project(f.center + f.side * L * 0.75);
    if (!b || !t || !o)
        return false;
    base = *b, tip = *t, offset = *o;
    return true;
}

H ExtrudeHandles::pick(QPointF px, const ViewInfo& view) const
{
    if (!active())
        return H::None;
    Frame f;
    QPointF base, tip, offset;
    if (!frame(f) || !layout(f, view, base, tip, offset))
        return H::None;
    if (std::abs(px.x() - offset.x()) <= 8 && std::abs(px.y() - offset.y()) <= 8)
        return H::Offset;
    if (segmentDistance(px, base, tip) <= 8 || std::hypot(px.x() - tip.x(), px.y() - tip.y()) <= 11)
        return H::Thickness;
    return H::None;
}

void ExtrudeHandles::draw(OverlayPainter& p, const ViewInfo& view, Handle hover) const
{
    LILA_ZONE();

    Frame f = mDrag.frame;
    if (!dragging() && !frame(f))
        return;
    if (dragging())
    {
        // The live frame follows the cap while the start frame anchors the drag.
        Frame now;
        if (frame(now))
            f = now;
    }
    QPointF base, tip, offset;
    auto c = view.project(f.center);
    if (!c || !layout(f, view, base, tip, offset))
        return;
    auto colorOf = [&](Handle h) { return (dragging() ? mDrag.handle == h : hover == h) ? kHot : kGray; };

    glm::vec4 tc = colorOf(H::Thickness);
    double len = std::hypot(tip.x() - c->x(), tip.y() - c->y());
    if (len > 12)
    {
        QPointF dir = (tip - *c) / len;
        QPointF n(-dir.y(), dir.x());
        bool hot = tc == kHot;
        p.dashedLine(*c + dir * 10.0, base, 1.0f, 4.0f, {tc.r, tc.g, tc.b, 0.5f});
        p.line(base, tip - dir * 6.0, hot ? 3.5f : 2.0f, tc);
        p.triangle(tip + dir * 9.0, tip - dir * 4.0 + n * 6.0, tip - dir * 4.0 - n * 6.0, tc);
    }
    glm::vec4 oc = colorOf(H::Offset);
    p.dashedLine(*c, offset, 1.0f, 4.0f, {oc.r, oc.g, oc.b, 0.5f});
    QRectF box(offset.x() - 5, offset.y() - 5, 10, 10);
    p.fill({box.topLeft(), box.topRight(), box.bottomRight(), box.bottomLeft()}, {oc.r, oc.g, oc.b, oc == kHot ? 0.6f : 0.25f});
    p.polyline({box.topLeft(), box.topRight(), box.bottomRight(), box.bottomLeft()}, oc == kHot ? 2.0f : 1.5f, oc, true);

    if (dragging())
    {
        bool thickness = mDrag.handle == H::Thickness;
        p.label(mDrag.lastPx + QPointF(18, -14), QString("%1 %2 cm").arg(thickness ? "Thickness" : "Offset").arg(mDrag.value, 0, 'f', 2));
    }
    else if (hover != H::None)
        p.text((hover == H::Thickness ? tip : offset) + QPointF(14, 4), hover == H::Thickness ? "thickness" : "offset", kGray);
}

void ExtrudeHandles::begin(Handle h, QPointF px, const ViewInfo& view)
{
    Drag d;
    if (h == H::None || !frame(d.frame))
        return;
    d.modifier = extrudeLive() ? scene::kNoNode : selectedModifier();
    if (!d.modifier)
        lo::endDrag();
    d.handle = h;
    d.startThickness = flag("thickness");
    d.startOffset = flag("offset");
    d.value = h == H::Thickness ? d.startThickness : d.startOffset;
    d.lastPx = px;
    glm::dvec3 o, dir;
    view.ray(px, o, dir);
    d.startParam = gizmo::lineParam(d.frame.center, h == H::Thickness ? d.frame.normal : d.frame.side, o, dir);
    mDrag = d;
}

void ExtrudeHandles::drag(QPointF px, const ViewInfo& view)
{
    if (!dragging())
        return;
    mDrag.lastPx = px;
    glm::dvec3 o, dir;
    view.ray(px - mDrag.warp, o, dir);
    bool thickness = mDrag.handle == H::Thickness;
    const Frame& f = mDrag.frame;
    double moved = (gizmo::lineParam(f.center, thickness ? f.normal : f.side, o, dir) - mDrag.startParam) / f.worldPerLocal;
    // Pulling the offset square toward the centre insets the cap further.
    // Edges offset outward, so their square pulls the other way.
    const double sign = extrudeKind(mDrag.modifier) == scene::ComponentMode::Edge ? 1.0 : -1.0;
    double value = thickness ? mDrag.startThickness + moved : mDrag.startOffset + sign * moved;
    if (IncrementSnap inc = tools::incrementSnap(Tool::Move); inc.enabled && !(tools::snaps() & (SnapGrid | SnapEdge | SnapVertex)))
        value = gizmo::snapValue(value, inc.step);
    if (std::abs(value - mDrag.value) < 1e-9)
        return;
    if (mDrag.modifier)
    {
        setQuietly(mDrag.modifier, thickness ? "thickness" : "offset", value);
        mDrag.value = value;
        return;
    }
    try
    {
        lo::rerun({{thickness ? "thickness" : "offset", value}}, CallSource::Script, true);
        mDrag.value = value;
    }
    catch (const CommandError& e)
    {
        log::warn("extrude: {}", e.what());
    }
}

void ExtrudeHandles::end()
{
    if (!dragging())
        return;
    if (const Drag d = std::exchange(mDrag, {}); d.modifier)
    {
        const bool thickness = d.handle == H::Thickness;
        const char* name = thickness ? "thickness" : "offset";
        setQuietly(d.modifier, name, thickness ? d.startThickness : d.startOffset);
        try
        {
            ops::execute("setAttr", {{"args", {scene::Graph::current().plugName(modifierPlug(d.modifier, name)), d.value}}}, CallSource::UI);
        }
        catch (const CommandError& e)
        {
            log::error("{}", e.what());
        }
        return;
    }
    lo::endDrag();
    if (const lo::Op* op = lo::current())
        log::write(LogLevel::Echo, ops::toPython(op->command, op->call) + "\n");
}

void ExtrudeHandles::cancel()
{
    if (!dragging())
        return;
    if (const Drag d = std::exchange(mDrag, {}); d.modifier)
    {
        setQuietly(d.modifier, "thickness", d.startThickness);
        setQuietly(d.modifier, "offset", d.startOffset);
        return;
    }
    lo::endDrag(false);
}
} // namespace lila::ui
