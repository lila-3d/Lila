#include "retarget/Bvh.h"
#include "retarget/SceneRetarget.h"

#include "core/Profile.h"
#include "fbx/Fbx.h"
#include "io/FileTranslators.h"
#include "ops/Command.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/Rig.h"
#include "scene/Take.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace lila::retarget
{
using namespace scene;
namespace fs = std::filesystem;

namespace
{
Graph& graph() { return Graph::current(); }

template <class F> auto guarded(const char* cmd, F&& f)
{
    try
    {
        return f();
    }
    catch (const CommandError&)
    {
        throw;
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string(cmd) + ": " + e.what());
    }
}

Node& named(const std::string& name, const char* cmd)
{
    Node* n = graph().find(name);
    if (!n)
        throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
    return *n;
}

std::string positional(const Args& a, const char* cmd, const char* what)
{
    if (a.positional.size() != 1 || !a.positional[0].is_string())
        throw CommandError(std::string(cmd) + ": name " + what);
    return a.positional[0].get<std::string>();
}

NodeId topOf(Graph& g, const std::string& name, const char* cmd)
{
    Node& n = named(name, cmd);
    if (!rig::isJoint(n))
        throw CommandError(std::string(cmd) + ": " + name + " is not a joint");
    return guarded(cmd, [&] { return sceneSkeleton(g, n.id()).top; });
}

/** A rig node from its own name or any joint of its skeleton. */
NodeId rigNode(Graph& g, const std::string& name, const char* cmd)
{
    Node& n = named(name, cmd);
    if (n.type().name == kRigType)
        return n.id();
    const NodeId id = rigNodeFor(g, topOf(g, name, cmd));
    if (id == kNoNode)
        throw CommandError(std::string(cmd) + ": " + name + "'s skeleton has no retarget rig; run characterize first");
    return id;
}

NodeId retargeterNode(Graph&, const std::string& name, const char* cmd)
{
    Node& n = named(name, cmd);
    if (n.type().name != kRetargeterType)
        throw CommandError(std::string(cmd) + ": " + name + " is not a retargeter");
    return n.id();
}

json rigSummary(Graph& g, NodeId rn)
{
    json j = rigOf(g, rn).toJson();
    j.erase("poseRotation");
    j.erase("poseTranslate");
    j["rig"] = g.path(rn);
    return j;
}

/** Applies a partial JSON object onto settings (RFC 7386 merge), keeping the rest. */
Settings patched(const Settings& s, const json& patch)
{
    json j = s.toJson();
    j.merge_patch(patch);
    return Settings::fromJson(j);
}

json runCharacterize(const Args& a)
{
    LILA_ZONE();
    Graph& g = graph();
    const std::string joint = positional(a, "characterize", "a joint of the skeleton");
    const NodeId top = topOf(g, joint, "characterize");
    const NodeId rn = guarded("characterize", [&] { return characterizeSkeleton(g, top, a.get<std::string>("name", "")); });
    return rigSummary(g, rn);
}

json runEditRig(const Args& a)
{
    Graph& g = graph();
    const NodeId rn = rigNode(g, positional(a, "editRetargetRig", "a retarget rig or a joint"), "editRetargetRig");
    if (a.get<bool>("query", false))
    {
        json j = rigOf(g, rn).toJson();
        j["rig"] = g.path(rn);
        return j;
    }
    return guarded("editRetargetRig", [&] {
        Rig r = a.has("definition") ? Rig::fromJson(a.flags["definition"]) : rigOf(g, rn);
        if (a.has("pelvis"))
            r.pelvis = a.get<std::string>("pelvis");
        if (a.has("ground"))
            r.ground = a.get<double>("ground");
        if (a.has("removeChain"))
        {
            const std::string name = a.get<std::string>("removeChain");
            std::erase_if(r.chains, [&](const Chain& c) { return c.name == name; });
        }
        if (a.has("chain"))
        {
            const json& c = a.flags["chain"];
            if (!c.is_object() || !c.contains("name"))
                throw CommandError("editRetargetRig: chain is {'name': ..., 'role', 'side', 'index', 'start', 'end', 'goal'}");
            Chain* have = r.chain(c["name"].get<std::string>());
            json merged = json::object();
            if (have)
                merged = Rig{.chains = {*have}}.toJson()["chains"][0];
            merged.merge_patch(c);
            if (!merged.contains("start"))
                throw CommandError("editRetargetRig: a new chain needs a start joint");
            const Chain made = Rig::fromJson(json{{"chains", json::array({merged})}}).chains.front();
            if (have)
                *have = made;
            else
                r.chains.push_back(made);
        }
        if (a.get<bool>("capturePose", false))
        {
            const SceneSkeleton s = sceneSkeleton(g, rigSkeletonTop(g, rn));
            r.setRetargetPose(s.skeleton, currentPose(g, s));
        }
        setRig(g, rn, r);
        return rigSummary(g, rn);
    });
}

json runCreateRetargeter(const Args& a)
{
    Graph& g = graph();
    if (!a.has("source") || !a.has("target"))
        throw CommandError("retargeter: give source and target (a joint of each skeleton)");
    const NodeId s = topOf(g, a.get<std::string>("source"), "retargeter"), t = topOf(g, a.get<std::string>("target"), "retargeter");
    const NodeId r =
        guarded("retargeter", [&] { return createRetargeter(g, s, t, a.get<std::string>("name", ""), a.get<bool>("align", true)); });
    return {{"retargeter", g.path(r)}, {"settings", settingsOf(g, r).toJson()}};
}

json runEditRetargeter(const Args& a)
{
    Graph& g = graph();
    const NodeId r = retargeterNode(g, positional(a, "editRetargeter", "a retargeter"), "editRetargeter");
    if (a.get<bool>("query", false))
        return settingsOf(g, r).toJson();
    return guarded("editRetargeter", [&] {
        Settings s = settingsOf(g, r);
        if (a.get<bool>("autoMap", false))
            s = autoMap(pairedRig(g, r, false), pairedRig(g, r, true));
        if (a.has("settings"))
            s = Settings::fromJson(a.flags["settings"]);
        json patch = json::object();
        for (const char* key : {"root", "warp", "pins", "contacts", "curves", "scale", "enableRoot", "enableFk", "enableIk", "copyCurves", "fullBody"})
            if (a.has(key))
                patch[key] = a.flags[key];
        if (!patch.empty())
            s = patched(s, patch);
        if (a.has("chain"))
        {
            const json& c = a.flags["chain"];
            if (!c.is_object() || !c.contains("target"))
                throw CommandError("editRetargeter: chain is {'target': chain name, 'source', 'fk': {...}, 'ik': {...}, 'plant': {...}}");
            json all = s.toJson();
            bool found = false;
            for (json& m : all["chains"])
                if (m["target"] == c["target"])
                {
                    m.merge_patch(c);
                    found = true;
                }
            if (!found)
            {
                if (!pairedRig(g, r, true).chain(c["target"].get<std::string>()))
                    throw CommandError("editRetargeter: the target rig has no chain " + c["target"].get<std::string>());
                all["chains"].push_back(c);
            }
            s = Settings::fromJson(all);
        }
        setSettings(g, r, s);
        return s.toJson();
    });
}

json runAlign(const Args& a)
{
    Graph& g = graph();
    const NodeId r = retargeterNode(g, positional(a, "alignRetargetPose", "a retargeter"), "alignRetargetPose");
    return guarded("alignRetargetPose", [&] {
        if (a.get<bool>("reset", false))
        {
            g.setValue({r, g.node(r)->type().attrIndex("sourcePose")}, std::string());
            g.setValue({r, g.node(r)->type().attrIndex("targetPose")}, std::string());
        }
        for (bool target : {false, true})
            if (a.get<bool>(target ? "captureTarget" : "captureSource", false))
            {
                const SceneSkeleton s = sceneSkeleton(g, target ? targetTopOf(g, r) : sourceTopOf(g, r));
                setPairedPose(g, r, target, s.skeleton, currentPose(g, s));
            }
        if (a.get<bool>("align", true))
            alignPairedPose(g, r);
        return json{{"retargeter", g.path(r)}};
    });
}

/** The skeleton a file import made: the top joint of its joints. */
NodeId importedTop(Graph& g, const std::vector<NodeId>& joints)
{
    for (NodeId j : joints)
        if (g.node(j) && rig::isJoint(*g.node(j)))
            return sceneSkeleton(g, j).top;
    return kNoNode;
}

struct Imported
{
    NodeId top = kNoNode;
    std::vector<NodeId> nodes; // everything the import made, to delete afterwards
    double start = 0.0, end = 0.0, fps = 0.0;
};

Imported importSource(Graph& g, const fs::path& path, double scale)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) { return char(std::tolower(static_cast<unsigned char>(c))); });
    Imported out;
    if (ext == ".bvh")
    {
        const bvh::ImportResult r = bvh::import(g, path, {.scale = scale, .setPlayback = false});
        out.top = r.root;
        out.nodes = r.joints;
        out.end = r.end;
        out.fps = r.fps;
        return out;
    }
    if (ext == ".fbx")
    {
        const fbx::ImportResult r = fbx::importFbx(g, path, {.setPlayback = false});
        if (!r.ok)
            throw std::invalid_argument(path.filename().string() + ": " + r.error);
        out.top = importedTop(g, r.joints);
        out.nodes = r.nodes;
        out.start = r.start;
        out.end = r.end;
        out.fps = r.fps;
        return out;
    }
    throw std::invalid_argument(path.filename().string() + ": retargeting reads .bvh and .fbx files");
}

void deleteImported(Graph& g, const Imported& im)
{
    // Rigs and retargeters made for the imported skeleton go with it.
    std::vector<NodeId> extra;
    for (NodeId id : g.nodes())
    {
        const Node* n = g.node(id);
        if (n && (n->type().name == kRigType ? rigNodeFor(g, im.top) == id : n->type().name == kRetargeterType && sourceTopOf(g, id) == im.top))
            extra.push_back(id);
    }
    // And the curves animating it.
    for (NodeId id : im.nodes)
        if (g.node(id))
            for (const anim::AnimatedPlug& ap : anim::animatedPlugs(g, id))
                if (std::find(extra.begin(), extra.end(), ap.curve) == extra.end())
                    extra.push_back(ap.curve);
    for (NodeId id : extra)
        if (g.node(id))
            g.deleteNode(id);
    for (auto it = im.nodes.rbegin(); it != im.nodes.rend(); ++it)
        if (g.node(*it))
            g.deleteNode(*it);
}

RunOptions runOptions(const Args& a)
{
    RunOptions o;
    if (a.has("start"))
        o.start = a.get<double>("start");
    if (a.has("end"))
        o.end = a.get<double>("end");
    o.fps = a.get<double>("fps", 0.0);
    o.take = a.get<std::string>("take", "");
    o.lockChains = a.get<bool>("lockChains", true);
    o.mode = a.get<std::string>("mode", "auto");
    o.gait = a.get<std::string>("gait", "auto");
    return o;
}

/** The active take's name for the skeleton's top joint, else empty. */
std::string activeTakeName(Graph& g, NodeId top)
{
    for (NodeId t : anim::activeTakes(g))
        if (const std::optional<PlugRef> src = g.source({t, anim::TakeNode::kRoot, -1}); src && src->node == top)
            return g.node(t)->name();
    return {};
}

json runRetarget(const Args& a)
{
    LILA_ZONE();
    Graph& g = graph();
    if (!a.has("target") && !a.has("retargeter"))
        throw CommandError("retarget: give the target (a joint of its skeleton), or a retargeter");
    NodeId template_ = a.has("retargeter") ? retargeterNode(g, a.get<std::string>("retargeter"), "retarget") : kNoNode;
    const NodeId target = a.has("target") ? topOf(g, a.get<std::string>("target"), "retarget") : targetTopOf(g, template_);

    std::vector<fs::path> files;
    for (const json& f : a.has("files") ? a.flags["files"] : json::array())
        files.push_back(fs::path(f.get<std::string>()));
    if (a.has("folder"))
    {
        const fs::path dir = a.get<std::string>("folder");
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir, ec))
        {
            std::string ext = e.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) { return char(std::tolower(static_cast<unsigned char>(c))); });
            if (e.is_regular_file() && (ext == ".bvh" || ext == ".fbx"))
                files.push_back(e.path());
        }
        if (ec)
            throw CommandError("retarget: can't list " + dir.string() + ": " + ec.message());
        std::sort(files.begin(), files.end());
    }

    if (files.empty())
    {
        NodeId r = template_;
        if (a.has("source"))
        {
            const NodeId source = topOf(g, a.get<std::string>("source"), "retarget");
            r = retargeterFor(g, source, target);
            if (r == kNoNode)
                r = guarded("retarget", [&] { return createRetargeter(g, source, target); });
        }
        if (r == kNoNode)
            throw CommandError("retarget: give the source (a joint of its skeleton), files, a folder or a retargeter");
        RunOptions o = runOptions(a);
        if (!a.has("take"))
        {
            o.take = activeTakeName(g, sourceTopOf(g, r));
            if (o.take.empty())
                o.take = g.node(sourceTopOf(g, r))->name() + "_retarget";
        }
        RunReport rep = guarded("retarget", [&] { return run(g, r, o); });
        json out = rep.toJson();
        out["retargeter"] = g.path(r);
        return out;
    }

    // Files: each is imported, retargeted into a take named after it, then removed.
    const double scale = a.get<double>("scale", 1.0);
    const bool keep = a.get<bool>("keepSources", false);
    json reports = json::array();
    for (const fs::path& file : files)
    {
        json one = {{"file", file.string()}};
        try
        {
            const Imported im = importSource(g, file, scale);
            if (im.top == kNoNode)
                throw std::invalid_argument("no skeleton in the file");
            const NodeId r = createRetargeter(g, im.top, target, {}, template_ == kNoNode);
            if (template_ != kNoNode)
            {
                setSettings(g, r, settingsOf(g, template_));
                for (bool side : {false, true})
                {
                    const int attr = g.node(template_)->type().attrIndex(side ? "targetPose" : "sourcePose");
                    g.setValue({r, attr}, g.node(template_)->as<std::string>(attr));
                }
            }
            RunOptions o = runOptions(a);
            o.take = file.stem().string();
            if (!o.start && im.end > im.start)
                o.start = im.start, o.end = im.end;
            if (o.fps <= 0.0 && im.fps > 0.0)
                o.fps = im.fps;
            one.update(run(g, r, o).toJson());
            if (!keep)
                deleteImported(g, im);
        }
        catch (const std::exception& e)
        {
            one["error"] = e.what();
        }
        reports.push_back(std::move(one));
    }
    return {{"files", reports}};
}

json runSaveClip(const Args& a)
{
    Graph& g = graph();
    const std::string path = positional(a, "saveRetargetClip", "the file to write");
    if (!a.has("source"))
        throw CommandError("saveRetargetClip: give the source (a joint of its skeleton)");
    const NodeId source = topOf(g, a.get<std::string>("source"), "saveRetargetClip");
    const semantic::Clip clip = guarded("saveRetargetClip", [&] { return encodeSkeleton(g, source, runOptions(a)); });
    std::ofstream out(fs::path(path), std::ios::binary);
    if (!out)
        throw CommandError("saveRetargetClip: can't write " + path);
    out << clip.toJson().dump();
    return {{"file", path}, {"frames", clip.frames}, {"fps", clip.fps}, {"limbs", clip.limbs.size()}};
}

json runApplyClip(const Args& a)
{
    Graph& g = graph();
    const std::string path = positional(a, "applyRetargetClip", "a clip file");
    if (!a.has("target"))
        throw CommandError("applyRetargetClip: give the target (a joint of its skeleton)");
    const NodeId target = topOf(g, a.get<std::string>("target"), "applyRetargetClip");
    std::ifstream in(fs::path(path), std::ios::binary);
    if (!in)
        throw CommandError("applyRetargetClip: can't read " + path);
    const json j = json::parse(in, nullptr, false);
    if (j.is_discarded())
        throw CommandError("applyRetargetClip: " + path + " is not JSON");
    RunOptions o = runOptions(a);
    if (!a.has("take"))
        o.take = fs::path(path).stem().string();
    return guarded("applyRetargetClip", [&] { return applySemantic(g, semantic::Clip::fromJson(j), target, o).toJson(); });
}

json runImportBvh(const Args& a)
{
    Graph& g = graph();
    const std::string path = positional(a, "importBvh", "a .bvh file");
    bvh::ImportOptions o;
    o.scale = a.get<double>("scale", 1.0);
    o.setPlayback = a.get<bool>("setPlayback", true);
    o.endSites = a.get<bool>("endSites", true);
    if (a.has("parent"))
        o.parent = named(a.get<std::string>("parent"), "importBvh").id();
    const bvh::ImportResult r = guarded("importBvh", [&] { return bvh::import(g, path, o); });
    json joints = json::array();
    for (NodeId j : r.joints)
        joints.push_back(g.path(j));
    return {{"root", g.path(r.root)}, {"joints", joints}, {"frames", r.frames}, {"fps", r.fps}, {"start", r.start}, {"end", r.end}};
}
} // namespace

void registerCommands()
{
    if (ops::find("retarget"))
        return;
    registerTypes();
    using T = ArgType;
    ops::registerCommand({
        .name = "characterize",
        .doc = "Find what each part of a skeleton is for retargeting, as it is posed now: the pelvis, and chains (spine, neck, head, "
               "arms, legs, toes, fingers, tails, wings) with their side and number. Works from joint names (UE, Mixamo, Maya, "
               "BVH and others) and from shape, on any number of legs. Makes or replaces the skeleton's retargetRig node; the "
               "current pose becomes its retarget pose. Returns the chains and anything doubtful under 'notes'.",
        .positionalDoc = "a joint of the skeleton",
        .flags = {{"name", "n", T::String, nullptr, "Name of a new retargetRig node."}},
        .run = runCharacterize,
        .examples = {"characterize('pelvis')"},
    });
    ops::registerCommand({
        .name = "editRetargetRig",
        .doc = "Edit or query a skeleton's retarget rig: its chains, pelvis, ground height and retarget pose.",
        .positionalDoc = "the retargetRig node, or a joint of its skeleton",
        .flags = {
            {"query", "q", T::Bool, false, "Return the whole rig, retarget pose included."},
            {"definition", "d", T::Any, nullptr, "Replace the rig with this JSON (as query returns it)."},
            {"chain", "c", T::Any, nullptr,
             "Add a chain or change one by name: {'name', 'role' (spine, neck, head, jaw, clavicle, arm, leg, toe, finger, tail, wing, "
             "antenna, root, other), 'side' (center, left, right), 'index', 'start', 'end', 'goal'}."},
            {"removeChain", "rc", T::String, nullptr, "Chain to remove."},
            {"pelvis", "p", T::String, nullptr, "The retarget root joint."},
            {"ground", "g", T::Float, nullptr, "Height of the floor along up, cm."},
            {"capturePose", "cp", T::Bool, false, "The skeleton's current pose becomes the retarget pose."},
        },
        .run = runEditRig,
        .examples = {"editRetargetRig('pelvis', chain={'name': 'Tail', 'role': 'tail', 'start': 'tail_01', 'end': 'tail_08'})"},
    });
    ops::registerCommand({
        .name = "retargeter",
        .doc = "Make a retargeter from one skeleton to another: characterises either when it has no rig yet, maps chains by name, "
               "else role, side and number, and aligns the target's retarget pose to the source's.",
        .flags = {
            {"source", "s", T::String, nullptr, "A joint of the source skeleton."},
            {"target", "t", T::String, nullptr, "A joint of the target skeleton."},
            {"name", "n", T::String, nullptr, "Name of the retargeter node."},
            {"align", "a", T::Bool, true, "Align the target's retarget pose to the source's."},
        },
        .run = runCreateRetargeter,
        .examples = {"retargeter(source='Hips', target='pelvis')"},
    });
    ops::registerCommand({
        .name = "editRetargeter",
        .doc = "Edit or query a retargeter's settings, which mirror Unreal's IK Retargeter. Objects are merged into what is there "
               "(set a key to null to reset it). Returns the settings.",
        .positionalDoc = "the retargeter",
        .flags = {
            {"query", "q", T::Bool, false, "Return the settings."},
            {"settings", "s", T::Any, nullptr, "Replace every setting with this JSON."},
            {"autoMap", "am", T::Bool, false, "Map the chains again from the rigs."},
            {"chain", "c", T::Any, nullptr,
             "One chain: {'target': name, 'source': name or '', 'fk': {'enabled', 'rotation' (interpolated, oneToOne, "
             "oneToOneReversed, matchChain, none), 'rotationAlpha', 'translation' (none, globallyScaled, absolute, stretchUniform, "
             "stretchNonUniform), 'translationAlpha', 'poleMatching', 'poleMaintainOffset', 'poleOffset'}, 'ik': {'enabled', "
             "'blendToSource', 'blendToSourceWeights', 'staticOffset', 'staticLocalOffset', 'staticRotationOffset', 'scaleVertical', "
             "'extension', 'affectedByWarping'}, 'plant': {'enabled', 'speedThreshold', 'heightTolerance', 'blendFrames', 'minFrames'}}."},
            {"root", "r", T::Any, nullptr,
             "{'enabled', 'rotationAlpha', 'translationAlpha', 'blendToSource', 'blendToSourceWeights', 'scaleHorizontal', 'scaleVertical', "
             "'translationOffset', 'rotationOffset', 'affectIkHorizontal', 'affectIkVertical'}."},
            {"warp", "w", T::Any, nullptr, "Stride warping: {'enabled', 'useVelocity', 'warpForwards', 'sidewaysOffset'}."},
            {"pins", "pn", T::List, nullptr, "Bones copying another's transform: [{'bone', 'to', 'translate', 'rotate', 'maintainOffset'}]."},
            {"contacts", "ct", T::List, nullptr,
             "Hands held on the body: [{'effector': chain with IK, 'body': chain, 'bodyParam': 0..1 along it, 'distance': cm}]."},
            {"curves", "cv", T::List, nullptr, "Animated attributes copied renamed: [{'source', 'target'}]."},
            {"scale", "sc", T::Float, nullptr, "Target size over source size; 0 measures it from the legs."},
            {"enableRoot", "er", T::Bool, nullptr, "Run the root (pelvis) pass."},
            {"enableFk", "ef", T::Bool, nullptr, "Run the FK pass."},
            {"enableIk", "ei", T::Bool, nullptr, "Run the IK pass."},
            {"copyCurves", "cc", T::Bool, nullptr, "Copy the top joint's other animated attributes by name."},
            {"fullBody", "fb", T::Bool, nullptr, "Where a limb can't reach, solve the whole body with every goal pinned, the pelvis free."},
        },
        .run = runEditRetargeter,
        .examples = {"editRetargeter('Hips_to_root', chain={'target': 'LeftLeg', 'ik': {'extension': 0.98}})"},
    });
    ops::registerCommand({
        .name = "alignRetargetPose",
        .doc = "Set a retargeter's retarget poses: align the target's chains to point where the source's do, or capture either "
               "skeleton's current pose.",
        .positionalDoc = "the retargeter",
        .flags = {
            {"align", "a", T::Bool, true, "Align the target to the source."},
            {"captureSource", "cs", T::Bool, false, "The source's current pose becomes its retarget pose for this pair."},
            {"captureTarget", "ct", T::Bool, false, "The target's current pose becomes its retarget pose for this pair."},
            {"reset", "rs", T::Bool, false, "Drop this pair's poses, back to each rig's own."},
        },
        .run = runAlign,
    });
    ops::registerCommand({
        .name = "retarget",
        .doc = "Retarget animation onto a skeleton and key it into a take. The source is a skeleton in the scene, or .bvh/.fbx files "
               "(each imported, retargeted into a take named after the file, then removed). Without a retargeter, one is made with "
               "everything automatic: rigs, chain mapping, pose alignment, planted feet. Returns a report per run.",
        .flags = {
            {"source", "s", T::String, nullptr, "A joint of the source skeleton."},
            {"target", "t", T::String, nullptr, "A joint of the target skeleton."},
            {"retargeter", "r", T::String, nullptr, "Use this retargeter's settings (for files: as the template every file gets)."},
            {"files", "f", T::List, nullptr, "Animation files (.bvh, .fbx) to retarget, one take each."},
            {"folder", "fo", T::String, nullptr, "Retarget every .bvh and .fbx in the folder."},
            {"scale", "sc", T::Float, 1.0, "Scale for imported files (BVH has no units)."},
            {"start", "st", T::Float, nullptr, "Start, seconds; default the source's first key."},
            {"end", "e", T::Float, nullptr, "End, seconds; default the source's last key."},
            {"fps", "fps", T::Float, nullptr, "Frames per second keyed; default the scene's (a file's own for files)."},
            {"take", "tk", T::String, nullptr, "Take to key into, replaced if it exists; default the source's active take, else "
                                               "<source>_retarget. '' keys the target's current animation."},
            {"lockChains", "lc", T::Bool, true, "Key the lock channel of the target's IK chains where feet are planted."},
            {"keepSources", "ks", T::Bool, false, "Keep imported file skeletons in the scene."},
            {"mode", "m", T::String, "auto",
             "fk: chain to chain, for the same kind of body. semantic: body-independent, for a body with other limbs (a biped clip "
             "on a dog or a spider). auto: semantic when the leg counts differ.",
             {"auto", "fk", "semantic"}},
            {"gait", "g", T::String, "auto", "Semantic mode: the gait the target's legs keep.",
             {"auto", "walk", "trot", "pace", "canter", "gallop", "tripod", "tetrapod", "wave"}},
        },
        .run = runRetarget,
        .examples = {"retarget(source='Hips', target='pelvis')", "retarget(target='pelvis', folder='D:/mocap/walks')"},
    });
    ops::registerCommand({
        .name = "saveRetargetClip",
        .doc = "Save a skeleton's animation as a body-independent clip (.lilaclip): body motion in leg lengths, limb goals, "
               "spine and tail bends and foot contacts. applyRetargetClip plays it on any body, whatever its limbs.",
        .positionalDoc = "the file to write",
        .flags = {
            {"source", "s", T::String, nullptr, "A joint of the source skeleton."},
            {"start", "st", T::Float, nullptr, "Start, seconds; default the first key."},
            {"end", "e", T::Float, nullptr, "End, seconds; default the last key."},
            {"fps", "fps", T::Float, nullptr, "Samples per second; default the scene's."},
        },
        .undoable = false,
        .run = runSaveClip,
    });
    ops::registerCommand({
        .name = "applyRetargetClip",
        .doc = "Play a body-independent clip (saveRetargetClip) on a skeleton: its legs keep the gait, scaled to their length. "
               "Keys a take named after the file.",
        .positionalDoc = "the .lilaclip file",
        .flags = {
            {"target", "t", T::String, nullptr, "A joint of the target skeleton."},
            {"gait", "g", T::String, "auto", "The gait the target's legs keep.",
             {"auto", "walk", "trot", "pace", "canter", "gallop", "tripod", "tetrapod", "wave"}},
            {"take", "tk", T::String, nullptr, "Take to key into; default the file's name."},
            {"start", "st", T::Float, nullptr, "Time of the first frame, seconds; default 0."},
            {"lockChains", "lc", T::Bool, true, "Key the lock channel of the target's IK chains where feet are planted."},
        },
        .run = runApplyClip,
    });
    ops::registerCommand({
        .name = "importBvh",
        .doc = "Import a BVH motion capture file as joints keyed every frame.",
        .positionalDoc = "the .bvh file",
        .flags = {
            {"scale", "sc", T::Float, 1.0, "Multiplies offsets and positions (BVH has no units)."},
            {"parent", "p", T::String, nullptr, "Parent the skeleton under this node."},
            {"setPlayback", "sp", T::Bool, true, "Set the scene fps and range from the file."},
            {"endSites", "es", T::Bool, true, "Make end sites as non-deform joints."},
        },
        .run = runImportBvh,
    });
    if (!io::translators::find("bvh"))
        io::translators::add({.id = "bvh",
                              .label = "BVH",
                              .doc = "Motion capture: a skeleton keyed every frame.",
                              .extensions = {".bvh"},
                              .import = {.command = "importBvh"}});
}
} // namespace lila::retarget
