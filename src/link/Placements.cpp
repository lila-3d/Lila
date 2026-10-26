#include "link/UnrealLink.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/MainQueue.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "link/Exporters.h"
#include "math/Matrix.h"
#include "math/Transform.h"
#include "math/Unreal.h"
#include "scene/ArrayGroup.h"
#include "scene/Graph.h"
#include "scene/Instancing.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <map>
#include <random>
#include <unordered_map>

namespace lila::link
{
namespace
{
std::string assetSafe(const std::string& name)
{
    std::string out;
    for (char c : name)
        out += std::isalnum(static_cast<unsigned char>(c)) || c == '_' ? c : '_';
    return out.empty() ? "Unnamed" : out;
}

bool placeable(const scene::Node* n) { return n && n->type().isA("mesh") && !n->type().isA("lattice"); }

json vec(const math::Vector& v) { return json::array({v.x, v.y, v.z}); }

json placementAt(const glm::dmat4& world, json id, std::string name)
{
    const math::Transform t = math::toUnreal(math::Transform(math::Matrix(world)));
    return {{"id", std::move(id)},
            {"name", std::move(name)},
            {"location", vec(t.translation)},
            {"rotation", json::array({t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w})},
            {"scale", vec(t.scale3d)}};
}
std::string newLinkId()
{
    static std::mt19937_64 rng{std::random_device{}()};
    return std::format("{:016x}", rng());
}

/** Placement keys by node: each node's linkId, unless an older node holds the same one (a pasted or imported copy). */
class LinkKeys
{
public:
    LinkKeys(scene::Graph& g, bool assign) : mGraph(g), mAssign(assign)
    {
        for (scene::NodeId id : g.nodes())
            if (const std::string* s = linkIdOf(id); s && !s->empty())
                if (auto [it, fresh] = mOwner.try_emplace(*s, id); !fresh && id < it->second)
                    it->second = id;
    }

    std::string operator()(scene::NodeId id)
    {
        const std::string* s = linkIdOf(id);
        if (s && !s->empty() && mOwner.at(*s) == id)
            return *s;
        if (!mAssign || !s)
            return std::to_string(id);
        std::string fresh = newLinkId();
        mGraph.setValue({id, scene::Transform::kLinkId, -1}, fresh);
        mOwner[fresh] = id;
        return fresh;
    }

private:
    const std::string* linkIdOf(scene::NodeId id) const
    {
        const scene::Node* n = mGraph.node(id);
        if (!n || !n->type().dag || n->type().attrs.size() <= size_t(scene::Transform::kLinkId))
            return nullptr;
        return std::get_if<std::string>(&n->value(scene::Transform::kLinkId));
    }

    scene::Graph& mGraph;
    bool mAssign;
    std::unordered_map<std::string, scene::NodeId> mOwner;
};

/** 0 to 1, the same for a key every send: per-instance variation (colour, wear) a material reads as custom primitive data 0. */
double stableRandom(const std::string& key)
{
    uint64_t h = 1469598103934665603ull;
    for (char c : key)
        h = (h ^ uint8_t(c)) * 1099511628211ull;
    return double(h >> 11) / double(1ull << 53);
}

json placement(scene::Graph& g, scene::NodeId id, const std::string& key)
{
    json p = placementAt(std::get<glm::dmat4>(g.value({id, scene::Transform::kWorldMatrix, -1})), key, g.node(id)->path());
    p["customData"] = json::array({stableRandom(key)});
    return p;
}

/** The node's array copies, keyed "<node key>:<copy>" so Unreal tracks each one. */
std::vector<json> arrayPlacements(scene::Graph& g, scene::NodeId id, const std::string& key)
{
    std::vector<json> out;
    const glm::dmat4 world = std::get<glm::dmat4>(g.value({id, scene::Transform::kWorldMatrix, -1}));
    const std::vector<scene::arrays::Copy> copies = scene::arrays::copiesOf(g, id);
    for (size_t k = 0; k < copies.size(); ++k)
        out.push_back(placementAt(copies[k].transform * world, std::format("{}:{}", key, k + 1), std::format("{}_{}", g.node(id)->path(), k + 1)));
    return out;
}
} // namespace

json placementPlan(scene::Graph& g, const std::vector<scene::NodeId>& roots, const PlacementOptions& o)
{
    LILA_ZONE();

    if (o.mode != "auto" && o.mode != "actors" && o.mode != "ism" && o.mode != "hism")
        throw CommandError("unrealPlaceInstances: mode is auto, actors, ism or hism, not '" + o.mode + "'");
    g.evaluate();
    // Every mesh under the roots counts with its whole instance group: Unreal drops a group's placements Lila no longer sends.
    std::vector<scene::NodeId> sources;
    auto touch = [&](scene::NodeId id) {
        if (!placeable(g.node(id)))
            return;
        const scene::NodeId src = scene::instancing::root(g, id);
        if (std::ranges::find(sources, src) == sources.end())
            sources.push_back(src);
    };
    for (scene::NodeId r : roots)
    {
        touch(r);
        for (scene::NodeId d : g.descendants(r))
            touch(d);
    }
    std::string folder = o.folder.empty() ? "/Game/Lila/Instances" : o.folder;
    while (folder.size() > 1 && folder.back() == '/')
        folder.pop_back();
    LinkKeys keys(g, o.assignIds);
    json groups = json::array();
    for (scene::NodeId src : sources)
    {
        std::vector<scene::NodeId> placed{src};
        for (scene::NodeId m : scene::instancing::members(g, src))
            if (m != src && scene::instancing::root(g, m) == src)
                placed.push_back(m);
        const scene::Node* n = g.node(src);
        const std::string asset = "SM_" + assetSafe(n->name());
        json list = json::array();
        for (scene::NodeId id : placed)
        {
            // Hidden members stay out of the level: a library asset's master only supplies the mesh.
            if (!scene::effectivelyVisible(g, id))
                continue;
            const std::string key = keys(id);
            list.push_back(placement(g, id, key));
            for (json& copy : arrayPlacements(g, id, key))
                list.push_back(std::move(copy));
        }
        const std::string mode = o.mode != "auto" ? o.mode : int(list.size()) > o.threshold ? "hism" : "actors";
        groups.push_back({{"key", keys(src)},
                          {"source", n->fullPath()},
                          {"sourceId", src},
                          {"contentPath", folder + "/" + asset},
                          {"objectPath", folder + "/" + asset + "." + asset},
                          {"mode", mode},
                          {"placements", std::move(list)}});
    }
    return {{"groups", std::move(groups)}};
}

void registerPlacementCommands()
{
    using T = ArgType;

    ops::registerCommand({
        .name = "unrealPlaceInstances",
        .doc = "Send meshes to Unreal as one static mesh asset per instance source, placed in the open level: an actor per "
               "member referencing that asset, or one instanced static mesh component (ISM or HISM) for a large group. "
               "Placements are keyed by each node's linkId, given on the first send and saved with the scene, so sending "
               "a group again moves, adds and removes its actors to match, also after a rename or reopening the scene. "
               "Returns the plan, with each group's import and placement results when wait=True.",
        .positionalDoc = "objects",
        .flags = {{"folder", "f", T::String, "/Game/Lila/Instances", "Content folder the assets go in; each is SM_<source name>."},
                  {"mode", "m", T::String, "auto",
                   "How members are placed: actors, ism, hism, or auto (actors up to threshold members, then hism).",
                   {"auto", "actors", "ism", "hism"}},
                  {"threshold", "t", T::Int, nullptr, "auto switches to hism above this many members (default the unreal pref /instanceThreshold, 50)."},
                  {"dryRun", "", T::Bool, false, "Return the plan without exporting or contacting Unreal."},
                  {"wait", "w", T::Bool, false, "Block until Unreal has imported and placed everything."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            scene::Graph& g = scene::Graph::current();
            std::vector<scene::NodeId> roots;
            for (const std::string& name : a.objects())
            {
                scene::Node* n = g.find(name);
                if (!n)
                    throw CommandError("unrealPlaceInstances: no node named '" + name + "'");
                roots.push_back(n->id());
            }
            PlacementOptions o;
            o.folder = a.get<std::string>("folder", o.folder);
            o.mode = a.get<std::string>("mode", o.mode);
            o.threshold = a.has("threshold") ? a.get<int>("threshold") : PrefStore::get().value<int>("unreal", "/instanceThreshold", 50);
            o.assignIds = !a.get<bool>("dryRun");
            json plan;
            {
                UndoChunk chunk("Unreal Link Ids");
                plan = placementPlan(g, roots, o);
            }
            if (plan["groups"].empty())
                throw CommandError("unrealPlaceInstances: select meshes to send");
            if (a.get<bool>("dryRun"))
                return plan;

            ExportFn exporter = findExporter("fbx:instanceSource");
            if (!exporter)
                exporter = findExporter("fbx");
            if (!exporter)
                throw CommandError("unrealPlaceInstances: no FBX exporter is registered");
            for (json& grp : plan["groups"])
                grp["materials"] = materialParams(g, {scene::NodeId(grp["sourceId"].get<uint64_t>())});
            try
            {
                Link::get().ensureConnected();
            }
            catch (const std::runtime_error& e)
            {
                throw CommandError(std::string("not connected to Unreal: ") + e.what());
            }

            auto job = [plan, exporter, dir = cacheDir()]() mutable {
                LILA_ZONE_N("Unreal placements");
                std::shared_ptr<RpcClient> client = Link::get().client();
                if (!client)
                    throw std::runtime_error("not connected");
                std::error_code ec;
                fs::create_directories(dir, ec);
                for (json& grp : plan["groups"])
                {
                    const std::string source = grp["source"].get<std::string>();
                    const fs::path file = dir / (assetSafe(source) + "_instance.fbx");
                    std::string err;
                    if (!exporter({scene::NodeId(grp["sourceId"].get<uint64_t>())}, file, &err))
                    {
                        grp["error"] = "export failed: " + err;
                        continue;
                    }
                    const json mapping = {{"contentPath", grp["contentPath"]}, {"kind", "static"}};
                    RpcResult imported = client->call("importAsset", importParams(file, mapping, grp["materials"]), importCallOptions());
                    grp["import"] = imported.ok ? imported.result : json{{"error", imported.error}};
                    if (!imported.ok)
                        continue;
                    std::string mesh = grp["objectPath"].get<std::string>();
                    if (json assets = imported.result.value("assets", json::array()); assets.is_array() && !assets.empty() && assets[0].is_string())
                        mesh = assets[0].get<std::string>();
                    RpcResult placed = client->call("placeInstances", {{"group", grp["key"]},
                                                                       {"label", source},
                                                                       {"mesh", mesh},
                                                                       {"mode", grp["mode"]},
                                                                       {"placements", grp["placements"]}});
                    grp["placed"] = placed.ok ? placed.result : json{{"error", placed.error}};
                }
                return plan;
            };
            if (a.get<bool>("wait"))
                return job();
            Link::get().post([job]() mutable {
                json done = job();
                mainq::post([done] {
                    for (const json& grp : done["groups"])
                    {
                        const std::string source = grp.value("source", std::string());
                        if (grp.contains("error"))
                            log::error("Unreal placements of {}: {}", source, grp["error"].get<std::string>());
                        else if (grp.contains("placed") && grp["placed"].contains("error"))
                            log::error("Unreal placements of {}: {}", source, grp["placed"]["error"].get<std::string>());
                        else if (grp.contains("import") && grp["import"].contains("error"))
                            log::error("Unreal import of {}: {}", source, grp["import"]["error"].get<std::string>());
                        else
                            log::info("Unreal: placed {} member(s) of {} as {}", grp["placements"].size(), source, grp["mode"].get<std::string>());
                    }
                });
            });
            return {{"queued", true}, {"groups", plan["groups"].size()}};
        },
        .examples = {"unrealPlaceInstances(\"Bolt\", folder=\"/Game/Kit\")", "unrealPlaceInstances(mode=\"hism\", wait=True)"},
    });

    ops::registerCommand({
        .name = "unrealRemovePlacements",
        .doc = "Remove from Unreal's open level what unrealPlaceInstances placed for the instance groups of the given meshes. "
               "Returns how many actors and instances went.",
        .positionalDoc = "objects",
        .undoable = false,
        .run = [](const Args& a) -> json {
            scene::Graph& g = scene::Graph::current();
            std::vector<scene::NodeId> roots;
            for (const std::string& name : a.objects())
                if (scene::Node* n = g.find(name))
                    roots.push_back(n->id());
            const json plan = placementPlan(g, roots, PlacementOptions{});
            json groups = json::array();
            for (const json& grp : plan["groups"])
                groups.push_back(grp["key"]);
            if (groups.empty())
                throw CommandError("unrealRemovePlacements: select meshes whose placements to remove");
            try
            {
                Link::get().ensureConnected();
            }
            catch (const std::runtime_error& e)
            {
                throw CommandError(std::string("not connected to Unreal: ") + e.what());
            }
            std::shared_ptr<RpcClient> client = Link::get().client();
            if (!client)
                throw CommandError("not connected to Unreal");
            RpcResult r = client->call("removePlacements", {{"groups", groups}});
            if (!r.ok)
                throw CommandError("unrealRemovePlacements: " + r.error);
            return r.result;
        },
    });
}
} // namespace lila::link
