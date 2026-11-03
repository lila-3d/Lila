#include "link/TextureSend.h"

#include "bake/Bake.h"
#include "bake/BakeNodes.h"
#include "bake/BakePipeline.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "core/ImageIO.h"
#include "link/UnrealLink.h"
#include "ops/Command.h"
#include "scene/Graph.h"

#include <algorithm>
#include <filesystem>
#include <map>

namespace lila::link
{
namespace fs = std::filesystem;

namespace
{
const std::map<std::string, std::string, std::less<>>& defaultParameters()
{
    // The names Lila's own generated Unreal materials use, so one of them can be the parent.
    static const std::map<std::string, std::string, std::less<>> p = {
        {"baseColor", "BaseColorMap"}, {"normal", "NormalMap"},     {"orm", "ORMMap"},           {"mro", "MROMap"},
        {"ao", "AmbientOcclusionMap"}, {"roughness", "RoughnessMap"}, {"metallic", "MetallicMap"}, {"emissive", "EmissiveMap"},
        {"height", "HeightMap"},       {"opacity", "OpacityMap"},
    };
    return p;
}

std::string instanceName(const std::string& slot)
{
    return "MI_" + (slot.starts_with("M_") ? slot.substr(2) : slot);
}

scene::Node* findCurrentNode(const std::string& path)
{
    try
    {
        return scene::Graph::current().find(path);
    }
    catch (const std::exception&)
    {
        return nullptr;
    }
}

/** Lila mesh path to the static mesh a mapping sends it as. */
json mappedMeshes()
{
    json out = json::object();
    for (const json& m : mappings())
    {
        if (!m.is_object() || m.value("kind", std::string("static")) != "static")
            continue;
        if (scene::Node* n = findCurrentNode(m.value("node", std::string())))
            out[n->path()] = m.value("contentPath", std::string());
    }
    return out;
}
} // namespace

json textureSettings(std::string_view map, std::string_view colorSpace, std::string_view format, int bits)
{
    const bool hdr = format == "exr" || bits >= 32;
    if (map == "normal")
        return {{"srgb", false}, {"compression", hdr ? "hdr" : "normalmap"}, {"lodGroup", "WorldNormalMap"}};
    if (map == "id" || map == "uvIslands")
        return {{"srgb", true}, {"compression", "uncompressed"}, {"lodGroup", "World"}};
    if (colorSpace == "srgb")
        return {{"srgb", !hdr}, {"compression", hdr ? "hdr" : "default"}, {"lodGroup", "World"}};
    if (hdr)
        return {{"srgb", false}, {"compression", "hdr"}, {"lodGroup", "World"}};
    if (map == "height")
        return {{"srgb", false}, {"compression", "grayscale"}, {"lodGroup", "World"}};
    if (map == "objectNormal" || map == "position" || map == "bentNormal")
        return {{"srgb", false}, {"compression", "uncompressed"}, {"lodGroup", "World"}};
    return {{"srgb", false}, {"compression", "masks"}, {"lodGroup", "World"}};
}

std::string textureAssetName(const std::string& file, int udim)
{
    std::string stem = fs::path(file).stem().string();
    for (const char* style : {"1001", "uv", "uv0"})
    {
        const std::string label = image::udimLabel(udim, style);
        if (stem.size() > label.size() + 1 && stem.ends_with(label) && (stem[stem.size() - label.size() - 1] == '.' || stem[stem.size() - label.size() - 1] == '_'))
        {
            stem.resize(stem.size() - label.size() - 1);
            break;
        }
    }
    std::replace(stem.begin(), stem.end(), '.', '_');
    std::replace(stem.begin(), stem.end(), ' ', '_');
    return stem;
}

json textureImportParams(const json& bakeResult, const json& options, json* warnings)
{
    const std::string folder = options.value("folder", std::string("/Game/Textures"));
    const std::string mode = options.value("materialMode", std::string("none"));
    json params = {{"folder", folder}, {"materialMode", mode}, {"textures", json::array()}, {"materials", json::array()}, {"options", json::object()}};
    for (const char* k : {"save"})
        if (options.contains(k))
            params["options"][k] = options[k];
    if (options.contains("materialParent"))
        params["materialParent"] = options["materialParent"];
    if (options.contains("materialFolder"))
        params["materialFolder"] = options["materialFolder"];

    // UDIM tiles of one texture share its asset name.
    std::map<std::string, size_t> byName;
    struct Use
    {
        std::string map;
        std::string asset;
    };
    std::map<std::string, std::vector<Use>> bySlot;
    std::map<std::string, std::vector<std::string>> slotMeshes;
    std::vector<std::string> slotOrder;
    for (const json& o : bakeResult.value("outputs", json::array()))
    {
        const std::string file = o.value("file", std::string()), map = o.value("map", std::string());
        if (!o.value("layer", std::string()).empty())
        {
            if (warnings)
                warnings->push_back(file + ": a PSD layer (" + map + ") can't be imported as a texture; bake to png, tga, tif or exr");
            continue;
        }
        const int udim = o.value("udim", 1001);
        const std::string name = textureAssetName(file, udim);
        auto it = byName.find(name);
        if (it == byName.end())
        {
            json t = textureSettings(map, o.value("colorSpace", std::string("linear")), o.value("format", std::string()), o.value("bits", 8));
            t["name"] = name;
            t["map"] = map;
            t["files"] = json::array();
            t["flipGreen"] = o.value("normalFormat", std::string()) == "opengl";
            if (options.contains("lodGroup") && options["lodGroup"].is_string())
                t["lodGroup"] = options["lodGroup"];
            if (options.value("virtualTexture", false))
                t["virtualTexture"] = true;
            it = byName.emplace(name, params["textures"].size()).first;
            params["textures"].push_back(std::move(t));
        }
        json& t = params["textures"][it->second];
        t["files"].push_back({{"file", file}, {"udim", udim}});
        // Unreal only takes a UDIM set as a virtual texture.
        if (t["files"].size() > 1)
            t["virtualTexture"] = true;
        for (const json& mat : o.value("materials", json::array()))
        {
            const std::string slot = mat.get<std::string>();
            if (!bySlot.contains(slot))
                slotOrder.push_back(slot);
            std::vector<Use>& uses = bySlot[slot];
            if (std::ranges::none_of(uses, [&](const Use& u) { return u.asset == name; }))
                uses.push_back({map, name});
            for (const json& mesh : o.value("meshes", json::array()))
                if (std::ranges::find(slotMeshes[slot], mesh.get<std::string>()) == slotMeshes[slot].end())
                    slotMeshes[slot].push_back(mesh.get<std::string>());
        }
    }

    if (mode == "instance")
    {
        const json overrides = options.value("parameters", json::object());
        const json meshes = options.value("meshes", json::object());
        for (const std::string& slot : slotOrder)
        {
            json p = json::object();
            for (const Use& u : bySlot[slot])
            {
                std::string param;
                if (overrides.is_object() && overrides.contains(u.map) && overrides[u.map].is_string())
                    param = overrides[u.map].get<std::string>();
                else if (auto it = defaultParameters().find(u.map); it != defaultParameters().end())
                    param = it->second;
                if (!param.empty())
                    p[param] = u.asset;
            }
            json targets = json::array();
            for (const std::string& mesh : slotMeshes[slot])
                if (meshes.is_object() && meshes.contains(mesh) && meshes[mesh].is_string() && !meshes[mesh].get<std::string>().empty())
                    targets.push_back(meshes[mesh]);
            params["materials"].push_back({{"name", instanceName(slot)}, {"slot", slot}, {"parameters", p}, {"meshes", targets}});
        }
    }
    return params;
}

void registerTextureCommands()
{
    using T = ArgType;
    if (ops::find("unrealSendTextures"))
        return;

    ops::registerCommand({
        .name = "unrealSendTextures",
        .doc = "Send a bake project's last textures to Unreal, updating the texture assets in place (sRGB, compression, flip green "
               "and LOD group per map; UDIM sets as virtual textures). materialMode instance also creates or updates a material "
               "instance per material slot from materialParent and sets it on the slot of every mapped mesh (unrealMap). "
               "assign=True first wires the textures into the Lila materials (bakeAssign). Returns {queued} or, with wait=True, "
               "Unreal's {assets, warnings, errors}.",
        .flags = {{"project", "pj", T::String, nullptr, "The bake project; defaults to the scene's only one."},
                  {"folder", "fo", T::String, nullptr, "Content folder for the textures; the unreal pref /textureFolder, else /Game/Textures."},
                  {"materialMode", "mm", T::String, nullptr, "none (textures only) or instance; instance when materialParent or the pref is set.",
                   {"none", "instance"}},
                  {"materialParent", "mp", T::String, nullptr, "Parent material of the instances; the unreal pref /materialParent."},
                  {"materialFolder", "mf", T::String, nullptr, "Where the instances go; the texture folder by default."},
                  {"parameters", "pa", T::Any, nullptr, "Instance texture parameter per map; defaults NormalMap, ORMMap, AmbientOcclusionMap, BaseColorMap, ... as Lila's Unreal materials name them."},
                  {"lodGroup", "lg", T::String, nullptr, "Texture group for every texture instead of the per map default."},
                  {"virtualTexture", "vt", T::Bool, nullptr, "Make every texture a virtual texture (UDIM sets always are)."},
                  {"assign", "as", T::Bool, true, "Run bakeAssign first."},
                  {"save", "sv", T::Bool, nullptr, "Save the packages in Unreal afterwards."},
                  {"wait", "w", T::Bool, false, "Block until Unreal finishes and return its reply."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            scene::Graph& g = scene::Graph::current();
            scene::Node* project = nullptr;
            if (a.has("project"))
                project = findCurrentNode(a.get<std::string>("project"));
            else if (const auto ps = bake::projects(g); ps.size() == 1)
                project = g.node(ps.front());
            if (!project || !bake::isProject(*project))
                throw CommandError("unrealSendTextures: name the bake project with project=");
            json result = bake::lastResult(project->id());
            if (!result.is_object() || !result.contains("outputs"))
                result = bake::readManifest(*project);
            if (!result.is_object() || result.value("outputs", json::array()).empty())
                throw CommandError("unrealSendTextures: " + project->name() + " has no bake to send; bake it first");
            if (a.get<bool>("assign", true))
                ops::execute("bakeAssign", {{"project", project->path()}});

            PrefStore& prefs = PrefStore::get();
            json options = json::object();
            options["folder"] = a.has("folder") ? a.get<std::string>("folder") : prefs.value<std::string>(kUnrealPrefFile, "/textureFolder", "/Game/Textures");
            const std::string parent = a.has("materialParent") ? a.get<std::string>("materialParent") : prefs.value<std::string>(kUnrealPrefFile, "/materialParent", "");
            if (!parent.empty())
                options["materialParent"] = parent;
            options["materialMode"] = a.has("materialMode") ? a.get<std::string>("materialMode") : std::string(parent.empty() ? "none" : "instance");
            if (options["materialMode"] == "instance" && parent.empty())
                throw CommandError("unrealSendTextures: materialMode instance needs materialParent (or the unreal pref /materialParent)");
            for (const char* k : {"materialFolder", "parameters", "lodGroup", "virtualTexture", "save"})
                if (a.has(k))
                    options[k] = a.flags[k];
            options["meshes"] = mappedMeshes();
            json warnings = json::array();
            const json params = textureImportParams(result, options, &warnings);
            for (const json& w : warnings)
                log::warn("unrealSendTextures: {}", w.get<std::string>());
            if (params["textures"].empty())
                throw CommandError("unrealSendTextures: nothing Unreal can import in " + project->name() + "'s bake");

            try
            {
                Link::get().ensureConnected();
            }
            catch (const std::runtime_error& e)
            {
                throw CommandError(std::string("unrealSendTextures: not connected to Unreal: ") + e.what());
            }
            std::shared_ptr<RpcClient> client = Link::get().client();
            if (!client)
                throw CommandError("unrealSendTextures: not connected to Unreal");
            const json features = Link::get().status().value("features", json::array());
            if (!features.is_array() || std::ranges::find(features, json("textures")) == features.end())
                throw CommandError("unrealSendTextures: the LilaLink plugin in this editor is too old for importTextures; rebuild it from Lila's ue/LilaLink");
            if (a.get<bool>("wait"))
            {
                RpcResult r = client->call("importTextures", params, importCallOptions());
                if (!r.ok)
                    throw CommandError("unrealSendTextures: " + r.error);
                return r.result;
            }
            const std::string what = project->name() + " textures";
            client->callOnMain("importTextures", params, importCallOptions(), [what](RpcResult r) {
                if (!r.ok)
                    return log::error("Unreal: sending {} failed: {}", what, r.error);
                for (const json& w : r.result.value("warnings", json::array()))
                    log::warn("Unreal: {}: {}", what, w.dump());
                const json errors = r.result.value("errors", json::array());
                for (const json& e : errors)
                    log::error("Unreal: {}: {}", what, e.dump());
                if (errors.empty())
                    log::info("Unreal: {} -> {}", what, r.result.value("assets", json::array()).dump());
            });
            return {{"queued", true}, {"textures", params["textures"].size()}, {"materials", params["materials"].size()}};
        },
    });
}
} // namespace lila::link
