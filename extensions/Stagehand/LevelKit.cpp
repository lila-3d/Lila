// The level kit: modular snap points (addSnapPoint, snapPoints, snapTo, snapAll, openSnapPoints) and placement helpers
// (dropToSurface, alignObjects, distribute, randomizeTransforms).

#include "Kit.h"

#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/Mesh.h>
#include <lila/Profile.h>
#include <lila/Relocate.h>
#include <lila/Transform.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <random>
#include <set>

using namespace lila;

namespace stagehand
{
namespace kit
{
namespace
{
bool hasAssetTag(Graph& g, NodeId n)
{
    for (NodeId c : g.children(n))
        if (Node* k = g.node(c); k && k->type().name == "stagehandAsset")
            return true;
    return false;
}

SnapPoint pointOf(Graph& g, Node& n)
{
    SnapPoint p;
    p.node = n.id();
    p.piece = pieceOf(g, n.id());
    p.type = n.as<std::string>(n.type().attrIndex("snapType"));
    p.accepts = acceptsOf(n);
    p.world = n.as<glm::dmat4>(scene::Transform::kWorldMatrix);
    return p;
}

glm::dvec3 axis(const glm::dmat4& m, int c) { return glm::normalize(glm::dvec3(m[c])); }
} // namespace

std::vector<std::string> acceptsOf(const Node& n)
{
    std::vector<std::string> out;
    for (const auto& [i, v] : n.elements(n.type().attrIndex("accepts")))
        if (const auto* t = std::get_if<std::string>(&v); t && !t->empty())
            out.push_back(*t);
    return out;
}

NodeId pieceOf(Graph& g, NodeId n)
{
    // A placed library asset is the piece; otherwise the top of the hierarchy.
    NodeId at = g.parent(n), top = n;
    while (at != scene::kNoNode)
    {
        if (hasAssetTag(g, at))
            return at;
        top = at;
        at = g.parent(at);
    }
    return top;
}

std::vector<SnapPoint> snapPointsOf(Graph& g, NodeId piece)
{
    g.evaluate();
    std::vector<SnapPoint> out;
    std::vector<NodeId> stack{piece};
    while (!stack.empty())
    {
        const NodeId id = stack.back();
        stack.pop_back();
        if (Node* n = g.node(id); n && n->type().name == kSnapType)
            out.push_back(pointOf(g, *n));
        for (NodeId c : g.children(id))
            stack.push_back(c);
    }
    return out;
}

std::vector<SnapPoint> allSnapPoints(Graph& g)
{
    g.evaluate();
    std::vector<SnapPoint> out;
    for (NodeId id : g.nodes())
        if (Node* n = g.node(id); n && n->type().name == kSnapType && scene::effectivelyVisible(g, id))
            out.push_back(pointOf(g, *n));
    return out;
}

bool compatible(const SnapPoint& a, const SnapPoint& b)
{
    if (a.piece == b.piece)
        return false;
    auto takes = [](const SnapPoint& x, const std::string& t) { return std::find(x.accepts.begin(), x.accepts.end(), t) != x.accepts.end(); };
    if (a.accepts.empty() && b.accepts.empty())
        return a.type == b.type;
    return (a.accepts.empty() || takes(a, b.type)) && (b.accepts.empty() || takes(b, a.type));
}

glm::dmat4 joinedPieceMatrix(Graph& g, const SnapPoint& mover, const SnapPoint& target)
{
    // The mover's point lands on the target's, turned half round its up so the two face each other.
    const glm::dmat4 face = glm::rotate(glm::dmat4(1.0), 3.14159265358979323846, glm::dvec3(0, 1, 0));
    glm::dmat4 t = target.world;
    // Unscaled frames, so a scaled piece still lands exactly on the point.
    for (int c = 0; c < 3; ++c)
        t[c] = glm::dvec4(axis(t, c), 0.0);
    glm::dmat4 m = mover.world;
    for (int c = 0; c < 3; ++c)
        m[c] = glm::dvec4(axis(m, c), 0.0);
    const glm::dmat4 piece = g.node(mover.piece)->as<glm::dmat4>(scene::Transform::kWorldMatrix);
    // piece * pointInPiece = mover's frame; keep pointInPiece, put the frame where the target says.
    const glm::dmat4 pointInPiece = glm::inverse(piece) * m;
    return t * face * glm::inverse(pointInPiece);
}

bool joined(const SnapPoint& a, const SnapPoint& b, double tolerance)
{
    if (glm::length(glm::dvec3(a.world[3]) - glm::dvec3(b.world[3])) > tolerance)
        return false;
    const double facing = glm::dot(axis(a.world, 2), -axis(b.world, 2));
    const double ups = glm::dot(axis(a.world, 1), axis(b.world, 1));
    const double limit = std::cos(glm::radians(2.0));
    return facing >= limit && ups >= limit;
}
} // namespace kit

namespace
{
using json = nlohmann::json;
using T = ArgType;
using kit::SnapPoint;

json v3(const glm::dvec3& v) { return json::array({v.x, v.y, v.z}); }

glm::dvec3 vec(const json& j, const glm::dvec3& fallback)
{
    if (!j.is_array() || j.size() != 3)
        return fallback;
    return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}

NodeId nodeArg(Graph& g, const std::string& name, const char* cmd)
{
    Node* n = g.find(name);
    if (!n)
        throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
    return n->id();
}

json pointJson(Graph& g, const SnapPoint& p, const std::vector<SnapPoint>& all, double tolerance)
{
    json joinedTo = nullptr;
    for (const SnapPoint& o : all)
        if (o.node != p.node && kit::compatible(p, o) && kit::joined(p, o, tolerance))
            joinedTo = g.node(o.node)->path();
    return {{"node", g.node(p.node)->path()},
            {"piece", g.node(p.piece)->path()},
            {"type", p.type},
            {"accepts", p.accepts},
            {"position", v3(glm::dvec3(p.world[3]))},
            {"normal", v3(glm::normalize(glm::dvec3(p.world[2])))},
            {"joinedTo", joinedTo}};
}

/** World bounds of every mesh under a node. */
bool worldBoundsOf(Graph& g, NodeId id, glm::dvec3& lo, glm::dvec3& hi)
{
    glm::dvec3 a, b;
    if (!boundsUnder(g, id, a, b))
        return false;
    const glm::dmat4 w = g.node(id)->as<glm::dmat4>(scene::Transform::kWorldMatrix);
    lo = glm::dvec3(1e300);
    hi = glm::dvec3(-1e300);
    for (int k = 0; k < 8; ++k)
    {
        const glm::dvec3 q(w * glm::dvec4((k & 1) ? b.x : a.x, (k & 2) ? b.y : a.y, (k & 4) ? b.z : a.z, 1.0));
        lo = glm::min(lo, q);
        hi = glm::max(hi, q);
    }
    return true;
}

void moveBy(Graph& g, NodeId id, const glm::dvec3& by)
{
    const glm::dmat4 w = g.node(id)->as<glm::dmat4>(scene::Transform::kWorldMatrix);
    setWorld(g, id, glm::translate(glm::dmat4(1.0), by) * w);
    g.evaluate();
}

int axisArg(const Args& a, const char* cmd)
{
    const std::string s = a.get<std::string>("axis", "x");
    if (s == "x")
        return 0;
    if (s == "y")
        return 1;
    if (s == "z")
        return 2;
    throw CommandError(std::string(cmd) + ": axis is x, y or z");
}
} // namespace

bool registerKitNodes(ext::Context& ctx)
{
    const scene::NodeType* locator = scene::types::find("locator");
    if (!locator)
        return false;
    scene::NodeType t;
    t.name = kit::kSnapType;
    t.base = locator;
    t.dag = true;
    t.icon = "plug-connected";
    t.doc = "A modular kit snap point: where the piece it sits under joins another. Its +Z points out of the piece along the join, "
            "+Y up. Pieces snap when their points' types are compatible.";
    t.ownAttrs = {
        {.name = "snapType", .type = scene::PlugType::String, .defaultValue = std::string("joint"), .doc = "What kind of join this is: wallEnd, floorEdge, doorFrame..."},
        {.name = "accepts", .type = scene::PlugType::String, .defaultValue = std::string(),
         .flags = scene::AttrInput | scene::AttrStorable | scene::AttrArray, .doc = "Types it joins to; empty joins only its own type."},
    };
    t.create = locator->create;
    lila::scene::relocate::registerNeutral(t.name);
    return scene::types::registerType(ctx, std::move(t)) != nullptr;
}

void registerKitCommands(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = "addSnapPoint",
        .doc = "Add a modular snap point to a piece: where it joins other pieces, facing out along normal. Returns the point's path.\n\n"
               "position and normal are in the piece's own space. Saved to the library with libraryAdd and carried by every placement.",
        .positionalDoc = "piece",
        .flags = {{"type", "t", T::String, "joint", "What kind of join: wallEnd, floorEdge, doorFrame..."},
                  {"accepts", "ac", T::List, json::array(), "Types it joins to; empty joins only its own type."},
                  {"position", "p", T::List, json::array({0.0, 0.0, 0.0}), "Where, in the piece's space, cm."},
                  {"normal", "nr", T::List, json::array({0.0, 0.0, 1.0}), "Out of the piece along the join, in its space."},
                  {"up", "u", T::List, json::array({0.0, 1.0, 0.0}), "Up, in its space."},
                  {"name", "n", T::String, "snap", "Name of the point."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const std::vector<std::string> objs = a.objects();
            if (objs.size() != 1)
                throw CommandError("addSnapPoint: give one piece");
            const NodeId piece = nodeArg(g, objs[0], "addSnapPoint");
            const json made = ops::execute("createNode", {{"args", {kit::kSnapType}}, {"name", a.get<std::string>("name", "snap")},
                                                          {"parent", g.node(piece)->path()}, {"skipSelect", true}});
            const std::string path = made.get<std::string>();
            const glm::dvec3 z = glm::normalize(vec(a.flags["normal"], {0, 0, 1}));
            glm::dvec3 y = vec(a.flags["up"], {0, 1, 0});
            y = glm::normalize(y - z * glm::dot(y, z));
            const glm::dvec3 x = glm::cross(y, z);
            glm::dmat4 m(1.0);
            m[0] = glm::dvec4(x, 0);
            m[1] = glm::dvec4(y, 0);
            m[2] = glm::dvec4(z, 0);
            m[3] = glm::dvec4(vec(a.flags["position"], glm::dvec3(0.0)), 1);
            scene::Transform::setLocalMatrix(g, g.find(path)->id(), m);
            ops::execute("setAttr", {{"args", {path + ".snapType", a.get<std::string>("type", "joint")}}});
            ops::execute("setAttr", {{"args", {path + ".accepts", a.flags["accepts"]}}});
            return path;
        },
        .examples = {"addSnapPoint('Wall_A', type='wallEnd', position=[200, 0, 0], normal=[1, 0, 0])",
                     "addSnapPoint('Wall_A', type='wallEnd', position=[-200, 0, 0], normal=[-1, 0, 0])"},
    });

    ctx.registerCommand({
        .name = "snapPoints",
        .doc = "List snap points: node, piece, type, accepts, world position and normal, and the point it's joined to (or null).",
        .positionalDoc = "pieces (default every snap point in the scene)",
        .flags = {{"tolerance", "tol", T::Float, 0.5, "How close counts as joined, cm."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const std::vector<SnapPoint> all = kit::allSnapPoints(g);
            std::vector<SnapPoint> pts;
            if (a.positional.empty())
                pts = all;
            else
                for (const json& n : a.positional)
                    for (const SnapPoint& p : kit::snapPointsOf(g, nodeArg(g, n.get<std::string>(), "snapPoints")))
                        pts.push_back(p);
            json out = json::array();
            for (const SnapPoint& p : pts)
                out.push_back(pointJson(g, p, all, a.get<double>("tolerance", 0.5)));
            return out;
        },
    });

    ctx.registerCommand({
        .name = "snapTo",
        .doc = "Join a piece onto another by their snap points: the nearest compatible pair, or the ones named. The target stays put. "
               "Returns the points joined.",
        .positionalDoc = "piece to move",
        .flags = {{"target", "tg", T::String, nullptr, "The piece to join onto."},
                  {"point", "pt", T::String, nullptr, "The moving piece's snap point to use."},
                  {"targetPoint", "tp", T::String, nullptr, "The target's snap point to use."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const std::vector<std::string> objs = a.objects();
            if (objs.size() != 1 || !a.has("target"))
                throw CommandError("snapTo: give one piece and target=");
            const NodeId piece = kit::pieceOf(g, nodeArg(g, objs[0], "snapTo"));
            const NodeId target = nodeArg(g, a.get<std::string>("target"), "snapTo");
            std::vector<SnapPoint> mine = kit::snapPointsOf(g, piece), theirs = kit::snapPointsOf(g, target);
            const NodeId wantPoint = a.has("point") ? nodeArg(g, a.get<std::string>("point"), "snapTo") : scene::kNoNode;
            const NodeId wantTarget = a.has("targetPoint") ? nodeArg(g, a.get<std::string>("targetPoint"), "snapTo") : scene::kNoNode;
            const SnapPoint* best[2] = {nullptr, nullptr};
            double bestD = 1e300;
            for (const SnapPoint& m : mine)
            {
                if (wantPoint != scene::kNoNode && m.node != wantPoint)
                    continue;
                for (const SnapPoint& t : theirs)
                {
                    if (wantTarget != scene::kNoNode && t.node != wantTarget)
                        continue;
                    if (!kit::compatible(m, t))
                        continue;
                    const double d = glm::length(glm::dvec3(m.world[3]) - glm::dvec3(t.world[3]));
                    if (d < bestD)
                        bestD = d, best[0] = &m, best[1] = &t;
                }
            }
            if (!best[0])
                throw CommandError("snapTo: no compatible snap points between " + g.node(piece)->path() + " and " + g.node(target)->path());
            setWorld(g, piece, kit::joinedPieceMatrix(g, *best[0], *best[1]));
            return {{"point", g.node(best[0]->node)->path()}, {"targetPoint", g.node(best[1]->node)->path()}, {"distance", bestD}};
        },
        .examples = {"snapTo('Wall_B', target='Wall_A')"},
    });

    ctx.registerCommand({
        .name = "snapAll",
        .doc = "Join every piece whose compatible snap point is within tolerance of another's, in order: the first piece stays, each "
               "later one moves onto what is already placed. Returns the joins.",
        .positionalDoc = "pieces (default every piece with snap points)",
        .flags = {{"tolerance", "tol", T::Float, 25.0, "How near two points have to be to join, cm."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            std::vector<NodeId> pieces;
            if (a.positional.empty())
            {
                std::set<NodeId> seen;
                for (const SnapPoint& p : kit::allSnapPoints(g))
                    if (seen.insert(p.piece).second)
                        pieces.push_back(p.piece);
            }
            else
                for (const json& n : a.positional)
                    pieces.push_back(kit::pieceOf(g, nodeArg(g, n.get<std::string>(), "snapAll")));
            const double tol = a.get<double>("tolerance", 25.0);
            json joins = json::array();
            std::set<NodeId> placed;
            if (!pieces.empty())
                placed.insert(pieces[0]);
            for (size_t k = 1; k < pieces.size(); ++k)
            {
                const std::vector<SnapPoint> mine = kit::snapPointsOf(g, pieces[k]);
                const SnapPoint* best[2] = {nullptr, nullptr};
                double bestD = tol;
                std::vector<SnapPoint> fixed;
                for (NodeId p : placed)
                    for (const SnapPoint& s : kit::snapPointsOf(g, p))
                        fixed.push_back(s);
                for (const SnapPoint& m : mine)
                    for (const SnapPoint& f : fixed)
                    {
                        const double d = glm::length(glm::dvec3(m.world[3]) - glm::dvec3(f.world[3]));
                        if (kit::compatible(m, f) && d <= bestD)
                            bestD = d, best[0] = &m, best[1] = &f;
                    }
                if (best[0])
                {
                    setWorld(g, pieces[k], kit::joinedPieceMatrix(g, *best[0], *best[1]));
                    g.evaluate();
                    joins.push_back({{"piece", g.node(pieces[k])->path()}, {"point", g.node(best[0]->node)->path()}, {"to", g.node(best[1]->node)->path()}});
                }
                placed.insert(pieces[k]);
            }
            return joins;
        },
    });

    ctx.registerCommand({
        .name = "openSnapPoints",
        .doc = "Snap points joined to nothing: gaps in a kit layout. Returns them as snapPoints does.",
        .positionalDoc = "pieces (default the whole scene)",
        .flags = {{"tolerance", "tol", T::Float, 0.5, "How close counts as joined, cm."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const std::vector<SnapPoint> all = kit::allSnapPoints(g);
            std::set<NodeId> only;
            for (const json& n : a.positional)
                only.insert(kit::pieceOf(g, nodeArg(g, n.get<std::string>(), "openSnapPoints")));
            json out = json::array();
            for (const SnapPoint& p : all)
            {
                if (!only.empty() && !only.count(p.piece))
                    continue;
                const json j = pointJson(g, p, all, a.get<double>("tolerance", 0.5));
                if (j["joinedTo"].is_null())
                    out.push_back(j);
            }
            return out;
        },
    });

    ctx.registerCommand({
        .name = "dropToSurface",
        .doc = "Move objects straight down until their bottoms touch the first surface below, as Unreal's End key does. No physics: "
               "they keep their turn. align=True also stands them along that surface. Returns how far each moved.",
        .positionalDoc = "objects (default the selection)",
        .flags = {{"align", "al", T::Bool, false, "Stand along the surface normal."}, {"offset", "of", T::Float, 0.0, "Stop this far above it, cm."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            g.evaluate();
            json out = json::array();
            for (const std::string& name : a.objects())
            {
                const NodeId id = nodeArg(g, name, "dropToSurface");
                glm::dvec3 lo, hi;
                if (!worldBoundsOf(g, id, lo, hi))
                    continue;
                std::set<NodeId> skip;
                for (NodeId m : meshesOf(g, id))
                    skip.insert(m);
                const glm::dvec3 bottom((lo.x + hi.x) * 0.5, lo.y, (lo.z + hi.z) * 0.5);
                const auto hit = surfaceUnder(g, bottom, skip, 0.01);
                if (!hit)
                {
                    out.push_back({{"node", name}, {"moved", 0.0}, {"hit", false}});
                    continue;
                }
                const double dy = hit->point.y + a.get<double>("offset", 0.0) - lo.y;
                glm::dmat4 w = glm::translate(glm::dmat4(1.0), glm::dvec3(0, dy, 0)) * g.node(id)->as<glm::dmat4>(scene::Transform::kWorldMatrix);
                if (a.get<bool>("align", false))
                {
                    const glm::dvec3 up(0, 1, 0), n = glm::normalize(hit->normal);
                    const double d = glm::dot(up, n);
                    if (d < 1.0 - 1e-9 && d > -1.0 + 1e-9)
                    {
                        const glm::dquat q = glm::normalize(glm::dquat(1.0 + d, glm::cross(up, n)));
                        w = glm::translate(glm::dmat4(1.0), hit->point) * glm::mat4_cast(q) * glm::translate(glm::dmat4(1.0), -hit->point) * w;
                    }
                }
                setWorld(g, id, w);
                out.push_back({{"node", g.node(id)->path()}, {"moved", -dy}, {"hit", true}});
            }
            return out;
        },
        .examples = {"dropToSurface(['chair_1', 'chair_2'])"},
    });

    ctx.registerCommand({
        .name = "alignObjects",
        .doc = "Line objects up on one axis by their bounds: min, max or center of all of them, or to the first or last selected.",
        .positionalDoc = "objects (default the selection)",
        .flags = {{"axis", "ax", T::String, "x", "x, y or z.", {"x", "y", "z"}},
                  {"to", "to", T::String, "min", "min, max, center, first or last.", {"min", "max", "center", "first", "last"}},
                  {"side", "sd", T::String, "min", "Which side of each object lines up: min, max or center.", {"min", "max", "center"}}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            g.evaluate();
            const int ax = axisArg(a, "alignObjects");
            const std::string to = a.get<std::string>("to", "min"), side = a.get<std::string>("side", "min");
            std::vector<std::pair<NodeId, std::pair<glm::dvec3, glm::dvec3>>> objs;
            for (const std::string& n : a.objects())
            {
                glm::dvec3 lo, hi;
                const NodeId id = nodeArg(g, n, "alignObjects");
                if (worldBoundsOf(g, id, lo, hi))
                    objs.push_back({id, {lo, hi}});
            }
            if (objs.size() < 2)
                throw CommandError("alignObjects: give two or more objects with meshes");
            auto sideOf = [&](const std::pair<glm::dvec3, glm::dvec3>& b) {
                return side == "max" ? b.second[ax] : side == "center" ? (b.first[ax] + b.second[ax]) * 0.5 : b.first[ax];
            };
            double goal = 0.0;
            if (to == "first")
                goal = sideOf(objs.front().second);
            else if (to == "last")
                goal = sideOf(objs.back().second);
            else
            {
                double lo = 1e300, hi = -1e300;
                for (const auto& o : objs)
                {
                    lo = std::min(lo, o.second.first[ax]);
                    hi = std::max(hi, o.second.second[ax]);
                }
                goal = to == "max" ? hi : to == "center" ? (lo + hi) * 0.5 : lo;
            }
            json out = json::array();
            for (const auto& o : objs)
            {
                glm::dvec3 by(0.0);
                by[ax] = goal - sideOf(o.second);
                moveBy(g, o.first, by);
                out.push_back(g.node(o.first)->path());
            }
            return out;
        },
    });

    ctx.registerCommand({
        .name = "distribute",
        .doc = "Space objects evenly along an axis by their bounds: equal gaps between the outermost two, or spacing= cm apart.",
        .positionalDoc = "objects (default the selection)",
        .flags = {{"axis", "ax", T::String, "x", "x, y or z.", {"x", "y", "z"}}, {"spacing", "sp", T::Float, nullptr, "Gap between neighbours, cm."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            g.evaluate();
            const int ax = axisArg(a, "distribute");
            struct O
            {
                NodeId id;
                glm::dvec3 lo, hi;
            };
            std::vector<O> objs;
            for (const std::string& n : a.objects())
            {
                O o{nodeArg(g, n, "distribute")};
                if (worldBoundsOf(g, o.id, o.lo, o.hi))
                    objs.push_back(o);
            }
            if (objs.size() < 2)
                throw CommandError("distribute: give two or more objects with meshes");
            std::sort(objs.begin(), objs.end(), [&](const O& x, const O& y) { return x.lo[ax] < y.lo[ax]; });
            double total = 0.0;
            for (const O& o : objs)
                total += o.hi[ax] - o.lo[ax];
            const double span = objs.back().hi[ax] - objs.front().lo[ax];
            const double gap = a.has("spacing") ? a.get<double>("spacing") : (span - total) / double(objs.size() - 1);
            double at = objs.front().lo[ax];
            json out = json::array();
            for (const O& o : objs)
            {
                glm::dvec3 by(0.0);
                by[ax] = at - o.lo[ax];
                moveBy(g, o.id, by);
                at += (o.hi[ax] - o.lo[ax]) + gap;
                out.push_back(g.node(o.id)->path());
            }
            return out;
        },
    });

    ctx.registerCommand({
        .name = "randomizeTransforms",
        .doc = "Nudge objects by seeded random amounts: up to translate cm and rotate degrees either way per axis, scale between "
               "the two given. Turning is about each object's own bounds centre.",
        .positionalDoc = "objects (default the selection)",
        .flags = {{"translate", "t", T::List, json::array({0.0, 0.0, 0.0}), "Most it moves on each axis, cm."},
                  {"rotate", "r", T::List, json::array({0.0, 15.0, 0.0}), "Most it turns about each axis, degrees."},
                  {"scale", "s", T::List, json::array({1.0, 1.0}), "Uniform scale range [min, max]."},
                  {"seed", "sd", T::Int, int64_t(1), "Another seed gives other amounts."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            g.evaluate();
            std::mt19937 rng(uint32_t(a.get<int64_t>("seed", 1)) * 2654435761u + 1u);
            std::uniform_real_distribution<double> u(-1.0, 1.0);
            const glm::dvec3 t = vec(a.flags["translate"], glm::dvec3(0.0)), r = vec(a.flags["rotate"], glm::dvec3(0.0));
            const json& sc = a.flags["scale"];
            const double smin = sc.is_array() && sc.size() == 2 ? sc[0].get<double>() : 1.0, smax = sc.is_array() && sc.size() == 2 ? sc[1].get<double>() : 1.0;
            json out = json::array();
            for (const std::string& n : a.objects())
            {
                const NodeId id = nodeArg(g, n, "randomizeTransforms");
                glm::dvec3 lo, hi;
                const glm::dmat4 w = g.node(id)->as<glm::dmat4>(scene::Transform::kWorldMatrix);
                const glm::dvec3 c = worldBoundsOf(g, id, lo, hi) ? (lo + hi) * 0.5 : glm::dvec3(w[3]);
                const glm::dvec3 move(t.x * u(rng), t.y * u(rng), t.z * u(rng));
                const glm::dvec3 turn(r.x * u(rng), r.y * u(rng), r.z * u(rng));
                const double s = smin + (smax - smin) * (u(rng) * 0.5 + 0.5);
                glm::dmat4 m = glm::translate(glm::dmat4(1.0), c + move);
                m = glm::rotate(m, glm::radians(turn.y), glm::dvec3(0, 1, 0));
                m = glm::rotate(m, glm::radians(turn.x), glm::dvec3(1, 0, 0));
                m = glm::rotate(m, glm::radians(turn.z), glm::dvec3(0, 0, 1));
                m = glm::scale(m, glm::dvec3(s));
                setWorld(g, id, m * glm::translate(glm::dmat4(1.0), -c) * w);
                out.push_back(g.node(id)->path());
            }
            return out;
        },
        .examples = {"randomizeTransforms(['crate_1', 'crate_2', 'crate_3'], rotate=[0, 25, 0], scale=[0.9, 1.1])"},
    });
}
} // namespace stagehand
