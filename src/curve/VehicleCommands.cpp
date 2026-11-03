#include "curve/VehicleNodes.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "curve/CurveNodes.h"
#include "curve/Fair.h"
#include "curve/Intersect.h"
#include "curve/NodeHelpers.h"
#include "curve/Tessellate.h"
#include "geo/Ops.h"
#include "geo/Topology.h"
#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/Transform.h"

#include <algorithm>
#include <map>
#include <tuple>

namespace lila::curve
{
using namespace scene;
using namespace helpers;
using json = nlohmann::json;
using D3 = glm::dvec3;
using T = ArgType;

namespace
{
const std::vector<std::string> kEnds = {"nearest", "start", "end"};

std::vector<Node*> namedCurves(const Args& a, const char* flag, const char* cmd)
{
    std::vector<Node*> out;
    for (const json& p : a.flags.value(flag, json::array()))
        out.push_back(&curveNamed(graph(), p.get<std::string>(), cmd));
    return out;
}

/** Every visible curve but the excluded ones: what trim and extend cut against by default. */
std::vector<Node*> otherCurves(const std::vector<Node*>& exclude)
{
    std::vector<Node*> out;
    Graph& g = graph();
    for (NodeId id : g.nodes())
        if (Node* n = g.node(id); n && n->type().isA("curve") && effectivelyVisible(g, id) && std::ranges::find(exclude, n) == exclude.end())
            out.push_back(n);
    return out;
}

const Splines& worldSplines(Node& c)
{
    graph().evaluate();
    return splinesOf(c.value(CurveShape::kWorldSpace));
}

Splines worldOf(const std::vector<Node*>& curves)
{
    Splines out;
    for (Node* c : curves)
        for (const Spline& s : worldSplines(*c))
            out.push_back(s);
    return out;
}

glm::dmat4 worldMatrix(Node& n) { return n.as<glm::dmat4>(Transform::kWorldMatrix); }

/** Replaces a curve's splines (given in world space), freezing any history feeding it. */
void setWorldSplines(Node& c, Splines s)
{
    glm::dmat4 toLocal = glm::inverse(worldMatrix(c));
    for (Spline& sp : s)
        transform(sp, toLocal);
    Graph& g = graph();
    PlugRef create{c.id(), CurveShape::kCreate, -1};
    if (g.source(create))
        g.disconnect(create);
    g.setValue(create, makeCurveData(std::move(s)));
}

D3 vec3(const Args& a, const char* flag)
{
    const json& v = a.flags[flag];
    if (!v.is_array() || v.size() != 3)
        throw CommandError(std::string(flag) + ": expected [x, y, z]");
    return D3(v[0].get<double>(), v[1].get<double>(), v[2].get<double>());
}

IntersectOptions intersectOf(const Args& a)
{
    IntersectOptions o;
    o.tolerance = std::max(1e-6, a.get<double>("tolerance", 0.01));
    if (a.has("direction"))
        o.direction = vec3(a, "direction");
    return o;
}

int choice(const Args& a, const char* flag, const std::vector<std::string>& names, const char* cmd, int def)
{
    if (!a.has(flag))
        return def;
    auto it = std::ranges::find(names, a.get<std::string>(flag));
    if (it == names.end())
        throw CommandError(std::string(cmd) + ": " + flag + " is one of " + [&] {
            std::string s;
            for (const std::string& n : names)
                s += (s.empty() ? "" : ", ") + n;
            return s;
        }());
    return int(it - names.begin());
}

std::vector<FlagDef> intersectFlags()
{
    return {{"tolerance", "tol", T::Float, 0.01, "Curves closer than this meet, cm.", {}, {}, 1e-5},
            {"direction", "dir", T::Vec3, nullptr, "Intersect as seen along this direction (the view), for curves that only cross on screen."}};
}

/** The spline of a curve a world point is nearest, and the span parameter there. */
std::pair<size_t, double> nearestSpline(const Splines& ss, const D3& p)
{
    size_t best = 0;
    double bu = 0.0, bd = std::numeric_limits<double>::max();
    for (size_t k = 0; k < ss.size(); ++k)
    {
        D3 at;
        double u = closestParam(ss[k], p, &at);
        if (double d = glm::distance(at, p); d < bd)
        {
            bd = d;
            best = k;
            bu = u;
        }
    }
    return {best, bu};
}

/** Selected edges by mesh, from the given flags or the edge selection. */
std::vector<std::pair<Node*, std::vector<geo::Index>>> edgeSides(const Args& a, const char* cmd)
{
    std::vector<std::pair<Node*, std::vector<geo::Index>>> out;
    if (a.has("meshA"))
    {
        out.push_back({&meshNamed(graph(), a.get<std::string>("meshA"), cmd), a.flags.value("edgesA", json::array()).get<std::vector<geo::Index>>()});
        if (a.has("meshB"))
            out.push_back({&meshNamed(graph(), a.get<std::string>("meshB"), cmd), a.flags.value("edgesB", json::array()).get<std::vector<geo::Index>>()});
        return out;
    }
    Graph& g = graph();
    for (auto& [id, ids] : components::selected(ComponentMode::Edge))
        if (Node* n = g.node(id); n && n->type().isA("mesh") && !ids.empty())
            out.push_back({n, std::vector<geo::Index>(ids.begin(), ids.end())});
    return out;
}

MeshPtr cageOf(Node& mesh)
{
    Graph& g = graph();
    g.evaluate();
    MeshPtr m = modifiers::cage(g, mesh.id());
    return m ? m : mesh.as<MeshPtr>(MeshNode::kOutMesh);
}

// blendCurve

json runBlend(const Args& a)
{
    const char* cmd = "blendCurve";
    Graph& g = graph();
    std::vector<Node*> cs = curvesArg(graph(), a, cmd);
    Node* mesh = a.has("mesh") ? &meshNamed(graph(), a.get<std::string>("mesh"), cmd) : nullptr;
    if (!mesh && cs.size() < 2)
        throw CommandError("blendCurve: give two curves, or a curve and mesh= with edge=");
    Node* ca = cs[0];
    Node* cb = mesh ? nullptr : cs[1];
    const Splines& sa = worldSplines(*ca);
    if (sa.empty() || (sa.front().closed && !a.has("parameterA")))
        throw CommandError("blendCurve: " + ca->path() + " has no open end; give parameterA= for a point along it");
    int endA = choice(a, "endA", kEnds, cmd, 0), endB = choice(a, "endB", kEnds, cmd, 0);
    int64_t edge = a.get<int>("edge", -1);
    if (mesh)
    {
        MeshPtr m = mesh->as<MeshPtr>(MeshNode::kOutMesh);
        if (!m || edge < 0 || !m->edgeValid(geo::Index(edge)) || !m->isBoundaryEdge(geo::Index(edge)))
            throw CommandError("blendCurve: edge= must be a border edge of " + mesh->path());
    }
    // Node enums: 0 start, 1 end, 2 parameter.
    int64_t nodeA = a.has("parameterA") ? 2 : endA == 1 ? 0 : endA == 2 ? 1 : -1;
    int64_t nodeB = mesh ? 0 : a.has("parameterB") ? 2 : endB == 1 ? 0 : endB == 2 ? 1 : -1;
    if (nodeA < 0 || nodeB < 0)
    {
        // Nearest: the pair of ends (or end and border point) closest together.
        const Spline& A = sa.front();
        std::vector<std::pair<int64_t, D3>> aEnds, bEnds;
        if (nodeA < 0)
            aEnds = {{0, pointAt(A, 0.0)}, {1, pointAt(A, spanCount(A))}};
        else
            aEnds = {{nodeA, nodeA == 2 ? pointAt(A, a.get<double>("parameterA")) : pointAt(A, nodeA == 0 ? 0.0 : spanCount(A))}};
        if (mesh)
        {
            MeshPtr m = mesh->as<MeshPtr>(MeshNode::kOutMesh);
            glm::dmat4 w = worldMatrix(*mesh);
            D3 p = glm::mix(m->position(m->edgePoint(geo::Index(edge), 0)), m->position(m->edgePoint(geo::Index(edge), 1)), a.get<double>("edgeT", 0.5));
            bEnds = {{0, D3(w * glm::dvec4(p, 1.0))}};
        }
        else
        {
            const Splines& sb = worldSplines(*cb);
            if (sb.empty())
                throw CommandError("blendCurve: " + cb->path() + " is empty");
            const Spline& B = sb.front();
            if (nodeB < 0 && B.closed)
                throw CommandError("blendCurve: " + cb->path() + " has no open end; give parameterB=");
            if (nodeB < 0)
                bEnds = {{0, pointAt(B, 0.0)}, {1, pointAt(B, spanCount(B))}};
            else
                bEnds = {{nodeB, nodeB == 2 ? pointAt(B, a.get<double>("parameterB")) : pointAt(B, nodeB == 0 ? 0.0 : spanCount(B))}};
        }
        double best = std::numeric_limits<double>::max();
        for (auto& [ea, pa] : aEnds)
            for (auto& [eb, pb] : bEnds)
                if (double d = glm::distance(pa, pb); d < best)
                {
                    best = d;
                    nodeA = ea;
                    nodeB = eb;
                }
    }
    Node* op = g.createNode(*types::find("curveBlend"), {}, kNoNode, true);
    auto set = [&](const char* attr, PlugValue v) { g.setValue({op->id(), op->type().attrIndex(attr), -1}, std::move(v)); };
    set("endA", nodeA);
    set("endB", nodeB);
    set("parameterA", a.get<double>("parameterA", 0.0));
    set("parameterB", a.get<double>("parameterB", 0.0));
    set("continuityA", int64_t(choice(a, "continuityA", kContinuity, cmd, 2)));
    set("continuityB", int64_t(choice(a, "continuityB", kContinuity, cmd, 2)));
    set("bulgeA", std::max(0.01, a.get<double>("bulgeA", 1.0)));
    set("bulgeB", std::max(0.01, a.get<double>("bulgeB", 1.0)));
    set("flipA", a.get<bool>("flipA", false));
    set("flipB", a.get<bool>("flipB", false));
    g.connect({ca->id(), CurveShape::kWorldSpace, -1}, {op->id(), op->type().attrIndex("curveA"), -1});
    if (mesh)
    {
        set("meshEdge", edge);
        set("meshEdgeT", std::clamp(a.get<double>("edgeT", 0.5), 0.0, 1.0));
        g.connect({mesh->id(), MeshNode::kOutMesh, -1}, {op->id(), op->type().attrIndex("inMesh"), -1});
        g.connect({mesh->id(), Transform::kWorldMatrix, -1}, {op->id(), op->type().attrIndex("meshMatrix"), -1});
    }
    else
        g.connect({cb->id(), CurveShape::kWorldSpace, -1}, {op->id(), op->type().attrIndex("curveB"), -1});
    g.evaluate();
    try
    {
        BlendSetup st = blendSetupOf(*op);
        blendCurve(st.a, st.b);
    }
    catch (const std::exception& e)
    {
        g.deleteNode(op->id());
        throw CommandError(std::string("blendCurve: ") + e.what());
    }
    Node* result = g.createNode(*types::find("curve"), a.get<std::string>("name", "Blend"), kNoNode, true);
    g.connect({op->id(), op->type().attrIndex("outCurve"), -1}, {result->id(), CurveShape::kCreate, -1});
    json opName = op->path();
    if (!a.get<bool>("constructionHistory", true))
    {
        bake(g, *op, {result->id(), CurveShape::kCreate, -1});
        opName = nullptr;
    }
    g.select({result->id()});
    return json::array({result->path(), opName});
}

// matchCurve

json runMatch(const Args& a)
{
    const char* cmd = "matchCurve";
    std::vector<Node*> cs = curvesArg(graph(), a, cmd);
    Node* mesh = a.has("mesh") ? &meshNamed(graph(), a.get<std::string>("mesh"), cmd) : nullptr;
    if (!mesh && cs.size() < 2)
        throw CommandError("matchCurve: give the curve to change and the curve to match it to (or mesh= with edge=)");
    Node* c = cs[0];
    Node* tgt = mesh ? nullptr : cs[1];
    if (tgt == c)
        throw CommandError("matchCurve: a curve cannot match itself");
    Splines sc = worldSplines(*c);
    if (sc.empty() || sc.front().closed)
        throw CommandError("matchCurve: " + c->path() + " has no open end");
    Spline s = sc.front();
    MatchOptions o;
    o.continuity = Continuity(choice(a, "continuity", {"g0", "g1", "g2"}, cmd, 1));
    o.preserveOther = a.get<bool>("preserveOther", true);
    o.flip = a.get<bool>("flip", false);
    int endMode = choice(a, "end", kEnds, cmd, 0);
    int tgtMode = choice(a, "targetEnd", {"nearest", "start", "end", "closest"}, cmd, 0);
    D3 sStart = pointAt(s, 0.0), sEnd = pointAt(s, spanCount(s));

    Geometry target;
    bool atEnd = true;
    Spline t;
    bool tAtEnd = false, tIsEnd = false;
    try
    {
        if (mesh)
        {
            MeshPtr m = mesh->as<MeshPtr>(MeshNode::kOutMesh);
            int64_t edge = a.get<int>("edge", -1);
            if (!m || edge < 0)
                throw CommandError("matchCurve: give edge= (a border edge of " + mesh->path() + ")");
            geo::Mesh w = *m;
            glm::dmat4 mw = worldMatrix(*mesh);
            for (D3& p : w.positions())
                p = D3(mw * glm::dvec4(p, 1.0));
            std::optional<Geometry> bg = borderGeometry(w, geo::Index(edge), a.get<double>("edgeT", 0.5));
            if (!bg)
                throw CommandError("matchCurve: edge " + std::to_string(edge) + " is not a border edge of " + mesh->path());
            atEnd = endMode == 0 ? glm::distance(sEnd, bg->p) <= glm::distance(sStart, bg->p) : endMode == 2;
            // The curve runs on into the surface.
            target = reversed(*bg);
        }
        else
        {
            Splines st = worldSplines(*tgt);
            if (st.empty())
                throw CommandError("matchCurve: " + tgt->path() + " is empty");
            t = st.front();
            D3 tS = pointAt(t, 0.0), tE = pointAt(t, spanCount(t));
            if (a.has("targetParameter"))
                tgtMode = 4;
            if (tgtMode == 0 && t.closed)
                tgtMode = 3;
            if (endMode == 0)
            {
                double dS = std::min(glm::distance(sStart, tS), glm::distance(sStart, tE)), dE = std::min(glm::distance(sEnd, tS), glm::distance(sEnd, tE));
                if (tgtMode >= 3)
                {
                    D3 q;
                    closestParam(t, sStart, &q);
                    dS = glm::distance(q, sStart);
                    closestParam(t, sEnd, &q);
                    dE = glm::distance(q, sEnd);
                }
                atEnd = dE <= dS;
            }
            else
                atEnd = endMode == 2;
            D3 from = atEnd ? sEnd : sStart;
            if (tgtMode == 0)
                tgtMode = glm::distance(from, tS) <= glm::distance(from, tE) ? 1 : 2;
            if (tgtMode == 1 || tgtMode == 2)
            {
                tAtEnd = tgtMode == 2;
                tIsEnd = true;
                target = reversed(endGeometry(t, tAtEnd));
            }
            else
            {
                double u = tgtMode == 4 ? std::clamp(a.get<double>("targetParameter"), 0.0, double(spanCount(t))) : closestParam(t, from);
                target = facing(geometryAt(t, u, true), endGeometry(s, atEnd).t);
            }
        }
        if (a.get<bool>("average", false))
        {
            if (!tIsEnd)
                throw CommandError("matchCurve: average needs the target's end (targetEnd start, end or nearest)");
            Geometry avg = averageGeometry(endGeometry(s, atEnd), endGeometry(t, tAtEnd));
            MatchOptions ot = o;
            ot.flip = false;
            Spline tm = matchEnd(t, tAtEnd, reversed(avg), ot);
            Splines all = worldSplines(*tgt);
            all.front() = tm;
            sc.front() = matchEnd(s, atEnd, avg, o);
            setWorldSplines(*tgt, std::move(all));
            setWorldSplines(*c, std::move(sc));
            return json::array({c->path(), tgt->path()});
        }
        sc.front() = matchEnd(s, atEnd, target, o);
    }
    catch (const std::invalid_argument& e)
    {
        throw CommandError(std::string("matchCurve: ") + e.what());
    }
    setWorldSplines(*c, std::move(sc));
    return json::array({c->path()});
}

// trimCurve, extendCurve, splitCurves

json runTrim(const Args& a)
{
    const char* cmd = "trimCurve";
    Graph& g = graph();
    std::vector<Node*> cs = curvesArg(graph(), a, cmd);
    Node* c = cs[0];
    std::vector<Node*> cutters = a.has("cutters") ? namedCurves(a, "cutters", cmd) : otherCurves({c});
    std::erase(cutters, c);
    Splines all = worldSplines(*c);
    if (all.empty())
        throw CommandError("trimCurve: " + c->path() + " is empty");
    size_t si = size_t(std::max(0, a.get<int>("spline", 0)));
    double u = a.get<double>("parameter", 0.0);
    if (a.has("point"))
        std::tie(si, u) = nearestSpline(all, vec3(a, "point"));
    else if (!a.has("parameter"))
        throw CommandError("trimCurve: give parameter= or point= (the stretch to delete)");
    if (si >= all.size())
        throw CommandError("trimCurve: no spline " + std::to_string(si));
    IntersectOptions io = intersectOf(a);
    Splines others = worldOf(cutters);
    for (size_t k = 0; k < all.size(); ++k)
        if (k != si)
            others.push_back(all[k]);
    std::vector<double> cuts = intersectionParams(all[si], others, io);
    Splines left = trimAt(all[si], cuts, u);
    json names = json::array();
    Splines keep = all;
    keep.erase(keep.begin() + ptrdiff_t(si));
    if (!left.empty())
        keep.insert(keep.begin() + ptrdiff_t(si), left.front());
    if (keep.empty())
    {
        components::select(c->id(), ComponentMode::Vertex, {}, components::Op::Replace);
        g.deleteNode(c->id());
    }
    else
    {
        components::select(c->id(), ComponentMode::Vertex, {}, components::Op::Replace);
        setWorldSplines(*c, keep);
        names.push_back(c->path());
    }
    for (size_t k = 1; k < left.size(); ++k)
        names.push_back(createCurve(graph(), {left[k]}, c->name())->path());
    return names;
}

json runExtend(const Args& a)
{
    const char* cmd = "extendCurve";
    std::vector<Node*> cs = curvesArg(graph(), a, cmd);
    double byLength = a.get<double>("length", 0.0);
    std::vector<Node*> bounds = a.has("boundaries") ? namedCurves(a, "boundaries", cmd) : (byLength > 0.0 ? std::vector<Node*>{} : otherCurves(cs));
    ExtendOptions o;
    o.mode = ExtendMode::Smooth;
    if (a.has("mode") && !parseExtendMode(a.get<std::string>("mode"), &o.mode))
        throw CommandError("extendCurve: mode is line, arc or smooth");
    o.maxLength = std::max(0.0, a.get<double>("maxLength", 0.0));
    o.intersect = intersectOf(a);
    int which = choice(a, "end", {"end", "start", "both"}, cmd, 0);
    json names = json::array();
    for (Node* c : cs)
    {
        std::vector<Node*> own = bounds;
        std::erase(own, c);
        Splines bs = worldOf(own);
        Splines all = worldSplines(*c);
        size_t si = a.has("point") && !all.empty() ? nearestSpline(all, vec3(a, "point")).first : 0;
        if (all.empty() || all[si].closed)
            throw CommandError("extendCurve: " + c->path() + " has no open end");
        for (size_t k = 0; k < all.size(); ++k)
            if (k != si)
                bs.push_back(all[k]);
        Spline s = all[si];
        std::vector<bool> ends;
        if (a.has("point"))
        {
            D3 p = vec3(a, "point");
            ends = {glm::distance(p, pointAt(s, spanCount(s))) <= glm::distance(p, pointAt(s, 0.0))};
        }
        else if (which == 2)
            ends = {true, false};
        else
            ends = {which == 0};
        bool any = false;
        for (bool atEnd : ends)
        {
            if (byLength > 0.0 && !a.has("boundaries"))
            {
                s = extendBy(s, atEnd, o.mode, byLength);
                any = true;
                continue;
            }
            ExtendResult r = extendTo(s, atEnd, bs, o);
            if (r.hit)
            {
                s = r.curve;
                any = true;
            }
        }
        if (!any)
            throw CommandError("extendCurve: no boundary curve in reach of " + c->path());
        all[si] = s;
        setWorldSplines(*c, std::move(all));
        names.push_back(c->path());
    }
    return names;
}

json runSplit(const Args& a)
{
    const char* cmd = "splitCurves";
    Graph& g = graph();
    std::vector<Node*> cs = curvesArg(graph(), a, cmd);
    std::vector<Node*> extra = namedCurves(a, "cutters", cmd);
    if (cs.size() < 2 && extra.empty())
        throw CommandError("splitCurves: give at least two curves, or cutters=");
    IntersectOptions io = intersectOf(a);
    std::vector<Splines> before;
    for (Node* c : cs)
        before.push_back(worldSplines(*c));
    json names = json::array();
    std::vector<NodeId> sel;
    for (size_t k = 0; k < cs.size(); ++k)
    {
        Splines others = worldOf(extra);
        for (size_t j = 0; j < cs.size(); ++j)
            if (j != k)
                others.insert(others.end(), before[j].begin(), before[j].end());
        Splines keep, made;
        for (const Spline& s : before[k])
        {
            Splines parts = splitAt(s, intersectionParams(s, others, io));
            if (parts.empty())
                continue;
            keep.push_back(parts.front());
            made.insert(made.end(), parts.begin() + 1, parts.end());
        }
        if (keep.empty())
            continue;
        components::select(cs[k]->id(), ComponentMode::Vertex, {}, components::Op::Replace);
        setWorldSplines(*cs[k], keep);
        names.push_back(cs[k]->path());
        sel.push_back(cs[k]->id());
        for (Spline& m : made)
        {
            Node* n = createCurve(graph(), {m}, cs[k]->name());
            names.push_back(n->path());
            sel.push_back(n->id());
        }
    }
    g.select(sel);
    return names;
}

// fairCurve

json runFair(const Args& a)
{
    FairOptions o;
    o.tolerance = std::max(1e-6, a.get<double>("tolerance", 0.1));
    o.keepTangents = a.get<bool>("keepTangents", true);
    o.keepCorners = a.get<bool>("keepCorners", true);
    o.iterations = std::clamp(a.get<int>("iterations", 200), 1, 10000);
    json out = json::array();
    for (Node* c : curvesArg(graph(), a, "fairCurve"))
    {
        Splines all = worldSplines(*c);
        double worst = 0.0, before = 0.0, after = 0.0;
        for (Spline& s : all)
        {
            FairResult r = fair(s, o);
            s = r.curve;
            worst = std::max(worst, r.maxDeviation);
            before += r.variationBefore;
            after += r.variationAfter;
        }
        components::select(c->id(), ComponentMode::Vertex, {}, components::Op::Replace);
        setWorldSplines(*c, std::move(all));
        log::info("fairCurve: {} moved at most {:.4f} cm, curvature variation {:.4g} -> {:.4g}", c->path(), worst, before, after);
        out.push_back({{"curve", c->path()}, {"maxDeviation", worst}, {"variationBefore", before}, {"variationAfter", after}});
    }
    return out;
}

// curveNetwork

json runNetwork(const Args& a)
{
    const char* cmd = "curveNetwork";
    Graph& g = graph();
    std::vector<Node*> cs = curvesArg(graph(), a, cmd);
    std::vector<Node*> us = namedCurves(a, "uCurves", cmd), vs = namedCurves(a, "vCurves", cmd);
    for (Node* n : us)
        if (std::ranges::find(cs, n) == cs.end())
            cs.push_back(n);
    for (Node* n : vs)
        if (std::ranges::find(cs, n) == cs.end())
            cs.push_back(n);
    if (cs.size() < 3)
        throw CommandError("curveNetwork: give at least three crossing curves");
    Node* op = g.createNode(*types::find("curveNetwork"), {}, kNoNode, true);
    auto set = [&](const char* attr, PlugValue v) { g.setValue({op->id(), op->type().attrIndex(attr), -1}, std::move(v)); };
    set("tolerance", std::max(1e-5, a.get<double>("tolerance", 0.1)));
    set("spansU", int64_t(std::max(1, a.get<int>("spansU", 4))));
    set("spansV", int64_t(std::max(1, a.get<int>("spansV", 4))));
    set("flip", a.get<bool>("flip", false));
    std::string uSlots, vSlots;
    for (size_t k = 0; k < cs.size(); ++k)
    {
        g.connect({cs[k]->id(), CurveShape::kWorldSpace, -1}, {op->id(), op->type().attrIndex("curves"), -1, int(k)});
        if (std::ranges::find(us, cs[k]) != us.end())
            uSlots += std::to_string(k) + " ";
        if (std::ranges::find(vs, cs[k]) != vs.end())
            vSlots += std::to_string(k) + " ";
    }
    set("uSlots", uSlots);
    set("vSlots", vSlots);
    Node* mesh = g.createNode(*types::find("mesh"), a.get<std::string>("name", "Network"), kNoNode, true);
    g.connect({op->id(), op->type().attrIndex("outMesh"), -1}, {mesh->id(), MeshNode::kInMesh, -1});
    g.evaluate();
    MeshPtr made = mesh->as<MeshPtr>(MeshNode::kOutMesh);
    if (!made || made->numFaces() == 0)
    {
        std::string why = op->as<std::string>(op->type().attrIndex("warning"));
        throw CommandError("curveNetwork: " + (why.empty() ? std::string("the curves make no surface") : why));
    }
    json opName = op->path();
    if (!a.get<bool>("constructionHistory", true))
    {
        bake(g, *op, {mesh->id(), MeshNode::kInMesh, -1});
        opName = nullptr;
    }
    g.select({mesh->id()});
    return json::array({mesh->path(), opName});
}

// borderBlend

/** Splits the chain's edges at the stations between its points and returns the mesh point at every station. */
std::vector<geo::Index> pointsAtStations(geo::Mesh& m, const geo::EdgePath& chain, const std::vector<SideStation>& st)
{
    std::vector<geo::Index> out(st.size(), geo::kInvalid);
    std::map<size_t, std::vector<std::pair<double, size_t>>> bySegment;
    for (size_t c = 0; c < st.size(); ++c)
        if (st[c].t <= 1e-9)
            out[c] = chain.points[st[c].segment % chain.points.size()];
        else
            bySegment[st[c].segment].push_back({st[c].t, c});
    for (auto& [seg, list] : bySegment)
    {
        std::sort(list.begin(), list.end());
        geo::Index from = chain.points[seg], to = chain.points[(seg + 1) % chain.points.size()];
        geo::Index e = m.findEdge(from, to);
        double done = 0.0;
        for (auto [t, c] : list)
        {
            if (e == geo::kInvalid)
                break;
            double local = (t - done) / std::max(1e-12, 1.0 - done);
            bool forward = m.edgePoint(e, 0) == from;
            geo::Index np = m.splitEdge(e, forward ? local : 1.0 - local);
            out[c] = np;
            // What is left to split runs from the new point on to `to`.
            if (forward)
                e = m.lastSplitEdge();
            from = np;
            done = t;
        }
    }
    return out;
}

json runBorderBlend(const Args& a)
{
    const char* cmd = "borderBlend";
    Graph& g = graph();
    g.evaluate();
    auto sides = edgeSides(a, cmd);
    struct Side
    {
        Node* mesh = nullptr;
        std::vector<geo::Index> edges;
    };
    std::vector<Side> picked;
    for (auto& [node, edges] : sides)
    {
        MeshPtr cage = cageOf(*node);
        if (!cage)
            continue;
        for (const geo::EdgePath& ch : geo::chainEdges(*cage, edges))
            picked.push_back({node, ch.edges});
    }
    if (picked.size() != 2)
        throw CommandError("borderBlend: select two edge chains (border loops or edge runs), on one mesh or two; found " + std::to_string(picked.size()));
    std::vector<Node*> profiles = namedCurves(a, "profiles", cmd);
    Node* op = g.createNode(*types::find("meshBorderBlend"), {}, kNoNode, true);
    auto attr = [&](const char* name) { return PlugRef{op->id(), op->type().attrIndex(name), -1}; };
    g.setValue(attr("segments"), int64_t(std::max(1, a.get<int>("segments", 6))));
    g.setValue(attr("tensionA"), std::max(0.0, a.get<double>("tensionA", 1.0)));
    g.setValue(attr("tensionB"), std::max(0.0, a.get<double>("tensionB", 1.0)));
    g.setValue(attr("flip"), a.get<bool>("flip", false));
    for (int k = 0; k < 2; ++k)
    {
        const char* m = k == 0 ? "inMeshA" : "inMeshB";
        const char* x = k == 0 ? "matrixA" : "matrixB";
        g.connect({picked[size_t(k)].mesh->id(), MeshNode::kOutMesh, -1}, attr(m));
        g.connect({picked[size_t(k)].mesh->id(), Transform::kWorldMatrix, -1}, attr(x));
        g.setValue(attr(k == 0 ? "edgesA" : "edgesB"), edgeListToJson(picked[size_t(k)].edges));
    }
    for (Node* p : profiles)
        g.connect({p->id(), CurveShape::kWorldSpace, -1}, attr("profiles"));
    g.evaluate();
    MeshPtr strip = op->as<MeshPtr>(op->type().attrIndex("outMesh"));
    if (!strip || strip->numFaces() == 0)
    {
        std::string why = op->as<std::string>(op->type().attrIndex("warning"));
        g.deleteNode(op->id());
        throw CommandError("borderBlend: " + (why.empty() ? std::string("the sides make no strip") : why));
    }
    if (a.get<bool>("merge", false))
    {
        // Rebuilt here rather than read off the node: the merge needs where each column meets each side.
        Node* target = picked[0].mesh;
        PlugRef base = modifiers::editPlug(g, target->id());
        g.evaluate();
        const MeshPtr& stored = std::get<MeshPtr>(g.storedValue(base));
        geo::Mesh mesh = stored ? *stored : geo::Mesh();
        glm::dmat4 wa = worldMatrix(*target), toLocal = glm::inverse(wa);
        auto chainIn = [&](const geo::Mesh& m, const std::vector<geo::Index>& edges) {
            std::vector<geo::EdgePath> ch = geo::chainEdges(m, edges);
            if (ch.size() != 1)
                throw CommandError("borderBlend: merge needs the edges on the mesh's own topology (bake its modifiers first)");
            return ch[0];
        };
        bool sameMesh = picked[1].mesh == target;
        geo::Mesh other;
        if (!sameMesh)
            other = *cageOf(*picked[1].mesh);
        geo::Mesh worldA = mesh;
        for (D3& p : worldA.positions())
            p = D3(wa * glm::dvec4(p, 1.0));
        geo::Mesh worldB = sameMesh ? worldA : other;
        if (!sameMesh)
        {
            glm::dmat4 wb = worldMatrix(*picked[1].mesh);
            for (D3& p : worldB.positions())
                p = D3(wb * glm::dvec4(p, 1.0));
        }
        geo::EdgePath ca = chainIn(worldA, picked[0].edges), cb = chainIn(worldB, picked[1].edges);
        auto centre = [](const geo::Mesh& m, const geo::EdgePath& c) {
            D3 s(0.0);
            for (geo::Index p : c.points)
                s += m.position(p);
            return s / double(c.points.size());
        };
        Splines prof = worldOf(profiles);
        BorderBlendOptions bo;
        bo.segments = std::max(1, a.get<int>("segments", 6));
        bo.tensionA = std::max(0.0, a.get<double>("tensionA", 1.0));
        bo.tensionB = std::max(0.0, a.get<double>("tensionB", 1.0));
        bo.flip = a.get<bool>("flip", false);
        BorderBlendResult r;
        try
        {
            r = borderBlend(borderSide(worldA, ca, centre(worldB, cb)), borderSide(worldB, cb, centre(worldA, ca)), prof, bo);
        }
        catch (const std::invalid_argument& e)
        {
            throw CommandError(std::string("borderBlend: ") + e.what());
        }
        for (D3& p : r.mesh.positions())
            p = D3(toLocal * glm::dvec4(p, 1.0));
        std::vector<geo::Index> atA = pointsAtStations(mesh, ca, r.stationsA);
        std::vector<geo::Index> atB = sameMesh ? pointsAtStations(mesh, cb, r.stationsB) : std::vector<geo::Index>{};
        geo::Index off = mesh.append(r.mesh);
        std::vector<geo::WeldPair> pairs;
        for (size_t c = 0; c < atA.size(); ++c)
            if (atA[c] != geo::kInvalid)
                pairs.push_back({r.rowA[c] + off, atA[c]});
        for (size_t c = 0; c < atB.size(); ++c)
            if (atB[c] != geo::kInvalid)
                pairs.push_back({r.rowB[c] + off, atB[c]});
        geo::targetWeld(mesh, pairs);
        mesh.compact();
        g.deleteNode(op->id());
        for (ComponentMode m : {ComponentMode::Vertex, ComponentMode::Edge, ComponentMode::Face})
            components::select(target->id(), m, {}, components::Op::Replace);
        g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(mesh))));
        if (!sameMesh)
            log::warn("borderBlend: merged into {}; side B is on {} and stays open there", target->path(), picked[1].mesh->path());
        g.select({target->id()});
        return json::array({target->path(), nullptr});
    }
    Node* mesh = g.createNode(*types::find("mesh"), a.get<std::string>("name", "BorderBlend"), kNoNode, true);
    g.connect({op->id(), op->type().attrIndex("outMesh"), -1}, {mesh->id(), MeshNode::kInMesh, -1});
    json opName = op->path();
    if (!a.get<bool>("constructionHistory", true))
    {
        bake(g, *op, {mesh->id(), MeshNode::kInMesh, -1});
        opName = nullptr;
    }
    g.select({mesh->id()});
    return json::array({mesh->path(), opName});
}

void registerVehicleOptionLayouts()
{
    namespace opt = ops::options;
    opt::Override blend;
    blend.order = {"continuityA", "continuityB", "bulgeA", "bulgeB", "endA", "endB"};
    blend.hidden = {"parameterA", "parameterB", "mesh", "edge", "edgeT", "name"};
    blend.ranges["bulgeA"] = blend.ranges["bulgeB"] = {0.01, 10.0, 0.05, ""};
    blend.labels = {{"continuityA", "First Continuity"}, {"continuityB", "Second Continuity"}, {"bulgeA", "First Bulge"}, {"bulgeB", "Second Bulge"},
                    {"endA", "First End"}, {"endB", "Second End"}, {"flipA", "Flip First"}, {"flipB", "Flip Second"}};
    opt::setOverride("blendCurve", std::move(blend));

    opt::Override match;
    match.order = {"continuity", "preserveOther", "average", "flip", "end", "targetEnd"};
    match.hidden = {"targetParameter", "mesh", "edge", "edgeT"};
    opt::setOverride("matchCurve", std::move(match));

    opt::Override fairO;
    fairO.ranges["tolerance"] = {1e-4, 1e4, 0.01, " cm"};
    opt::setOverride("fairCurve", std::move(fairO));

    opt::Override net;
    net.hidden = {"uCurves", "vCurves", "name"};
    net.ranges["tolerance"] = {1e-5, 1e4, 0.01, " cm"};
    opt::setOverride("curveNetwork", std::move(net));

    opt::Override bb;
    bb.hidden = {"meshA", "edgesA", "meshB", "edgesB", "profiles", "name"};
    bb.labels = {{"merge", "Merge into Mesh (bakes)"}};
    opt::setOverride("borderBlend", std::move(bb));

    for (const char* c : {"trimCurve", "extendCurve", "splitCurves"})
    {
        opt::Override o;
        o.hidden = {"direction", "point", "parameter", "spline", "cutters", "boundaries"};
        o.ranges["tolerance"] = {1e-6, 1e4, 0.01, " cm"};
        opt::setOverride(c, std::move(o));
    }
}
} // namespace

void registerVehicleCommands()
{
    ops::registerCommand({
        .name = "blendCurve",
        .doc = "Bridge two curves with a blend curve, live: G0 position, G1 tangent, G2 curvature or G3 curvature-rate continuity "
               "at each end, with bulge. Joins the nearest ends unless endA/endB or parameterA/parameterB say otherwise; mesh= "
               "and edge= blend to a mesh border instead of the second curve. Returns [curve, curveBlend].",
        .positionalDoc = "curveA [curveB]",
        .flags = {{"continuityA", "ca", T::String, "g2", "Continuity with the first curve.", kContinuity},
                  {"continuityB", "cb", T::String, "g2", "Continuity with the second curve or mesh.", kContinuity},
                  {"bulgeA", "ba", T::Float, 1.0, "How far the blend follows the first tangent before turning, in gaps (0.01 to 10).", {}, {}, 0.01, 10.0},
                  {"bulgeB", "bb", T::Float, 1.0, "How far the blend follows the second tangent, in gaps.", {}, {}, 0.01, 10.0},
                  {"endA", "", T::String, "nearest", "Which end of the first curve.", kEnds},
                  {"endB", "", T::String, "nearest", "Which end of the second curve.", kEnds},
                  {"parameterA", "pa", T::Float, nullptr, "Leave the first curve at this point (spans from its start) instead of an end.", {}, {}, 0.0},
                  {"parameterB", "pb", T::Float, nullptr, "Meet the second curve at this point instead of an end.", {}, {}, 0.0},
                  {"flipA", "", T::Bool, false, "Leave the first curve the other way (for a point along it)."},
                  {"flipB", "", T::Bool, false, "Meet the second curve or mesh from the other side."},
                  {"mesh", "m", T::String, nullptr, "Blend to this mesh's border instead of a second curve."},
                  {"edge", "e", T::Int, nullptr, "Border edge of mesh= to blend to.", {}, {}, 0.0},
                  {"edgeT", "", T::Float, 0.5, "Where along the edge, 0 to 1.", {}, {}, 0.0, 1.0},
                  chFlag(),
                  {"name", "n", T::String, nullptr, "Name of the blend curve."}},
        .run = runBlend,
    });

    ops::registerCommand({
        .name = "matchCurve",
        .doc = "Move a curve's end onto another curve (its end or any point) or a mesh border, with G0, G1 or G2 continuity, changing "
               "only the control points that end needs. average moves both curves to meet halfway. Returns the changed curves.",
        .positionalDoc = "curve target",
        .flags = {{"continuity", "c", T::String, "g1", "Position, tangent or curvature.", {"g0", "g1", "g2"}},
                  {"end", "", T::String, "nearest", "Which end of the curve moves.", kEnds},
                  {"targetEnd", "te", T::String, "nearest", "Where on the target: its nearest end, its start or end, or the closest point.",
                   {"nearest", "start", "end", "closest"}},
                  {"targetParameter", "tp", T::Float, nullptr, "Meet the target at this point (spans from its start).", {}, {}, 0.0},
                  {"average", "avg", T::Bool, false, "Both curves move to meet halfway (target ends only)."},
                  {"preserveOther", "po", T::Bool, true, "Keep the curve's other end exactly; spans are added if the curve is too short."},
                  {"flip", "f", T::Bool, false, "Run on the other way along the target."},
                  {"mesh", "m", T::String, nullptr, "Match to this mesh's border instead of a curve."},
                  {"edge", "e", T::Int, nullptr, "Border edge of mesh=.", {}, {}, 0.0},
                  {"edgeT", "", T::Float, 0.5, "Where along the edge, 0 to 1.", {}, {}, 0.0, 1.0}},
        .run = runMatch,
    });

    ops::registerCommand({
        .name = "trimCurve",
        .doc = "Delete the stretch of a curve between the intersections around a point (or parameter): with other curves (cutters, "
               "default every other visible curve), in 3D or as seen along direction. A curve meeting nothing is deleted. Returns "
               "what is left.",
        .positionalDoc = "curve",
        .flags = withFlags({{"point", "p", T::Vec3, nullptr, "World point on (or near) the stretch to delete."},
                            {"parameter", "u", T::Float, nullptr, "The stretch to delete, by span parameter.", {}, {}, 0.0},
                            {"spline", "", T::Int, 0, "Which spline of the curve, with parameter.", {}, {}, 0.0},
                            {"cutters", "", T::List, nullptr, "Curves to trim against; default every other visible curve."}},
                           intersectFlags()),
        .run = runTrim,
    });

    ops::registerCommand({
        .name = "extendCurve",
        .doc = "Extend curve ends to the nearest boundary curve their extension meets (boundaries, default every other visible "
               "curve): straight on, round an arc, or carrying the end span on smoothly. length extends by a distance instead. "
               "Returns the curves.",
        .positionalDoc = "curves",
        .flags = withFlags({{"end", "", T::String, "end", "Which end.", {"end", "start", "both"}},
                            {"point", "p", T::Vec3, nullptr, "Extend the end nearest this world point."},
                            {"mode", "m", T::String, "smooth", "How the extension continues.", {"line", "arc", "smooth"}},
                            {"boundaries", "", T::List, nullptr, "Curves to extend to; default every other visible curve."},
                            {"length", "l", T::Float, 0.0, "Extend by this many cm instead of to a boundary.", {}, {}, 0.0},
                            {"maxLength", "", T::Float, 0.0, "Furthest to look for a boundary, cm; 0 is ten times the scene's curves' size.", {}, {}, 0.0}},
                           intersectFlags()),
        .run = runExtend,
    });

    ops::registerCommand({
        .name = "splitCurves",
        .doc = "Split curves at all their mutual intersections (and with cutters), in 3D or as seen along direction. Returns every piece.",
        .positionalDoc = "curves",
        .flags = withFlags({{"cutters", "", T::List, nullptr, "More curves to split at, left whole."}}, intersectFlags()),
        .run = runSplit,
    });

    ops::registerCommand({
        .name = "fairCurve",
        .doc = "Fair curves: even out their curvature (a smoother comb) without moving any part further than tolerance. Ends stay; "
               "end tangents and corners are kept unless turned off. Returns each curve's largest move.",
        .positionalDoc = "curves",
        .flags = {{"tolerance", "tol", T::Float, 0.1, "Largest move allowed, cm.", {}, {}, 1e-5},
                  {"keepTangents", "kt", T::Bool, true, "Keep the end tangents' directions."},
                  {"keepCorners", "kc", T::Bool, true, "Leave corners sharp and in place."},
                  {"iterations", "it", T::Int, 200, "Most smoothing passes.", {}, {}, 1.0, 10000.0}},
        .run = runFair,
    });

    ops::registerCommand({
        .name = "curveNetwork",
        .doc = "Surface a network of crossing curves with quads, live: the curves sort into U and V families (or uCurves/vCurves "
               "say), become edge loops, and the surface stays smooth across them. Every U curve must cross every V curve; three-sided "
               "patches where one family meets at a point are fine. Returns [mesh, curveNetwork].",
        .positionalDoc = "curves",
        .flags = {{"tolerance", "tol", T::Float, 0.1, "Curves closer than this meet, cm.", {}, {}, 1e-5},
                  {"spansU", "su", T::Int, 4, "Quads along the U curves between neighbouring V curves.", {}, {}, 1.0},
                  {"spansV", "sv", T::Int, 4, "Quads along the V curves between neighbouring U curves.", {}, {}, 1.0},
                  {"uCurves", "", T::List, nullptr, "Curves forced into the U family."},
                  {"vCurves", "", T::List, nullptr, "Curves forced into the V family."},
                  {"flip", "", T::Bool, false, "Flip the faces."},
                  chFlag(),
                  {"name", "n", T::String, nullptr, "Mesh name."}},
        .run = runNetwork,
    });

    ops::registerCommand({
        .name = "borderBlend",
        .doc = "Bridge two mesh border loops or edge chains (the selected edges, on one mesh or two) with a strip of quads tangent "
               "to the faces on both sides, live; profiles shape its cross-section. merge builds it into the first mesh, splitting "
               "border edges where the sides' point counts differ. Returns [mesh, meshBorderBlend].",
        .flags = {{"segments", "s", T::Int, 6, "Quads across the strip.", {}, {}, 1.0},
                  {"tensionA", "ta", T::Float, 1.0, "How far the strip runs on from side A before turning.", {}, {}, 0.0, 10.0},
                  {"tensionB", "tb", T::Float, 1.0, "How far the strip runs on from side B before turning.", {}, {}, 0.0, 10.0},
                  {"flip", "", T::Bool, false, "Flip the faces."},
                  {"profiles", "", T::List, nullptr, "Profile curves drawn across the gap."},
                  {"merge", "", T::Bool, false, "Build the strip into side A's mesh and weld it on (no history)."},
                  {"meshA", "", T::String, nullptr, "Mesh of side A (instead of the selection)."},
                  {"edgesA", "", T::List, nullptr, "Edges of side A."},
                  {"meshB", "", T::String, nullptr, "Mesh of side B."},
                  {"edgesB", "", T::List, nullptr, "Edges of side B."},
                  chFlag(),
                  {"name", "n", T::String, nullptr, "Mesh name."}},
        .run = runBorderBlend,
    });
    registerVehicleOptionLayouts();
}
} // namespace lila::curve
