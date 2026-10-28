#include "ops/Command.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Workspace.h"
#include "link/UnrealLink.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/Rig.h"
#include "scene/Skin.h"
#include "scene/Take.h"

#include <cmath>

namespace lila::link
{
namespace
{
using scene::Graph;
using scene::NodeId;

fs::path resolveFolder(const std::string& s)
{
    fs::path p(s);
    if (p.is_relative())
        p = (Workspace::get().isOpen() ? Workspace::get().root() : fs::current_path()) / p;
    return p.lexically_normal();
}

/** Synchronous importAsset; returns the first asset path. */
std::string importInto(RpcClient& client, const fs::path& file, json mapping, json& log)
{
    RpcResult r = client.call("importAsset", importParams(file, mapping), importCallOptions());
    if (!r.ok)
        throw CommandError("exportMLDeformer: importing " + file.filename().string() + " failed: " + r.error);
    const json errors = r.result.value("errors", json::array());
    if (!errors.empty())
        throw CommandError("exportMLDeformer: importing " + file.filename().string() + ": " + errors.dump());
    for (const json& w : r.result.value("warnings", json::array()))
        log.push_back(w);
    const json assets = r.result.value("assets", json::array());
    if (assets.empty() || !assets[0].is_string())
        throw CommandError("exportMLDeformer: Unreal imported nothing from " + file.filename().string());
    return assets[0].get<std::string>();
}
} // namespace

void registerMlDeformerCommands()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "exportMLDeformer",
        .doc = "Training data for an Unreal ML Deformer: a range-of-motion take (or the given take), the skinned mesh as a skeletal "
               "FBX (its non-linear skinning fitted to linear weights), the take as an animation FBX and the full deformer stack over it "
               "as an Alembic geometry cache. When connected to Unreal it imports all three and creates the ML Deformer asset on them; "
               "training stays a click in the ML Deformer editor. Returns {files, take, frames, assets}.",
        .positionalDoc = "mesh",
        .flags = {{"take", "", T::String, nullptr, "Take to train on; by default a new range-of-motion take is made."},
                  {"poses", "", T::Int, 400, "Random range-of-motion poses (with sweep poses first when sweep is on).", {}, {}, 0.0},
                  {"angle", "", T::Float, 45.0, "Degrees each joint turns either way in the range of motion.", {}, {}, 0.0, 180.0},
                  {"sweep", "", T::Bool, true, "Range of motion starts with one joint, one axis at a time."},
                  {"seed", "", T::Int, 1, "Seed of the random poses."},
                  {"folder", "fo", T::String, nullptr, "Where the files go; relative paths are under the workspace. Default the Unreal cache folder."},
                  {"contentPath", "cp", T::String, nullptr, "Unreal folder for the assets, like /Game/Characters/Hero/MLDeformer."},
                  {"send", "", T::Bool, true, "Import into the connected Unreal editor and create the ML Deformer asset."},
                  {"save", "", T::Bool, true, "Save the Unreal assets afterwards."}},
        .undoable = true,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("exportMLDeformer: give a skinned mesh");
            const NodeId skin = scene::skin::resolve(g, objs.back(), "exportMLDeformer");
            const NodeId mesh = scene::skin::meshOf(g, skin);
            const std::string name = g.node(mesh)->name();
            NodeId root = scene::kNoNode;
            for (const scene::skin::Influence& i : scene::skin::influences(g, skin))
                if (i.joint != scene::kNoNode && root == scene::kNoNode)
                {
                    root = i.joint;
                    while (g.parent(root) != scene::kNoNode && scene::rig::isJoint(*g.node(g.parent(root))))
                        root = g.parent(root);
                }
            if (root == scene::kNoNode)
                throw CommandError("exportMLDeformer: " + name + "'s skin has no joints");

            std::string take;
            int64_t start = 0, end = 0;
            const double fps = std::max(scene::anim::playback().fps, 1e-6);
            if (a.has("take"))
            {
                take = a.get<std::string>("take");
                NodeId t = scene::kNoNode;
                for (NodeId id : scene::anim::takes(g))
                    if (g.node(id)->name() == take)
                        t = id;
                if (t == scene::kNoNode)
                    throw CommandError("exportMLDeformer: no take named " + take);
                scene::anim::setActiveTake(g, t);
                start = int64_t(std::llround(g.node(t)->as<double>(scene::anim::TakeNode::kStart) * fps));
                end = int64_t(std::llround(g.node(t)->as<double>(scene::anim::TakeNode::kEnd) * fps));
            }
            else
            {
                json rom = ops::execute("rangeOfMotion", {{"args", {g.node(mesh)->path()}},
                                                          {"randomPoses", a.get<int>("poses")},
                                                          {"angle", a.get<double>("angle")},
                                                          {"sweep", a.get<bool>("sweep")},
                                                          {"seed", a.get<int>("seed")},
                                                          {"name", name + "_ROM"}});
                take = rom["take"].get<std::string>();
                start = rom["start"].get<int64_t>();
                end = rom["end"].get<int64_t>();
            }

            const fs::path folder = a.has("folder") ? resolveFolder(a.get<std::string>("folder")) : cacheDir() / "mldeformer" / name;
            fs::create_directories(folder);
            const fs::path meshFile = folder / (name + ".fbx"), animFile = folder / (name + "_" + take + ".fbx"),
                           cacheFile = folder / (name + "_" + take + ".abc");
            const std::string meshPath = g.node(mesh)->path(), rootPath = g.node(root)->path();
            ops::execute("exportFbx", {{"args", {meshFile.generic_string(), meshPath, rootPath}}, {"kind", "skeletal"}, {"animation", false}});
            ops::execute("exportFbx", {{"args", {animFile.generic_string(), rootPath}},
                                       {"kind", "animation"},
                                       {"start", double(start)},
                                       {"end", double(end)},
                                       {"take", take}});
            ops::execute("exportAlembic", {{"args", {meshPath}}, {"path", cacheFile.generic_string()}, {"start", double(start)}, {"end", double(end)}});

            json out = {{"files", {meshFile.generic_string(), animFile.generic_string(), cacheFile.generic_string()}},
                        {"take", take},
                        {"frames", end - start + 1},
                        {"assets", json::object()}};
            if (!a.get<bool>("send"))
                return out;
            if (!Link::get().connected())
            {
                log::warn("exportMLDeformer: not connected to Unreal; wrote the training files to {}", folder.generic_string());
                out["sent"] = false;
                return out;
            }
            std::shared_ptr<RpcClient> client = Link::get().client();
            if (!client)
                throw CommandError("exportMLDeformer: not connected to Unreal");
            const std::string cp = a.has("contentPath") ? a.get<std::string>("contentPath") : "/Game/MLDeformer/" + name;
            const bool save = a.get<bool>("save");
            json warnings = json::array();
            const std::string skel = importInto(*client, meshFile, {{"contentPath", cp + "/SK_" + name}, {"kind", "skeletal"}, {"save", save}}, warnings);
            RpcResult info = client->call("assetInfo", {{"path", skel}});
            const std::string skeleton = info.ok ? info.result.value("skeleton", std::string()) : std::string();
            json animMapping = {{"contentPath", cp + "/A_" + name + "_" + take}, {"kind", "animation"}, {"save", save}};
            if (!skeleton.empty())
                animMapping["skeleton"] = skeleton;
            const std::string anim = importInto(*client, animFile, animMapping, warnings);
            const std::string cache = importInto(*client, cacheFile, {{"contentPath", cp + "/GC_" + name + "_" + take}, {"kind", "geometryCache"}, {"save", save}},
                                                 warnings);
            RpcResult made = client->call("createMLDeformer",
                                          {{"path", cp + "/MLD_" + name}, {"skeletalMesh", skel}, {"animSequence", anim}, {"geometryCache", cache}, {"save", save}},
                                          60000);
            if (!made.ok)
                throw CommandError("exportMLDeformer: creating the ML Deformer failed: " + made.error);
            out["assets"] = {{"skeletalMesh", skel}, {"animSequence", anim}, {"geometryCache", cache}, {"mlDeformer", made.result.value("asset", std::string())},
                             {"model", made.result.value("model", std::string())}};
            out["warnings"] = warnings;
            out["sent"] = true;
            return out;
        },
    });
}
} // namespace lila::link
