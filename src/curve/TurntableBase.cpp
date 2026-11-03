#include "curve/TurntableBase.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "geo/Mesh.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"
#include "scene/NodeBounds.h"
#include "scene/Turntable.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <string_view>

namespace lila::curve
{
namespace
{
using namespace scene;
using T = ArgType;
namespace tt = scene::turntable;

/** A profile in (radius, height) from the top face's centre, top first, ending on the axis underneath. */
using BaseProfile = std::vector<glm::dvec2>;

/** A quarter round at a corner: from `from` along the first edge to `to`, centred on the corner offset inwards. */
void roundCorner(BaseProfile& p, glm::dvec2 corner, glm::dvec2 in, glm::dvec2 out, double radius, int segments)
{
    if (radius <= 1e-6 || segments < 1)
    {
        p.push_back(corner);
        return;
    }
    // in: direction arriving at the corner, out: direction leaving it; both unit and perpendicular.
    const glm::dvec2 centre = corner - in * radius + out * radius;
    const glm::dvec2 a = -out, b = in;
    for (int i = 0; i <= segments; ++i)
    {
        const double t = double(i) / segments * glm::half_pi<double>();
        p.push_back(centre + (a * std::cos(t) + b * std::sin(t)) * radius);
    }
}

/** A disc from height top down to bottom, radius r, with its outer edges rounded by bevel. */
void disc(BaseProfile& p, double r, double top, double bottom, double bevel, int segments)
{
    const double b = std::min({bevel, r * 0.5, (top - bottom) * 0.5});
    roundCorner(p, {r, top}, {1, 0}, {0, -1}, b, segments);
    roundCorner(p, {r, bottom}, {0, -1}, {-1, 0}, b, segments);
}

BaseProfile profileFor(std::string_view style, double r, double h, double bevel)
{
    const int seg = 4;
    BaseProfile p{{0.0, 0.0}};
    if (style == "stepped")
    {
        const double b = std::min(bevel, h * 0.2);
        roundCorner(p, {r, 0.0}, {1, 0}, {0, -1}, b, seg);
        p.push_back({r, -h * 0.5});
        disc(p, r * 1.15, -h * 0.5, -h, b, seg);
    }
    else if (style == "record")
    {
        // A thin plate with grooved rings on top and a raised lip round the edge.
        const double plate = h * 0.5, lip = h * 0.25, groove = h * 0.06;
        for (double f : {0.35, 0.55, 0.75})
        {
            const double w = r * 0.02;
            p.push_back({r * f - w, 0.0});
            p.push_back({r * f - w * 0.5, -groove});
            p.push_back({r * f + w * 0.5, -groove});
            p.push_back({r * f + w, 0.0});
        }
        p.push_back({r * 0.92, 0.0});
        p.push_back({r * 0.92, lip});
        disc(p, r, lip, -plate, std::min(bevel, lip), seg);
    }
    else if (style == "pedestal")
    {
        const double plate = h, column = std::max(r * 1.2, h * 4.0), foot = h * 1.2;
        disc(p, r, 0.0, -plate, bevel, seg);
        p.push_back({r * 0.55, -plate});
        p.push_back({r * 0.55, -plate - column});
        disc(p, r * 1.1, -plate - column, -plate - column - foot, bevel, seg);
    }
    else if (style == "lazySusan")
        disc(p, r, 0.0, -h * 0.5, bevel, seg); // the turning top; the foot is its own profile
    else
        disc(p, r, 0.0, -h, bevel, seg);
    p.push_back({0.0, p.back().y});
    return p;
}

/** The fixed foot under a lazy susan's top. */
BaseProfile footFor(double r, double h, double bevel)
{
    BaseProfile p{{0.0, -h * 0.55}};
    disc(p, r * 0.85, -h * 0.55, -h * 1.3, bevel, 4);
    p.push_back({0.0, p.back().y});
    return p;
}

double signedVolume(const geo::Mesh& m)
{
    double v = 0.0;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        const std::vector<geo::Index> pts = m.facePoints(f);
        for (size_t i = 1; i + 1 < pts.size(); ++i)
            v += glm::dot(m.position(pts[0]), glm::cross(m.position(pts[i]), m.position(pts[i + 1])));
    }
    return v / 6.0;
}

struct Made
{
    NodeId mesh = kNoNode, curve = kNoNode, revolve = kNoNode;
};

/** Revolves profile about the axis through pivot into a mesh; the profile curve is kept, hidden, for live edits. */
Made revolve(Graph& g, const BaseProfile& profile, const glm::dvec3& pivot, const glm::dvec3& up, int sections, const std::string& name)
{
    // The profile lies in the plane of up and a horizontal axis.
    const glm::dvec3 side = std::abs(up.x) < 0.9 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 0, 1);
    json pts = json::array();
    for (const glm::dvec2& q : profile)
    {
        const glm::dvec3 w = pivot + side * q.x + up * q.y;
        pts.push_back({w.x, w.y, w.z});
    }
    Made out;
    const std::string curve = ops::execute("curve", {{"points", pts}, {"type", "poly"}, {"name", name + "Profile"}}).get<std::string>();
    out.curve = g.find(curve)->id();
    const json made = ops::execute("revolve", {{"args", {curve}},
                                               {"axisOrigin", {pivot.x, pivot.y, pivot.z}},
                                               {"axis", {up.x, up.y, up.z}},
                                               {"sections", sections},
                                               {"name", name}});
    out.mesh = g.find(made[0].get<std::string>())->id();
    out.revolve = g.find(made[1].get<std::string>())->id();
    g.evaluate();
    // Profiles run outward over the top and back underneath; whichever way that faces, the faces point out.
    if (const MeshPtr& m = g.node(out.mesh)->as<MeshPtr>(MeshNode::kOutMesh); m && signedVolume(*m) < 0.0)
        g.setValue({out.revolve, g.node(out.revolve)->type().attrIndex("flip"), -1}, true);
    g.setValue({out.curve, Transform::kVisibility, -1}, false);
    return out;
}

struct Preset
{
    const char* name;
    const char* material;
    glm::dvec3 color;
    double roughness, metallic;
};

constexpr Preset kPresets[] = {
    {"matteBlack", "M_TurntableMatteBlack", {0.03, 0.03, 0.03}, 0.85, 0.0},
    {"satin", "M_TurntableSatin", {0.18, 0.18, 0.19}, 0.35, 0.0},
    {"rubber", "M_TurntableRubber", {0.05, 0.05, 0.05}, 0.95, 0.0},
    {"brushedMetal", "M_TurntableBrushedMetal", {0.62, 0.62, 0.64}, 0.4, 1.0},
    {"wood", "M_TurntableWood", {0.32, 0.17, 0.08}, 0.6, 0.0},
    {"marble", "M_TurntableMarble", {0.8, 0.79, 0.76}, 0.15, 0.0},
};

/** Sets the revolves' material to a preset, made on first use, so the bases keep their live history. */
void applyMaterial(Graph& g, const std::string& preset, const std::vector<NodeId>& revolves)
{
    if (preset == "default")
        return;
    const Preset* p = nullptr;
    for (const Preset& k : kPresets)
        if (preset == k.name)
            p = &k;
    if (!p)
        throw CommandError("turntableBase: unknown material '" + preset + "'");
    if (!materials::find(g, p->material))
        ops::execute("createMaterial", {{"args", {p->material}},
                                        {"baseColor", {p->color.x, p->color.y, p->color.z}},
                                        {"roughness", p->roughness},
                                        {"metallic", p->metallic}});
    for (NodeId id : revolves)
        g.setValue({id, g.node(id)->type().attrIndex("material"), -1}, std::string(p->material));
}

constexpr const char* kBaseAttr = "turntableBase";

/** The base group id is, or is under; kNoNode when none. */
NodeId baseAbove(const Graph& g, NodeId id)
{
    for (NodeId cur = id; cur != kNoNode; cur = g.parent(cur))
        if (const Node* n = g.node(cur); n && n->type().attrIndex(kBaseAttr) >= 0)
            return cur;
    return kNoNode;
}

/** The base standing under a turntable's members, or kNoNode. */
NodeId baseOf(const Graph& g, NodeId table)
{
    for (NodeId m : tt::members(g, table))
        if (NodeId b = baseAbove(g, m); b != kNoNode)
            return b;
    return kNoNode;
}

/** The turntable a base's meshes are on, or kNoNode. */
NodeId turntableOfBase(const Graph& g, NodeId base)
{
    for (NodeId d : g.descendants(base))
        if (NodeId t = tt::turntableOf(g, d); t != kNoNode)
            return t;
    return kNoNode;
}

/** Deletes what a base group holds (its curves, meshes and their revolves), keeping the group. */
void clearBase(Graph& g, NodeId base)
{
    std::vector<NodeId> doomed;
    for (NodeId d : g.descendants(base))
    {
        doomed.push_back(d);
        if (const Node* n = g.node(d); n && n->type().isA("mesh"))
            if (auto src = g.source({d, MeshNode::kInMesh, -1}))
                doomed.push_back(src->node);
    }
    for (NodeId id : doomed)
        if (g.node(id))
            g.deleteNode(id);
}

/** Radius and height of what stands on the turntable, about its axis. */
std::pair<double, double> footprint(Graph& g, const std::vector<NodeId>& nodes, const glm::dvec3& pivot, const glm::dvec3& up)
{
    double r = 0.0, hMax = 0.0;
    auto addBounds = [&](NodeId id) {
        const Node* n = g.node(id);
        if (!n || !n->type().isA("mesh"))
            return;
        const geo::Bounds b = worldBounds(g, id);
        if (!b.valid())
            return;
        for (int c = 0; c < 8; ++c)
        {
            const glm::dvec3 corner((c & 1) ? b.max.x : b.min.x, (c & 2) ? b.max.y : b.min.y, (c & 4) ? b.max.z : b.min.z);
            const double h = glm::dot(corner - pivot, up);
            hMax = std::max(hMax, h);
            // Corners overstate a round model's reach; the box's inscribed reach is closer to what it covers.
            r = std::max(r, glm::length(corner - pivot - up * h) * 0.8);
        }
    };
    for (NodeId id : nodes)
    {
        addBounds(id);
        for (NodeId d : g.descendants(id))
            addBounds(d);
    }
    return {std::max(r, 5.0), std::max(hMax, 5.0)};
}
} // namespace

void registerTurntableBaseCommand()
{
    std::vector<std::string> materialNames{"default"};
    for (const Preset& p : kPresets)
        materialNames.push_back(p.name);
    ops::registerCommand({
        .name = "turntableBase",
        .doc = "Build a turntable base under objects (the selection, or a selected turntable's members) from a revolved "
               "profile curve, live: edit the hidden profile curve or the revolve to reshape it. Puts the objects on a "
               "turntable if they aren't on one, and the base on it with them. Run on a base, or on a turntable that has one, it rebuilds that base. Returns [group, mesh, curve, revolve].",
        .positionalDoc = "objects",
        .flags = {{"style", "st", T::String, "stepped",
                   "plinth (bevelled disc), stepped (two tiers), record (thin plate, lip, grooves), pedestal (column on a foot), "
                   "lazySusan (a turning top on a fixed foot).",
                   {"plinth", "stepped", "record", "pedestal", "lazySusan"}},
                  {"margin", "m", T::Float, 0.15, "Radius beyond the objects' footprint, as a fraction of it.", {}, {}, 0.0, 5.0},
                  {"height", "h", T::Float, 0.06, "Thickness, as a fraction of the objects' height.", {}, {}, 0.005, 2.0},
                  {"bevel", "bv", T::Float, 0.3, "Edge rounding, as a fraction of the thickness.", {}, {}, 0.0, 0.5},
                  {"segments", "sg", T::Int, 64, "Divisions around.", {}, {}, 3.0, 512.0},
                  {"fixedBase", "fb", T::Bool, true, "lazySusan: the foot stays still while the top turns."},
                  {"material", "mat", T::String, "default", "The default material, or a preset.", materialNames}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            g.evaluate();
            NodeId table = kNoNode, base = kNoNode;
            std::vector<NodeId> on;
            for (const std::string& name : a.objects())
            {
                Node* n = g.find(name);
                if (!n)
                    throw CommandError("turntableBase: no node called '" + name + "'");
                if (NodeId b = baseAbove(g, n->id()); b != kNoNode)
                {
                    base = b;
                    if (table == kNoNode)
                        table = turntableOfBase(g, b);
                }
                else if (tt::isTurntable(*n))
                    table = n->id();
                else if (n->type().dag)
                {
                    on.push_back(n->id());
                    if (table == kNoNode)
                        table = tt::turntableOf(g, n->id());
                }
            }
            // A turntable that has a base gets that base rebuilt, never a second one.
            if (table != kNoNode)
            {
                if (base == kNoNode)
                    base = baseOf(g, table);
                for (NodeId m : tt::members(g, table))
                    if (baseAbove(g, m) == kNoNode && std::find(on.begin(), on.end(), m) == on.end())
                        on.push_back(m);
            }
            if (on.empty())
                throw CommandError("turntableBase: select the objects to stand on it, or a turntable");

            UndoChunk chunk("Turntable Base");
            if (table == kNoNode)
            {
                json args = json::array();
                for (NodeId id : on)
                    args.push_back(g.node(id)->path());
                table = g.find(ops::execute("turntable", {{"args", args}}).get<std::string>())->id();
            }
            // Measured unturned, so the base fits the model rather than a turned view of its box.
            tt::Turntable* t = tt::get(g, table);
            const double angle = t->angle;
            tt::setAngle(g, table, 0.0);
            g.evaluate();
            const glm::dvec3 pivot = tt::pivot(g, table), up = tt::axis(g, table);
            const auto [reach, tall] = footprint(g, on, pivot, up);
            const double r = reach * (1.0 + a.get<double>("margin", 0.15));
            const double h = std::max(tall * a.get<double>("height", 0.06), 0.2);
            const double bevel = h * a.get<double>("bevel", 0.3);
            const int sections = a.get<int>("segments", 64);
            const std::string style = a.get<std::string>("style", "stepped");

            Node* group = base != kNoNode ? g.node(base) : nullptr;
            if (group)
                clearBase(g, base);
            else
            {
                group = g.createNode(*types::find("transform"), "turntableBase");
                g.addAttr(group->id(), AttrDef{.name = kBaseAttr, .type = PlugType::Bool, .defaultValue = true,
                                               .flags = AttrInput | AttrStorable | AttrHidden, .doc = "Made by turntableBase: running it again rebuilds this base."});
            }
            const Made top = revolve(g, profileFor(style, r, h, bevel), pivot, up, sections, "turntableBaseMesh");
            std::vector<NodeId> revolves{top.revolve};
            std::vector<NodeId> turning{top.mesh};
            Made foot;
            if (style == "lazySusan")
            {
                foot = revolve(g, footFor(r, h, bevel), pivot, up, sections, "turntableBaseFoot");
                revolves.push_back(foot.revolve);
                if (!a.get<bool>("fixedBase", true))
                    turning.push_back(foot.mesh);
            }
            for (NodeId id : {top.curve, top.mesh, foot.curve, foot.mesh})
                if (id != kNoNode)
                    g.setParent(id, group->id(), true);
            applyMaterial(g, a.get<std::string>("material", "default"), revolves);

            std::vector<NodeId> list = tt::members(g, table);
            for (NodeId id : turning)
                list.push_back(id);
            tt::setMembers(g, table, list);
            tt::setAngle(g, table, angle);
            g.select({group->id()});
            return json::array({group->path(), g.node(top.mesh)->path(), g.node(top.curve)->path(), g.node(top.revolve)->path()});
        },
        .examples = {"turntableBase(\"statue\", style=\"stepped\", material=\"marble\")", "turntableBase(\"turntable1\", style=\"lazySusan\")"},
    });
}
} // namespace lila::curve
