#include "link/UnrealLink.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/ExtensionContext.h"
#include "core/Log.h"
#include "core/MainQueue.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "link/Exporters.h"
#include "math/Matrix.h"
#include "math/Transform.h"
#include "math/Unreal.h"
#include "scene/ArrayGroup.h"
#include "scene/AttrComponents.h"
#include "scene/Graph.h"
#include "scene/Instancing.h"
#include "scene/Light.h"
#include "scene/LightingChannels.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"

#include <algorithm>
#include <array>
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

/** UCX_ and the other collision proxies Unreal reads by name: part of their parent's asset, never placed themselves. */
bool collisionProxy(const std::string& name)
{
    return name.starts_with("UCX_") || name.starts_with("UBX_") || name.starts_with("USP_") || name.starts_with("UCP_");
}

bool placeable(const scene::Node* n) { return n && n->type().isA("mesh") && !n->type().isA("lattice") && !collisionProxy(n->name()); }

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

constexpr const char* kAssetAttr = "unrealAsset";

/**
 * Asset names by instance source: the one a previous send saved on it, else SM_<name>, with the first eight characters of
 * its key appended when another source in the scene has the same name. A name a source saved stays its own (an older
 * node wins a copied one), so adding a second "Wall" later never renames the first one's asset.
 */
class AssetNames
{
public:
    AssetNames(scene::Graph& g, bool assign) : mGraph(g), mAssign(assign)
    {
        for (scene::NodeId id : g.nodes())
        {
            const scene::Node* n = g.node(id);
            if (!placeable(n) || scene::instancing::root(g, id) != id)
                continue;
            ++mBaseCount[base(*n)];
            if (std::string s = stored(*n); !s.empty())
                if (auto [it, fresh] = mOwner.try_emplace(s, id); !fresh && id < it->second)
                    it->second = id;
        }
    }

    std::string operator()(scene::NodeId src, const std::string& key)
    {
        const scene::Node& n = *mGraph.node(src);
        if (std::string s = stored(n); !s.empty() && mOwner.at(s) == src)
            return s;
        const std::string b = base(n);
        std::string name = b;
        if (mBaseCount[b] > 1 || mOwner.contains(b))
            name = b + "_" + assetSafe(key.substr(0, 8));
        while (mOwner.contains(name) && mOwner[name] != src)
            name += "_";
        mOwner[name] = src;
        if (mAssign)
        {
            int i = n.type().attrIndex(kAssetAttr);
            if (i < 0)
                i = mGraph.addAttr(src, scene::AttrDef{.name = kAssetAttr, .type = scene::PlugType::String, .defaultValue = std::string(),
                                                       .flags = scene::AttrHidden, .doc = "The Unreal asset unrealPlaceInstances sends this mesh as."});
            mGraph.setValue({src, i, -1}, name);
        }
        return name;
    }

private:
    static std::string base(const scene::Node& n) { return "SM_" + assetSafe(n.name()); }

    static std::string stored(const scene::Node& n)
    {
        const int i = n.type().attrIndex(kAssetAttr);
        if (i < 0)
            return {};
        const std::string* s = std::get_if<std::string>(&n.value(i));
        return s ? *s : std::string();
    }

    scene::Graph& mGraph;
    bool mAssign;
    std::map<std::string, int> mBaseCount;
    std::map<std::string, scene::NodeId> mOwner;
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
    for (const std::string& extraId : placementExtras().idsInOrder())
        if (const PlacementExtra* extra = placementExtras().find(extraId); extra && *extra)
            if (const json more = (*extra)(g, id); more.is_object())
                for (const auto& [k, v] : more.items())
                    p[k] = v;
    return p;
}

/** Simulated members need actors: instances of an (H)ISM can't each simulate. */
bool simulates(const json& placements)
{
    for (const json& p : placements)
        if (p.contains("physics") && p["physics"].is_object() && p["physics"].value("simulate", false))
            return true;
    return false;
}

/**
 * A light actor's settings: position and the directions it shines down (-Z) and holds up (+Y) in Unreal space, so
 * the actor's X axis points where it shines and a rect light's height runs along its Z.
 */
json lightPlacement(scene::Graph& g, scene::NodeId id, const std::string& key)
{
    namespace lights = scene::lights;
    const glm::dmat4 world = std::get<glm::dmat4>(g.value({id, scene::Transform::kWorldMatrix, -1}));
    auto axis = [&](const glm::dvec4& local) { return vec(math::toUnreal(math::Vector(glm::normalize(glm::dvec3(world * local))))); };
    const lights::Settings s = lights::worldSettings(*g.node(id));
    return {{"id", key},
            {"name", g.node(id)->path()},
            {"kind", lights::kindName(s.kind)},
            {"location", vec(math::toUnreal(math::Vector(glm::dvec3(world[3]))))},
            {"forward", axis({0, 0, -1, 0})},
            {"up", axis({0, 1, 0, 0})},
            {"color", vec(s.color)},
            {"intensity", s.intensity},
            {"units", std::array{"unitless", "candela", "lumens", "lux", "nits", "watts"}.at(size_t(s.units))},
            {"useTemperature", s.useTemperature},
            {"temperature", s.temperature},
            {"range", s.range},
            {"innerConeAngle", s.innerConeAngle},
            {"outerConeAngle", s.outerConeAngle},
            {"width", s.width},
            {"height", s.height},
            {"sourceRadius", s.sourceRadius},
            {"castShadows", s.castShadows}};
}

json channelBits(const std::array<bool, 3>& b) { return json::array({b[0], b[1], b[2]}); }

/** Says in the log which lighting channels a send carries, so one nobody meant to send is easy to spot. */
void logChannels(const json& report)
{
    if (report["lights"].empty() && report["objects"].empty())
        return;
    std::string list;
    for (const json& c : report["channels"])
        list += (list.empty() ? "" : ", ") + c.get<std::string>();
    log::info("unrealPlaceInstances: lighting channels {} on {} light(s) and {} object(s) {}", list.empty() ? "default" : list,
              report["lights"].size(), report["objects"].size(), report["written"].get<bool>() ? "sent" : "left out (lightingChannels=False)");
    if (report.contains("note"))
        log::warn("unrealPlaceInstances: {}", report["note"].get<std::string>());
}

bool hasFeature(const char* feature)
{
    const json features = Link::get().status().value("features", json::array());
    return features.is_array() && std::ranges::find(features, json(feature)) != features.end();
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

Registry<PlacementExtra>& placementExtras()
{
    static Registry<PlacementExtra> r;
    return r;
}

bool addPlacementExtra(ext::Context& ctx, const std::string& id, PlacementExtra extra)
{
    if (placementExtras().contains(id))
    {
        log::error("extension {}: placement extra '{}' is already registered", ctx.owner(), id);
        return false;
    }
    ctx.add(placementExtras(), "placementExtra", id, std::move(extra));
    return true;
}

json placementPlan(scene::Graph& g, const std::vector<scene::NodeId>& roots, const PlacementOptions& o)
{
    LILA_ZONE();

    if (o.mode != "auto" && o.mode != "actors" && o.mode != "ism" && o.mode != "hism")
        throw CommandError("unrealPlaceInstances: mode is auto, actors, ism or hism, not '" + o.mode + "'");
    if (o.collision != "none" && o.collision != "box" && o.collision != "convex" && o.collision != "complexAsSimple" && o.collision != "ucx")
        throw CommandError("unrealPlaceInstances: collision is none, box, convex, complexAsSimple or ucx, not '" + o.collision + "'");
    g.evaluate();
    // Every mesh under the roots counts with its whole instance group: Unreal drops a group's placements Lila no longer sends.
    std::vector<scene::NodeId> sources, lightNodes;
    auto touch = [&](scene::NodeId id) {
        if (const scene::Node* n = g.node(id); n && n->type().isA("light"))
        {
            if (std::ranges::find(lightNodes, id) == lightNodes.end() && scene::effectivelyVisible(g, id))
                lightNodes.push_back(id);
            return;
        }
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
    AssetNames assetNames(g, o.assignIds);
    std::vector<std::vector<scene::NodeId>> placedOf;
    std::vector<scene::NodeId> carried = lightNodes;
    for (scene::NodeId src : sources)
    {
        std::vector<scene::NodeId>& placed = placedOf.emplace_back(std::vector<scene::NodeId>{src});
        for (scene::NodeId m : scene::instancing::members(g, src))
            if (m != src && scene::instancing::root(g, m) == src)
                placed.push_back(m);
        carried.insert(carried.end(), placed.begin(), placed.end());
    }
    namespace lc = scene::lightingChannels;
    const lc::Usage use = lc::usage(g, carried);
    const lc::UnrealChannels unreal = lc::unrealChannels(use);
    std::string note;
    if (o.lightingChannels && !unreal.dropped.empty())
    {
        for (const std::string& d : unreal.dropped)
            note += (note.empty() ? "" : ", ") + d;
        note = "Unreal has three lighting channels: " + note + " dropped; lights and objects left in none of the three go on channel 0";
    }
    json groups = json::array();
    for (size_t k = 0; k < sources.size(); ++k)
    {
        const scene::NodeId src = sources[k];
        const std::vector<scene::NodeId>& placed = placedOf[k];
        const scene::Node* n = g.node(src);
        const std::string groupKey = keys(src);
        const std::string asset = assetNames(src, groupKey);
        json list = json::array();
        for (scene::NodeId id : placed)
        {
            // Hidden members stay out of the level: a library asset's master only supplies the mesh.
            if (!scene::effectivelyVisible(g, id))
                continue;
            const std::string key = keys(id);
            const size_t first = list.size();
            list.push_back(placement(g, id, key));
            for (json& copy : arrayPlacements(g, id, key))
                list.push_back(std::move(copy));
            const json components = scene::attrComponents::unrealPayload(g, id);
            for (size_t p = first; p < list.size(); ++p)
            {
                list[p]["components"] = components;
                // Array copies are the member again, so they carry what extras gave it.
                if (p > first)
                    for (const auto& [field, value] : list[first].items())
                        if (!list[p].contains(field))
                            list[p][field] = value;
                if (o.lightingChannels)
                    list[p]["lightingChannels"] = channelBits(lc::unrealBits(*g.node(id), unreal));
            }
        }
        const std::string mode = o.mode != "auto" ? o.mode : int(list.size()) > o.threshold && !simulates(list) ? "hism" : "actors";
        groups.push_back({{"key", groupKey},
                          {"source", n->fullPath()},
                          {"sourceId", src},
                          {"contentPath", folder + "/" + asset},
                          {"objectPath", folder + "/" + asset + "." + asset},
                          {"mode", mode},
                          {"collision", o.collision},
                          {"nanite", o.nanite},
                          {"placements", std::move(list)}});
    }
    json lightList = json::array();
    json skipped = json::array();
    for (scene::NodeId id : lightNodes)
    {
        if (scene::lights::settings(*g.node(id)).kind == scene::lights::Kind::Dome)
        {
            skipped.push_back(g.node(id)->path() + ": dome lights have no Unreal light actor here (use a Sky Light)");
            continue;
        }
        json l = lightPlacement(g, id, keys(id));
        if (o.lightingChannels)
            l["lightingChannels"] = channelBits(lc::unrealBits(*g.node(id), unreal));
        lightList.push_back(std::move(l));
    }
    return {{"groups", std::move(groups)},
            {"lights", std::move(lightList)},
            {"skipped", std::move(skipped)},
            {"lightingChannels", lc::report(g, use, o.lightingChannels && !use.empty(), note, &unreal)}};
}

void registerPlacementCommands()
{
    using T = ArgType;

    ops::registerCommand({
        .name = "unrealPlaceInstances",
        .doc = "Send meshes to Unreal as one static mesh asset per instance source, placed in the open level: an actor per "
               "member referencing that asset, or one instanced static mesh component (ISM or HISM) for a large group. "
               "Lights among them go as light actors (point, spot, directional, rect; discs as square rect lights, domes skipped). "
               "Lighting channels map onto Unreal's three: default is channel 0, the next two channels the sent nodes use (in "
               "scene order) 1 and 2; any more are dropped, with a warning naming them, and what is left in none goes on 0. "
               "Placements are keyed by each node's linkId, given on the first send and saved with the scene, so sending "
               "a group again moves, adds and removes its actors to match, also after a rename or reopening the scene. "
               "Returns the plan, with each group's import and placement results when wait=True.",
        .positionalDoc = "objects",
        .flags = {{"folder", "f", T::String, "/Game/Lila/Instances", "Content folder the assets go in; each is SM_<source name>, with part of the source's link id appended when another mesh in the scene has the same name. The name is saved on the source, so it never changes."},
                  {"mode", "m", T::String, "auto",
                   "How members are placed: actors, ism, hism, or auto (actors up to threshold members, then hism).",
                   {"auto", "actors", "ism", "hism"}},
                  {"threshold", "t", T::Int, nullptr, "auto switches to hism above this many members (default the unreal pref /instanceThreshold, 50).", {}, {}, 0.0},
                  {"lightingChannels", "lc", T::Bool, true,
                   "Set the actors' and lights' Lighting Channels from Lila's channels. The plan's lightingChannels lists the "
                   "channels, lights and objects, the Unreal slot of each channel and any dropped."},
                  {"collision", "col", T::String, "complexAsSimple",
                   "Collision the assets get: none (the placed actors and instances don't collide either), box, convex (Unreal's "
                   "auto convex decomposition), complexAsSimple (the render mesh) or ucx (the source's UCX_ child meshes, sent "
                   "with it; its other children go into the asset too).",
                   {"none", "box", "convex", "complexAsSimple", "ucx"}},
                  {"nanite", "na", T::Bool, false, "Build Nanite data for the assets."},
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
            o.lightingChannels = a.get<bool>("lightingChannels", true);
            o.collision = a.get<std::string>("collision", o.collision);
            o.nanite = a.get<bool>("nanite", false);
            json plan;
            {
                UndoChunk chunk("Unreal Link Ids");
                plan = placementPlan(g, roots, o);
            }
            if (plan["groups"].empty() && plan["lights"].empty())
                throw CommandError("unrealPlaceInstances: select meshes or lights to send");
            logChannels(plan["lightingChannels"]);
            if (a.get<bool>("dryRun"))
                return plan;

            // ucx sends the source with its children, so its UCX_ meshes reach the import.
            ExportFn withChildren = findExporter("fbx");
            ExportFn exporter = o.collision == "ucx" ? withChildren : findExporter("fbx:instanceSource");
            if (!exporter)
                exporter = withChildren;
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
            if (!plan["lights"].empty() && !hasFeature("lights"))
            {
                log::warn("unrealPlaceInstances: the LilaLink plugin predates lights; rebuild it from Lila's ue/LilaLink to send them");
                plan["lights"] = json::array();
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
                    const json mapping = {{"contentPath", grp["contentPath"]}, {"kind", "static"},       {"collision", grp["collision"]},
                                          {"nanite", grp["nanite"]},           {"reapplyOptions", true}};
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
                                                                       {"collision", grp["collision"]},
                                                                       {"placements", grp["placements"]}});
                    grp["placed"] = placed.ok ? placed.result : json{{"error", placed.error}};
                }
                if (!plan["lights"].empty())
                {
                    RpcResult placed = client->call("placeLights", {{"lights", plan["lights"]}});
                    plan["lightsPlaced"] = placed.ok ? placed.result : json{{"error", placed.error}};
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
                        if (grp.contains("placed"))
                            for (const json& w : grp["placed"].value("warnings", json::array()))
                                log::warn("Unreal placements of {}: {}", source, w.is_string() ? w.get<std::string>() : w.dump());
                    }
                    if (const json lit = done.value("lightsPlaced", json()); lit.contains("error"))
                        log::error("Unreal lights: {}", lit["error"].get<std::string>());
                    else if (!lit.is_null())
                        log::info("Unreal: placed {} light(s)", done["lights"].size());
                });
            });
            return {{"queued", true}, {"groups", plan["groups"].size()}, {"lights", plan["lights"].size()},
                    {"lightingChannels", plan["lightingChannels"]}};
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
            json groups = json::array(), ids = json::array();
            for (const json& grp : plan["groups"])
                groups.push_back(grp["key"]);
            for (const json& l : plan["lights"])
                ids.push_back(l["id"]);
            if (groups.empty() && ids.empty())
                throw CommandError("unrealRemovePlacements: select meshes or lights whose placements to remove");
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
            RpcResult r = client->call("removePlacements", {{"groups", groups}, {"ids", ids}});
            if (!r.ok)
                throw CommandError("unrealRemovePlacements: " + r.error);
            return r.result;
        },
    });
}
} // namespace lila::link
