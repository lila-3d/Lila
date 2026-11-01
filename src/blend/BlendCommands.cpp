#include "blend/Blend.h"
#include "blend/BlendFile.h"
#include "blend/BlendInternal.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "core/Progress.h"
#include "io/References.h"
#include "io/SceneFile.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"

#include <algorithm>
#include <format>
#include <map>

namespace lila::blend
{
namespace
{
fs::path utf8Path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::vector<std::string> names(const Graph& g, const std::vector<NodeId>& ids)
{
    std::vector<std::string> out;
    for (NodeId id : ids)
        if (const scene::Node* n = g.node(id))
            out.push_back(n->path());
    return out;
}

std::string pathArg(const Args& a, const char* flag, const char* cmd)
{
    if (a.has(flag))
        return a.get<std::string>(flag);
    if (!a.positional.empty() && a.positional[0].is_string())
        return a.positional[0].get<std::string>();
    throw CommandError(std::string(cmd) + ": " + flag + " required");
}

ImportOptions optionsFrom(const Args& a, Graph& g, const char* cmd)
{
    ImportOptions o;
    if (a.has("parent"))
    {
        scene::Node* p = g.find(a.get<std::string>("parent"));
        if (!p)
            throw CommandError(std::string(cmd) + ": no node named '" + a.get<std::string>("parent") + "'");
        o.parent = p->id();
    }
    o.nameSpace = a.get<std::string>("namespace", "");
    o.collections = a.get<bool>("collections", true);
    o.materials = a.get<bool>("materials", true);
    o.skins = a.get<bool>("skins", true);
    o.morphs = a.get<bool>("morphs", true);
    o.animation = a.get<bool>("animation", true);
    o.modifiers = a.get<bool>("modifiers", true);
    o.hidden = a.get<bool>("hidden", true);
    o.evaluate = a.get<std::string>("evaluate", "off");
    if (o.evaluate != "off" && o.evaluate != "unsupported" && o.evaluate != "all")
        throw CommandError(std::string(cmd) + ": evaluate must be off, unsupported or all");
    o.blenderPath = a.get<std::string>("blenderPath", "");
    o.setPlayback = a.get<bool>("setPlayback", true);
    o.textureDir = a.get<std::string>("textureDir", "");
    return o;
}

const std::vector<FlagDef>& importFlags()
{
    using T = ArgType;
    static const std::vector<FlagDef> k = {
        {"namespace", "ns", T::String, "", "Put every imported name in this namespace."},
        {"collections", "", T::Bool, true, "Collections become groups holding their objects; off puts every object at the top."},
        {"materials", "", T::Bool, true, "Create materials from the Principled BSDF and its image textures; packed images are written out."},
        {"skins", "", T::Bool, true, "Rebuild Armature modifiers as skins from the vertex groups."},
        {"morphs", "", T::Bool, true, "Rebuild shape keys as morphs."},
        {"animation", "anim", T::Bool, true, "Import keyframed actions on objects, bones, shape keys, camera lenses and light power."},
        {"modifiers", "", T::Bool, true, "Rebuild the modifiers Lila has (mirror, subdivision, bevel, array, triangulate, solidify, decimate, weighted normals) live."},
        {"hidden", "", T::Bool, true, "Import objects and collections hidden in Blender's viewport, hidden; off skips them."},
        {"evaluate", "", T::String, "off",
         "off; unsupported: objects with modifiers Lila can't rebuild get Blender's evaluated mesh, baked; all: every object with modifiers does. "
         "Runs an installed Blender in the background.",
         {"off", "unsupported", "all"}},
        {"blenderPath", "", T::String, "", "The Blender executable for evaluate; the newest installed one when empty."},
        {"setPlayback", "", T::Bool, true, "Set the scene fps and playback range from the file."},
        {"textureDir", "", T::String, "", "Where packed images are written; <file>.textures beside the .blend when empty."},
    };
    return k;
}

json reportJson(const Graph& g, const ImportResult& r)
{
    return {{"nodes", names(g, r.nodes)},     {"roots", names(g, r.roots)},       {"meshes", names(g, r.meshes)},
            {"joints", names(g, r.joints)},   {"skinClusters", names(g, r.skins)}, {"morphs", names(g, r.morphs)},
            {"cameras", names(g, r.cameras)}, {"lights", names(g, r.lights)},     {"curves", names(g, r.curves)},
            {"materials", r.materials},       {"modifiers", r.modifiers},         {"objects", r.objectNames},
            {"evaluated", r.evaluated},       {"skipped", r.skipped},             {"warnings", r.warnings},
            {"version", r.version},           {"fps", r.fps}};
}

void logReport(const char* cmd, const fs::path& path, const ImportResult& r)
{
    for (const std::string& s : r.skipped)
        log::info("{}: {}: not converted: {}", cmd, utf8(path.filename()), s);
    for (const std::string& w : r.warnings)
        log::warn("{}: {}: {}", cmd, utf8(path.filename()), w);
}

std::unique_ptr<File> readWithProgress(const fs::path& path, const char* cmd, progress::Scope& root)
{
    std::unique_ptr<File> f;
    try
    {
        progress::Scope read("Reading " + utf8(path.filename()), 0.0, progress::None, 2.0);
        progress::runOnWorker([&] { f = std::make_unique<File>(path); });
    }
    catch (const progress::Cancelled&)
    {
        throw CommandError(std::string(cmd) + ": cancelled");
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string(cmd) + ": " + utf8(path.filename()) + ": " + e.what());
    }
    if (root.cancelled())
        throw CommandError(std::string(cmd) + ": cancelled");
    root.setCancellable(false);
    return f;
}

/** Replaces the scene with a Blender file, like opening a .lila: not undoable, untitled afterwards. */
ImportResult openAsScene(const File& file, const ImportOptions& opts)
{
    Graph& g = Graph::current();
    progress::Scope convert("", 0.0, progress::None, 8.0);
    io::refs::clear();
    g.clear();
    io::setCurrentPath({});
    UndoStack::get().setEnabled(false);
    ImportResult r;
    try
    {
        r = importBlend(g, file, opts);
    }
    catch (...)
    {
        UndoStack::get().setEnabled(true);
        throw;
    }
    UndoStack::get().setEnabled(true);
    UndoStack::get().clear();
    return r;
}
} // namespace

ImportResult importBlend(scene::Graph& graph, const File& file, const ImportOptions& opts)
{
    LILA_ZONE();

    ImportResult r;
    Importer im(graph, file, opts, r);
    try
    {
        im.run();
    }
    catch (const std::exception& e)
    {
        r.ok = false;
        r.error = e.what();
    }
    return r;
}

ImportResult importBlend(scene::Graph& graph, const fs::path& path, const ImportOptions& opts)
{
    try
    {
        const File file(path);
        return importBlend(graph, file, opts);
    }
    catch (const std::exception& e)
    {
        ImportResult r;
        r.error = e.what();
        return r;
    }
}

json info(const File& file)
{
    std::map<std::string, int> ids, objectTypes, modifiers, nodes, constraints;
    static const std::map<int, const char*> kTypes = {
        {ob::kEmpty, "empty"},           {ob::kMesh, "mesh"},          {ob::kCurve, "curve"},          {ob::kSurface, "surface"},
        {ob::kFont, "text"},             {ob::kMeta, "metaball"},      {ob::kLight, "light"},          {ob::kCamera, "camera"},
        {ob::kSpeaker, "speaker"},       {ob::kLightProbe, "lightProbe"}, {ob::kLattice, "lattice"},  {ob::kArmature, "armature"},
        {ob::kGpencilLegacy, "greasePencilLegacy"}, {ob::kCurves, "hairCurves"}, {ob::kPointCloud, "pointCloud"}, {ob::kVolume, "volume"},
        {ob::kGreasePencil, "greasePencil"}};
    json libraries = json::array();
    size_t packed = 0;
    for (const Block& b : file.blocks())
    {
        if (!b.isId())
            continue;
        const std::string code(b.code);
        ++ids[code];
        const View v = file.viewOf(b);
        if (code == "OB")
        {
            const int t = v.i("type");
            auto it = kTypes.find(t);
            ++objectTypes[it == kTypes.end() ? std::to_string(t) : it->second];
            for (const View& m : v.list("modifiers"))
            {
                const View head = m.has("modifier") ? m.sub("modifier") : m;
                const int type = head.i("type");
                std::string name = md::name(type);
                if (type == md::kNodes)
                    if (const View group = m.deref("node_group"))
                        name += ": " + group.idName();
                ++modifiers[name];
            }
            for (const View& c : v.list("constraints"))
                ++constraints[std::to_string(c.i("type"))];
        }
        else if (code == "LI")
            libraries.push_back(v.str("filepath"));
        else if (code == "IM" && (v.ptr("packedfile") || !v.list("packedfiles").empty()))
            ++packed;
    }
    for (const Block& b : file.blocks())
        if (!b.isId() && file.typeOf(b) && file.typeOf(b)->name == "bNode")
            for (uint32_t i = 0; i < std::max<uint32_t>(1, b.count); ++i)
            {
                const View n = file.viewOf(b).element(i);
                ++nodes[n.str("idname")];
            }
    return {{"version", file.versionString()},
            {"ids", ids},
            {"objectTypes", objectTypes},
            {"modifiers", modifiers},
            {"constraintTypes", constraints},
            {"nodes", nodes},
            {"libraries", libraries},
            {"packedImages", packed}};
}

void registerCommands()
{
    using T = ArgType;

    {
        std::vector<FlagDef> flags = {{"path", "", T::String, nullptr, "The .blend file (or pass it first)."},
                                      {"parent", "p", T::String, nullptr, "Parent the imported roots under this node."}};
        flags.insert(flags.end(), importFlags().begin(), importFlags().end());
        flags.push_back({"report", "", T::Bool, false, "Return the import report instead of just the names."});
        flags.push_back({"select", "", T::Bool, true, "Select the imported roots."});
        ops::registerCommand({
            .name = "blendImport",
            .doc = "Import a Blender .blend file into the scene, without Blender: collections, objects, meshes (UVs, colors, sharp "
                   "edges, creases, attributes), materials with their image textures, cameras, lights, curves, collection instances, "
                   "armatures with skins, shape keys, animation and the modifiers Lila has. Anything it can't convert is reported "
                   "(report=True returns the lists), and objects with modifiers it dropped are flagged as issues.",
            .positionalDoc = "path",
            .flags = flags,
            .run = [](const Args& a) -> json {
                const fs::path path = utf8Path(pathArg(a, "path", "blendImport"));
                Graph& g = Graph::current();
                const ImportOptions opts = optionsFrom(a, g, "blendImport");
                progress::Scope scope("Importing " + utf8(path.filename()) + "...", 10.0, progress::Cancellable);
                const std::unique_ptr<File> file = readWithProgress(path, "blendImport", scope);
                ImportResult r;
                {
                    progress::Scope convert("", 0.0, progress::None, 8.0);
                    r = importBlend(g, *file, opts);
                }
                if (!r.ok)
                    throw CommandError("blendImport: " + r.error);
                logReport("blendImport", path, r);
                if (a.get<bool>("select", true))
                    g.select(r.roots);
                return a.get<bool>("report", false) ? reportJson(g, r) : json(names(g, r.nodes));
            },
        });
    }

    {
        std::vector<FlagDef> flags = {{"path", "", T::String, nullptr, "The .blend file (or pass it first)."},
                                      {"force", "f", T::Bool, false, "Discard unsaved changes."}};
        flags.insert(flags.end(), importFlags().begin(), importFlags().end());
        ops::registerCommand({
            .name = "blendOpen",
            .doc = "Open a Blender .blend file as a new, untitled scene (save it to keep it as .lila). Not undoable. Returns the import report.",
            .positionalDoc = "path",
            .flags = flags,
            .undoable = false,
            .run = [](const Args& a) -> json {
                const fs::path path = utf8Path(pathArg(a, "path", "blendOpen"));
                if (io::modified() && !a.get<bool>("force", false))
                    throw CommandError("blendOpen: the scene has unsaved changes; save it or pass force=True");
                Graph& g = Graph::current();
                const ImportOptions opts = optionsFrom(a, g, "blendOpen");
                progress::Scope scope("Opening " + utf8(path.filename()) + "...", 10.0, progress::Cancellable);
                const std::unique_ptr<File> file = readWithProgress(path, "blendOpen", scope);
                ImportResult r = openAsScene(*file, opts);
                if (!r.ok)
                    throw CommandError("blendOpen: " + r.error);
                logReport("blendOpen", path, r);
                return reportJson(g, r);
            },
        });
    }

    {
        std::vector<FlagDef> flags = {
            {"source", "src", T::String, nullptr, "The .blend file."},
            {"destination", "dst", T::String, "", "The .lila or .lilaa to write; <file>.lila beside the source by default."},
            {"force", "f", T::Bool, false, "Discard unsaved changes in the current scene."},
        };
        flags.insert(flags.end(), importFlags().begin(), importFlags().end());
        ops::registerCommand({
            .name = "blendConvert",
            .doc = "Convert a Blender .blend file to a Lila scene: opened as its own scene, saved, and the scene left empty. Returns the "
                   "destination, skipped features and warnings.",
            .positionalDoc = "source",
            .flags = flags,
            .undoable = false,
            .run = [](const Args& a) -> json {
                const fs::path src = utf8Path(pathArg(a, "source", "blendConvert"));
                if (io::modified() && !a.get<bool>("force", false))
                    throw CommandError("blendConvert: the scene has unsaved changes; save it or pass force=True");
                const std::string dstArg = a.get<std::string>("destination", "");
                fs::path dst = dstArg.empty() ? fs::path(src).replace_extension(".lila") : utf8Path(dstArg);
                if (dst.extension() != ".lila" && dst.extension() != ".lilaa")
                    dst += ".lila";
                const ImportOptions opts = optionsFrom(a, Graph::current(), "blendConvert");
                progress::Scope scope("Converting " + utf8(src.filename()) + "...", 10.0, progress::Cancellable);
                const std::unique_ptr<File> file = readWithProgress(src, "blendConvert", scope);
                ImportResult r = openAsScene(*file, opts);
                if (!r.ok)
                    throw CommandError("blendConvert: " + r.error);
                logReport("blendConvert", src, r);
                std::error_code ec;
                fs::create_directories(dst.parent_path(), ec);
                io::save(dst);
                io::refs::clear();
                Graph::current().clear();
                UndoStack::get().clear();
                io::setCurrentPath({});
                io::markClean();
                return {{"source", utf8(src)}, {"destination", utf8(dst)}, {"skipped", r.skipped}, {"warnings", r.warnings}};
            },
        });
    }

    ops::registerCommand({
        .name = "blendInfo",
        .doc = "Read a Blender .blend file without importing it: the Blender version that saved it, its datablocks by type, object types, "
               "the modifiers, constraints and nodes it uses, linked libraries and packed images.",
        .positionalDoc = "path",
        .flags = {{"path", "", T::String, nullptr, "The .blend file (or pass it first)."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const fs::path path = utf8Path(pathArg(a, "path", "blendInfo"));
            try
            {
                const File f(path);
                return info(f);
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("blendInfo: ") + e.what());
            }
        },
    });
}
} // namespace lila::blend
