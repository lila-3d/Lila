#include "curve/SketchNode.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "curve/CurveNodes.h"
#include "curve/NodeHelpers.h"
#include "ops/Command.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace lila::curve
{
using namespace scene;
using namespace helpers;
using json = nlohmann::json;
using D3 = glm::dvec3;
using T = ArgType;
namespace sk = sketch;

namespace
{
int sketchAttr(const Node& n) { return n.type().attrIndex("sketch"); }
} // namespace

sk::Sketch sketchOf(const Node& curve)
{
    const int a = sketchAttr(curve);
    if (a < 0)
        return {};
    try
    {
        return sk::Sketch::fromText(curve.as<std::string>(a));
    }
    catch (const std::exception& e)
    {
        log::warn("{}: sketch constraints unreadable ({}); ignored", curve.name(), e.what());
        return {};
    }
}

bool hasSketch(const Node& curve)
{
    const int a = sketchAttr(curve);
    return a >= 0 && !curve.as<std::string>(a).empty();
}

Splines sketchMoved(const Node& curve, const Splines& stored, const std::vector<std::pair<uint32_t, glm::dvec3>>& moves)
{
    LILA_ZONE();

    if (!hasSketch(curve))
        return movedComponents(stored, moves);
    sk::Sketch k = sketchOf(curve);
    if (k.empty())
        return movedComponents(stored, moves);
    Splines s = stored;
    sk::SolveOptions o;
    o.findConflicts = false;
    std::vector<std::pair<uint32_t, glm::dvec3>> direct;
    std::map<size_t, std::vector<std::pair<size_t, D3>>> roundMoves;
    for (auto& [id, p] : moves)
    {
        size_t si, i;
        PointRole role;
        decodeComponent(id, &si, &i, &role);
        if (si >= s.size())
            continue;
        const sk::CircleFit cf = sk::circleOf(s[si], k.plane);
        if (cf.shape != sk::Shape::Points && role == PointRole::Point)
        {
            const bool end = cf.shape == sk::Shape::Arc && (i == 0 || i + 1 == s[si].size());
            if (!end)
            {
                roundMoves[si].push_back({i, p});
                continue;
            }
            o.drags.push_back({{sk::RefKind::Point, int(si), i == 0 ? 0 : -1}, p});
            continue;
        }
        switch (role)
        {
        case PointRole::Point: o.drags.push_back({{sk::RefKind::Point, int(si), int(i)}, p}); break;
        case PointRole::HandleL:
        case PointRole::HandleR:
            // Handles the solver doesn't hold as unknowns move directly; the solve then keeps them.
            direct.push_back({id, p});
            break;
        case PointRole::Edit: direct.push_back({id, p}); break;
        }
    }
    if (!direct.empty())
        s = movedComponents(s, direct);
    // A circle or arc moved by its inner points: all of them moves the whole curve, fewer pull the radius.
    for (auto& [si, list] : roundMoves)
    {
        const sk::CircleFit cf = sk::circleOf(s[si], k.plane);
        D3 delta(0.0);
        for (auto& [i, p] : list)
            delta += p - s[si].points[i];
        delta /= double(list.size());
        const size_t inner = cf.shape == sk::Shape::Arc ? s[si].size() - 2 : s[si].size();
        if (list.size() >= inner)
            o.drags.push_back({{sk::RefKind::Center, int(si), 0}, k.plane.to3d(cf.center, cf.height) + delta});
        else
        {
            const double r = glm::distance(k.plane.to2d(list.front().second), cf.center);
            Splines one = {s[si]};
            sk::Sketch radiusOnly;
            radiusOnly.plane = k.plane;
            sk::Constraint c;
            c.id = 1;
            c.type = sk::Type::Radius;
            c.refs = {{sk::RefKind::Curve, 0, 0}};
            c.value = std::max(r, 1e-6);
            radiusOnly.constraints.push_back(c);
            sk::solve(one, radiusOnly, {});
            s[si] = one[0];
        }
    }
    // A sketch that can't hold keeps its geometry; the viewport shows the conflict, so a drag doesn't log every step.
    sk::solve(s, k, o);
    return s;
}

namespace
{
Node& sketchCurve(const Args& a, const char* cmd)
{
    Graph& g = graph();
    std::string name;
    if (!a.positional.empty())
        name = a.positional[0].get<std::string>();
    if (name.empty())
    {
        for (NodeId id : g.selection())
            if (Node* n = g.node(id); n && n->type().isA("curve"))
                return *n;
        for (auto& [id, ids] : components::selected(ComponentMode::Vertex))
            if (Node* n = g.node(id); n && n->type().isA("curve"))
                return *n;
        throw CommandError(std::string(cmd) + ": select a curve");
    }
    Node* n = g.find(name);
    if (!n)
        throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
    if (!n->type().isA("curve"))
        throw CommandError(std::string(cmd) + ": " + name + " is not a curve");
    return *n;
}

/** Writes splines and sketch together; history feeding the curve is frozen first, as any direct edit does. */
void store(Graph& g, Node& n, const Splines& s, const sk::Sketch& k)
{
    PlugRef create{n.id(), CurveShape::kCreate, -1};
    if (g.source(create))
        g.disconnect(create);
    g.setValue(create, makeCurveData(s));
    g.setValue({n.id(), sketchAttr(n), -1}, k.toText());
}

/** The curve's sketch, given a plane from its points when it has none yet. */
sk::Sketch sketchFor(Node& n, const Splines& s, const Args& a)
{
    sk::Sketch k = sketchOf(n);
    if (!hasSketch(n))
        k.plane = sk::fitPlane(s, sceneUp());
    if (a.has("planeNormal"))
    {
        const json& v = a.flags["planeNormal"];
        D3 nrm(v[0].get<double>(), v[1].get<double>(), v[2].get<double>());
        if (glm::length(nrm) < 1e-9)
            throw CommandError("sketch: planeNormal is zero");
        D3 origin = k.plane.origin;
        if (a.has("planeOrigin"))
        {
            const json& o = a.flags["planeOrigin"];
            origin = D3(o[0].get<double>(), o[1].get<double>(), o[2].get<double>());
        }
        k.plane = sk::planeFrom(origin, nrm);
    }
    return k;
}

std::vector<sk::Ref> refsArg(const Args& a, const char* cmd)
{
    std::vector<sk::Ref> out;
    for (const json& j : a.flags.value("refs", json::array()))
    {
        sk::Ref r;
        if (!j.is_string() || !sk::parseRef(j.get<std::string>(), &r))
            throw CommandError(std::string(cmd) + ": bad ref " + j.dump() + " (use p0.3, l0.3, c1, o1, hl0.3, hr0.3)");
        out.push_back(r);
    }
    return out;
}

/** Point refs from the curve's selected points (vertex mode), when no refs are given. */
std::vector<sk::Ref> selectedRefs(const Node& n)
{
    std::vector<sk::Ref> out;
    for (uint32_t id : components::selected(n.id(), ComponentMode::Vertex))
    {
        size_t si, i;
        PointRole role;
        decodeComponent(id, &si, &i, &role);
        if (role == PointRole::Point)
            out.push_back({sk::RefKind::Point, int(si), int(i)});
        else if (role == PointRole::HandleL || role == PointRole::HandleR)
            out.push_back({role == PointRole::HandleL ? sk::RefKind::HandleL : sk::RefKind::HandleR, int(si), int(i)});
    }
    return out;
}

/** The expression with every whole-word use of name replaced. */
std::string replaceName(const std::string& e, const std::string& name, const std::string& with)
{
    std::string out;
    size_t i = 0;
    while (i < e.size())
    {
        if (std::isalpha(static_cast<unsigned char>(e[i])) || e[i] == '_')
        {
            const size_t b = i;
            while (i < e.size() && (std::isalnum(static_cast<unsigned char>(e[i])) || e[i] == '_'))
                ++i;
            const std::string word = e.substr(b, i - b);
            // Right after a digit it is a unit, not a name.
            const bool unit = b > 0 && (std::isdigit(static_cast<unsigned char>(e[b - 1])) || e[b - 1] == '.');
            out += word == name && !unit ? with : word;
        }
        else
            out += e[i++];
    }
    return out;
}

std::string describe(const sk::Sketch& k, uint32_t id)
{
    const sk::Constraint* c = k.find(id);
    if (!c)
        return "#" + std::to_string(id);
    std::string out = "#" + std::to_string(id) + " " + sk::typeName(c->type);
    if (!c->name.empty())
        out += " " + c->name;
    for (const sk::Ref& r : c->refs)
        out += " " + sk::refName(r);
    return out;
}

std::string describeAll(const sk::Sketch& k, const std::vector<uint32_t>& ids)
{
    std::string out;
    for (uint32_t id : ids)
        out += (out.empty() ? "" : ", ") + describe(k, id);
    return out;
}

/** Tangency through a Bezier end or anchor needs handles the solver may turn: make them free so nothing recomputes them. */
void freeHandles(Splines& s, const sk::Constraint& c)
{
    if (c.type != sk::Type::Tangent)
        return;
    auto freeAt = [&](int si, size_t i, int side) {
        if (si < 0 || size_t(si) >= s.size())
            return;
        Spline& sp = s[size_t(si)];
        if (sp.kind != Kind::Bezier || i >= sp.size() || sp.typeL.size() != sp.size())
            return;
        if (side != 1)
            sp.typeL[i] = HandleType::Free;
        if (side != 0)
            sp.typeR[i] = HandleType::Free;
    };
    if (c.refs.size() == 1)
    {
        freeAt(c.refs[0].spline, sk::normalized(s, c.refs[0]).index, 2);
        return;
    }
    for (size_t k = 0; k < 2 && k < c.refs.size(); ++k)
    {
        const sk::Ref& r = c.refs[k];
        const int end = k == 0 ? c.endA : c.endB;
        if (end < 0 || size_t(r.spline) >= s.size())
            continue;
        const size_t n = s[size_t(r.spline)].size();
        if (r.kind == sk::RefKind::Line)
            freeAt(r.spline, end == 0 ? size_t(r.index) : (size_t(r.index) + 1) % n, end == 0 ? 1 : 0);
        else if (r.kind == sk::RefKind::Curve && n)
            freeAt(r.spline, end == 0 ? 0 : n - 1, end == 0 ? 1 : 0);
    }
}

/**
 * Adds constraints after checking each: one that conflicts (or, unless allowed, adds nothing) throws naming the
 * constraints in the way. Returns the ids added; s ends up solved.
 */
std::vector<uint32_t> addChecked(const char* cmd, Splines& s, sk::Sketch& k, std::vector<sk::Constraint> cs, bool force)
{
    std::vector<uint32_t> ids;
    for (sk::Constraint& c : cs)
    {
        try
        {
            sk::prepare(c, s, k.plane);
        }
        catch (const std::invalid_argument& e)
        {
            throw CommandError(std::string(cmd) + ": " + e.what());
        }
        Splines trial = s;
        freeHandles(trial, c);
        sk::Sketch probe = k;
        const uint32_t id = probe.add(c);
        const sk::Constraint added = *probe.find(id);
        const sk::AddCheck check = sk::checkAdd(trial, k, added);
        if (!check.result.ok && !force)
        {
            std::vector<uint32_t> others;
            for (uint32_t o : check.result.conflicting)
                if (o != id)
                    others.push_back(o);
            throw CommandError(std::string(cmd) + ": " + sk::typeName(c.type) + " conflicts with the sketch" +
                               (others.empty() ? std::string() : ": " + describeAll(k, others)));
        }
        if (check.redundant && !force)
            throw CommandError(std::string(cmd) + ": " + sk::typeName(c.type) + " adds nothing, the other constraints already imply it (force=True keeps it)");
        k = std::move(probe);
        s = std::move(trial);
        sk::SolveResult r = sk::solve(s, k, {});
        if (!r.ok)
            log::warn("{}: the sketch cannot hold all its constraints: {}", cmd, describeAll(k, r.conflicting));
        ids.push_back(id);
    }
    return ids;
}

json resultJson(const sk::Sketch& k, const sk::SolveResult& r)
{
    return {{"ok", r.ok}, {"error", r.error}, {"conflicting", r.conflicting}, {"invalid", r.invalid}, {"warnings", r.warnings},
            {"conflictText", describeAll(k, r.conflicting)}};
}

/** One constraint per line or curve for single-target types given several, else the refs as one constraint. */
std::vector<sk::Constraint> expand(sk::Type t, const std::vector<sk::Ref>& refs)
{
    std::vector<sk::Constraint> out;
    auto one = [&](std::vector<sk::Ref> r) {
        sk::Constraint c;
        c.type = t;
        c.refs = std::move(r);
        out.push_back(std::move(c));
    };
    const bool allLines = !refs.empty() && std::ranges::all_of(refs, [](const sk::Ref& r) { return r.kind == sk::RefKind::Line; });
    if ((t == sk::Type::Horizontal || t == sk::Type::Vertical) && allLines && refs.size() > 1)
    {
        for (const sk::Ref& r : refs)
            one({r});
        return out;
    }
    if ((t == sk::Type::Horizontal || t == sk::Type::Vertical) && refs.size() > 2)
    {
        // Points in a row: each with the first.
        for (size_t k = 1; k < refs.size(); ++k)
            one({refs[0], refs[k]});
        return out;
    }
    if (t == sk::Type::Fixed && refs.size() > 1)
    {
        for (const sk::Ref& r : refs)
            one({r});
        return out;
    }
    if ((t == sk::Type::Equal || t == sk::Type::Parallel || t == sk::Type::Coincident || t == sk::Type::Concentric) && refs.size() > 2)
    {
        for (size_t k = 1; k < refs.size(); ++k)
            one({refs[0], refs[k]});
        return out;
    }
    one(refs);
    return out;
}

std::vector<std::string> typeChoices(bool dimensions)
{
    std::vector<std::string> out;
    for (int t = 0; t < int(sk::Type::Count); ++t)
        if (sk::isDimension(sk::Type(t)) == dimensions)
            out.push_back(sk::typeName(sk::Type(t)));
    return out;
}

const char* kRefsDoc = "What it applies to: \"p0.3\" point 3 of spline 0 (\"p0.end\" its last), \"l0.3\" the line from point 3 to 4, \"c1\" "
                       "spline 1 (a circle, arc or any curve), \"o1\" a circle's centre, \"hl0.3\"/\"hr0.3\" Bezier handles. Default: the "
                       "selected points.";

FlagDef planeFlag() { return {"planeNormal", "", T::Vec3, nullptr, "The sketch plane's normal (object space) when the curve has no sketch yet; default its best-fit plane."}; }
FlagDef originFlag() { return {"planeOrigin", "", T::Vec3, nullptr, "A point on the sketch plane, with planeNormal."}; }
FlagDef forceFlag() { return {"force", "", T::Bool, false, "Keep a constraint even when it conflicts or adds nothing."}; }

json infoJson(Graph& g, Node& n)
{
    Splines s = storedSplines(g, n);
    sk::Sketch k = sketchOf(n);
    json cs = json::array();
    std::map<uint32_t, double> values;
    std::string valueError;
    try
    {
        values = sk::dimensionValues(k);
    }
    catch (const std::exception& e)
    {
        valueError = e.what();
    }
    for (const sk::Constraint& c : k.constraints)
    {
        json j = c.toJson();
        bool valid = true;
        for (const sk::Ref& r : c.refs)
            valid = valid && sk::refValid(s, k.plane, r);
        j["valid"] = valid;
        if (sk::isDimension(c.type))
        {
            if (auto it = values.find(c.id); it != values.end())
                j["value"] = it->second;
            if (valid)
                j["measured"] = sk::measure(c, s, k.plane);
        }
        cs.push_back(j);
    }
    sk::Status st = sk::analyze(s, k);
    Splines check = s;
    sk::SolveResult r = sk::solve(check, k, {});
    json fixed = json::array();
    for (size_t si = 0; si < st.pointFixed.size(); ++si)
    {
        json row = json::array();
        for (bool b : st.pointFixed[si])
            row.push_back(b);
        fixed.push_back(row);
    }
    const D3& o = k.plane.origin;
    return {{"plane", {{"origin", {o.x, o.y, o.z}}, {"u", {k.plane.u.x, k.plane.u.y, k.plane.u.z}}, {"v", {k.plane.v.x, k.plane.v.y, k.plane.v.z}}}},
            {"constraints", cs},
            {"dof", st.dof},
            {"fullyConstrained", st.dof == 0 && !s.empty()},
            {"pointFixed", fixed},
            {"splineFixed", st.splineFixed},
            {"redundant", st.redundant},
            {"ok", r.ok},
            {"conflicting", r.conflicting},
            {"invalid", r.invalid},
            {"warnings", valueError.empty() ? json(r.warnings) : json::array({valueError})}};
}
} // namespace

void registerSketchCommands()
{
    ops::registerCommand({
        .name = "sketchConstrain",
        .doc = "Add a geometric constraint between a curve's points, lines and circles, and solve the sketch. Several lines for "
               "horizontal, vertical or fixed add one each; three or more refs for equal, parallel, coincident or concentric tie each "
               "to the first. Refuses a constraint that conflicts (naming what it conflicts with) or adds nothing. Returns the ids.",
        .positionalDoc = "curve",
        .flags = {{"type", "t", T::String, "coincident", "The constraint.", typeChoices(false)},
                  {"refs", "r", T::List, nullptr, kRefsDoc},
                  forceFlag(),
                  planeFlag(),
                  originFlag()},
        .run = [](const Args& a) -> json {
            Node& n = sketchCurve(a, "sketchConstrain");
            Graph& g = graph();
            sk::Type t;
            if (!sk::parseType(a.get<std::string>("type", "coincident"), &t) || sk::isDimension(t))
                throw CommandError("sketchConstrain: unknown type '" + a.get<std::string>("type") + "' (dimensions go through sketchDimension)");
            std::vector<sk::Ref> refs = a.has("refs") ? refsArg(a, "sketchConstrain") : selectedRefs(n);
            if (refs.empty())
                throw CommandError("sketchConstrain: give refs= or select points");
            Splines s = storedSplines(g, n);
            sk::Sketch k = sketchFor(n, s, a);
            std::vector<uint32_t> ids = addChecked("sketchConstrain", s, k, expand(t, refs), a.get<bool>("force"));
            store(g, n, s, k);
            return ids;
        },
    });

    ops::registerCommand({
        .name = "sketchDimension",
        .doc = "Add a dimension (distance, horizontal or vertical distance, length, radius, diameter or angle) and solve the sketch "
               "to it. Without a value it keeps the current measurement. expr makes it parametric: other dimensions by name (d1), "
               "units (mm, cm, m, in, deg), + - * / ^ and sin, cos, sqrt... Returns {id, name, value}.",
        .positionalDoc = "curve",
        .flags = {{"type", "t", T::String, "distance", "The dimension.", typeChoices(true)},
                  {"refs", "r", T::List, nullptr, kRefsDoc},
                  {"value", "v", T::Float, nullptr, "cm, or degrees for angle; default the current measurement."},
                  {"expr", "e", T::String, nullptr, "An expression for the value, kept live (e.g. \"d1 * 2\", \"25mm\")."},
                  {"name", "n", T::String, nullptr, "Name other expressions use; default d1, d2..."},
                  forceFlag(),
                  planeFlag(),
                  originFlag()},
        .run = [](const Args& a) -> json {
            Node& n = sketchCurve(a, "sketchDimension");
            Graph& g = graph();
            sk::Type t;
            if (!sk::parseType(a.get<std::string>("type", "distance"), &t) || !sk::isDimension(t))
                throw CommandError("sketchDimension: unknown dimension '" + a.get<std::string>("type") + "'");
            std::vector<sk::Ref> refs = a.has("refs") ? refsArg(a, "sketchDimension") : selectedRefs(n);
            if (refs.empty())
                throw CommandError("sketchDimension: give refs= or select points");
            Splines s = storedSplines(g, n);
            sk::Sketch k = sketchFor(n, s, a);
            sk::Constraint c;
            c.type = t;
            c.refs = refs;
            if (a.has("name"))
            {
                c.name = a.get<std::string>("name");
                for (const sk::Constraint& o : k.constraints)
                    if (o.name == c.name)
                        throw CommandError("sketchDimension: the name " + c.name + " is taken");
            }
            try
            {
                sk::prepare(c, s, k.plane);
            }
            catch (const std::invalid_argument& e)
            {
                throw CommandError(std::string("sketchDimension: ") + e.what());
            }
            c.value = sk::measure(c, s, k.plane);
            if (a.has("value"))
                c.value = a.get<double>("value");
            if (a.has("expr"))
            {
                c.expr = a.get<std::string>("expr");
                std::map<std::string, double> names;
                for (auto& [id, v] : sk::dimensionValues(k))
                    names[k.find(id)->name] = v;
                std::string why;
                std::optional<double> v = sk::evalExpression(c.expr, names, &why);
                if (!v)
                    throw CommandError("sketchDimension: " + c.expr + ": " + why);
                c.value = *v;
            }
            if (c.value < 0.0 && t != sk::Type::Angle)
                throw CommandError("sketchDimension: a distance can't be negative");
            std::vector<uint32_t> ids = addChecked("sketchDimension", s, k, {c}, a.get<bool>("force"));
            store(g, n, s, k);
            const sk::Constraint& added = *k.find(ids.front());
            return {{"id", added.id}, {"name", added.name}, {"value", added.value}};
        },
    });

    ops::registerCommand({
        .name = "sketchEditDimension",
        .doc = "Change a dimension's value or expression and solve the sketch to it. Refused (nothing changes) when the sketch "
               "can't reach it.",
        .positionalDoc = "curve",
        .flags = {{"id", "", T::Int, nullptr, "The dimension's id.", {}, {}, 0.0},
                  {"dimension", "d", T::String, nullptr, "The dimension's name (instead of id)."},
                  {"value", "v", T::Float, nullptr, "cm, or degrees for angle. Clears an expression."},
                  {"expr", "e", T::String, nullptr, "An expression for the value; empty clears it."},
                  {"rename", "", T::String, nullptr, "A new name for it."}},
        .run = [](const Args& a) -> json {
            Node& n = sketchCurve(a, "sketchEditDimension");
            Graph& g = graph();
            Splines s = storedSplines(g, n);
            sk::Sketch k = sketchOf(n);
            sk::Constraint* c = nullptr;
            for (sk::Constraint& o : k.constraints)
                if ((a.has("id") && o.id == uint32_t(a.get<int64_t>("id"))) || (a.has("dimension") && o.name == a.get<std::string>("dimension")))
                    c = &o;
            if (!c || !sk::isDimension(c->type))
                throw CommandError("sketchEditDimension: no such dimension on " + n.path());
            if (a.has("value"))
            {
                c->value = a.get<double>("value");
                c->expr.clear();
            }
            if (a.has("expr"))
                c->expr = a.get<std::string>("expr");
            if (a.has("rename"))
            {
                std::string nm = a.get<std::string>("rename");
                for (const sk::Constraint& o : k.constraints)
                    if (o.id != c->id && o.name == nm)
                        throw CommandError("sketchEditDimension: the name " + nm + " is taken");
                // Expressions follow the rename.
                for (sk::Constraint& o : k.constraints)
                    o.expr = replaceName(o.expr, c->name, nm);
                c->name = nm;
            }
            uint32_t id = c->id;
            try
            {
                std::map<uint32_t, double> values = sk::dimensionValues(k);
                c->value = values.at(id);
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("sketchEditDimension: ") + e.what());
            }
            if (c->value < 0.0 && c->type != sk::Type::Angle)
                throw CommandError("sketchEditDimension: a distance can't be negative");
            sk::SolveResult r = sk::solve(s, k, {});
            if (!r.ok)
                throw CommandError("sketchEditDimension: the sketch can't reach that value" +
                                   (r.conflicting.empty() ? std::string() : " (conflicts: " + describeAll(k, r.conflicting) + ")"));
            store(g, n, s, k);
            return k.find(id)->value;
        },
    });

    ops::registerCommand({
        .name = "sketchDelete",
        .doc = "Delete sketch constraints or dimensions by id; the geometry stays where it is. Dimensions others' expressions use "
               "leave their value behind in those expressions.",
        .positionalDoc = "curve",
        .flags = {{"ids", "", T::List, nullptr, "Constraint ids."}},
        .run = [](const Args& a) -> json {
            Node& n = sketchCurve(a, "sketchDelete");
            Graph& g = graph();
            sk::Sketch k = sketchOf(n);
            std::map<uint32_t, double> values;
            try
            {
                values = sk::dimensionValues(k);
            }
            catch (const std::exception&)
            {
            }
            int removed = 0;
            for (const json& j : a.flags.value("ids", json::array()))
            {
                const uint32_t id = j.get<uint32_t>();
                const sk::Constraint* c = k.find(id);
                if (!c)
                    throw CommandError("sketchDelete: no constraint " + std::to_string(id) + " on " + n.path());
                if (sk::isDimension(c->type))
                {
                    // Expressions naming it keep working: its value takes its place.
                    const std::string name = c->name;
                    const double v = values.count(id) ? values[id] : c->value;
                    for (sk::Constraint& o : k.constraints)
                        if (o.id != id)
                            o.expr = replaceName(o.expr, name, "(" + std::to_string(v) + ")");
                }
                k.remove(id);
                ++removed;
            }
            g.setValue({n.id(), sketchAttr(n), -1}, k.toText());
            return removed;
        },
    });

    ops::registerCommand({
        .name = "sketchClear",
        .doc = "Remove every sketch constraint and dimension from curves (the geometry stays).",
        .positionalDoc = "curves",
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::vector<Node*> nodes;
            for (const json& p : a.positional)
            {
                Node* n = g.find(p.get<std::string>());
                if (!n || !n->type().isA("curve"))
                    throw CommandError("sketchClear: " + p.get<std::string>() + " is not a curve");
                nodes.push_back(n);
            }
            if (nodes.empty())
                nodes.push_back(&sketchCurve(a, "sketchClear"));
            for (Node* n : nodes)
                g.setValue({n->id(), sketchAttr(*n), -1}, std::string());
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "sketchSolve",
        .doc = "Solve a curve's sketch again (after editing points by other means). Returns {ok, error, conflicting, invalid, warnings}; "
               "a sketch that can't hold keeps its geometry.",
        .positionalDoc = "curve",
        .run = [](const Args& a) -> json {
            Node& n = sketchCurve(a, "sketchSolve");
            Graph& g = graph();
            Splines s = storedSplines(g, n);
            sk::Sketch k = sketchOf(n);
            sk::SolveResult r = sk::solve(s, k, {});
            if (r.ok)
                store(g, n, s, k);
            return resultJson(k, r);
        },
    });

    ops::registerCommand({
        .name = "sketchInfo",
        .doc = "A curve's sketch: plane, constraints (with each dimension's value and measurement), degrees of freedom, which points "
               "are fully constrained, redundant and conflicting constraints.",
        .positionalDoc = "curve",
        .undoable = false,
        .run = [](const Args& a) -> json { return infoJson(graph(), sketchCurve(a, "sketchInfo")); },
    });

    ops::registerCommand({
        .name = "sketchDrag",
        .doc = "Drag sketch points (or circle centres, or a circle's rim for its radius) to a position or by an offset, solving the "
               "rest so every constraint keeps holding: what dragging them in the viewport does.",
        .positionalDoc = "curve",
        .flags = {{"point", "p", T::String, nullptr, "The point: \"p0.3\", \"o1\", \"hl0.3\", or \"c1\" for a circle's radius."},
                  {"points", "", T::List, nullptr, "Several points, moved together by by= (instead of point)."},
                  {"to", "", T::Vec3, nullptr, "Where the point goes, cm."},
                  {"by", "", T::Vec3, nullptr, "How far the points move, cm (instead of to)."},
                  {"worldSpace", "ws", T::Bool, false, "to and by are in world space (default object space)."},
                  planeFlag(),
                  originFlag()},
        .run = [](const Args& a) -> json {
            Node& n = sketchCurve(a, "sketchDrag");
            Graph& g = graph();
            std::vector<sk::Ref> refs;
            auto addRef = [&](const std::string& t) {
                sk::Ref r;
                if (!sk::parseRef(t, &r) || !(r.isPointLike() || r.kind == sk::RefKind::Curve))
                    throw CommandError("sketchDrag: '" + t + "' is not a point ref like p0.3 or o1");
                refs.push_back(r);
            };
            if (a.has("point"))
                addRef(a.get<std::string>("point"));
            for (const json& j : a.flags.value("points", json::array()))
                addRef(j.get<std::string>());
            if (refs.empty())
                throw CommandError("sketchDrag: give point= or points=");
            if (!a.has("to") && !a.has("by"))
                throw CommandError("sketchDrag: give to= or by=");
            auto v3 = [&](const char* f) {
                const json& t = a.flags[f];
                return D3(t[0].get<double>(), t[1].get<double>(), t[2].get<double>());
            };
            const glm::dmat4 toLocal = glm::inverse(n.as<glm::dmat4>(Transform::kWorldMatrix));
            const bool ws = a.get<bool>("worldSpace");
            Splines s = storedSplines(g, n);
            sk::Sketch k = sketchFor(n, s, a);
            sk::SolveOptions o;
            o.findConflicts = false;
            for (const sk::Ref& r : refs)
            {
                std::string why;
                if (!sk::refValid(s, k.plane, r, &why))
                    throw CommandError("sketchDrag: " + why);
                D3 to;
                if (a.has("to"))
                    to = ws ? D3(toLocal * glm::dvec4(v3("to"), 1.0)) : v3("to");
                else
                {
                    const D3 by = ws ? D3(toLocal * glm::dvec4(v3("by"), 0.0)) : v3("by");
                    to = sk::refPosition(s, k.plane, r) + by;
                }
                o.drags.push_back({r, to});
            }
            sk::SolveResult res = sk::solve(s, k, o);
            store(g, n, s, k);
            return resultJson(k, res);
        },
    });

    ops::registerCommand({
        .name = "sketchPlane",
        .doc = "Set the plane a curve's sketch solves in: a normal and origin, or fit=True for the best fit through its points. "
               "Horizontal and vertical follow the plane's axes.",
        .positionalDoc = "curve",
        .flags = {{"normal", "nr", T::Vec3, nullptr, "Plane normal, object space."},
                  {"origin", "o", T::Vec3, nullptr, "A point on the plane, object space."},
                  {"fit", "", T::Bool, false, "Fit the plane through the curve's points."}},
        .run = [](const Args& a) -> json {
            Node& n = sketchCurve(a, "sketchPlane");
            Graph& g = graph();
            Splines s = storedSplines(g, n);
            sk::Sketch k = sketchOf(n);
            auto v3 = [&](const char* f) {
                const json& j = a.flags[f];
                return D3(j[0].get<double>(), j[1].get<double>(), j[2].get<double>());
            };
            if (a.get<bool>("fit"))
                k.plane = sk::fitPlane(s, a.has("normal") ? v3("normal") : sceneUp());
            else if (a.has("normal"))
            {
                if (glm::length(v3("normal")) < 1e-9)
                    throw CommandError("sketchPlane: normal is zero");
                k.plane = sk::planeFrom(a.has("origin") ? v3("origin") : k.plane.origin, v3("normal"));
            }
            else
                throw CommandError("sketchPlane: give normal= or fit=True");
            sk::SolveResult r = sk::solve(s, k, {});
            if (r.ok)
                store(g, n, s, k);
            else
                g.setValue({n.id(), sketchAttr(n), -1}, k.toText());
            return resultJson(k, r);
        },
    });

    ops::registerCommand({
        .name = "sketchCombine",
        .doc = "Merge curves into the first one, so constraints can tie them together: their splines join it (kept where they are) "
               "and their sketch constraints come along. The others are deleted unless keepOriginal. Returns the first curve.",
        .positionalDoc = "curves",
        .flags = {{"keepOriginal", "kp", T::Bool, false, "Keep the other curves."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::vector<Node*> nodes;
            for (const json& p : a.positional)
            {
                Node* n = g.find(p.get<std::string>());
                if (!n || !n->type().isA("curve"))
                    throw CommandError("sketchCombine: " + p.get<std::string>() + " is not a curve");
                nodes.push_back(n);
            }
            if (nodes.empty())
                for (NodeId id : g.selection())
                    if (Node* n = g.node(id); n && n->type().isA("curve"))
                        nodes.push_back(n);
            if (nodes.size() < 2)
                throw CommandError("sketchCombine: give two or more curves");
            Node& first = *nodes[0];
            Splines s = storedSplines(g, first);
            sk::Sketch k = sketchFor(first, s, a);
            const glm::dmat4 toFirst = glm::inverse(first.as<glm::dmat4>(Transform::kWorldMatrix));
            for (size_t ni = 1; ni < nodes.size(); ++ni)
            {
                Node& o = *nodes[ni];
                const glm::dmat4 m = toFirst * o.as<glm::dmat4>(Transform::kWorldMatrix);
                const int offset = int(s.size());
                for (Spline sp : storedSplines(g, o))
                {
                    transform(sp, m);
                    s.push_back(std::move(sp));
                }
                for (sk::Constraint c : sketchOf(o).constraints)
                {
                    for (sk::Ref& r : c.refs)
                        r.spline += offset;
                    for (D3& p : c.anchors)
                        p = D3(m * glm::dvec4(p, 1.0));
                    // Dimension names stay unique: a clash gets a fresh one.
                    if (sk::isDimension(c.type))
                        for (const sk::Constraint& e : k.constraints)
                            if (e.name == c.name)
                                c.name.clear();
                    c.expr.clear();
                    k.add(c);
                }
            }
            sk::SolveResult r = sk::solve(s, k, {});
            if (!r.ok)
                log::warn("sketchCombine: the combined sketch cannot hold all its constraints: {}", describeAll(k, r.conflicting));
            store(g, first, s, k);
            if (!a.get<bool>("keepOriginal"))
                for (size_t ni = 1; ni < nodes.size(); ++ni)
                    g.deleteNode(nodes[ni]->id());
            g.select({first.id()});
            return first.path();
        },
    });

    ops::registerCommand({
        .name = "sketchAdd",
        .doc = "Add splines to a sketch curve (or a new one) with constraints on them, as the sketch tool does when drawing: new "
               "splines take indices from the curve's spline count on, and constraints that would conflict are dropped with a "
               "warning. Returns {curve, splines (their indices), constraints (ids)}.",
        .positionalDoc = "curve",
        .flags = {{"splines", "s", T::List, nullptr, "Splines in curveData's format, object space of the curve (world space for a new one)."},
                  {"constraints", "c", T::List, nullptr, "[{type, refs, value}, ...]: constraints and dimensions to add after them."},
                  {"name", "n", T::String, nullptr, "Name of a new curve."},
                  planeFlag(),
                  originFlag()},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            Node* n = nullptr;
            bool created = false;
            if (!a.positional.empty() && !a.positional[0].get<std::string>().empty())
                n = &sketchCurve(a, "sketchAdd");
            Splines add;
            try
            {
                for (const json& j : a.flags.value("splines", json::array()))
                    add.push_back(splineFromJson(j));
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("sketchAdd: ") + e.what());
            }
            if (!n)
            {
                if (add.empty())
                    throw CommandError("sketchAdd: give splines for a new curve");
                n = g.createNode(*types::find("curve"), a.get<std::string>("name", "Sketch"), kNoNode, true);
                created = true;
            }
            Splines s = created ? Splines() : storedSplines(g, *n);
            const int first = int(s.size());
            s.insert(s.end(), add.begin(), add.end());
            sk::Sketch k = sketchFor(*n, s, a);
            json ids = json::array();
            for (const json& cj : a.flags.value("constraints", json::array()))
            {
                sk::Constraint c;
                if (!sk::parseType(cj.value("type", std::string()), &c.type))
                    throw CommandError("sketchAdd: unknown constraint type " + cj.value("type", std::string()));
                for (const json& r : cj.value("refs", json::array()))
                {
                    sk::Ref ref;
                    if (!r.is_string() || !sk::parseRef(r.get<std::string>(), &ref))
                        throw CommandError("sketchAdd: bad ref " + r.dump());
                    c.refs.push_back(ref);
                }
                try
                {
                    sk::prepare(c, s, k.plane);
                    if (sk::isDimension(c.type))
                        c.value = cj.contains("value") ? cj["value"].get<double>() : sk::measure(c, s, k.plane);
                    std::vector<uint32_t> got = addChecked("sketchAdd", s, k, {c}, false);
                    ids.push_back(got.front());
                }
                catch (const std::exception& e)
                {
                    log::warn("sketchAdd: {} skipped: {}", cj.value("type", std::string()), e.what());
                }
            }
            store(g, *n, s, k);
            json made = json::array();
            for (int i = first; i < int(s.size()); ++i)
                made.push_back(i);
            g.select({n->id()});
            return {{"curve", n->path()}, {"splines", made}, {"constraints", ids}};
        },
    });
}
} // namespace lila::curve
