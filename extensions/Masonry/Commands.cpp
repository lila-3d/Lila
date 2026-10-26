// masonryWall builds a wall as a geometry-nodes group anyone can open (a Masonry Wall node fed by the mesh's footprint
// edges), with the settings that matter exposed; masonryInfo reports what a wall is made of and checks it.

#include "Masonry.h"
#include "MasonryCore.h"

#include <lila/Command.h>
#include <lila/Deformer.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/Mesh.h>
#include <lila/Node.h>
#include <lila/Profile.h>

#include <algorithm>
#include <cctype>
#include <memory>

using namespace lila;

namespace masonry
{
namespace
{
using json = nlohmann::json;
using scene::Graph;
using scene::Node;
using T = ArgType;

/** Exposed group inputs, in the order the attribute editor shows them: name, type, panel, min, soft max, description. */
struct Exposed
{
    const char* name;
    const char* type;
    const char* panel;
    double min;
    double softMax;
    const char* description;
};

constexpr Exposed kInputs[] = {
    {"bond", "string", "Wall", 0, 0, "running, stack, english, flemish, header or gardenWall."},
    {"height", "float", "Wall", 0, 1000, "Wall height, cm."},
    {"thickness", "float", "Wall", 1, 200, "Wall thickness, cm. Under 1.5 stone depths the wall is one stone thick."},
    {"cornerAngle", "float", "Wall", 1, 90, "Turns sharper than this, degrees, are corners; gentler ones curve the wall."},
    {"stoneLength", "float", "Stones", 1, 100, "Stone length along the face, cm."},
    {"stoneHeight", "float", "Stones", 0.5, 50, "Stone height, cm. Courses are fitted to the wall height."},
    {"stoneDepth", "float", "Stones", 0.5, 50, "Stone depth into the wall, cm; also a header's width on the face."},
    {"joint", "float", "Stones", 0, 5, "Mortar joint, cm."},
    {"recess", "float", "Stones", 0, 3, "How far the mortar sits behind the stone faces, cm."},
    {"bevel", "float", "Look", 0, 3, "Stone edge chamfer, cm."},
    {"wear", "float", "Look", 0, 1, "How knocked about stone corners are, 0 to 1."},
    {"jitter", "float", "Look", 0, 1, "How far stones wander and tilt inside their joints, 0 to 1."},
    {"protrude", "float", "Look", 0, 2, "Stones pushed out of the face by up to this, cm."},
    {"variants", "int", "Look", 1, 16, "How many differently worn versions of each stone size."},
    {"detail", "int", "Look", 0, 2, "0 worn, 1 clean chamfer, 2 plain boxes (fast preview)."},
    {"seed", "int", "Look", 0, 0, "Another seed gives other wander, wear and cut positions."},
};

std::vector<FlagDef> wallFlags()
{
    const Units u;
    const WallParams w;
    const BuildOptions o;
    return {
        {"bond", "b", T::String, "running", "How stones are arranged.", bondNames()},
        {"height", "h", T::Float, w.height, "Wall height, cm."},
        {"thickness", "th", T::Float, w.thickness, "Wall thickness, cm."},
        {"cornerAngle", "ca", T::Float, w.cornerAngle, "Turns sharper than this, degrees, are corners."},
        {"stoneLength", "sl", T::Float, u.length, "Stone length, cm."},
        {"stoneHeight", "sh", T::Float, u.height, "Stone height, cm."},
        {"stoneDepth", "sd", T::Float, u.depth, "Stone depth, cm."},
        {"joint", "j", T::Float, u.joint, "Mortar joint, cm."},
        {"recess", "r", T::Float, w.recess, "Mortar set back from the stone faces, cm."},
        {"bevel", "bv", T::Float, o.bevel, "Stone edge chamfer, cm."},
        {"wear", "w", T::Float, o.wear, "Corner wear, 0 to 1."},
        {"jitter", "ji", T::Float, w.jitter, "Wander and tilt inside the joints, 0 to 1."},
        {"protrude", "pr", T::Float, w.protrude, "Push out of the face, cm."},
        {"variants", "v", T::Int, int64_t(o.variants), "Worn versions per stone size."},
        {"detail", "d", T::Int, int64_t(0), "0 worn, 1 clean chamfer, 2 plain boxes."},
        {"seed", "s", T::Int, int64_t(w.seed), "Random seed."},
    };
}

BuildOptions optionsFromJson(const json& values)
{
    auto num = [&](const char* id) { return values.at(id).get<double>(); };
    auto integer = [&](const char* id) { return int64_t(std::llround(values.at(id).get<double>())); };
    try
    {
        return optionsFrom(num, integer, values.at("bond").get<std::string>());
    }
    catch (const gn::ExecError& e)
    {
        throw CommandError(e.what());
    }
}

size_t triangles(const geo::Mesh& m)
{
    size_t n = 0;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
            n += size_t(std::max<geo::Index>(m.faceSize(f), 2) - 2);
    return n;
}

/** What a wall is made of, for agents and the status line. */
json statsOf(const Built& b, std::span<const Footprint> fps)
{
    size_t stoneTris = 0, uniqueTris = 0;
    std::vector<size_t> perMesh(b.meshes.size());
    for (size_t r = 0; r < b.meshes.size(); ++r)
    {
        perMesh[r] = triangles(b.meshes[r]);
        uniqueTris += perMesh[r];
    }
    for (int r : b.reference)
        stoneTris += perMesh[size_t(r)];
    json roles = json::object();
    const char* roleNames[] = {"stretcher", "header", "bat", "closer", "filler"};
    for (const char* n : roleNames)
        roles[n] = 0;
    geo::Bounds bounds;
    for (const Stone& s : b.stones)
    {
        roles[roleNames[size_t(s.role)]] = roles[roleNames[size_t(s.role)]].get<int64_t>() + 1;
        bounds.extend(s.center);
    }
    return {{"walls", int64_t(fps.size())},
            {"stones", int64_t(b.stones.size())},
            {"courses", b.courses},
            {"roles", roles},
            {"stoneMeshes", int64_t(b.meshes.size())},
            {"trimmed", b.trimmed},
            {"dropped", b.dropped},
            {"stoneTriangles", int64_t(stoneTris)},
            {"stoneMeshTriangles", int64_t(uniqueTris)},
            {"coreTriangles", int64_t(triangles(b.core))},
            {"boundsMin", bounds.valid() ? json::array({bounds.min.x, bounds.min.y, bounds.min.z}) : json()},
            {"boundsMax", bounds.valid() ? json::array({bounds.max.x, bounds.max.y, bounds.max.z}) : json()}};
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

std::string identifier(const std::string& s)
{
    std::string out;
    for (char c : s)
        out += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    if (out.empty() || !std::isalpha(static_cast<unsigned char>(out[0])))
        out = "Wall" + out;
    return out;
}

Footprint footprintFromJson(const json& points, bool closed)
{
    if (!points.is_array() || points.size() < 2)
        throw CommandError("masonryWall: points takes at least two [x, y, z] points");
    Footprint f;
    for (const json& p : points)
    {
        if (!p.is_array() || p.size() != 3)
            throw CommandError("masonryWall: each point is [x, y, z]");
        f.points.emplace_back(p[0].get<double>(), p[1].get<double>(), p[2].get<double>());
    }
    f.closed = closed;
    return f;
}

/** The mesh's own edges and points, under its modifiers: where the footprint lives. */
scene::MeshPtr baseMesh(Graph& g, const Node& n) { return scene::modifiers::cage(g, n.id()); }

/** The Masonry Wall modifier in a mesh's stack (the modifier itself may be named). */
Node* wallModifier(Graph& g, const std::string& name, const char* cmd)
{
    Node* n = g.find(name);
    if (!n)
        throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
    auto isWall = [](const Node& m) { return m.type().attrIndex("stoneLength") >= 0 && m.type().attrIndex("bond") >= 0; };
    if (isWall(*n))
        return n;
    for (scene::NodeId m : scene::modifiers::stack(g, n->id()))
        if (Node* mod = g.node(m); mod && isWall(*mod))
            return mod;
    throw CommandError(std::string(cmd) + ": " + n->path() + " has no masonry wall (make one with masonryWall)");
}

json modifierValues(const Node& mod)
{
    json values = json::object();
    for (const Exposed& e : kInputs)
        values[e.name] = ops::execute("getAttr", {{"args", {mod.path() + "." + e.name}}});
    return values;
}
} // namespace

void registerCommands(ext::Context& ctx)
{
    std::vector<FlagDef> flags = {
        {"points", "p", T::List, nullptr, "Footprint, [[x, y, z], ...] in cm; the wall stands on the lowest point, Y up."},
        {"closed", "c", T::Bool, false, "Join the last point back to the first."},
        {"name", "n", T::String, "MasonryWall", "Name of the mesh made to hold the wall."},
    };
    for (FlagDef& f : wallFlags())
        flags.push_back(std::move(f));

    ctx.registerCommand({
        .name = "masonryWall",
        .doc = "Build a masonry wall along a line or loop: stones laid in courses, corners bonded, a mortar core behind them.\n\n"
               "Give points= (a new mesh holds them as edges) or a mesh whose loose edges are the footprint. The wall is a "
               "geometry-nodes modifier around a Masonry Wall node; every flag becomes a keyable input on it, so change the wall "
               "later with setAttr on '<modifier>.<flag>' and check it with masonryInfo. Stones are instances of a few shared "
               "meshes with stone, course, role, face, random and variant attributes. Returns the mesh, modifier, group and stats "
               "(stones, courses, roles, stone meshes, trimmed, dropped, triangle counts, bounds). Lengths are cm.",
        .positionalDoc = "mesh whose loose edges are the footprint (optional)",
        .flags = flags,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            json values = json::object();
            for (const Exposed& e : kInputs)
                values[e.name] = a.flags[e.name];
            const BuildOptions o = optionsFromJson(values);

            std::vector<Footprint> fps;
            std::string host;
            if (!a.positional.empty())
            {
                Node* mesh = g.find(a.positional[0].get<std::string>());
                if (!mesh || !mesh->type().isA("mesh"))
                    throw CommandError("masonryWall: " + a.positional[0].dump() + " is not a mesh");
                if (auto m = baseMesh(g, *mesh))
                    fps = footprintsFromEdges(*m);
                if (fps.empty())
                    throw CommandError("masonryWall: " + mesh->path() + " has no loose edges to build along");
                host = mesh->path();
            }
            else
            {
                if (!a.has("points"))
                    throw CommandError("masonryWall: give points= or a mesh with loose edges");
                fps.push_back(footprintFromJson(a.flags["points"], a.get<bool>("closed", false)));
                json made = ops::execute("createMesh", {{"points", json::array()}, {"faces", json::array()},
                                                        {"name", a.get<std::string>("name", "MasonryWall")}});
                host = made.is_string() ? made.get<std::string>() : made.at(0).get<std::string>();
                Node* mesh = g.find(host);
                g.setValue({mesh->id(), scene::MeshNode::kInMesh, -1},
                           scene::MeshPtr(std::make_shared<geo::Mesh>(edgesFromFootprints(fps))));
            }

            const std::string group = uniqueGroupName(identifier(g.find(host)->name()) + "_Masonry");
            json inputs = json::array();
            json links = json::array({"GroupInput.geometry -> Wall.footprint", "Wall.geometry -> GroupOutput.geometry"});
            for (const Exposed& e : kInputs)
            {
                json in = {{"name", e.name}, {"type", e.type}, {"default", values[e.name]}, {"panel", e.panel}, {"description", e.description}};
                if (std::string_view(e.type) != "string")
                {
                    in["min"] = e.min;
                    if (e.softMax > e.min)
                        in["softMax"] = e.softMax;
                }
                inputs.push_back(in);
                links.push_back(std::string("GroupInput.") + e.name + " -> Wall." + e.name);
            }
            json spec = {{"name", group},
                         {"description", "Lays stones in courses along the mesh's loose edges, with a mortar core behind them."},
                         {"inputs", inputs},
                         {"nodes", json::array({{{"name", "Wall"}, {"type", "MasonryWall"}}})},
                         {"links", links}};
            ops::execute("gnBuildGroup", {{"args", {spec}}, {"replace", false}});
            json mod = ops::execute("gnApply", {{"args", {host, group}}});

            json out = statsOf(buildWalls(fps, o), fps);
            out["mesh"] = host;
            out["modifier"] = mod;
            out["group"] = group;
            return out;
        },
        .examples = {"masonryWall(points=[[0, 0, 0], [400, 0, 0], [400, 0, 300]], bond='flemish', height=250)",
                     "tower = masonryWall(points=[[100 * math.cos(a / 12 * math.tau), 0, 100 * math.sin(a / 12 * math.tau)] for a in range(12)],\n"
                     "                    closed=True, thickness=40, height=600)\n"
                     "setAttr(tower['modifier'] + '.bond', 'english')\n"
                     "masonryInfo(tower['mesh'])"},
    });

    ctx.registerCommand({
        .name = "masonryInfo",
        .doc = "What a masonry wall is made of, and whether it is clean.\n\n"
               "Returns the same stats masonryWall does (stones, courses, roles, stone meshes, trimmed, dropped, triangle "
               "counts, bounds), the current settings, and checks: overlaps (stone pairs intersecting by more than 0.01 cm, "
               "0 when clean) and ok.",
        .positionalDoc = "mesh or its masonry modifier (default the selection)",
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("masonryInfo: give one mesh");
            Node* mod = wallModifier(g, names[0], "masonryInfo");
            const scene::NodeId meshId = scene::modifiers::meshOf(g, mod->id());
            Node* mesh = meshId != scene::kNoNode ? g.node(meshId) : g.find(names[0]);
            std::vector<Footprint> fps;
            if (auto m = mesh ? baseMesh(g, *mesh) : nullptr)
                fps = footprintsFromEdges(*m);
            const json values = modifierValues(*mod);
            const Built b = buildWalls(fps, optionsFromJson(values));
            json out = statsOf(b, fps);
            const size_t overlapping = countOverlaps(b.stones);
            out["settings"] = values;
            out["checks"] = {{"overlaps", int64_t(overlapping)}, {"ok", overlapping == 0 && !b.stones.empty()}};
            out["mesh"] = mesh ? mesh->path() : std::string();
            out["modifier"] = mod->path();
            return out;
        },
        .examples = {"masonryInfo('MasonryWall')['checks']"},
    });
}
} // namespace masonry
