#include "curve/CurveNodes.h"

#include "core/Profile.h"
#include "curve/EdgeToCurve.h"
#include "curve/Fit.h"
#include "curve/Ops.h"
#include "curve/PipeNode.h"
#include "ops/Command.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <glm/gtc/constants.hpp>

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
Graph& graph() { return Graph::current(); }

std::vector<D3> pointsArg(const Args& a, const char* flag)
{
    std::vector<D3> out;
    if (!a.has(flag))
        return out;
    for (const json& p : a.flags[flag])
    {
        if (!p.is_array() || p.size() != 3)
            throw CommandError(std::string(flag) + ": expected [[x, y, z], ...]");
        out.emplace_back(p[0].get<double>(), p[1].get<double>(), p[2].get<double>());
    }
    return out;
}

Node& curveNamed(const std::string& name, const char* cmd)
{
    Node* n = graph().find(name);
    if (!n)
        throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
    if (!n->type().isA("curve"))
        throw CommandError(std::string(cmd) + ": " + name + " is not a curve");
    return *n;
}

/** The curves among the given names (or the selection, and the curves holding selected points). */
std::vector<Node*> curvesOf(const Args& a, const char* cmd, size_t from = 0)
{
    std::vector<Node*> out;
    Graph& g = graph();
    std::vector<std::string> names;
    for (size_t k = from; k < a.positional.size(); ++k)
        names.push_back(a.positional[k].get<std::string>());
    if (names.empty() && from == 0)
    {
        for (NodeId id : g.selection())
            if (Node* n = g.node(id); n && n->type().isA("curve"))
                out.push_back(n);
        for (auto& [id, ids] : components::selected(ComponentMode::Vertex))
            if (Node* n = g.node(id); n && n->type().isA("curve") && std::ranges::find(out, n) == out.end())
                out.push_back(n);
        if (out.empty())
            throw CommandError(std::string(cmd) + ": select a curve");
        return out;
    }
    for (const std::string& name : names)
        out.push_back(&curveNamed(name, cmd));
    return out;
}

Node* createCurve(Graph& g, Splines s, const std::string& name)
{
    for (const Spline& sp : s)
    {
        std::string why;
        if (!isValid(sp, &why))
            throw CommandError("curve: " + why);
    }
    Node* n = g.createNode(*types::find("curve"), name.empty() ? "Curve" : name, kNoNode, true);
    g.setValue({n->id(), CurveShape::kCreate, -1}, makeCurveData(std::move(s)));
    return n;
}

Splines storedSplines(Graph& g, Node& n)
{
    g.evaluate();
    return splinesOf(g.value({n.id(), CurveShape::kLocal, -1}));
}

/** Replaces a curve's splines, freezing any history feeding it. */
void setSplines(Graph& g, Node& n, Splines s)
{
    PlugRef create{n.id(), CurveShape::kCreate, -1};
    if (g.source(create))
        g.disconnect(create);
    g.setValue(create, makeCurveData(std::move(s)));
}

/** Option flags for every editable attr of a node type (enums by name). */
std::vector<FlagDef> attrFlags(const std::string& type)
{
    std::vector<FlagDef> out;
    const NodeType* t = types::find(type);
    for (const AttrDef& d : t->ownAttrs)
    {
        if (!(d.flags & AttrInput) || (d.flags & AttrHidden))
            continue;
        FlagDef f{d.name, "", T::Float, nullptr, d.doc};
        switch (d.type)
        {
        case PlugType::Bool: f.type = T::Bool; break;
        case PlugType::Int: f.type = T::Int; break;
        case PlugType::Vec3: f.type = T::Vec3; break;
        case PlugType::Enum:
            f.type = T::String;
            f.choices = d.enumNames;
            break;
        default: break;
        }
        out.push_back(std::move(f));
    }
    return out;
}

void applyAttrFlags(Graph& g, Node& n, const Args& a)
{
    for (const AttrDef& d : n.type().ownAttrs)
    {
        if (!(d.flags & AttrInput) || (d.flags & AttrHidden) || !a.has(d.name))
            continue;
        int idx = n.type().attrIndex(d.name);
        const json& v = a.flags[d.name];
        if (d.type == PlugType::Enum && v.is_string())
        {
            auto it = std::ranges::find(d.enumNames, v.get<std::string>());
            if (it == d.enumNames.end())
                throw CommandError(d.name + ": unknown value '" + v.get<std::string>() + "'");
            g.setValue({n.id(), idx, -1}, int64_t(it - d.enumNames.begin()));
            continue;
        }
        try
        {
            g.setValue({n.id(), idx, -1}, fromJson(d, v));
        }
        catch (const std::exception& e)
        {
            throw CommandError(d.name + ": " + e.what());
        }
    }
}

std::vector<FlagDef> withFlags(std::vector<FlagDef> base, std::vector<FlagDef> extra)
{
    base.insert(base.end(), extra.begin(), extra.end());
    return base;
}

FlagDef chFlag() { return {"constructionHistory", "ch", T::Bool, true, "Keep the history node so its settings stay live."}; }

/** Bakes what a history node makes into its destination and drops the node. */
void bake(Graph& g, Node& op, const PlugRef& dst)
{
    g.evaluate();
    PlugValue v = g.value(dst);
    g.disconnect(dst);
    g.setValue(dst, v);
    g.deleteNode(op.id());
}

/** A curve to curve history node on each curve: in place (replaceOriginal) or into a new curve. */
json curveOp(const std::string& type, const Args& a, bool rpoDefault, const std::function<void(Graph&, Node& op, Node& curve)>& extra = {})
{
    Graph& g = graph();
    std::vector<Node*> curves = curvesOf(a, type.c_str());
    bool ch = a.get<bool>("constructionHistory", true), rpo = a.get<bool>("replaceOriginal", rpoDefault);
    json names = json::array();
    std::vector<NodeId> results;
    for (Node* c : curves)
    {
        Node* op = g.createNode(*types::find(type), {}, kNoNode, true);
        applyAttrFlags(g, *op, a);
        int inA = op->type().attrIndex("inCurve"), outA = op->type().attrIndex("outCurve");
        if (extra)
            extra(g, *op, *c);
        Node* result = c;
        PlugRef create{c->id(), CurveShape::kCreate, -1};
        if (rpo)
        {
            if (std::optional<PlugRef> src = g.source(create))
                g.connect(*src, {op->id(), inA, -1});
            else
            {
                // Re-stored first, so undoing the connection below brings the original value back rather than the last pulled one.
                g.setValue(create, makeCurveData(splinesOf(g.storedValue(create))));
                g.setValue({op->id(), inA, -1}, g.storedValue(create));
            }
            g.connect({op->id(), outA, -1}, create, true);
        }
        else
        {
            g.connect({c->id(), CurveShape::kWorldSpace, -1}, {op->id(), inA, -1});
            result = g.createNode(*types::find("curve"), a.get<std::string>("name", c->name()), kNoNode, true);
            g.connect({op->id(), outA, -1}, {result->id(), CurveShape::kCreate, -1});
        }
        if (!ch)
            bake(g, *op, {result->id(), CurveShape::kCreate, -1});
        results.push_back(result->id());
        names.push_back(result->path());
    }
    g.select(results);
    return names;
}

/** A curve to mesh history node feeding a new mesh. Returns [mesh, node]. */
json meshOp(const std::string& type, const Args& a, const std::function<void(Graph&, Node& op)>& connect, const char* meshName)
{
    Graph& g = graph();
    Node* op = g.createNode(*types::find(type), {}, kNoNode, true);
    applyAttrFlags(g, *op, a);
    connect(g, *op);
    Node* mesh = g.createNode(*types::find("mesh"), a.get<std::string>("name", meshName), kNoNode, true);
    int out = op->type().attrIndex("outMesh");
    g.connect({op->id(), out, -1}, {mesh->id(), MeshNode::kInMesh, -1});
    json opName = op->path();
    if (!a.get<bool>("constructionHistory", true))
    {
        bake(g, *op, {mesh->id(), MeshNode::kInMesh, -1});
        opName = nullptr;
    }
    g.select({mesh->id()});
    return json::array({mesh->path(), opName});
}

PlugRef worldSpace(Node& c) { return {c.id(), CurveShape::kWorldSpace, -1}; }

/** Point indices of a curve's selected points (vertex mode), per spline. */
std::map<size_t, std::set<size_t>> selectedPoints(NodeId id, bool anchorsOnly)
{
    std::map<size_t, std::set<size_t>> out;
    for (uint32_t c : components::selected(id, ComponentMode::Vertex))
    {
        size_t s, p;
        PointRole r;
        decodeComponent(c, &s, &p, &r);
        if (!anchorsOnly || r == PointRole::Point)
            out[s].insert(p);
    }
    return out;
}

Spline splineFrom(const Args& a, std::vector<D3> pts)
{
    Spline s;
    std::string type = a.get<std::string>("type", "nurbs");
    if (!parseKind(type, &s.kind))
        throw CommandError("curve: type is poly, bezier or nurbs");
    s.closed = a.get<bool>("closed", false);
    s.degree = a.get<int>("degree", 3);
    if (s.kind == Kind::Nurbs && a.get<bool>("editPoints", false))
        return interpolate(pts, s.degree, s.closed);
    s.points = std::move(pts);
    if (s.kind == Kind::Nurbs)
    {
        s.degree = std::clamp(s.degree, 1, std::max(1, int(s.size()) - 1));
        if (a.has("knots"))
            s.knots = a.flags["knots"].get<std::vector<double>>();
        if (a.has("weights"))
            s.weights = a.flags["weights"].get<std::vector<double>>();
    }
    if (s.kind == Kind::Bezier)
    {
        HandleType ht = HandleType::Auto;
        if (!parseHandleType(a.get<std::string>("handleType", "auto"), &ht))
            throw CommandError("curve: handleType is auto, vector, aligned or free");
        s.handleL = pointsArg(a, "handleL");
        s.handleR = pointsArg(a, "handleR");
        bool given = s.handleL.size() == s.size() && s.handleR.size() == s.size();
        if (!given)
        {
            s.handleL.assign(s.size(), D3(0.0));
            s.handleR.assign(s.size(), D3(0.0));
        }
        s.typeL.assign(s.size(), given && !a.has("handleType") ? HandleType::Free : ht);
        s.typeR = s.typeL;
        updateHandles(s);
    }
    for (const char* attr : {"tilt", "radius", "cornerRadius"})
        if (a.has(attr))
        {
            std::vector<double> v = a.flags[attr].get<std::vector<double>>();
            (std::string(attr) == "tilt" ? s.tilt : std::string(attr) == "radius" ? s.radius : s.cornerRadius) = v;
        }
    s.normalize();
    try
    {
        validate(s);
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string("curve: ") + e.what());
    }
    return s;
}

const std::vector<FlagDef> kCurveFlags = {
    {"points", "p", T::List, nullptr, "[[x, y, z], ...] cm: poly vertices, Bezier anchors, NURBS control points (or edit points with editPoints)."},
    {"type", "t", T::String, "nurbs", "Spline kind.", {"poly", "bezier", "nurbs"}},
    {"degree", "d", T::Int, 3, "NURBS degree (1 to 7)."},
    {"closed", "c", T::Bool, false, "A closed loop (periodic for NURBS)."},
    {"editPoints", "ep", T::Bool, false, "NURBS: the points are edit points the curve passes through."},
    {"knots", "k", T::List, nullptr, "NURBS knots: points + degree + 1 values, or points + 1 breakpoints when closed."},
    {"weights", "w", T::List, nullptr, "NURBS weights, one per point."},
    {"handleL", "", T::List, nullptr, "Bezier incoming handles, one per anchor."},
    {"handleR", "", T::List, nullptr, "Bezier outgoing handles, one per anchor."},
    {"handleType", "ht", T::String, nullptr, "Bezier handle type for every anchor.", {"auto", "vector", "aligned", "free"}},
    {"tilt", "", T::List, nullptr, "Twist per point, radians."},
    {"radius", "", T::List, nullptr, "Sweep scale per point."},
    {"cornerRadius", "", T::List, nullptr, "Fillet radius per point, cm."},
    {"name", "n", T::String, nullptr, "Name; made unique."},
    {"splines", "", T::List, nullptr, "Whole splines as curveData returns them, instead of points and the flags above."},
};
} // namespace

void registerCommands()
{
    ops::registerCommand({
        .name = "curve",
        .doc = "Create a curve: poly, Bezier or NURBS. Returns its name.",
        .flags = kCurveFlags,
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            Splines given;
            if (a.has("splines"))
                try
                {
                    for (const json& j : a.flags["splines"])
                        given.push_back(splineFromJson(j));
                }
                catch (const std::exception& e)
                {
                    throw CommandError(std::string("curve: ") + e.what());
                }
            else
                given.push_back(splineFrom(a, pointsArg(a, "points")));
            Node* n = createCurve(g, std::move(given), a.get<std::string>("name", "Curve"));
            g.select({n->id()});
            return n->path();
        },
    });

    ops::registerCommand({
        .name = "curveEP",
        .doc = "Create a NURBS curve through edit points. Returns its name.",
        .flags = {{"points", "p", T::List, nullptr, "[[x, y, z], ...] edit points, cm."},
                  {"degree", "d", T::Int, 3, "Degree (1 to 7)."},
                  {"closed", "c", T::Bool, false, "A closed loop through the points."},
                  {"name", "n", T::String, nullptr, "Name; made unique."}},
        .run = [](const Args& a) -> json {
            std::vector<D3> pts = pointsArg(a, "points");
            if (pts.size() < 2)
                throw CommandError("curveEP: give at least two points");
            Graph& g = graph();
            Node* n = createCurve(g, {interpolate(pts, a.get<int>("degree", 3), a.get<bool>("closed", false))}, a.get<std::string>("name", "Curve"));
            g.select({n->id()});
            return n->path();
        },
    });

    ops::registerCommand({
        .name = "bezierCurve",
        .doc = "Create a Bezier curve through anchors, with auto handles unless handles are given. Returns its name.",
        .flags = {{"points", "p", T::List, nullptr, "[[x, y, z], ...] anchors, cm."},
                  {"handleL", "", T::List, nullptr, "Incoming handles, one per anchor."},
                  {"handleR", "", T::List, nullptr, "Outgoing handles, one per anchor."},
                  {"handleType", "ht", T::String, nullptr, "Handle type for every anchor.", {"auto", "vector", "aligned", "free"}},
                  {"closed", "c", T::Bool, false, "A closed loop."},
                  {"name", "n", T::String, nullptr, "Name; made unique."}},
        .run = [](const Args& a0) -> json {
            Args a = a0;
            a.flags["type"] = "bezier";
            Graph& g = graph();
            Node* n = createCurve(g, {splineFrom(a, pointsArg(a, "points"))}, a.get<std::string>("name", "Curve"));
            g.select({n->id()});
            return n->path();
        },
    });

    ops::registerCommand({
        .name = "circle",
        .doc = "Create an exact circle (a rational NURBS). Returns its name.",
        .flags = {{"center", "c", T::Vec3, json::array({0, 0, 0}), "Centre, cm."},
                  {"normal", "nr", T::Vec3, json::array({0, 1, 0}), "Normal of its plane."},
                  {"radius", "r", T::Float, 10.0, "cm."},
                  {"name", "n", T::String, nullptr, "Name; made unique."}},
        .run = [](const Args& a) -> json {
            auto v = [&](const char* f) { return D3(a.flags[f][0].get<double>(), a.flags[f][1].get<double>(), a.flags[f][2].get<double>()); };
            Graph& g = graph();
            Node* n = createCurve(g, {circle(v("center"), v("normal"), a.get<double>("radius", 10.0))}, a.get<std::string>("name", "Circle"));
            g.select({n->id()});
            return n->path();
        },
    });

    ops::registerCommand({
        .name = "curveData",
        .doc = "A curve's splines: [{type, closed, degree, points, knots, weights, handleL, handleR, typeL, typeR, tilt, radius, "
               "cornerRadius}, ...], object space unless worldSpace=True. Also its length and spans.",
        .positionalDoc = "curve",
        .flags = {{"worldSpace", "ws", T::Bool, false, "World space."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::vector<Node*> cs = curvesOf(a, "curveData");
            Graph& g = graph();
            g.evaluate();
            Node& n = *cs.back();
            const Splines& s = splinesOf(g.value({n.id(), a.get<bool>("worldSpace") ? CurveShape::kWorldSpace : CurveShape::kLocal, -1}));
            json out = json::array();
            for (const Spline& sp : s)
            {
                json j = splineToJson(sp);
                j["length"] = length(sp);
                j["spans"] = spanCount(sp);
                out.push_back(j);
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "setCurveData",
        .doc = "Replace a curve's splines (object space), in curveData's format. Any history feeding it is frozen.",
        .positionalDoc = "curve",
        .flags = {{"splines", "s", T::List, nullptr, "Splines as curveData returns them."}},
        .run = [](const Args& a) -> json {
            std::vector<Node*> cs = curvesOf(a, "setCurveData");
            Splines s;
            try
            {
                for (const json& j : a.flags.value("splines", json::array()))
                    s.push_back(splineFromJson(j));
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("setCurveData: ") + e.what());
            }
            setSplines(graph(), *cs.back(), std::move(s));
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "extrude",
        .doc = "Sweep a profile along path curves into a quad mesh with live history: a circle or rectangle, or the profile "
               "curve given second (or with profile=). Returns [mesh, curveSweep].",
        .positionalDoc = "path [profile]",
        .flags = withFlags(attrFlags("curveSweep"), {{"profileCurve", "pc", T::String, nullptr, "Profile curve (instead of the second positional)."},
                                                     chFlag(),
                                                     {"name", "n", T::String, nullptr, "Mesh name; made unique."}}),
        .run = [](const Args& a) -> json {
            std::vector<Node*> cs = curvesOf(a, "extrude");
            Node* path = cs.front();
            Node* prof = cs.size() > 1 ? cs[1] : nullptr;
            if (a.has("profileCurve"))
                prof = &curveNamed(a.get<std::string>("profileCurve"), "extrude");
            return meshOp("curveSweep", a, [&](Graph& g, Node& op) {
                g.connect(worldSpace(*path), {op.id(), op.type().attrIndex("path"), -1});
                if (prof)
                    g.connect(worldSpace(*prof), {op.id(), op.type().attrIndex("profile"), -1});
            }, "Sweep");
        },
    });

    ops::registerCommand({
        .name = "revolve",
        .doc = "Revolve profile curves around an axis into a quad mesh with live history. Returns [mesh, curveRevolve].",
        .positionalDoc = "profiles",
        .flags = withFlags(attrFlags("curveRevolve"), {chFlag(), {"name", "n", T::String, nullptr, "Mesh name; made unique."}}),
        .run = [](const Args& a) -> json {
            std::vector<Node*> cs = curvesOf(a, "revolve");
            if (cs.size() > 1)
                throw CommandError("revolve: give one profile curve");
            return meshOp("curveRevolve", a, [&](Graph& g, Node& op) {
                g.connect(worldSpace(*cs[0]), {op.id(), op.type().attrIndex("profile"), -1});
            }, "Revolve");
        },
    });

    ops::registerCommand({
        .name = "loft",
        .doc = "Loft a quad mesh through section curves, in order, with live history; rails= steers it with one or two rail curves. "
               "Returns [mesh, curveLoft].",
        .positionalDoc = "sections",
        .flags = withFlags(attrFlags("curveLoft"), {{"rails", "", T::List, nullptr, "One or two rail curves."},
                                                    chFlag(),
                                                    {"name", "n", T::String, nullptr, "Mesh name; made unique."}}),
        .run = [](const Args& a) -> json {
            std::vector<Node*> cs = curvesOf(a, "loft");
            if (cs.size() < 2)
                throw CommandError("loft: give at least two section curves");
            if (cs.size() > 12)
                throw CommandError("loft: at most 12 section curves");
            std::vector<Node*> rails;
            for (const json& r : a.flags.value("rails", json::array()))
                rails.push_back(&curveNamed(r.get<std::string>(), "loft"));
            if (rails.size() > 2)
                throw CommandError("loft: at most two rails");
            return meshOp("curveLoft", a, [&](Graph& g, Node& op) {
                for (size_t k = 0; k < cs.size(); ++k)
                    g.connect(worldSpace(*cs[k]), {op.id(), op.type().attrIndex("section" + std::to_string(k)), -1});
                for (size_t k = 0; k < rails.size(); ++k)
                    g.connect(worldSpace(*rails[k]), {op.id(), op.type().attrIndex("rail" + std::to_string(k)), -1});
            }, "Loft");
        },
    });

    ops::registerCommand({
        .name = "patchFill",
        .doc = "Fill four boundary curves (in any order and direction, end to end) with a Coons patch of quads, live. Returns [mesh, curvePatch].",
        .positionalDoc = "curves",
        .flags = withFlags(attrFlags("curvePatch"), {chFlag(), {"name", "n", T::String, nullptr, "Mesh name; made unique."}}),
        .run = [](const Args& a) -> json {
            std::vector<Node*> cs = curvesOf(a, "patchFill");
            if (cs.size() != 4)
                throw CommandError("patchFill: give four boundary curves");
            return meshOp("curvePatch", a, [&](Graph& g, Node& op) {
                for (size_t k = 0; k < 4; ++k)
                    g.connect(worldSpace(*cs[k]), {op.id(), op.type().attrIndex("side" + std::to_string(k)), -1});
            }, "Patch");
        },
    });

    auto opFlags = [](const char* type, bool rpo) {
        return withFlags(attrFlags(type), {chFlag(),
                                           {"replaceOriginal", "rpo", T::Bool, rpo, "Change the curve itself rather than making a new one."},
                                           {"name", "n", T::String, nullptr, "Name of a new curve."}});
    };
    ops::registerCommand({
        .name = "filletCurve",
        .doc = "Round (arc), cut (chamfer) or blend (g2) the corners of curves. Per point cornerRadius wins over radius. Returns the curves.",
        .positionalDoc = "curves",
        .flags = opFlags("filletCurve", true),
        .run = [](const Args& a) -> json { return curveOp("filletCurve", a, true); },
    });
    ops::registerCommand({
        .name = "offsetCurve",
        .doc = "Offset curves within their plane by distance, trimming loops; makes new curves. Returns them.",
        .positionalDoc = "curves",
        .flags = opFlags("offsetCurve", false),
        .run = [](const Args& a) -> json { return curveOp("offsetCurve", a, false); },
    });
    ops::registerCommand({
        .name = "rebuildCurve",
        .doc = "Rebuild curves with even spans (poly, Bezier or NURBS), corners kept. Returns the curves.",
        .positionalDoc = "curves",
        .flags = opFlags("rebuildCurve", true),
        .run = [](const Args& a) -> json { return curveOp("rebuildCurve", a, true); },
    });
    ops::registerCommand({
        .name = "fitCurve",
        .doc = "Refit curves as Beziers with as few points as tolerance allows, corners kept. Returns the curves.",
        .positionalDoc = "curves",
        .flags = opFlags("fitCurve", true),
        .run = [](const Args& a) -> json { return curveOp("fitCurve", a, true); },
    });
    ops::registerCommand({
        .name = "projectCurve",
        .doc = "Project curves onto a mesh along direction (the view, say) or to the closest point, as new curves that follow the "
               "mesh while history is on. Returns them.",
        .positionalDoc = "curves",
        .flags = withFlags(opFlags("projectCurve", false), {{"mesh", "m", T::String, nullptr, "The mesh to project onto."}}),
        .run = [](const Args& a) -> json {
            if (!a.has("mesh"))
                throw CommandError("projectCurve: give mesh=");
            Node* mesh = graph().find(a.get<std::string>("mesh"));
            if (!mesh || !mesh->type().isA("mesh"))
                throw CommandError("projectCurve: " + a.get<std::string>("mesh") + " is not a mesh");
            return curveOp("projectCurve", a, false, [mesh](Graph& g, Node& op, Node&) {
                g.connect({mesh->id(), MeshNode::kOutMesh, -1}, {op.id(), op.type().attrIndex("inMesh"), -1});
                g.connect({mesh->id(), Transform::kWorldMatrix, -1}, {op.id(), op.type().attrIndex("meshMatrix"), -1});
            });
        },
    });

    ops::registerCommand({
        .name = "attachCurve",
        .doc = "Join the second curve onto the first at their nearest ends (merged when they meet, else by a straight or blended span). "
               "The first curve takes the result; the second is deleted unless keepOriginal. Returns the first.",
        .positionalDoc = "curve curve",
        .flags = {{"blend", "bl", T::Bool, false, "Join with a smooth span rather than a straight one."},
                  {"keepOriginal", "kp", T::Bool, false, "Keep the second curve."}},
        .run = [](const Args& a) -> json {
            std::vector<Node*> cs = curvesOf(a, "attachCurve");
            if (cs.size() != 2)
                throw CommandError("attachCurve: give two curves");
            Graph& g = graph();
            Splines sa = storedSplines(g, *cs[0]), sb = splinesOf(g.value(worldSpace(*cs[1])));
            if (sa.empty() || sb.empty())
                throw CommandError("attachCurve: a curve is empty");
            glm::dmat4 toA = glm::inverse(cs[0]->as<glm::dmat4>(Transform::kWorldMatrix));
            Spline b = sb.front();
            transform(b, toA);
            sa.front() = attach(sa.front(), b, a.get<bool>("blend"));
            setSplines(g, *cs[0], std::move(sa));
            if (!a.get<bool>("keepOriginal"))
                g.deleteNode(cs[1]->id());
            g.select({cs[0]->id()});
            return cs[0]->path();
        },
    });

    ops::registerCommand({
        .name = "detachCurve",
        .doc = "Split curves at parameter (span units) or, in vertex mode, at the selected points. Open curves become two, closed "
               "ones open there. Returns the curves.",
        .positionalDoc = "curves",
        .flags = {{"parameter", "p", T::Float, nullptr, "Where to split, in spans from the start."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            json names = json::array();
            std::vector<NodeId> sel;
            for (Node* c : curvesOf(a, "detachCurve"))
            {
                Splines s = storedSplines(g, *c);
                if (s.empty())
                    continue;
                std::vector<double> cuts;
                size_t si = 0;
                if (a.has("parameter"))
                    cuts.push_back(a.get<double>("parameter"));
                else
                {
                    auto pts = selectedPoints(c->id(), false);
                    if (pts.empty())
                        throw CommandError("detachCurve: give parameter= or select points");
                    si = pts.begin()->first;
                    for (uint32_t id : components::selected(c->id(), ComponentMode::Vertex))
                    {
                        size_t sp, p;
                        PointRole r;
                        decodeComponent(id, &sp, &p, &r);
                        if (sp != si || si >= s.size())
                            continue;
                        if (r == PointRole::Edit || (r == PointRole::Point && s[si].kind != Kind::Nurbs))
                            cuts.push_back(double(p));
                        else if (r == PointRole::Point)
                        {
                            // A NURBS control point: the curve point nearest it.
                            cuts.push_back(closestParam(s[si], s[si].points[p]));
                        }
                    }
                }
                std::sort(cuts.rbegin(), cuts.rend());
                Splines pieces{s[si]};
                // Cut from the end so earlier parameters stay valid.
                for (double u : cuts)
                {
                    Spline last = pieces.front();
                    Splines parts = detach(last, u);
                    pieces.erase(pieces.begin());
                    pieces.insert(pieces.begin(), parts.begin(), parts.end());
                }
                Splines keep = s;
                keep.erase(keep.begin() + ptrdiff_t(si));
                keep.insert(keep.begin() + ptrdiff_t(si), pieces.front());
                components::select(c->id(), ComponentMode::Vertex, {}, components::Op::Replace);
                setSplines(g, *c, keep);
                names.push_back(c->path());
                sel.push_back(c->id());
                glm::dmat4 w = c->as<glm::dmat4>(Transform::kWorldMatrix);
                for (size_t k = 1; k < pieces.size(); ++k)
                {
                    // The piece is in c's object space; the new curve holds it in world space.
                    Spline moved = pieces[k];
                    transform(moved, w);
                    Node* n = createCurve(g, {moved}, c->name());
                    names.push_back(n->path());
                    sel.push_back(n->id());
                }
            }
            g.select(sel);
            return names;
        },
    });

    ops::registerCommand({
        .name = "reverseCurve",
        .doc = "Reverse curves' direction (same shape).",
        .positionalDoc = "curves",
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            for (Node* c : curvesOf(a, "reverseCurve"))
            {
                Splines s = storedSplines(g, *c);
                for (Spline& sp : s)
                    reverse(sp);
                setSplines(g, *c, std::move(s));
            }
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "closeCurve",
        .doc = "Close open curves: ends that meet merge, others get a straight span (or a smooth one with blend).",
        .positionalDoc = "curves",
        .flags = {{"blend", "bl", T::Bool, false, "Close with a smooth span."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            for (Node* c : curvesOf(a, "closeCurve"))
            {
                Splines s = storedSplines(g, *c);
                for (Spline& sp : s)
                    sp = closeCurve(sp, a.get<bool>("blend"));
                setSplines(g, *c, std::move(s));
            }
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "openCurve",
        .doc = "Open closed curves at parameter (span units, default their start), keeping the shape.",
        .positionalDoc = "curves",
        .flags = {{"parameter", "p", T::Float, 0.0, "Where to open, in spans from the start."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            for (Node* c : curvesOf(a, "openCurve"))
            {
                Splines s = storedSplines(g, *c);
                for (Spline& sp : s)
                    sp = openCurve(sp, a.get<double>("parameter", 0.0));
                setSplines(g, *c, std::move(s));
            }
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "insertCurvePoint",
        .doc = "Insert a point into a curve at parameter (span units) without changing its shape. Returns the new point's index.",
        .positionalDoc = "curve",
        .flags = {{"parameter", "p", T::Float, 0.5, "Where, in spans from the start."}, {"spline", "", T::Int, 0, "Which spline of the curve."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            Node* c = curvesOf(a, "insertCurvePoint").back();
            Splines s = storedSplines(g, *c);
            size_t si = size_t(std::max(0, a.get<int>("spline", 0)));
            if (si >= s.size())
                throw CommandError("insertCurvePoint: no spline " + std::to_string(si));
            size_t at = insertPointAt(s[si], a.get<double>("parameter", 0.5));
            setSplines(g, *c, std::move(s));
            return at;
        },
    });

    ops::registerCommand({
        .name = "deleteCurvePoints",
        .doc = "Delete the selected curve points (vertex mode); a spline left too short is removed.",
        .run = [](const Args&) -> json {
            Graph& g = graph();
            std::vector<NodeId> nodes;
            for (auto& [id, ids] : components::selected(ComponentMode::Vertex))
                if (Node* n = g.node(id); n && n->type().isA("curve"))
                    nodes.push_back(id);
            if (nodes.empty())
                throw CommandError("deleteCurvePoints: select curve points");
            for (NodeId id : nodes)
            {
                Node& c = *g.node(id);
                Splines s = storedSplines(g, c);
                auto pts = selectedPoints(id, true);
                components::select(id, ComponentMode::Vertex, {}, components::Op::Replace);
                Splines out;
                for (size_t si = 0; si < s.size(); ++si)
                {
                    Spline sp = s[si];
                    if (auto it = pts.find(si); it != pts.end())
                        for (auto p = it->second.rbegin(); p != it->second.rend(); ++p)
                            sp.erasePoint(*p);
                    if (sp.kind == Kind::Bezier)
                        updateHandles(sp);
                    if (sp.kind == Kind::Nurbs)
                        sp.degree = std::min(sp.degree, std::max(1, int(sp.size()) - 1));
                    if (isValid(sp))
                        out.push_back(std::move(sp));
                }
                if (out.empty())
                    g.deleteNode(id);
                else
                    setSplines(g, c, std::move(out));
            }
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "setCurveHandleType",
        .doc = "Set the Bezier handle type of the selected curve points (vertex mode), or of every point of the given curves.",
        .positionalDoc = "curves",
        .flags = {{"type", "t", T::String, "auto", "Handle type.", {"auto", "vector", "aligned", "free"}}},
        .run = [](const Args& a) -> json {
            HandleType ht;
            if (!parseHandleType(a.get<std::string>("type", "auto"), &ht))
                throw CommandError("setCurveHandleType: type is auto, vector, aligned or free");
            Graph& g = graph();
            for (Node* c : curvesOf(a, "setCurveHandleType"))
            {
                Splines s = storedSplines(g, *c);
                auto pts = selectedPoints(c->id(), false);
                for (size_t si = 0; si < s.size(); ++si)
                {
                    if (s[si].kind != Kind::Bezier)
                        continue;
                    if (pts.empty())
                        for (size_t i = 0; i < s[si].size(); ++i)
                            setHandleType(s[si], i, ht);
                    else if (auto it = pts.find(si); it != pts.end())
                        for (size_t i : it->second)
                            setHandleType(s[si], i, ht);
                }
                setSplines(g, *c, std::move(s));
            }
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "convertCurve",
        .doc = "Convert curves to another kind: poly, Bezier or NURBS (exact where the shape allows, fitted within tolerance otherwise).",
        .positionalDoc = "curves",
        .flags = {{"type", "t", T::String, "nurbs", "Target kind.", {"poly", "bezier", "nurbs"}},
                  {"tolerance", "tol", T::Float, 0.01, "Fitting tolerance, cm."}},
        .run = [](const Args& a) -> json {
            Kind k;
            if (!parseKind(a.get<std::string>("type", "nurbs"), &k))
                throw CommandError("convertCurve: type is poly, bezier or nurbs");
            Graph& g = graph();
            for (Node* c : curvesOf(a, "convertCurve"))
            {
                Splines s = storedSplines(g, *c);
                for (Spline& sp : s)
                    sp = convert(sp, k, a.get<double>("tolerance", 0.01));
                setSplines(g, *c, std::move(s));
            }
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "curveDrawMode",
        .doc = "Make a curve through points with an interpolating draw mode: centripetal Catmull-Rom or kappa (every point a "
               "curvature peak). Both give a Bezier. Returns its name.",
        .flags = {{"points", "p", T::List, nullptr, "[[x, y, z], ...] cm."},
                  {"mode", "m", T::String, "catmullRom", "Draw mode.", {"catmullRom", "kappa"}},
                  {"closed", "c", T::Bool, false, "A closed loop."},
                  {"name", "n", T::String, nullptr, "Name; made unique."}},
        .run = [](const Args& a) -> json {
            std::vector<D3> pts = pointsArg(a, "points");
            if (pts.size() < 2)
                throw CommandError("curveDrawMode: give at least two points");
            bool closed = a.get<bool>("closed");
            Spline s = a.get<std::string>("mode", "catmullRom") == "kappa" ? kappaCurve(pts, closed) : catmullRom(pts, closed);
            Graph& g = graph();
            Node* n = createCurve(g, {s}, a.get<std::string>("name", "Curve"));
            g.select({n->id()});
            return n->path();
        },
    });

    registerPipeCommands();
    registerEdgeToCurveCommand();
}
} // namespace lila::curve
