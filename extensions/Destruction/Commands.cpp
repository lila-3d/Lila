// breakApart: one command that turns meshes into a ready destruction setup, a geometry-nodes group anyone can open and
// edit afterwards (Fracture, Anchor Pieces, Rigid Body Solver), with the few settings that matter exposed in plain terms.

#include "Destruction.h"
#include "PieceTransform.h"
#include "RigidState.h"

#include <lila/Anim.h>
#include <lila/Command.h>
#include <lila/Deformer.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/Mesh.h>
#include <lila/Sequences.h>
#include <lila/Node.h>
#include <lila/Simulation.h>
#include <lila/Transform.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>

using namespace lila;

namespace destruction
{
namespace
{
using json = nlohmann::json;
using scene::Graph;
using scene::Node;

json v3(const glm::dvec3& v) { return json::array({v.x, v.y, v.z}); }

glm::dvec3 vecArg(const Args& a, const char* flag, const glm::dvec3& fallback)
{
    if (!a.has(flag))
        return fallback;
    const json& j = a.flags[flag];
    if (!j.is_array() || j.size() != 3)
        throw CommandError(std::string("breakApart: ") + flag + " takes [x, y, z]");
    return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}

std::string uniqueGroupName(const std::string& base)
{
    json existing = ops::execute("gnGroups", json::object());
    auto taken = [&](const std::string& n) {
        for (const json& e : existing)
            if ((e.is_string() && e.get<std::string>() == n) || (e.is_object() && e.value("name", std::string()) == n))
                return true;
        return false;
    };
    std::string name = base;
    for (int k = 2; taken(name); ++k)
        name = base + std::to_string(k);
    return name;
}

/** A name usable as a group name: letters, digits and underscores, starting with a letter. */
std::string identifier(const std::string& s)
{
    std::string out;
    for (char c : s)
        out += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    if (out.empty() || !std::isalpha(static_cast<unsigned char>(out[0])))
        out = "Mesh" + out;
    return out;
}

/** The outward normal of the surface nearest a point, in the mesh's space. */
glm::dvec3 surfaceNormalNear(const geo::Mesh& m, const glm::dvec3& p)
{
    geo::MeshBvh bvh;
    bvh.build(m);
    geo::BvhClosest hit;
    if (!bvh.closestPoint(p, hit) || hit.face == geo::kInvalid)
        return glm::dvec3(0, 0, 1);
    return m.faceNormal(hit.face);
}

geo::Bounds worldBounds(const Node& mesh)
{
    geo::Bounds b;
    const auto out = mesh.as<scene::MeshPtr>(mesh.type().attrIndex("outMesh"));
    if (!out)
        return b;
    const glm::dmat4 world = mesh.as<glm::dmat4>(scene::Transform::kWorldMatrix);
    for (geo::Index i = 0; i < out->pointCapacity(); ++i)
        if (out->pointValid(i))
            b.extend(glm::dvec3(world * glm::dvec4(out->position(i), 1.0)));
    return b;
}

/**
 * Several meshes breaking in one simulation, so their pieces hit each other: walls fall onto the ground they stand on.
 * A new "Destruction" mesh carries the modifier and reads the meshes through Object Info; the originals are hidden.
 * Pieces reaching the lowest bottom are anchored there. Colliders are hit but never move or break, and stay visible as
 * they are. Explosions come from addExplosion (impact adds the first).
 */
json buildTogether(const std::vector<Node*>& meshes, const std::vector<Node*>& colliders, const Args& a)
{
    const std::string materialName = a.get<std::string>("material", "concrete");
    const Preset& p = preset(materialName);
    const bool anchor = a.get<bool>("anchor", true);
    const int64_t startFrame = a.has("startFrame") ? a.get<int64_t>("startFrame")
                                                   : scene::sim::frameOf(scene::anim::playback().minTime);
    std::vector<geo::Bounds> bounds;
    double floor = 0.0;
    for (Node* m : meshes)
    {
        bounds.push_back(worldBounds(*m));
        if (!bounds.back().valid())
            throw CommandError("breakApart: " + m->path() + " has no faces");
        floor = std::min(floor, bounds.back().min.y);
    }

    json host = ops::execute("createMesh", {{"points", json::array()}, {"faces", json::array()}, {"name", "Destruction"}});
    const std::string hostName = host.is_string() ? host.get<std::string>() : host.at(0).get<std::string>();
    const std::string group = uniqueGroupName(identifier(hostName) + "_Break");

    json inputs = json::array({
        {{"name", "seed"}, {"type", "int"}, {"default", a.get<int64_t>("seed", 1)}, {"description", "Another seed gives another pattern."}},
        {{"name", "roughness"}, {"type", "float"}, {"default", p.roughness}, {"min", 0.0}, {"softMax", 10.0}, {"description", "How jagged the cracks are, cm."}},
        {{"name", "strength"}, {"type", "float"}, {"default", p.strength}, {"min", 0.0}, {"softMax", 200.0},
         {"description", "How hard it is to break: N per cm2 the pieces share."}},
        {{"name", "startFrame"}, {"type", "int"}, {"default", startFrame}, {"description", "Frame the simulation starts on."}},
    });
    json nodes = json::array({
        {{"name", "Solver"},
         {"type", "RigidBodySolver"},
         {"position", json::array({900, 0})},
         {"values",
          {{"gravity", v3({0, -981, 0})}, {"ground", true}, {"groundHeight", floor}, {"density", p.density}, {"friction", p.friction},
           {"bounce", p.bounce}, {"blastSpeed", 0.0}}}},
    });
    json links = json::array({"GroupInput.startFrame -> Solver.startFrame", "Solver.pieces -> GroupOutput.geometry"});
    // Each mesh's chain ends in "<node>.pieces" and "<node>.links"; the Join Pieces chain gathers them.
    std::vector<std::pair<std::string, std::string>> ends;
    for (size_t k = 0; k < meshes.size(); ++k)
    {
        const std::string id = identifier(meshes[k]->name());
        const std::string info = "Info_" + id, fracture = "Fracture_" + id, anchorNode = "Anchor_" + id;
        const double y = double(k) * 260.0;
        inputs.push_back({{"name", id + "_pieces"}, {"type", "int"}, {"default", a.get<int64_t>("pieces", p.pieces)}, {"min", 1}, {"softMax", 500},
                          {"label", meshes[k]->name() + " Pieces"}, {"panel", "Pieces"}, {"description", "Roughly how many pieces " + meshes[k]->name() + " breaks into."}});
        nodes.push_back({{"name", info}, {"type", "ObjectInfo"}, {"position", json::array({0, y})},
                         {"props", {{"object", meshes[k]->name()}, {"transformSpace", "world"}}}});
        nodes.push_back({{"name", fracture}, {"type", "Fracture"}, {"position", json::array({250, y})}, {"props", {{"material", p.material}}},
                         {"values", {{"impactFocus", 0.0}}}});
        links.push_back(info + ".geometry -> " + fracture + ".mesh");
        links.push_back("GroupInput." + id + "_pieces -> " + fracture + ".pieces");
        links.push_back("GroupInput.seed -> " + fracture + ".seed");
        links.push_back("GroupInput.roughness -> " + fracture + ".roughness");
        links.push_back("GroupInput.strength -> " + fracture + ".linkStrength");
        std::string linkEnd = fracture + ".links";
        if (anchor && bounds[k].min.y <= floor + 1.0)
        {
            nodes.push_back({{"name", anchorNode}, {"type", "AnchorPieces"}, {"position", json::array({450, y + 80})},
                             {"values", {{"height", floor}, {"tolerance", 1.0}}}});
            links.push_back(fracture + ".pieces -> " + anchorNode + ".pieces");
            links.push_back(fracture + ".links -> " + anchorNode + ".links");
            links.push_back("GroupInput.strength -> " + anchorNode + ".strength");
            linkEnd = anchorNode + ".links";
        }
        ends.push_back({fracture + ".pieces", linkEnd});
    }
    std::pair<std::string, std::string> joined = ends[0];
    for (size_t k = 1; k < ends.size(); ++k)
    {
        const std::string join = "Join" + std::to_string(k);
        nodes.push_back({{"name", join}, {"type", "JoinPieces"}, {"position", json::array({650, double(k) * 120.0})}});
        links.push_back(joined.first + " -> " + join + ".pieces");
        links.push_back(joined.second + " -> " + join + ".links");
        links.push_back(ends[k].first + " -> " + join + ".pieces2");
        links.push_back(ends[k].second + " -> " + join + ".links2");
        joined = {join + ".pieces", join + ".links"};
    }
    links.push_back(joined.first + " -> Solver.pieces");
    links.push_back(joined.second + " -> Solver.links");
    if (!colliders.empty())
    {
        nodes.push_back({{"name", "Colliders"}, {"type", "JoinGeometry"}, {"position", json::array({650, -260})}});
        links.push_back("Colliders.geometry -> Solver.colliders");
        for (size_t k = 0; k < colliders.size(); ++k)
        {
            const std::string info = "Collider_" + identifier(colliders[k]->name());
            nodes.push_back({{"name", info}, {"type", "ObjectInfo"}, {"position", json::array({400, -260 - double(k) * 120.0})},
                             {"props", {{"object", colliders[k]->name()}, {"transformSpace", "world"}}}});
            links.push_back(info + ".geometry -> Colliders.geometry");
        }
    }

    std::string names;
    for (Node* m : meshes)
        names += (names.empty() ? "" : ", ") + m->name();
    json spec = {{"name", group},
                 {"description", "Breaks " + names + " apart like " + p.material + " in one simulation."},
                 {"inputs", inputs},
                 {"nodes", nodes},
                 {"links", links}};
    ops::execute("gnBuildGroup", {{"args", {spec}}, {"replace", false}});
    json mod = ops::execute("gnApply", {{"args", {hostName, group}}});
    json hide = json::array();
    for (Node* m : meshes)
        hide.push_back(m->path());
    ops::execute("hide", {{"args", hide}});
    if (a.has("impact"))
        ops::execute("addExplosion", {{"args", {hostName}}, {"position", a.flags["impact"]}, {"frame", startFrame},
                                      {"blast", a.get<double>("blast", p.blast)}});
    return mod;
}

json buildOne(Node& mesh, const Args& a)
{
    const std::string materialName = a.get<std::string>("material", "concrete");
    const Preset& p = preset(materialName);
    const std::string damage = a.get<std::string>("damage", "none");
    const bool damaged = damage != "none";
    const bool simulate = a.get<bool>("simulate", true) && !damaged;
    const bool anchor = a.get<bool>("anchor", true);

    const auto out = mesh.as<scene::MeshPtr>(mesh.type().attrIndex("outMesh"));
    if (!out || out->numFaces() == 0)
        throw CommandError("breakApart: " + mesh.path() + " has no faces");
    const glm::dmat4 world = mesh.as<glm::dmat4>(scene::Transform::kWorldMatrix);
    const glm::dmat4 toLocal = glm::inverse(world);
    geo::Bounds wb;
    for (geo::Index i = 0; i < out->pointCapacity(); ++i)
        if (out->pointValid(i))
            wb.extend(glm::dvec3(world * glm::dvec4(out->position(i), 1.0)));

    const glm::dvec3 impactWorld = vecArg(a, "impact", wb.center());
    const glm::dvec3 impact = glm::dvec3(toLocal * glm::dvec4(impactWorld, 1.0));
    const glm::dmat3 rot = glm::dmat3(world);
    const glm::dvec3 gravity = glm::normalize(glm::inverse(rot) * glm::dvec3(0, -1, 0)) * 981.0;
    // The ground is the world floor (or the mesh's bottom when it sits below it), only while the mesh stands upright.
    const bool upright = glm::length(glm::normalize(rot * glm::dvec3(0, 1, 0)) - glm::dvec3(0, 1, 0)) < 1e-3;
    const double groundWorld = std::min(0.0, wb.min.y);
    const double ground = (toLocal * glm::dvec4(0, groundWorld, 0, 1)).y;
    const double radius = std::max(10.0, glm::length(wb.size()) * 0.35);
    // The blast pushes out from a point just outside the hit surface, so pieces are driven into the mesh, not along it.
    const glm::dvec3 blastOffset = surfaceNormalNear(*out, impact) * (radius * 0.35);
    const int64_t startFrame = a.has("startFrame") ? a.get<int64_t>("startFrame")
                                                   : scene::sim::frameOf(scene::anim::playback().minTime);

    const std::string group = uniqueGroupName(identifier(mesh.name()) + "_Break");
    // Chips come off edges and corners, which need more, smaller pieces to read as chips.
    const int64_t pieceCount = a.get<int64_t>("pieces", damage == "chipped" ? std::max<int64_t>(p.pieces * 2, 60) : p.pieces);
    json inputs = json::array({
        {{"name", "pieces"}, {"type", "int"}, {"default", pieceCount}, {"min", 1}, {"softMax", 500},
         {"description", "Roughly how many pieces."}},
        {{"name", "seed"}, {"type", "int"}, {"default", a.get<int64_t>("seed", 1)}, {"description", "Another seed gives another pattern."}},
        {{"name", "impact"}, {"type", "vector"}, {"default", v3(impact)}, {"description", "Where it's hit (the mesh's own space, cm)."}},
        {{"name", "roughness"}, {"type", "float"}, {"default", p.roughness}, {"min", 0.0}, {"softMax", 10.0}, {"description", "How jagged the cracks are, cm."}},
        {{"name", "strength"}, {"type", "float"}, {"default", p.strength}, {"min", 0.0}, {"softMax", 200.0},
         {"description", "How hard it is to break: N per cm2 the pieces share."}},
    });
    json nodes = json::array({
        {{"name", "Fracture"}, {"type", "Fracture"}, {"props", {{"material", p.material}}},
         {"values", {{"impactFocus", damage == "chipped" || damage == "cracked" ? 0.0 : p.impactFocus}}}},
    });
    json links = json::array({"GroupInput.geometry -> Fracture.mesh", "GroupInput.pieces -> Fracture.pieces", "GroupInput.seed -> Fracture.seed",
                              "GroupInput.impact -> Fracture.impact", "GroupInput.roughness -> Fracture.roughness",
                              "GroupInput.strength -> Fracture.linkStrength"});
    // Pieces touching the ground hold on to it, for a simulation or a collapse.
    auto anchorTo = [&](const std::string& into) {
        if (anchor && upright)
        {
            nodes.push_back({{"name", "Anchor"}, {"type", "AnchorPieces"}, {"values", {{"height", ground}, {"tolerance", 1.0}}}});
            links.push_back("Fracture.pieces -> Anchor.pieces");
            links.push_back("Fracture.links -> Anchor.links");
            links.push_back("GroupInput.strength -> Anchor.strength");
            links.push_back("Anchor.links -> " + into);
        }
        else
            links.push_back("Fracture.links -> " + into);
    };
    if (simulate)
    {
        inputs.push_back({{"name", "blast"}, {"type", "float"}, {"default", a.get<double>("blast", p.blast)}, {"min", 0.0}, {"softMax", 5000.0},
                          {"description", "How hard the impact hits: speed given to pieces at the impact, cm/s."}});
        inputs.push_back({{"name", "startFrame"}, {"type", "int"}, {"default", startFrame}, {"description", "Frame it breaks on."}});
        nodes.push_back({{"name", "Solver"},
                         {"type", "RigidBodySolver"},
                         {"values",
                          {{"gravity", v3(gravity)}, {"ground", upright}, {"groundHeight", ground}, {"density", p.density},
                           {"friction", p.friction}, {"bounce", p.bounce}, {"blastRadius", radius}}}});
        nodes.push_back({{"name", "Behind"}, {"type", "VectorMath"}, {"props", {{"operation", "add"}}}, {"values", {{"b", v3(blastOffset)}}}});
        links.push_back("GroupInput.impact -> Behind.a");
        links.push_back("Behind.vector -> Solver.blastCenter");
        links.push_back("GroupInput.blast -> Solver.blastSpeed");
        links.push_back("GroupInput.startFrame -> Solver.startFrame");
        links.push_back("GroupInput.startFrame -> Solver.blastFrame");
        links.push_back("Fracture.pieces -> Solver.pieces");
        links.push_back("Solver.pieces -> GroupOutput.geometry");
        anchorTo("Solver.links");
    }
    else if (damaged)
    {
        inputs.push_back({{"name", "amount"}, {"type", "float"}, {"default", a.get<double>("amount", 0.3)}, {"min", 0.0}, {"max", 1.0},
                          {"description", "How much is damaged, 0 to 1."}});
        json values = {{"gravity", v3(gravity)}, {"ground", upright}, {"groundHeight", ground}, {"density", p.density}};
        nodes.push_back({{"name", "Damage"}, {"type", "DamagePieces"}, {"props", {{"mode", damage}}}, {"values", values}});
        links.push_back("Fracture.pieces -> Damage.pieces");
        links.push_back("GroupInput.amount -> Damage.amount");
        links.push_back("GroupInput.seed -> Damage.seed");
        links.push_back("GroupInput.impact -> Damage.impact");
        links.push_back("Damage.pieces -> GroupOutput.geometry");
        if (damage == "cracked")
        {
            inputs.push_back({{"name", "gap"}, {"type", "float"}, {"default", a.get<double>("gap", 0.3)}, {"min", 0.0}, {"softMax", 5.0},
                              {"description", "Width of the cracks, cm."}});
            links.push_back("GroupInput.gap -> Damage.gap");
        }
        if (damage == "collapsed")
            anchorTo("Damage.links");
        else
            links.push_back("Fracture.links -> Damage.links");
    }
    else
        links.push_back("Fracture.pieces -> GroupOutput.geometry");

    const std::string what = simulate ? " and simulates it falling." : damaged ? ", " + damage + "." : ".";
    json spec = {{"name", group},
                 {"description", std::string("Breaks ") + mesh.name() + " apart like " + p.material + what},
                 {"inputs", inputs},
                 {"nodes", nodes},
                 {"links", links}};
    ops::execute("gnBuildGroup", {{"args", {spec}}, {"replace", false}});
    return ops::execute("gnApply", {{"args", {mesh.path(), group}}});
}

/** The simulating geometry-nodes modifier in a mesh's stack (the mesh itself may be given as the modifier). */
scene::NodeId simulationOf(Graph& g, const std::string& name, const char* cmd)
{
    Node* n = g.find(name);
    if (!n)
        throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
    if (scene::sim::isSimulation(*n))
        return n->id();
    for (scene::NodeId m : scene::modifiers::stack(g, n->id()))
        if (Node* mod = g.node(m); mod && scene::sim::isSimulation(*mod))
            return m;
    throw CommandError(std::string(cmd) + ": " + n->path() + " has no destruction simulation (run breakApart first)");
}

/** The mesh a modifier belongs to, or the node named when it is the mesh. */
scene::NodeId meshFor(Graph& g, scene::NodeId modifier, const std::string& named)
{
    const scene::NodeId m = scene::modifiers::meshOf(g, modifier);
    return m != scene::kNoNode ? m : g.find(named)->id();
}

struct Break
{
    int64_t frame = 0;
    glm::dvec3 position{0.0};
    double impulse = 0.0;
    int a = -1, b = -1;
};

/** Every link broken by `last`, placed where its first piece had carried it on the frame it broke, in world space. */
std::vector<Break> breaksOf(Graph& g, scene::NodeId modifier, scene::NodeId mesh, int64_t last)
{
    const std::vector<uint8_t> end = gn::simulationState(g, modifier, last);
    const StateView view(end);
    std::shared_ptr<const gn::SimInputs> in = gn::simulationInputs(modifier);
    if (!view.valid() || !in)
        throw CommandError("exportBreaks: the simulation has no state to read");
    gn::GeometryPtr links = in->geometry("links"), pieces = in->geometry("pieces");
    if (!links->curves || !pieces->instances)
        return {};
    const gn::CurveSet& c = *links->curves;
    const gn::Instances& inst = *pieces->instances;
    const glm::dmat4 world = g.node(mesh)->as<glm::dmat4>(scene::Transform::kWorldMatrix);
    std::span<const int32_t> pa = c.curveAttrs.get<int32_t>("pieceA"), pb = c.curveAttrs.get<int32_t>("pieceB");
    std::vector<Break> out;
    size_t link = 0;
    for (size_t k = 0; k < c.numCurves() && link < view.header().links; ++k)
    {
        if (c.curveSize(k) == 0)
            continue;
        const LinkRecord r = view.link(link++);
        if (r.brokenStep < 0)
            continue;
        Break b;
        b.frame = view.frameOfStep(r.brokenStep);
        b.impulse = r.impulse;
        b.a = k < pa.size() ? pa[k] : -1;
        b.b = k < pb.size() ? pb[k] : -1;
        glm::dvec3 anchor = c.curvePoints(k)[0];
        if (b.a >= 0 && size_t(b.a) < inst.size())
        {
            const std::vector<uint8_t> bytes = gn::simulationState(g, modifier, b.frame);
            const StateView at(bytes);
            if (at.valid() && size_t(b.a) < at.header().bodies)
            {
                const Decomposed rest = decompose(inst.transforms[size_t(b.a)]);
                anchor = glm::dvec3(compose(at.position(size_t(b.a)), at.rotation(size_t(b.a)), rest.scale) * glm::inverse(inst.transforms[size_t(b.a)]) * glm::dvec4(anchor, 1.0));
            }
        }
        b.position = glm::dvec3(world * glm::dvec4(anchor, 1.0));
        out.push_back(b);
    }
    std::stable_sort(out.begin(), out.end(), [](const Break& x, const Break& y) { return x.frame < y.frame; });
    return out;
}

int64_t playbackEnd() { return scene::sim::frameOf(scene::anim::playback().maxTime); }

/** The simulating modifier of a destruction and the group it runs. */
struct Setup
{
    scene::NodeId modifier = scene::kNoNode;
    scene::NodeId mesh = scene::kNoNode;
    Node* groupNode = nullptr;
};

Setup setupOf(Graph& g, const std::string& name, const char* cmd)
{
    Setup s;
    g.evaluate();
    s.modifier = simulationOf(g, name, cmd);
    s.mesh = meshFor(g, s.modifier, name);
    s.groupNode = gn::groupOfModifier(g, *g.node(s.modifier));
    if (!s.groupNode)
        throw CommandError(std::string(cmd) + ": the destruction's node group is missing");
    return s;
}

std::string destructionArg(const Args& a, const char* cmd)
{
    std::vector<std::string> names = a.objects();
    if (names.size() != 1)
        throw CommandError(std::string(cmd) + ": give one destruction (the mesh breakApart made or changed)");
    return names[0];
}

bool linked(const gn::Group& grp, const std::string& node, const std::string& socket) { return !grp.linksInto(node, socket).empty(); }

/** The scene mesh an Object Info node of a together-group reads, or null. */
Node* sourceOf(Graph& g, const gn::GNode& info)
{
    const auto it = info.values.find("object");
    return it != info.values.end() && it->is_string() ? g.find(it->get<std::string>()) : nullptr;
}

/** A mesh's part of a destruction group: its Fracture node and where its pieces and links leave its own chain. */
struct Part
{
    std::string fracture;
    std::string pieces, links; // "node.socket"
    glm::dmat4 toGroup{1.0};    // world to the group's space
    geo::Bounds bounds;         // in the group's space
};

Part partOf(Graph& g, const gn::Group& grp, const Setup& s, const std::string& name, const char* cmd)
{
    Part p;
    Node* mesh = g.node(s.mesh);
    p.toGroup = glm::inverse(mesh->as<glm::dmat4>(scene::Transform::kWorldMatrix));
    for (const gn::GNode& n : grp.nodes)
    {
        if (n.type != "ObjectInfo" || n.name.rfind("Info_", 0) != 0)
            continue;
        Node* src = sourceOf(g, n);
        if (!src || (name != src->name() && name != src->path()))
            continue;
        const std::string id = n.name.substr(5);
        p.fracture = "Fracture_" + id;
        p.pieces = p.fracture + ".pieces";
        p.links = grp.node("Anchor_" + id) ? "Anchor_" + id + ".links" : p.fracture + ".links";
        p.toGroup = glm::dmat4(1.0);
        p.bounds = worldBounds(*src);
        return p;
    }
    if (!name.empty() && name != mesh->name() && name != mesh->path())
        throw CommandError(std::string(cmd) + ": " + name + " isn't part of this destruction");
    if (!grp.node("Fracture"))
        throw CommandError(std::string(cmd) + ": this destruction has no Fracture node");
    p.fracture = "Fracture";
    p.pieces = "Fracture.pieces";
    p.links = grp.node("Anchor") ? "Anchor.links" : "Fracture.links";
    const auto out = mesh->as<scene::MeshPtr>(mesh->type().attrIndex("inMesh"));
    if (out)
        for (geo::Index i = 0; i < out->pointCapacity(); ++i)
            if (out->pointValid(i))
                p.bounds.extend(out->position(i));
    return p;
}

/** Every link leaving `from` ("node.socket") goes from `to` instead, except into the node `except`. */
void reroute(gn::Group& grp, const std::string& from, const std::string& to, const std::string& except)
{
    const size_t dot = from.find('.'), dot2 = to.find('.');
    for (gn::GLink& l : grp.links)
        if (l.fromNode == from.substr(0, dot) && l.fromSocket == from.substr(dot + 1) && l.toNode != except)
        {
            l.fromNode = to.substr(0, dot2);
            l.fromSocket = to.substr(dot2 + 1);
        }
}

double numberValue(const gn::GNode& n, const char* id, double fallback)
{
    const auto it = n.values.find(id);
    return it != n.values.end() && it->is_number() ? it->get<double>() : fallback;
}

} // namespace

const Preset& preset(const std::string& material)
{
    static const Preset concrete{"concrete", 40, 1.5, 20.0, 1500.0, 2400.0, 0.7, 0.05, 0.5};
    static const Preset glass{"glass", 40, 0.0, 6.0, 1200.0, 2500.0, 0.4, 0.15, 0.6};
    static const Preset wood{"wood", 24, 0.5, 30.0, 1000.0, 700.0, 0.6, 0.1, 0.3};
    if (material == "glass")
        return glass;
    if (material == "wood")
        return wood;
    if (material != "concrete")
        throw CommandError("breakApart: material must be concrete, glass or wood");
    return concrete;
}

void registerCommands(ext::Context& ctx)
{
    using AT = ArgType;
    ctx.registerCommand({
        .name = "breakApart",
        .doc = "Break meshes apart like concrete, glass or wood, and (by default) simulate them falling.\n\n"
               "Adds a geometry-nodes modifier to each mesh whose group (Fracture, Anchor Pieces, Rigid Body Solver) can be opened "
               "and edited in the Node Editor. The settings that matter (pieces, seed, impact, roughness, strength, blast, "
               "start frame) show on the modifier and can be keyed. The pieces touching the ground are anchored, so a wall "
               "stands until the impact. Several meshes break together in one simulation (their pieces hit each other) on a new "
               "Destruction mesh, with the originals hidden; add explosions with addExplosion. Colliders are meshes the pieces hit that "
               "never move or break. damage makes a damaged prop with no simulation (cracked, missing, chipped or collapsed), for "
               "level dressing; freezeDestruction then turns any of these into static meshes with collision. Returns the modifiers.",
        .positionalDoc = "meshes (default the selection)",
        .flags = {{"material", "m", AT::String, "concrete", "How it cracks.", {"concrete", "glass", "wood"}},
                  {"impact", "i", AT::Vec3, nullptr, "Where it's hit, world space cm. Default: the centre of the mesh."},
                  {"pieces", "p", AT::Int, nullptr, "Roughly how many pieces. Default: the material's.", {}, {}, 1.0},
                  {"seed", "", AT::Int, 1, "Another seed gives another pattern."},
                  {"blast", "b", AT::Float, nullptr, "How hard the impact hits, cm/s. Default: the material's.", {}, {}, 0.0},
                  {"startFrame", "", AT::Int, nullptr, "Frame it breaks on. Default: the start of the playback range."},
                  {"simulate", "", AT::Bool, true, "Simulate the pieces; False only fractures."},
                  {"damage", "d", AT::String, "none",
                   "Damage it without simulating: cracked (standing, cracks showing, some pieces slipped), missing (chunks gone "
                   "around the impact), chipped (edges and corners knocked off) or collapsed (the part around the impact gives way "
                   "and what it held falls and settles).",
                   {"none", "cracked", "missing", "chipped", "collapsed"}},
                  {"amount", "", AT::Float, 0.3, "With damage: how much is damaged, 0 to 1.", {}, {}, 0.0, 1.0},
                  {"gap", "", AT::Float, 0.3, "With damage=cracked: width of the cracks, cm.", {}, {}, 0.0},
                  {"anchor", "", AT::Bool, true, "Tie the pieces touching the ground to it."},
                  {"together", "", AT::Bool, true, "Several meshes: one simulation where they hit each other. False breaks each on its own."},
                  {"colliders", "", AT::List, nullptr, "Meshes the pieces hit that never move or break (the ground around a pit, say)."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<std::string> names = a.objects();
            if (names.empty())
                throw CommandError("breakApart: select a mesh");
            std::vector<Node*> meshes;
            for (const std::string& name : names)
            {
                Node* n = g.find(name);
                if (!n || n->type().attrIndex("outMesh") < 0)
                    throw CommandError("breakApart: " + name + " is not a mesh");
                meshes.push_back(n);
            }
            std::vector<Node*> colliders;
            if (a.has("colliders"))
                for (const json& c : a.flags["colliders"])
                {
                    Node* n = c.is_string() ? g.find(c.get<std::string>()) : nullptr;
                    if (!n || n->type().attrIndex("outMesh") < 0)
                        throw CommandError("breakApart: collider " + c.dump() + " is not a mesh");
                    colliders.push_back(n);
                }
            g.evaluate();
            json made = json::array();
            if ((meshes.size() > 1 || !colliders.empty()) && a.get<bool>("together", true) && a.get<bool>("simulate", true))
                made.push_back(buildTogether(meshes, colliders, a));
            else
                for (Node* n : meshes)
                    made.push_back(buildOne(*n, a));
            return made;
        },
        .examples = {"breakApart('Wall', material='concrete', impact=[0, 120, 15])", "breakApart(material='glass', pieces=60, blast=1500)",
                     "breakApart(['WallA', 'WallB', 'Ground'])", "breakApart('Pillar', damage='chipped', amount=0.4)",
                     "breakApart('Wall', damage='collapsed', impact=[0, 80, 10], amount=0.25)"},
    });

    ctx.registerCommand({
        .name = "addExplosion",
        .doc = "Set off an explosion in a destruction: on its frame, pieces within the radius are thrown outward, hardest at the "
               "centre. Add as many as you like, at different places and frames. A mesh not yet cracked finer near an explosion "
               "gets smaller pieces around the first one that reaches it. Invisible: add your own flash or smoke. Returns the "
               "explosion's node in the destruction's group.",
        .positionalDoc = "destruction (default the selection)",
        .flags = {{"position", "p", AT::Vec3, nullptr, "Its centre, world space cm."},
                  {"frame", "f", AT::Int, nullptr, "Frame it goes off. Default: the current frame."},
                  {"blast", "b", AT::Float, 2000.0, "Speed given to pieces at the centre, cm/s.", {}, {}, 0.0},
                  {"radius", "r", AT::Float, 300.0, "Pieces this far away or more aren't pushed, cm.", {}, {}, 1.0}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const Setup s = setupOf(g, destructionArg(a, "addExplosion"), "addExplosion");
            if (!a.has("position"))
                throw CommandError("addExplosion: position is required");
            gn::Group grp = gn::readGroup(*s.groupNode);
            const glm::dmat4 toLocal = glm::inverse(g.node(s.mesh)->as<glm::dmat4>(scene::Transform::kWorldMatrix));
            const glm::dvec3 world = vecArg(a, "position", glm::dvec3(0.0));
            const glm::dvec3 at = glm::dvec3(toLocal * glm::dvec4(world, 1.0));
            const int64_t frame = a.has("frame") ? a.get<int64_t>("frame") : scene::sim::frameOf(g.time());
            const double radius = std::max(1.0, a.get<double>("radius", 300.0));
            if (!grp.node("Solver"))
                throw CommandError("addExplosion: the destruction has no Rigid Body Solver named Solver");
            if (!grp.node("Explosions"))
            {
                grp.nodes.push_back({"Explosions", "JoinGeometry", grp.node("Solver")->position + glm::dvec2(-220.0, 320.0), json::object()});
                grp.links.push_back({"Explosions", "geometry", "Solver", "blasts"});
            }
            const std::string name = grp.uniqueNodeName("Explosion");
            const size_t count = grp.linksInto("Explosions", "geometry").size();
            grp.nodes.push_back({name, "Explosion", grp.node("Explosions")->position + glm::dvec2(-240.0, 140.0 * double(count)),
                                 {{"position", v3(at)}, {"frame", frame}, {"speed", a.get<double>("blast", 2000.0)}, {"radius", radius}}});
            grp.links.push_back({name, "blast", "Explosions", "geometry"});

            // Finer pieces around the first explosion to reach each mesh that has none yet.
            for (gn::GNode& n : grp.nodes)
            {
                if (n.type != "Fracture" || linked(grp, n.name, "impact") || linked(grp, n.name, "impactFocus") ||
                    numberValue(n, "impactFocus", 0.0) > 0.0)
                    continue;
                const std::string info = "Info_" + (n.name.size() > 9 ? n.name.substr(9) : std::string());
                const gn::GNode* src = grp.node(info);
                Node* mesh = src ? sourceOf(g, *src) : nullptr;
                if (!mesh)
                    continue;
                const geo::Bounds b = worldBounds(*mesh);
                if (!b.valid() || glm::length(glm::clamp(world, b.min, b.max) - world) > radius * 0.5)
                    continue;
                std::string material = "concrete";
                if (auto it = n.values.find("material"); it != n.values.end() && it->is_string())
                    material = it->get<std::string>();
                n.values["impact"] = v3(glm::clamp(world, b.min, b.max));
                n.values["impactFocus"] = preset(material).impactFocus;
                n.values["impactRadius"] = radius * 0.6;
            }
            gn::commitGroup(g, *s.groupNode, std::move(grp));
            return name;
        },
        .examples = {"addExplosion('Destruction', position=[0, 100, 0], frame=24)",
                     "addExplosion('Wall', position=[250, 80, 20], frame=40, blast=3000, radius=400)"},
    });

    ctx.registerCommand({
        .name = "collapse",
        .doc = "Make part of a destruction give way from one side to the other, like ground failing under structural damage. "
               "Its pieces hold still until the failure reaches them, rumble harder as it nears, then come loose and fall; "
               "anything standing on them falls too. The floor under it is lowered by depth so there's somewhere to fall. "
               "Running it again on the same part changes the settings. Returns the Collapse node.",
        .positionalDoc = "destruction (default the selection)",
        .flags = {{"part", "", AT::String, nullptr, "Which of the destruction's meshes gives way. Default: the one breakApart was run on."},
                  {"start", "s", AT::Float, nullptr, "Frame the first pieces fall. Default: the current frame."},
                  {"duration", "d", AT::Float, 60.0, "Frames for the failure to cross to the far side.", {}, {}, 0.0},
                  {"direction", "dir", AT::Vec3, nullptr, "Which way the failure travels (world space). Default: +X."},
                  {"rumble", "r", AT::Float, 2.0, "How far pieces shake just before they fall, cm.", {}, {}, 0.0},
                  {"lead", "", AT::Float, 30.0, "Frames of rumbling before a piece falls.", {}, {}, 0.0},
                  {"raggedness", "", AT::Float, 0.25, "0 is a straight front; 1 drops pieces in any order.", {}, {}, 0.0, 1.0},
                  {"depth", "", AT::Float, 3000.0, "How far below it the floor is, cm.", {}, {}, 0.0}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const Setup s = setupOf(g, destructionArg(a, "collapse"), "collapse");
            gn::Group grp = gn::readGroup(*s.groupNode);
            const Part part = partOf(g, grp, s, a.get<std::string>("part", ""), "collapse");
            const std::string node = part.fracture == "Fracture" ? "Collapse" : "Collapse_" + part.fracture.substr(9);
            if (!grp.node(node))
            {
                const gn::GNode* f = grp.node(part.fracture);
                grp.nodes.push_back({node, "CollapsePieces", f->position + glm::dvec2(420.0, -60.0), json::object()});
                reroute(grp, part.pieces, node + ".pieces", node);
                reroute(grp, part.links, node + ".links", node);
                const size_t dp = part.pieces.find('.'), dl = part.links.find('.');
                grp.links.push_back({part.pieces.substr(0, dp), part.pieces.substr(dp + 1), node, "pieces"});
                grp.links.push_back({part.links.substr(0, dl), part.links.substr(dl + 1), node, "links"});
                // Anchor Pieces reads the fracture's pieces, not the collapsing ones.
                for (gn::GLink& l : grp.links)
                    if (l.fromNode == node && l.fromSocket == "pieces" && l.toNode.rfind("Anchor", 0) == 0)
                    {
                        l.fromNode = part.pieces.substr(0, dp);
                        l.fromSocket = part.pieces.substr(dp + 1);
                    }
            }
            if (!grp.node("Time"))
                grp.nodes.push_back({"Time", "SceneTime", grp.node(node)->position + glm::dvec2(-220.0, -160.0), json::object()});
            grp.node("Time")->values["fps"] = scene::sim::framesPerSecond();
            if (!linked(grp, node, "frame"))
                grp.links.push_back({"Time", "frame", node, "frame"});
            const glm::dvec3 dirWorld = vecArg(a, "direction", glm::dvec3(1, 0, 0));
            gn::GNode& c = *grp.node(node);
            c.values["start"] = a.has("start") ? a.get<double>("start") : double(scene::sim::frameOf(g.time()));
            c.values["duration"] = a.get<double>("duration", 60.0);
            c.values["direction"] = v3(glm::dmat3(part.toGroup) * dirWorld);
            c.values["rumble"] = a.get<double>("rumble", 2.0);
            c.values["lead"] = a.get<double>("lead", 30.0);
            c.values["raggedness"] = a.get<double>("raggedness", 0.25);
            if (gn::GNode* solver = grp.node("Solver"); solver && part.bounds.valid())
            {
                const double floor = part.bounds.min.y - std::max(0.0, a.get<double>("depth", 3000.0));
                solver->values["groundHeight"] = std::min(numberValue(*solver, "groundHeight", 0.0), floor);
            }
            gn::commitGroup(g, *s.groupNode, std::move(grp));
            return node;
        },
        .examples = {"collapse('Destruction', part='Ground', start=90, duration=60, direction=[1, 0, 0])"},
    });

    ctx.registerCommand({
        .name = "addDebris",
        .doc = "Throw off small chunks and dust where a destruction breaks: chunks (more for harder breaks) settle on the ground "
               "and the pieces and join the result as instances of a few shared rock shapes; dust points are on the Debris node's "
               "dust output for Niagara. A rigid vertex animation bake leaves the chunks out; freezeDestruction(debris='only') turns them "
               "into their own group of instanced meshes to send alongside. Running it again changes the settings. Returns the Debris node.",
        .positionalDoc = "destruction (default the selection)",
        .flags = {{"amount", "a", AT::Float, 4.0, "Chunks per break, on average.", {}, {}, 0.0, 1000.0},
                  {"size", "s", AT::Float, 8.0, "Typical chunk size, cm.", {}, {}, 0.1},
                  {"spread", "", AT::Float, 300.0, "How fast chunks fly off, cm/s.", {}, {}, 0.0},
                  {"dust", "", AT::Int, 20, "Dust points per break.", {}, {}, 0.0, 10000.0},
                  {"settle", "", AT::Bool, true, "Let the chunks come to rest (level dressing); False leaves them flying, with velocity."},
                  {"seed", "", AT::Int, 1, "Another seed gives another scatter."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const Setup s = setupOf(g, destructionArg(a, "addDebris"), "addDebris");
            gn::Group grp = gn::readGroup(*s.groupNode);
            const gn::GNode* solver = grp.node("Solver");
            if (!solver)
                throw CommandError("addDebris: the destruction has no Rigid Body Solver named Solver");
            if (!grp.node("Debris"))
            {
                const std::vector<gn::GLink> out = grp.linksInto("GroupOutput", "geometry");
                if (out.empty())
                    throw CommandError("addDebris: nothing reaches the destruction's output");
                const glm::dvec2 at = solver->position + glm::dvec2(260.0, 220.0);
                grp.nodes.push_back({"Debris", "DebrisFromEvents", at, json::object()});
                grp.nodes.push_back({"WithDebris", "JoinGeometry", at + glm::dvec2(260.0, -120.0), json::object()});
                grp.links.push_back({"Solver", "breaks", "Debris", "events"});
                grp.links.push_back({"Solver", "pieces", "Debris", "colliders"});
                std::erase(grp.links, out[0]);
                grp.links.push_back({out[0].fromNode, out[0].fromSocket, "WithDebris", "geometry"});
                grp.links.push_back({"Debris", "debris", "WithDebris", "geometry"});
                grp.links.push_back({"WithDebris", "geometry", "GroupOutput", "geometry"});
            }
            const gn::GNode& sv = *grp.node("Solver");
            gn::GNode& d = *grp.node("Debris");
            d.values["amount"] = a.get<double>("amount", 4.0);
            d.values["size"] = a.get<double>("size", 8.0);
            d.values["spread"] = a.get<double>("spread", 300.0);
            d.values["dust"] = a.get<int64_t>("dust", 20);
            d.values["settle"] = a.get<bool>("settle", true);
            d.values["seed"] = a.get<int64_t>("seed", 1);
            for (const char* k : {"gravity", "ground", "groundHeight", "density"})
                if (auto it = sv.values.find(k); it != sv.values.end())
                    d.values[k] = *it;
            gn::commitGroup(g, *s.groupNode, std::move(grp));
            return "Debris";
        },
        .examples = {"addDebris('Wall', amount=6, size=5)", "addDebris('Wall', settle=False)"},
    });


    ctx.registerCommand({
        .name = "exportBreaks",
        .doc = "Write where and when each piece of a destruction broke off, to time sounds and effects in Unreal.\n\n"
               "One row per broken link, by frame: Frame, Time (seconds), X, Y, Z (cm, in the axis profile's space), Impulse "
               "(N s) and the two pieces (PieceB -1 for the ground). A .csv imports into Unreal as a Data Table (the first "
               "column is the row name); a .json is a list of objects. Returns {path, breaks}.",
        .positionalDoc = "mesh or its destruction modifier (default the selection)",
        .flags = {{"path", "p", AT::String, nullptr, "The .csv or .json file."},
                  {"end", "e", AT::Int, nullptr, "Last frame. Default: the end of the playback range."},
                  {"profile", "ax", AT::String, "ue5", "Axis profile of the positions: ue5, ue6 or generic (Lila's own).", {"ue5", "ue6", "generic"}}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("exportBreaks: give one mesh");
            if (!a.has("path"))
                throw CommandError("exportBreaks: path is required");
            const scene::NodeId mod = simulationOf(g, names[0], "exportBreaks");
            const scene::NodeId mesh = meshFor(g, mod, names[0]);
            const std::string profile = a.get<std::string>("profile", "ue5");
            const glm::dmat3 basis = profile == "generic" ? glm::dmat3(1.0) : lila::seq::unrealBasis(profile);
            const std::vector<Break> breaks = breaksOf(g, mod, mesh, a.has("end") ? a.get<int64_t>("end") : playbackEnd());
            const std::string file = a.get<std::string>("path");
            const std::filesystem::path path(std::u8string(file.begin(), file.end()));
            std::ofstream out(path, std::ios::binary);
            if (!out)
                throw CommandError("exportBreaks: can't write " + file);
            const double fps = scene::sim::framesPerSecond();
            const bool csv = path.extension() != ".json";
            json rows = json::array();
            if (csv)
                out << "---,Frame,Time,X,Y,Z,Impulse,PieceA,PieceB\n";
            for (size_t k = 0; k < breaks.size(); ++k)
            {
                const Break& b = breaks[k];
                const glm::dvec3 p = basis * b.position;
                if (csv)
                    out << std::format("Break_{:04},{},{:.4f},{:.3f},{:.3f},{:.3f},{:.3f},{},{}\n", k, b.frame, double(b.frame) / fps, p.x, p.y, p.z,
                                       b.impulse, b.a, b.b);
                else
                    rows.push_back({{"frame", b.frame}, {"time", double(b.frame) / fps}, {"position", json::array({p.x, p.y, p.z})},
                                    {"impulse", b.impulse}, {"pieceA", b.a}, {"pieceB", b.b}});
            }
            if (!csv)
                out << rows.dump(2);
            return {{"path", file}, {"breaks", int64_t(breaks.size())}};
        },
        .examples = {"exportBreaks('Wall', path='Export/Wall_breaks.csv')"},
    });

    ctx.registerCommand({
        .name = "wedgeDestruction",
        .doc = "Bake several variations of a destruction, one per seed, as rigid vertex animation.\n\n"
               "Sets the modifier's seed to seed, seed + 1, ... in turn, bakes each to <folder>/<mesh>_seed<n>, and puts the seed "
               "back. Needs a seed input on the modifier (breakApart makes one). Returns the bakes' file lists.",
        .positionalDoc = "mesh (default the selection)",
        .flags = {{"count", "c", AT::Int, 4, "How many variations.", {}, {}, 1.0, 1000.0},
                  {"seed", "s", AT::Int, nullptr, "First seed. Default: the one after the modifier's own."},
                  {"folder", "fo", AT::String, "destruction", "Output folder; relative paths are under the workspace."},
                  {"start", "st", AT::Float, nullptr, "First frame. Default: the playback range."},
                  {"end", "e", AT::Float, nullptr, "Last frame."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("wedgeDestruction: give one mesh");
            const scene::NodeId mod = simulationOf(g, names[0], "wedgeDestruction");
            Node* m = g.node(mod);
            const int seedAttr = m->type().attrIndex("seed");
            if (seedAttr < 0)
                throw CommandError("wedgeDestruction: " + m->path() + " has no seed input");
            Node* mesh = g.node(meshFor(g, mod, names[0]));
            const int64_t original = m->as<int64_t>(seedAttr);
            const int64_t first = a.has("seed") ? a.get<int64_t>("seed") : original + 1;
            const int64_t count = std::clamp<int64_t>(a.get<int64_t>("count", 4), 1, 1000);
            const std::string plug = m->path() + ".seed";
            json bakes = json::array();
            auto restore = [&] { ops::execute("setAttr", {{"args", {plug, original}}}); };
            try
            {
                for (int64_t k = 0; k < count; ++k)
                {
                    ops::execute("setAttr", {{"args", {plug, first + k}}});
                    json call = {{"args", {mesh->path()}}, {"folder", a.get<std::string>("folder", "destruction")}, {"mode", "rigid"},
                                 {"name", std::format("{}_seed{}", mesh->name(), first + k)}};
                    for (const char* f : {"start", "end"})
                        if (a.has(f))
                            call[f] = a.flags[f];
                    bakes.push_back(ops::execute("exportVertexAnimation", call)["files"]);
                }
            }
            catch (...)
            {
                restore();
                throw;
            }
            restore();
            return bakes;
        },
        .examples = {"wedgeDestruction('Wall', count=8, folder='Export/WallVariants')"},
    });
}
} // namespace destruction
