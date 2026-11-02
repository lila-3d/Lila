#include "curve/CurveControls.h"

#include "core/Profile.h"
#include "curve/CurveDeform.h"
#include "curve/CurveNodes.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/RigUtil.h"
#include "scene/Transform.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <map>
#include <set>

namespace lila::curve
{
using namespace scene;
using json = nlohmann::json;
using D3 = glm::dvec3;
using T = ArgType;

namespace
{
constexpr const char* kControlMatrix = "controls.matrix";
constexpr const char* kControlPoint = "controls.point";
constexpr const char* kControlOffset = "controls.offset";

struct ControlAttrs
{
    int matrix = -1, point = -1, offset = -1;
    bool valid() const { return matrix >= 0 && point >= 0 && offset >= 0; }
};

ControlAttrs controlAttrs(const Node& n)
{
    const NodeType& t = n.type();
    return {t.attrIndex(kControlMatrix), t.attrIndex(kControlPoint), t.attrIndex(kControlOffset)};
}

/** Moves points first so their handles ride along, then places driven handles, then lets auto handles follow. */
void applyDriven(Spline& s, std::vector<std::pair<uint32_t, D3>>& moves)
{
    std::ranges::sort(moves, [](const auto& a, const auto& b) { return (a.first & 3) < (b.first & 3); });
    const bool bezier = s.kind == Kind::Bezier && s.handleL.size() == s.size() && s.typeL.size() == s.size();
    for (const auto& [id, p] : moves)
    {
        size_t si, i;
        PointRole role;
        decodeComponent(id, &si, &i, &role);
        if (i >= s.size())
            continue;
        if (role == PointRole::Point)
        {
            const D3 delta = p - s.points[i];
            s.points[i] = p;
            if (bezier)
            {
                s.handleL[i] += delta;
                s.handleR[i] += delta;
            }
        }
        else if (bezier && (role == PointRole::HandleL || role == PointRole::HandleR))
        {
            const bool left = role == PointRole::HandleL;
            (left ? s.handleL : s.handleR)[i] = p;
            (left ? s.typeL : s.typeR)[i] = HandleType::Free;
        }
    }
    if (bezier)
        updateHandles(s);
}


std::vector<Node*> curvesArg(const Args& a, const char* cmd)
{
    Graph& g = Graph::current();
    std::vector<Node*> out;
    if (a.positional.empty())
    {
        for (NodeId id : g.selection())
            if (Node* n = g.node(id); n && n->type().isA("curve"))
                out.push_back(n);
        if (out.empty())
            throw CommandError(std::string(cmd) + ": select a curve");
        return out;
    }
    for (const json& j : a.positional)
    {
        Node* n = g.find(j.get<std::string>());
        if (!n)
            throw CommandError(std::string(cmd) + ": no node named '" + j.get<std::string>() + "'");
        if (!n->type().isA("curve"))
            throw CommandError(std::string(cmd) + ": " + n->name() + " is not a curve");
        out.push_back(n);
    }
    return out;
}

D3 worldPoint(const glm::dmat4& m, const D3& p) { return D3(m * glm::dvec4(p, 1.0)); }

/** The control names' side token by where the point sits across the mirror plane, and its pair number. */
struct Naming
{
    std::vector<int> number;
    std::vector<int> side; // 0 none, 1 left (positive), 2 right
    std::vector<int> partner;
};

Naming nameControls(const std::vector<D3>& world, bool mirror, int axis, double tolerance)
{
    Naming out;
    const size_t n = world.size();
    out.number.assign(n, -1);
    out.side.assign(n, 0);
    out.partner.assign(n, -1);
    int next = 0;
    for (size_t i = 0; i < n; ++i)
    {
        if (out.number[i] >= 0)
            continue;
        out.number[i] = next++;
        if (!mirror || std::abs(world[i][axis]) <= tolerance)
            continue;
        D3 m = world[i];
        m[axis] = -m[axis];
        for (size_t j = i + 1; j < n; ++j)
            if (out.number[j] < 0 && glm::length(world[j] - m) <= tolerance)
            {
                out.number[j] = out.number[i];
                out.partner[i] = int(j);
                out.partner[j] = int(i);
                out.side[i] = world[i][axis] > 0.0 ? 1 : 2;
                out.side[j] = world[j][axis] > 0.0 ? 1 : 2;
                break;
            }
    }
    return out;
}

Node* makeTransform(Graph& g, const char* type, const std::string& name, NodeId parent, const D3& translate)
{
    Node* n = rigutil::guarded([&] { return g.createNode(*types::find(type), name, parent, true); });
    if (translate != D3(0.0))
        g.setValue({n->id(), Transform::kTranslate, -1}, translate);
    return n;
}

json buildControls(Graph& g, Node& c, const Args& a)
{
    const ControlAttrs at = controlAttrs(c);
    if (!at.valid())
        throw CommandError("curveControls: " + c.name() + " can't take controls");
    if (hasControls(c) || !g.groupIndices(c.id(), "controls").empty())
        throw CommandError("curveControls: " + c.name() + " already has controls; remove them first (removeCurveControls)");

    // Controls drive the stored shape, so history feeding the curve is frozen into it.
    PlugRef create{c.id(), CurveShape::kCreate, -1};
    g.evaluate();
    Splines splines = splinesOf(g.value(create));
    if (splines.empty())
        throw CommandError("curveControls: " + c.name() + " has no splines");
    const int count = a.get<int>("count", 0);
    if (count > 0)
        for (Spline& s : splines)
            if (size_t(count) != s.size())
            {
                Spline even = evenSpacing(s, true, size_t(count));
                if (isValid(even))
                    s = std::move(even);
            }
    if (g.source(create))
        g.disconnect(create);
    g.setValue(create, makeCurveData(splines));
    g.evaluate();

    const bool handles = a.get<bool>("handles", false);
    const bool mirror = a.get<bool>("mirror", false);
    const std::string axisName = a.get<std::string>("mirrorAxis", "x");
    const int axis = axisName == "y" ? 1 : axisName == "z" ? 2 : 0;
    const glm::dmat4 curveWorld = c.as<glm::dmat4>(Transform::kWorldMatrix);
    const std::string base = a.get<std::string>("name", c.name());

    D3 lo(1e300), hi(-1e300);
    std::vector<D3> world;
    std::vector<uint32_t> ids;
    for (size_t si = 0; si < splines.size(); ++si)
        for (size_t i = 0; i < splines[si].size(); ++i)
        {
            world.push_back(worldPoint(curveWorld, splines[si].points[i]));
            ids.push_back(componentId(si, i, PointRole::Point));
            lo = glm::min(lo, world.back());
            hi = glm::max(hi, world.back());
        }
    const double extent = std::max(glm::length(hi - lo), 1e-3);
    const double size = a.get<double>("size", 0.0) > 0.0 ? a.get<double>("size", 0.0) : std::max(0.5, extent * 0.04);
    const Naming names = nameControls(world, mirror, axis, std::max(1e-4, extent * 1e-4));

    const NodeId parent = g.parent(c.id());
    Node* top = makeTransform(g, "transform", base + "_controls", parent, D3(0.0));
    g.evaluate();
    const glm::dmat4 topInv = glm::inverse(top->as<glm::dmat4>(Transform::kWorldMatrix));

    struct Made
    {
        NodeId control;
        uint32_t id;
        D3 world;
    };
    std::vector<Made> made;
    json result = json::array();
    auto addControl = [&](const std::string& name, NodeId under, const D3& at, double drawSize, uint32_t id, const D3& w) {
        Node* grp = makeTransform(g, "transform", name + "_grp", under, at);
        Node* ctl = makeTransform(g, "locator", name, grp->id(), D3(0.0));
        g.setValue({ctl->id(), ctl->type().attrIndex("drawSize"), -1}, drawSize);
        made.push_back({ctl->id(), id, w});
        result.push_back(ctl->path());
        return ctl;
    };
    static const char* kSide[] = {"", "_l", "_r"};
    for (size_t k = 0; k < ids.size(); ++k)
    {
        size_t si, i;
        PointRole role;
        decodeComponent(ids[k], &si, &i, &role);
        const std::string name = base + "_ctrl" + std::to_string(names.number[k]) + kSide[names.side[k]];
        Node* ctl = addControl(name, top->id(), worldPoint(topInv, world[k]), size, ids[k], world[k]);
        const Spline& s = splines[si];
        if (!handles || s.kind != Kind::Bezier || s.handleL.size() != s.size())
            continue;
        g.evaluate();
        const glm::dmat4 ctlInv = glm::inverse(ctl->as<glm::dmat4>(Transform::kWorldMatrix));
        const D3 wl = worldPoint(curveWorld, s.handleL[i]), wr = worldPoint(curveWorld, s.handleR[i]);
        // A mirrored partner traversed the other way names its handles swapped, so in and out match across the pair.
        bool swap = false;
        if (const int p = names.partner[k]; p >= 0 && size_t(p) < k)
        {
            size_t ps, pi;
            PointRole pr;
            decodeComponent(ids[size_t(p)], &ps, &pi, &pr);
            const Spline& sp = splines[ps];
            if (sp.kind == Kind::Bezier && sp.handleL.size() == sp.size())
            {
                D3 ml = worldPoint(curveWorld, sp.handleL[pi]);
                ml[axis] = -ml[axis];
                swap = glm::length(ml - wr) < glm::length(ml - wl);
            }
        }
        addControl(name + (swap ? "_out" : "_in"), ctl->id(), worldPoint(ctlInv, wl), size * 0.5, componentId(si, i, PointRole::HandleL), wl);
        addControl(name + (swap ? "_in" : "_out"), ctl->id(), worldPoint(ctlInv, wr), size * 0.5, componentId(si, i, PointRole::HandleR), wr);
    }

    g.evaluate();
    for (size_t k = 0; k < made.size(); ++k)
    {
        Node* ctl = g.node(made[k].control);
        const int e = int(k);
        rigutil::guarded([&] { return g.connect({ctl->id(), Transform::kWorldMatrix, -1}, {c.id(), at.matrix, -1, e}); });
        g.setValue({c.id(), at.point, -1, e}, int64_t(made[k].id));
        const glm::dmat4 offset = glm::translate(glm::dmat4(1.0), worldPoint(glm::inverse(ctl->as<glm::dmat4>(Transform::kWorldMatrix)), made[k].world));
        g.setValue({c.id(), at.offset, -1, e}, offset);
    }
    return result;
}

/** Removes the curve's controls, leaving its shape where they put it. Returns whether it had any. */
bool removeControls(Graph& g, Node& c)
{
    const ControlAttrs at = controlAttrs(c);
    if (!at.valid())
        return false;
    const std::vector<int> elements = g.groupIndices(c.id(), "controls");
    if (elements.empty())
        return false;
    g.evaluate();
    PlugRef create{c.id(), CurveShape::kCreate, -1};
    Splines shape = drivenSplines(c, splinesOf(g.value(create)));
    std::set<NodeId> controls;
    for (int e : elements)
    {
        if (auto src = g.source({c.id(), at.matrix, -1, e}))
            controls.insert(src->node);
        for (int attr : {at.matrix, at.point, at.offset})
            g.removeElement({c.id(), attr, -1, e});
    }
    if (g.source(create))
        g.disconnect(create);
    g.setValue(create, makeCurveData(std::move(shape)));

    std::set<NodeId> parents;
    for (NodeId id : controls)
        if (g.node(id))
        {
            parents.insert(g.parent(id));
            g.deleteNode(id);
        }
    // Offset groups and the controls group go once nothing else is left in them.
    while (!parents.empty())
    {
        std::set<NodeId> next;
        for (NodeId p : parents)
        {
            Node* n = g.node(p);
            if (!n || n->type().name != "transform" || !g.children(p).empty())
                continue;
            next.insert(g.parent(p));
            g.deleteNode(p);
        }
        next.erase(kNoNode);
        parents = std::move(next);
    }
    return true;
}
} // namespace

void addControlAttrs(NodeType& t)
{
    t.ownAttrs.push_back({.name = kControlMatrix, .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrInput | AttrHidden | AttrArray,
                          .doc = "World matrix of a control driving one of the curve's points or handles (curveControls)."});
    t.ownAttrs.push_back({.name = kControlPoint, .type = PlugType::Int, .defaultValue = int64_t(-1),
                          .flags = AttrInput | AttrStorable | AttrHidden | AttrArray, .doc = "The component the control drives, as a component id."});
    t.ownAttrs.push_back({.name = kControlOffset, .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0),
                          .flags = AttrInput | AttrStorable | AttrHidden | AttrArray,
                          .doc = "The component in the control's space: its translation is where the control holds it."});
}

bool hasControls(const Node& curve)
{
    const int a = curve.type().attrIndex(kControlMatrix);
    return a >= 0 && !curve.elements(a).empty();
}

Splines drivenSplines(const Node& curve, Splines s)
{
    LILA_ZONE();

    const ControlAttrs at = controlAttrs(curve);
    if (!at.valid() || s.empty())
        return s;
    const glm::dmat4 toObject = glm::inverse(curve.as<glm::dmat4>(Transform::kWorldMatrix));
    std::map<size_t, std::vector<std::pair<uint32_t, D3>>> bySpline;
    for (const auto& [e, v] : curve.elements(at.matrix))
    {
        const glm::dmat4* m = std::get_if<glm::dmat4>(&v);
        const int64_t id = curve.elementOr<int64_t>(at.point, e);
        if (!m || id < 0)
            continue;
        const D3 world = D3((*m * curve.elementOr<glm::dmat4>(at.offset, e))[3]);
        bySpline[size_t(id) >> 22].push_back({uint32_t(id), worldPoint(toObject, world)});
    }
    for (auto& [si, moves] : bySpline)
        if (si < s.size())
            applyDriven(s[si], moves);
    return s;
}

void registerCurveControlCommands()
{
    ops::registerCommand({
        .name = "curveControls",
        .doc = "Build controls that drive a curve's points: a locator per control point (in an offset group, under a "
               "<curve>_controls group beside the curve), optionally tangent handle controls under each Bezier point. The curve "
               "follows them live. Returns the controls.",
        .positionalDoc = "curves (default: the selected curves)",
        .flags = {{"count", "c", T::Int, 0, "Controls per spline: 0 for one per existing point, else the curve is respaced to this many points first.",
                   {}, {}, 0.0},
                  {"handles", "hd", T::Bool, false, "Bezier curves: a control for each tangent handle, under its point's control."},
                  {"mirror", "m", T::Bool, false, "Name mirrored pairs of controls _l and _r so pose mirroring swaps them."},
                  {"mirrorAxis", "ma", T::String, "x", "The axis the mirror plane faces.", {"x", "y", "z"}},
                  {"size", "s", T::Float, 0.0, "Control draw size, cm; 0 sizes them to the curve.", {}, {}, 0.0},
                  {"name", "n", T::String, nullptr, "Name prefix for the controls; default the curve's name."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            json out = json::array();
            for (Node* c : curvesArg(a, "curveControls"))
                for (json& n : buildControls(g, *c, a))
                    out.push_back(std::move(n));
            return out;
        },
        .examples = {"curveControls(\"Spine\", handles=True)", "curveControls(\"Lip\", count=7, mirror=True)"},
    });

    ops::registerCommand({
        .name = "removeCurveControls",
        .doc = "Delete a curve's controls, keeping the shape they gave it. Returns the curves.",
        .positionalDoc = "curves (default: the selected curves)",
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            json out = json::array();
            for (Node* c : curvesArg(a, "removeCurveControls"))
                if (removeControls(g, *c))
                    out.push_back(c->path());
            if (out.empty())
                throw CommandError("removeCurveControls: none of the curves has controls");
            return out;
        },
    });
}
} // namespace lila::curve
