#include "fbx/Fbx.h"
#include "fbx/GameExport.h"

#include "cmds/Command.h"
#include "core/Log.h"
#include "core/MainQueue.h"
#include "core/Profile.h"
#include "core/Progress.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/Skin.h"

#include <chrono>
#include <future>

namespace lila::fbx
{
namespace
{
using scene::Graph;
using scene::NodeId;

std::vector<std::string> positionalNames(const Args& a, size_t from)
{
    std::vector<std::string> out;
    for (size_t k = from; k < a.positional.size(); ++k)
    {
        const json& v = a.positional[k];
        if (v.is_string())
            out.push_back(v.get<std::string>());
        else if (v.is_array())
            for (const json& x : v)
                if (x.is_string())
                    out.push_back(x.get<std::string>());
    }
    return out;
}

fs::path utf8Path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::vector<std::string> names(const Graph& g, const std::vector<NodeId>& ids)
{
    std::vector<std::string> out;
    for (NodeId id : ids)
        out.push_back(g.node(id)->name());
    return out;
}

/** Skeletal when any of the nodes (or, with children, their descendants) is a skinned mesh, else static. */
ExportKind autoKind(Graph& g, const std::vector<NodeId>& ids, bool children)
{
    for (NodeId id : ids)
    {
        std::vector<NodeId> all{id};
        if (children)
            for (NodeId d : g.descendants(id))
                all.push_back(d);
        for (NodeId n : all)
            if (g.node(n) && g.node(n)->type().isA("mesh") && scene::skin::find(g, n) != scene::kNoNode)
                return ExportKind::Skeletal;
    }
    return ExportKind::Static;
}
} // namespace

bool exportForUnreal(const std::vector<scene::NodeId>& nodes, const fs::path& path, const std::string& profile, std::string* error,
                     const std::string& kind, bool atOrigin, const std::string& take)
{
    LILA_ZONE();

    ExportOptions opts;
    if (!parseProfile(profile.empty() ? "ue5" : profile, &opts.profile) || opts.profile == AxisProfile::Generic)
    {
        if (error)
            *error = "unknown Unreal profile '" + profile + "' (ue5 or ue6)";
        return false;
    }
    if (!parseKind(kind.empty() ? "static" : kind, &opts.kind))
    {
        if (error)
            *error = "unknown kind '" + kind + "' (static, skeletal or animation)";
        return false;
    }
    opts.atOrigin = atOrigin;
    if (!take.empty())
        opts.takes = {take};

    progress::Scope scope("Exporting " + path.filename().string(), 3.0);
    ExportScene snapshot;
    if (mainq::isMainThread())
        snapshot = gatherExport(Graph::current(), nodes, opts);
    else
    {
        auto promise = std::make_shared<std::promise<ExportScene>>();
        std::future<ExportScene> result = promise->get_future();
        mainq::post([promise, nodes, opts] {
            try
            {
                promise->set_value(gatherExport(Graph::current(), nodes, opts));
            }
            catch (...)
            {
                promise->set_exception(std::current_exception());
            }
        });
        if (result.wait_for(std::chrono::seconds(60)) != std::future_status::ready)
        {
            if (error)
                *error = "timed out waiting for the main thread to snapshot the scene";
            return false;
        }
        try
        {
            snapshot = result.get();
        }
        catch (const std::exception& e)
        {
            if (error)
                *error = e.what();
            return false;
        }
    }
    for (const std::string& w : snapshot.warnings)
        mainq::post([w] { log::warn("FBX export: {}", w); });
    scope.step();
    progress::Scope write("Writing " + path.filename().string(), 0.0, progress::None, 2.0);
    return writeFbx(snapshot, path, opts, error);
}

void registerCommands()
{
    using T = ArgType;

    cmds::registerCommand({
        .name = "importFbx",
        .doc = "Import an FBX file: its node hierarchy as transforms, joints and meshes, converted to the scene's axes and "
               "cm. LimbNodes become joints (PreRotation into jointOrient), skins become skinClusters with the file's "
               "weights and bind matrices, blend shapes become a blendShape with a target per channel, and one take is "
               "baked onto the channels. Cameras and lights are reported, not imported. Returns the new node names, or with report=True a dict of nodes, roots, "
               "joints, skinClusters, take, fps, skipped and warnings.",
        .positionalDoc = "path",
        .flags = {{"parent", "p", T::String, nullptr, "Parent the imported roots under this node."},
                  {"skins", "", T::Bool, true, "Rebuild skins as joints and skinClusters with their weights and bind pose."},
                  {"animation", "anim", T::Bool, true, "Bake the take onto translate/rotate/scale, one key per frame."},
                  {"take", "", T::String, "", "AnimStack to import; the file's active one (or first) by default."},
                  {"setPlayback", "", T::Bool, true, "Set the scene fps and playback range from the take."},
                  {"report", "", T::Bool, false, "Return the import report instead of just the names."},
                  {"select", "", T::Bool, true, "Select the imported roots."}},
        .run = [](const Args& a) -> json {
            if (a.positional.empty() || !a.positional[0].is_string())
                throw CommandError("importFbx: path required");
            std::string path = a.positional[0].get<std::string>();
            Graph& g = Graph::current();
            ImportOptions opts;
            if (a.has("parent"))
            {
                scene::Node* p = g.find(a.get<std::string>("parent"));
                if (!p)
                    throw CommandError("importFbx: no node named '" + a.get<std::string>("parent") + "'");
                opts.parent = p->id();
            }
            opts.skins = a.get<bool>("skins", true);
            opts.animation = a.get<bool>("animation", true);
            opts.take = a.get<std::string>("take", "");
            opts.setPlayback = a.get<bool>("setPlayback", true);
            ImportResult r = importFbx(g, utf8Path(path), opts);
            if (!r.ok)
                throw CommandError("importFbx: " + r.error);
            for (const std::string& s : r.skipped)
                log::info("importFbx: skipped {}", s);
            for (const std::string& w : r.warnings)
                log::warn("importFbx: {}", w);
            if (a.get<bool>("select", true))
                g.select(r.roots);
            if (!a.get<bool>("report", false))
                return names(g, r.nodes);
            return {{"nodes", names(g, r.nodes)}, {"roots", names(g, r.roots)}, {"joints", names(g, r.joints)},
                    {"skinClusters", names(g, r.skins)}, {"take", r.take}, {"fps", r.fps}, {"start", r.start}, {"end", r.end},
                    {"curves", r.curves}, {"skipped", r.skipped}, {"warnings", r.warnings}, {"sourceAxes", r.sourceAxes},
                    {"sourceUnitMeters", r.sourceUnitMeters}};
        },
    });

    cmds::registerCommand({
        .name = "exportFbx",
        .doc = "Export the evaluated meshes and transforms to a binary FBX 7.4 file. Objects default to the selection "
               "(or the whole scene with selectionOnly=False). Names go out intact, so UCX_/UBX_/USP_/UCP_ collision and "
               "SOCKET_ empties work in Unreal. kind=skeletal writes joints, skins (bind pose mesh, clusters, BindPose), "
               "blendShape morph targets with their weight curves, and the animation baked over the range; kind=animation writes only the skeleton and its curves. "
               "start/end are frames. Returns the path.",
        .positionalDoc = "path objects...",
        .flags = {{"profile", "", T::String, "ue5", "Axis profile: ue5 (Z up), ue6 (Y up) or generic (the scene's own axes)."},
                  {"kind", "k", T::String, "auto", "auto | static | skeletal | animation. auto is skeletal when a skinned mesh is exported."},
                  {"maxInfluences", "mi", T::Int, 8, "Skin influences per vertex, heaviest kept and renormalised (4 for mobile)."},
                  {"animation", "anim", T::Bool, true, "Write the take for skeletal exports (always for kind=animation)."},
                  {"bakeAnimation", "bake", T::Bool, true, "Sample every frame; False writes plain translate/rotate/scale curves as keys."},
                  {"deformOnly", "do", T::Bool, true, "Skeletal and animation kinds: deform joints only (IK helpers and rig-only joints stay behind)."},
                  {"start", "st", T::Float, nullptr, "First frame; the playback start by default."},
                  {"end", "et", T::Float, nullptr, "Last frame; the playback end by default."},
                  {"take", "", T::String, "", "Take (AnimStack) name; the active take's, else Take 001."},
                  {"takes", "", T::Any, nullptr, "Scene takes to write, one AnimStack each: 'all' or a list of names."},
                  {"triangulate", "", T::Bool, true, "Write triangles exactly as the viewport splits faces."},
                  {"tangents", "", T::Bool, true, "Write MikkTSpace tangents and binormals from uv0."},
                  {"worldSpace", "ws", T::Bool, false, "Bake world transforms into the meshes and flatten the hierarchy."},
                  {"atOrigin", "", T::Bool, false, "Drop the exported roots' own transforms, keeping their children relative."},
                  {"children", "", T::Bool, true, "Include the descendants of the given objects."},
                  {"smoothingGroups", "sg", T::Bool, true, "Write edge smoothing (hard and soft edges)."},
                  {"units", "u", T::String, "cm", "File units: cm, m, mm, in or ft. Lengths are scaled to them.", {"cm", "m", "mm", "in", "ft"}},
                  {"selectionOnly", "sl", T::Bool, true, "Without objects: the selection, or every root when False."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.positional.empty() || !a.positional[0].is_string())
                throw CommandError("exportFbx: path required");
            std::string path = a.positional[0].get<std::string>();
            Graph& g = Graph::current();

            ExportOptions opts;
            std::string profile = a.get<std::string>("profile", "ue5");
            if (!parseProfile(profile, &opts.profile))
                throw CommandError("exportFbx: unknown profile '" + profile + "' (ue5, ue6 or generic)");
            opts.triangulate = a.get<bool>("triangulate", true);
            opts.tangents = a.get<bool>("tangents", true);
            opts.worldSpace = a.get<bool>("worldSpace", false);
            opts.atOrigin = a.get<bool>("atOrigin", false);
            opts.children = a.get<bool>("children", true);
            opts.smoothingGroups = a.get<bool>("smoothingGroups", true);
            opts.unitScale = game::cmPerUnit(a.get<std::string>("units", "cm"));
            if (opts.unitScale <= 0.0)
                throw CommandError("exportFbx: units must be cm, m, mm, in or ft");
            opts.maxInfluences = a.get<int>("maxInfluences", 8);
            opts.animation = a.get<bool>("animation", true);
            opts.bakeAnimation = a.get<bool>("bakeAnimation", true);
            opts.deformOnly = a.get<bool>("deformOnly", true);
            opts.takeName = a.get<std::string>("take", "");
            if (a.has("takes"))
            {
                const json& t = a.flags["takes"];
                if (t.is_string())
                    opts.takes.push_back(t.get<std::string>());
                else if (t.is_array())
                    for (const json& n : t)
                        if (n.is_string())
                            opts.takes.push_back(n.get<std::string>());
            }
            if (a.has("start"))
                opts.start = scene::anim::secondsOf(a.get<double>("start"));
            if (a.has("end"))
                opts.end = scene::anim::secondsOf(a.get<double>("end"));

            std::vector<NodeId> ids;
            std::vector<std::string> objs = positionalNames(a, 1);
            for (const std::string& name : objs)
            {
                scene::Node* n = g.find(name);
                if (!n)
                    throw CommandError("exportFbx: no node named '" + name + "'");
                ids.push_back(n->id());
            }
            if (objs.empty())
                ids = a.get<bool>("selectionOnly", true) ? g.selection() : g.roots();
            std::string kind = a.get<std::string>("kind", "auto");
            if (kind == "auto")
                opts.kind = autoKind(g, ids, opts.children);
            else if (!parseKind(kind, &opts.kind))
                throw CommandError("exportFbx: unknown kind '" + kind + "' (auto, static, skeletal or animation)");

            progress::Scope scope("Exporting " + utf8Path(path).filename().string() + "...", 3.0);
            ExportScene snapshot;
            {
                progress::Scope gather("Gathering the scene");
                snapshot = gatherExport(g, ids, opts);
            }
            for (const std::string& w : snapshot.warnings)
                log::warn("exportFbx: {}", w);
            if (snapshot.nodes.empty())
                throw CommandError("exportFbx: nothing to export");
            std::string err;
            bool ok = false;
            {
                progress::Scope write("Writing " + utf8Path(path).filename().string(), 0.0, progress::None, 2.0);
                progress::runOnWorker([&] { ok = writeFbx(snapshot, utf8Path(path), opts, &err); });
            }
            if (!ok)
                throw CommandError("exportFbx: " + err);
            return path;
        },
    });

    game::registerCommands();
}
} // namespace lila::fbx
