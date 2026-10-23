#include "cmds/Command.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Workspace.h"
#include "link/Exporters.h"
#include "link/UnrealLink.h"
#include "scene/Graph.h"

#include <stdexcept>

namespace lila::link
{
namespace
{
fs::path resolve(const std::string& s)
{
    fs::path p(s);
    if (p.is_relative())
        p = (Workspace::get().isOpen() ? Workspace::get().root() : fs::current_path()) / p;
    return p.lexically_normal();
}

void checkKind(const std::string& kind)
{
    if (kind != "static" && kind != "skeletal" && kind != "animation")
        throw CommandError("kind must be static, skeletal or animation, not '" + kind + "'");
}

/** Flags shared by unrealImport and unrealMap; they become importAsset options. */
std::vector<FlagDef> importFlags()
{
    using T = ArgType;
    return {
        {"contentPath", "cp", T::String, nullptr, "Destination asset, like /Game/Props/SM_Crate."},
        {"kind", "k", T::String, "static", "static | skeletal | animation"},
        {"skeleton", "sk", T::String, nullptr, "Skeleton asset to reuse, like /Game/Chars/SK_Hero_Skeleton."},
        {"importMaterials", "im", T::Bool, nullptr, "Create the materials Unreal does not have yet (default on)."},
        {"materialFolder", "mf", T::String, nullptr,
         "Where materials are looked up first and created: /Game/... or relative to the asset's folder (default Materials)."},
        {"keepMaterials", "km", T::Bool, nullptr, "On reimport, keep materials reassigned in Unreal on slots that still exist (default on)."},
        {"importTextures", "it", T::Bool, nullptr, "Import textures the materials use (default on)."},
        {"combineMeshes", "cm", T::Bool, nullptr, "Merge every mesh into one static mesh (default on)."},
        {"autoCollision", "ac", T::Bool, nullptr, "Generate simple collision when there is no UCX_ mesh (default off)."},
        {"generateLightmapUVs", "glu", T::Bool, nullptr, "Let Unreal build a lightmap UV channel (default off)."},
        {"normals", "nm", T::String, nullptr, "import | compute"},
        {"lodGroup", "lg", T::String, nullptr, "Static mesh LOD group, like LargeProp."},
        {"nanite", "na", T::Bool, nullptr, "Build Nanite data (default off)."},
        {"existing", "ex", T::String, nullptr, "reimport (default: keep the asset and its references) | replace"},
        {"reapplyOptions", "ro", T::Bool, nullptr, "On reimport, apply these options instead of the asset's stored ones."},
        {"save", "sv", T::Bool, nullptr, "Save the imported packages in Unreal afterwards."},
    };
}

json optionFlags(const Args& a)
{
    json m = json::object();
    for (const FlagDef& f : importFlags())
        if (a.has(f.name))
            m[f.name] = a.flags[f.name];
    return m;
}

std::shared_ptr<RpcClient> requireClient()
{
    try
    {
        Link::get().ensureConnected();
    }
    catch (const std::runtime_error& e)
    {
        throw CommandError(std::string("not connected to Unreal: ") + e.what());
    }
    std::shared_ptr<RpcClient> c = Link::get().client();
    if (!c)
        throw CommandError("not connected to Unreal");
    return c;
}
} // namespace

void registerCommands()
{
    using T = ArgType;

    cmds::registerCommand({
        .name = "unrealConnect",
        .doc = "Connect to an Unreal editor running LilaLink. Without flags, uses the first one found through the "
               "projects in the unreal prefs or on the default ports. Returns what the editor reported.",
        .flags = {
            {"project", "p", T::String, nullptr, "Project name or directory to pick among running editors."},
            {"port", "pt", T::Int, nullptr, "Connect to this port directly."},
            {"host", "h", T::String, "127.0.0.1", "Host; LilaLink only listens on localhost."},
            {"timeout", "t", T::Int, 2000, "Connect timeout in milliseconds."},
            {"disconnect", "d", T::Bool, false, "Drop the current connection instead."},
        },
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.get<bool>("disconnect"))
            {
                Link::get().disconnect();
                return Link::get().status();
            }
            Link::ConnectOptions o;
            o.host = a.get<std::string>("host", "127.0.0.1");
            o.project = a.get<std::string>("project", "");
            o.timeoutMs = a.get<int>("timeout", 2000);
            if (a.has("port"))
                o.port = static_cast<uint16_t>(a.get<int>("port"));
            try
            {
                return Link::get().connect(o);
            }
            catch (const std::runtime_error& e)
            {
                throw CommandError(e.what());
            }
        },
    });

    cmds::registerCommand({
        .name = "unrealStatus",
        .doc = "Connection state, the connected editor's hello reply, and with scan=True every editor that can be found.",
        .flags = {
            {"scan", "s", T::Bool, false, "Also list running editors (port files and default ports)."},
        },
        .undoable = false,
        .run = [](const Args& a) -> json {
            json s = Link::get().status();
            s["mappings"] = mappings().size();
            json formats = json::array();
            for (const std::string& f : exporterFormats())
                formats.push_back(f);
            s["exporters"] = formats;
            if (a.get<bool>("scan"))
            {
                json list = json::array();
                for (const Instance& i : discover())
                    list.push_back(toJson(i));
                s["instances"] = list;
            }
            return s;
        },
    });

    std::vector<FlagDef> importCmdFlags = importFlags();
    importCmdFlags.push_back({"file", "f", T::String, nullptr, "File to import; or pass it positionally."});
    importCmdFlags.push_back({"wait", "w", T::Bool, false, "Block until Unreal finishes and return its reply."});
    cmds::registerCommand({
        .name = "unrealImport",
        .doc = "Import (or reimport) a file into the connected Unreal project. Connects first if needed. Returns "
               "{assets, warnings, errors} with wait=True; otherwise queues it and logs the result.",
        .positionalDoc = "file",
        .flags = importCmdFlags,
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::string file = a.get<std::string>("file", "");
            if (file.empty() && !a.positional.empty() && a.positional[0].is_string())
                file = a.positional[0].get<std::string>();
            if (file.empty())
                throw CommandError("unrealImport: give a file");
            if (!a.has("contentPath"))
                throw CommandError("unrealImport: give contentPath, like /Game/Props/SM_Crate");
            checkKind(a.get<std::string>("kind", "static"));

            json params = importParams(resolve(file), optionFlags(a));
            std::shared_ptr<RpcClient> client = requireClient();
            if (a.get<bool>("wait"))
            {
                RpcResult r = client->call("importAsset", params, importCallOptions());
                if (!r.ok)
                    throw CommandError("unrealImport: " + r.error);
                return r.result;
            }
            std::string what = params["contentPath"].get<std::string>();
            client->callOnMain("importAsset", params, importCallOptions(), [what](RpcResult r) {
                if (!r.ok)
                    return log::error("Unreal import of {} failed: {}", what, r.error);
                json errors = r.result.value("errors", json::array());
                for (const json& e : errors)
                    log::error("Unreal: {}: {}", what, e.dump());
                for (const json& w : r.result.value("warnings", json::array()))
                    log::warn("Unreal: {}: {}", what, w.dump());
                if (errors.empty())
                    log::info("Unreal: imported {}", r.result.value("assets", json::array()).dump());
            });
            return {{"queued", true}, {"contentPath", what}};
        },
    });

    cmds::registerCommand({
        .name = "unrealMap",
        .doc = "Map a node to an Unreal asset so saving the scene re-exports and reimports it. Stored in the "
               "workspace (.lila/unreal.json). Replaces any existing mapping for the node. An object set maps its "
               "members as one asset, exported together where they sit.",
        .positionalDoc = "node",
        .flags = importFlags(),
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::vector<std::string> nodes = a.objects();
            if (nodes.size() != 1)
                throw CommandError("unrealMap: give exactly one node");
            if (!scene::Graph::current().find(nodes[0]))
                throw CommandError("no node named '" + nodes[0] + "'");
            if (!a.has("contentPath"))
                throw CommandError("unrealMap: give contentPath, like /Game/Props/SM_Crate");
            checkKind(a.get<std::string>("kind", "static"));

            json m = optionFlags(a);
            m["node"] = nodes[0];
            m["kind"] = a.get<std::string>("kind", "static");
            setMapping(m);
            return m;
        },
    });

    cmds::registerCommand({
        .name = "unrealUnmap",
        .doc = "Remove the Unreal mapping of the given nodes (or the selection). Returns how many were removed.",
        .positionalDoc = "nodes",
        .undoable = false,
        .run = [](const Args& a) -> json {
            int removed = 0;
            for (const std::string& n : a.objects())
                removed += removeMapping(n) ? 1 : 0;
            return removed;
        },
    });

    cmds::registerCommand({
        .name = "unrealMappings",
        .doc = "Every node -> Unreal asset mapping in this workspace.",
        .undoable = false,
        .run = [](const Args&) -> json { return mappings(); },
    });

    cmds::registerCommand({
        .name = "unrealSync",
        .doc = "Export and reimport every mapped node now, as saving does. Returns how many were queued.",
        .undoable = false,
        .run = [](const Args&) -> json { return syncMappings(true); },
    });
}
} // namespace lila::link
