// masonryWall builds a wall as a geometry-nodes group anyone can open (a Masonry Wall node fed by the mesh's footprint
// edges), with the settings that matter exposed; masonryInfo reports what a wall is made of and checks it; masonryShip
// sends it to Unreal.

#include "Masonry.h"
#include "MasonryCore.h"
#include "MasonryShip.h"

#include <lila/Command.h>
#include <lila/Deformer.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Mesh.h>
#include <lila/Node.h>
#include <lila/Profile.h>
#include <lila/Tasks.h>
#include <lila/Transform.h>
#include <lila/Undo.h>
#include <lila/UnrealLink.h>

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <memory>
#include <random>
#include <set>

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

/** A wall named on the command line: its modifier, the mesh holding it, and its footprints. */
struct WallRef
{
    Node* mod = nullptr;
    Node* mesh = nullptr;
    std::vector<Footprint> footprints;
};

WallRef wallRef(Graph& g, const Args& a, const char* cmd)
{
    std::vector<std::string> names = a.objects();
    if (names.size() != 1)
        throw CommandError(std::string(cmd) + ": give one mesh");
    WallRef w;
    w.mod = wallModifier(g, names[0], cmd);
    const scene::NodeId meshId = scene::modifiers::meshOf(g, w.mod->id());
    w.mesh = meshId != scene::kNoNode ? g.node(meshId) : g.find(names[0]);
    if (auto m = w.mesh ? baseMesh(g, *w.mesh) : nullptr)
        w.footprints = footprintsFromEdges(*m);
    return w;
}

/** The mesh's link id, given one (undoably) if it has none, so Unreal knows the wall again after a rename. */
std::string wallKey(Graph& g, Node& mesh)
{
    if (const std::string* s = std::get_if<std::string>(&mesh.value(scene::Transform::kLinkId)); s && !s->empty())
        return *s;
    static std::mt19937_64 rng{std::random_device{}()};
    const std::string fresh = std::format("{:016x}", rng());
    UndoChunk chunk("Unreal Link Id");
    g.setValue({mesh.id(), scene::Transform::kLinkId, -1}, fresh);
    return fresh;
}

/** Group keys a wall sent last time, so the ones it no longer has can be taken out of the level. */
std::filesystem::path sentFile(const std::string& key) { return link::cacheDir() / "masonry" / (key + ".json"); }

std::set<std::string> readSent(const std::string& key)
{
    std::set<std::string> out;
    std::ifstream in(sentFile(key));
    if (!in)
        return out;
    const json j = json::parse(in, nullptr, false);
    if (j.is_array())
        for (const json& k : j)
            if (k.is_string())
                out.insert(k.get<std::string>());
    return out;
}

void writeSent(const std::string& key, const json& groups)
{
    json keys = json::array();
    for (const json& g : groups)
        keys.push_back(g["key"]);
    std::error_code ec;
    std::filesystem::create_directories(sentFile(key).parent_path(), ec);
    std::ofstream(sentFile(key)) << keys.dump();
}

/** Imports the plan's assets and places its groups, then removes the stale groups. Runs on the link worker. */
json sendPlan(const json& plan, const std::vector<std::filesystem::path>& files, const json& stale)
{
    LILA_ZONE_N("Masonry to Unreal");
    std::shared_ptr<link::RpcClient> client = link::Link::get().client();
    if (!client)
        throw std::runtime_error("not connected to Unreal");
    json errors = json::array();
    std::vector<std::string> meshes;
    for (size_t k = 0; k < files.size(); ++k)
    {
        const json& as = plan["assets"][k];
        const json mapping = {{"contentPath", as["contentPath"]}, {"kind", "static"}};
        link::RpcResult r = client->call("importAsset", link::importParams(files[k], mapping), link::importCallOptions());
        std::string mesh = as["objectPath"].get<std::string>();
        if (!r.ok)
            errors.push_back(as["name"].get<std::string>() + ": " + r.error);
        else if (json made = r.result.value("assets", json::array()); made.is_array() && !made.empty() && made[0].is_string())
            mesh = made[0].get<std::string>();
        meshes.push_back(mesh);
    }
    int64_t placed = 0;
    for (const json& grp : plan["groups"])
    {
        link::RpcResult r = client->call("placeInstances", {{"group", grp["key"]},
                                                            {"label", grp["label"]},
                                                            {"mesh", meshes[grp["asset"].get<size_t>()]},
                                                            {"mode", grp["mode"]},
                                                            {"placements", grp["placements"]}});
        if (r.ok)
            placed += int64_t(grp["placements"].size());
        else
            errors.push_back(grp["label"].get<std::string>() + ": " + r.error);
    }
    json removed = json::object();
    if (!stale.empty())
    {
        link::RpcResult r = client->call("removePlacements", {{"groups", stale}});
        if (r.ok)
            removed = r.result;
        else
            errors.push_back("removing old chunks: " + r.error);
    }
    return {{"imported", int64_t(files.size())}, {"placed", placed}, {"removed", removed}, {"errors", errors}};
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
            // Set on the modifier itself: a modifier type kept from an earlier scene's group of this name has its defaults.
            const std::string modName = mod.is_string() ? mod.get<std::string>() : mod.at(0).get<std::string>();
            for (const Exposed& e : kInputs)
                ops::execute("setAttr", {{"args", {modName + "." + e.name, values[e.name]}}});

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
            const WallRef w = wallRef(g, a, "masonryInfo");
            Node* mod = w.mod;
            Node* mesh = w.mesh;
            const std::vector<Footprint>& fps = w.footprints;
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

    ctx.registerCommand({
        .name = "masonryShip",
        .doc = "Send a masonry wall to Unreal's open level, instanced: each stone mesh is imported once as a static mesh with "
               "three LODs (worn, clean chamfer, plain box), and the stones are placed as one instanced component (HISM by "
               "default) per stone mesh per ground chunk, so streaming and culling work per chunk. The core goes as its own "
               "mesh. Instances carry custom primitive data 0 a stable random and 1 height up the wall (0 bottom course, 1 "
               "top). Sending again replaces the wall's chunks in place and removes the ones it no longer has.\n\n"
               "Returns stats (stones, assets, groups, chunks, average triangles per stone mesh at each LOD, removedGroups) "
               "plus, with wait=True, what Unreal imported and placed. dryRun=True returns the plan without writing files or "
               "connecting.",
        .positionalDoc = "mesh or its masonry modifier (default the selection)",
        .flags = {{"folder", "f", T::String, "/Game/Lila/Masonry", "Content folder; the wall's assets go in <folder>/<mesh name>."},
                  {"chunkSize", "cs", T::Float, 400.0, "Ground cell per instanced component, cm."},
                  {"mode", "m", T::String, "hism", "Instanced component type.", {"hism", "ism"}},
                  {"dryRun", "", T::Bool, false, "Return the plan, placements included, without writing files or contacting Unreal."},
                  {"wait", "w", T::Bool, false, "Block until Unreal has imported and placed everything."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            const WallRef w = wallRef(g, a, "masonryShip");
            if (!w.mesh)
                throw CommandError("masonryShip: the masonry modifier is on no mesh");
            BuildOptions o = optionsFromJson(modifierValues(*w.mod));
            o.lods = true;
            const Built b = buildWalls(w.footprints, o);
            if (b.stones.empty())
                throw CommandError("masonryShip: " + w.mesh->path() + " has no stones");

            ShipOptions so;
            so.folder = a.get<std::string>("folder", so.folder);
            so.chunkSize = std::max(10.0, a.get<double>("chunkSize", so.chunkSize));
            so.mode = a.get<std::string>("mode", so.mode);
            if (so.mode != "hism" && so.mode != "ism")
                throw CommandError("masonryShip: mode is hism or ism, not '" + so.mode + "'");
            g.evaluate();
            const glm::dmat4 world = std::get<glm::dmat4>(g.value({w.mesh->id(), scene::Transform::kWorldMatrix, -1}));
            const bool dry = a.get<bool>("dryRun", false);
            const std::string key = dry ? std::string("dryRun") : wallKey(g, *w.mesh);
            const std::string name = identifier(w.mesh->name());
            json plan = shipPlan(b, world, key, name, so);

            std::array<size_t, 3> lodTris{};
            for (const auto& chain : b.lods)
                for (size_t l = 0; l < chain.size(); ++l)
                    lodTris[l] += triangles(chain[l]);
            json stats = {{"stones", int64_t(b.stones.size())},
                          {"assets", int64_t(plan["assets"].size())},
                          {"groups", int64_t(plan["groups"].size())},
                          {"chunks", int64_t(plan["chunks"].size())},
                          {"lodTriangles", json::array()},
                          {"folder", plan["folder"]},
                          {"mesh", w.mesh->path()}};
            for (size_t t : lodTris)
                stats["lodTriangles"].push_back(double(t) / double(std::max<size_t>(1, b.lods.size())));
            if (dry)
            {
                stats["plan"] = std::move(plan);
                return stats;
            }

            const std::filesystem::path dir = link::cacheDir() / "masonry" / name;
            std::vector<std::filesystem::path> files;
            for (const json& as : plan["assets"])
            {
                const int mesh = as["mesh"].get<int>();
                const std::string assetName = as["name"].get<std::string>();
                const std::filesystem::path file = dir / (assetName + ".fbx");
                std::string err;
                const bool ok = mesh >= 0 ? writeLodFbx(b.lods[size_t(mesh)], assetName, file, &err)
                                          : writeLodFbx(std::span<const geo::Mesh>(&b.core, 1), assetName, file, &err);
                if (!ok)
                    throw CommandError("masonryShip: writing " + file.string() + " failed: " + err);
                files.push_back(file);
            }
            std::set<std::string> now;
            for (const json& grp : plan["groups"])
                now.insert(grp["key"].get<std::string>());
            json stale = json::array();
            for (const std::string& old : readSent(key))
                if (!now.count(old))
                    stale.push_back(old);
            stats["removedGroups"] = int64_t(stale.size());

            try
            {
                link::Link::get().ensureConnected();
            }
            catch (const std::runtime_error& e)
            {
                throw CommandError(std::string("masonryShip: not connected to Unreal: ") + e.what());
            }
            writeSent(key, plan["groups"]);
            if (a.get<bool>("wait", false))
            {
                stats["unreal"] = sendPlan(plan, files, stale);
                return stats;
            }
            link::Link::get().post([plan = std::move(plan), files, stale, name] {
                json done;
                try
                {
                    done = sendPlan(plan, files, stale);
                }
                catch (const std::exception& e)
                {
                    done = {{"errors", json::array({e.what()})}};
                }
                mainq::post([done, name] {
                    for (const json& e : done.value("errors", json::array()))
                        log::error("masonryShip {}: {}", name, e.get<std::string>());
                    if (done.contains("placed"))
                        log::info("masonryShip: placed {} stones of {} in Unreal", done["placed"].get<int64_t>(), name);
                });
            });
            stats["queued"] = true;
            return stats;
        },
        .examples = {"masonryShip('MasonryWall', dryRun=True)['chunks']", "masonryShip('MasonryWall', wait=True)['unreal']"},
    });
}
} // namespace masonry
