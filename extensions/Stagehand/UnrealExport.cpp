// What Stagehand sends to Unreal: body shapes as UCX_ simple collision, body settings on each placed actor (simulate,
// mass, physical material), and assemblies as Level Instances or Packed Level Actors.

#include "Stagehand.h"

#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/Fbx.h>
#include <lila/Mesh.h>
#include <lila/Profile.h>
#include <lila/Relocate.h>
#include <lila/Transform.h>
#include <lila/UnrealLink.h>

#include <cctype>
#include <format>
#include <random>

using namespace lila;

namespace stagehand
{
namespace
{
using UeArg = ArgType;

constexpr const char* kAssemblyType = "stagehandAssembly";
constexpr const char* kAssemblyKinds[] = {"levelInstance", "packedLevelActor"};

/** A mesh's body shape in its own space, unscaled: what its exported asset collides as. */
physics::Shape localShape(Graph& g, NodeId mesh, const BodySettings& s)
{
    Node* n = g.node(mesh);
    const int out = n ? n->type().attrIndex("outMesh") : -1;
    if (out < 0)
        return {};
    return shapeOfMesh(n->as<scene::MeshPtr>(out), glm::dvec3(1.0), s.shape, s.rounding, s.role == RoleChoice::Static, s.maxParts);
}

std::vector<geo::Mesh> collisionOf(Graph& g, NodeId mesh)
{
    const BodySettings s = bodySettings(g, mesh);
    if (!s.hasNode || s.role == RoleChoice::Off)
        return {};
    std::vector<geo::Mesh> out;
    for (const physics::ConvexMesh& c : physics::convexMeshes(localShape(g, mesh, s)))
    {
        std::vector<geo::Index> sizes, indices;
        for (const std::vector<int>& f : c.faces)
        {
            sizes.push_back(geo::Index(f.size()));
            for (int i : f)
                indices.push_back(geo::Index(i));
        }
        out.push_back(geo::Mesh::fromPolygons(c.points, sizes, indices));
    }
    return out;
}

/** The body's mass in kg as placed (its scale included). */
double massOf(Graph& g, NodeId mesh)
{
    WorldOptions o;
    o.movers = {mesh};
    o.includeDynamicBodies = false;
    o.collidersGiven = true;
    o.ground = false;
    World w(g, o);
    return w.movers() ? w.scene().mass(0) : 0.0;
}

json physicsOf(Graph& g, NodeId mesh)
{
    const BodySettings s = bodySettings(g, mesh);
    if (!s.hasNode || s.role == RoleChoice::Off)
        return nullptr;
    const physics::Shape shape = localShape(g, mesh, s);
    const bool complex = shape.kind == physics::Shape::Kind::Mesh || shape.kind == physics::Shape::Kind::Heightfield;
    const char* roles[] = {"dynamic", "static", "kinematic", "off"};
    json p = {{"simulate", s.role == RoleChoice::Dynamic},
              {"role", roles[int(s.role)]},
              {"material", s.material},
              {"density", s.density},
              {"friction", s.friction},
              {"restitution", s.bounce},
              {"collision", complex ? "complexAsSimple" : "simple"}};
    if (s.role == RoleChoice::Dynamic)
        p["mass"] = massOf(g, mesh);
    return {{"physics", p}};
}


std::string str(Node& n, const char* attr) { return n.as<std::string>(n.type().attrIndex(attr)); }

std::string madeName(const json& made) { return made.is_array() ? made.at(0).get<std::string>() : made.get<std::string>(); }

std::string assetSafe(const std::string& name)
{
    std::string out;
    for (char c : name)
        out += std::isalnum(static_cast<unsigned char>(c)) || c == '_' ? c : '_';
    return out.empty() ? "Assembly" : out;
}

/** The root an assembly command names: the group itself, or any node inside one. */
NodeId assemblyRoot(Graph& g, const std::string& name, const char* cmd)
{
    Node* n = g.find(name);
    if (!n)
        throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
    for (NodeId id = n->id(); id != scene::kNoNode; id = g.parent(id))
        if (assemblyTag(g, id))
            return id;
    throw CommandError(std::string(cmd) + ": " + n->path() + " is not in an assembly (assemblyCreate makes one)");
}
} // namespace

Node* assemblyTag(Graph& g, NodeId root)
{
    for (NodeId c : g.children(root))
        if (Node* n = g.node(c); n && n->type().name == kAssemblyType)
            return n;
    return nullptr;
}

std::string assemblyKind(Graph& g, NodeId root)
{
    Node* tag = assemblyTag(g, root);
    return tag ? kAssemblyKinds[std::clamp<int64_t>(tag->as<int64_t>(tag->type().attrIndex("kind")), 0, 1)] : std::string();
}

void tagAssembly(Graph& g, NodeId root, const std::string& kind, const std::string& level)
{
    const std::string tag =
        ops::execute("createNode", {{"args", {kAssemblyType}}, {"name", "assembly"}, {"parent", g.node(root)->path()}, {"skipSelect", true}}).get<std::string>();
    ops::execute("setAttr", {{"args", {tag + ".kind", kind == "packedLevelActor" ? int64_t(1) : int64_t(0)}}});
    if (!level.empty())
        ops::execute("setAttr", {{"args", {tag + ".level", level}}});
    static std::mt19937_64 rng{std::random_device{}()};
    ops::execute("setAttr", {{"args", {tag + ".key", std::format("{:016x}", rng())}}});
}

bool registerUnrealNodes(ext::Context& ctx)
{
    const scene::NodeType* base = scene::types::find("transform");
    if (!base)
        return false;
    scene::NodeType t;
    t.name = kAssemblyType;
    t.base = base;
    t.dag = true;
    t.icon = "package";
    t.doc = "Marks the group it sits under as an assembly: placed props that go to Unreal together as one Level Instance or "
            "Packed Level Actor.";
    t.ownAttrs = {
        {.name = "kind", .type = scene::PlugType::Enum, .defaultValue = int64_t(0), .enumNames = {std::begin(kAssemblyKinds), std::end(kAssemblyKinds)},
         .doc = "levelInstance: a level of actors placed as one. packedLevelActor: its meshes packed into instanced components."},
        {.name = "level", .type = scene::PlugType::String, .defaultValue = std::string(),
         .doc = "Unreal package of its level (default <folder>/LI_<name>)."},
        {.name = "key", .type = scene::PlugType::String, .defaultValue = std::string(), .flags = scene::AttrInput | scene::AttrStorable | scene::AttrHidden,
         .doc = "Matches the assembly to what an earlier send made in Unreal."},
    };
    t.create = base->create;
    lila::scene::relocate::registerNeutral(t.name);
    return scene::types::registerType(ctx, std::move(t)) != nullptr;
}

void registerUnrealExport(ext::Context& ctx)
{
    fbx::addCollisionSource(ctx, "stagehand.bodies", [](Graph& g, NodeId mesh) { return collisionOf(g, mesh); });
    link::addPlacementExtra(ctx, "stagehand.physics", [](Graph& g, NodeId mesh) { return physicsOf(g, mesh); });

    ctx.registerCommand({
        .name = "assemblyCreate",
        .doc = "Group placed objects into an assembly: a group that goes to Unreal as one Level Instance (or Packed Level Actor) "
               "with unrealPlaceAssembly. Returns the group.",
        .positionalDoc = "objects (default the selection)",
        .flags = {{"name", "n", UeArg::String, "Assembly", "Name of the group."},
                  {"kind", "k", UeArg::String, "levelInstance", "What Unreal makes of it.", {"levelInstance", "packedLevelActor"}},
                  {"level", "lv", UeArg::String, nullptr, "Unreal package for its level, e.g. /Game/Levels/Kits/LI_Gate."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const std::vector<std::string> objects = a.objects();
            if (objects.empty())
                throw CommandError("assemblyCreate: give objects");
            const std::string group = madeName(ops::execute("group", {{"args", objects}, {"name", a.get<std::string>("name", "Assembly")}}));
            tagAssembly(g, g.find(group)->id(), a.get<std::string>("kind", "levelInstance"), a.get<std::string>("level", ""));
            g.select({g.find(group)->id()});
            return group;
        },
        .examples = {"assemblyCreate(['Gate_Left', 'Gate_Right', 'Gate_Arch'], name='Gate', kind='packedLevelActor')"},
    });

    ctx.registerCommand({
        .name = "unrealPlaceAssembly",
        .doc = "Send an assembly to Unreal as one Level Instance (or Packed Level Actor): its meshes are imported and placed "
               "as actors (with their bodies' collision and physics), then made into a level of their own placed where they "
               "were. Sending it again replaces the one an earlier send made. Returns the placement plan and levelInstance "
               "(Unreal's answer, or the request with dryRun=True).",
        .positionalDoc = "assembly (a group made by assemblyCreate, or anything in one)",
        .flags = {{"folder", "f", UeArg::String, "/Game/Lila/Instances", "Content folder for the meshes and, by default, the level."},
                  {"kind", "k", UeArg::String, nullptr, "Override the assembly's kind.", {"levelInstance", "packedLevelActor"}},
                  {"dryRun", "", UeArg::Bool, false, "Return what would be sent without contacting Unreal."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            const std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("unrealPlaceAssembly: give one assembly");
            const NodeId root = assemblyRoot(g, names[0], "unrealPlaceAssembly");
            Node& tag = *assemblyTag(g, root);
            const std::string folder = a.get<std::string>("folder", "/Game/Lila/Instances");
            const std::string kind = a.has("kind") ? a.get<std::string>("kind")
                                                   : kAssemblyKinds[std::clamp<int64_t>(tag.as<int64_t>(tag.type().attrIndex("kind")), 0, 1)];
            const std::string name = assetSafe(g.node(root)->name());
            std::string level = str(tag, "level");
            if (level.empty())
                level = folder + "/LI_" + name;
            const bool dry = a.get<bool>("dryRun", false);
            const json plan = ops::execute("unrealPlaceInstances", {{"args", {g.node(root)->path()}}, {"folder", folder}, {"mode", "actors"},
                                                                   {"dryRun", dry}, {"wait", !dry}});
            json groups = json::array();
            for (const json& grp : plan.value("groups", json::array()))
                groups.push_back(grp.at("key"));
            const json request = {{"key", str(tag, "key")}, {"name", name}, {"kind", kind}, {"level", level}, {"groups", groups}};
            if (dry)
                return {{"plan", plan}, {"levelInstance", request}};
            std::shared_ptr<link::RpcClient> client = link::Link::get().client();
            if (!client)
                throw CommandError("unrealPlaceAssembly: not connected to Unreal");
            const link::RpcResult made = client->call("makeLevelInstance", request, 120000);
            return {{"plan", plan}, {"levelInstance", made.ok ? made.result : json{{"error", made.error}}}};
        },
        .examples = {"unrealPlaceAssembly('Gate')", "unrealPlaceAssembly('Gate', kind='packedLevelActor', dryRun=True)['levelInstance']"},
    });
}
} // namespace stagehand
