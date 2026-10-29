#include "maya/Maya.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Progress.h"
#include "io/References.h"
#include "io/SceneFile.h"
#include "maya/MayaFile.h"
#include "scene/Graph.h"

#include <algorithm>
#include <format>
#include <map>

namespace lila::maya
{
namespace
{
using scene::Graph;
using scene::NodeId;

fs::path utf8Path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string utf8(const fs::path& p)
{
    std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

std::vector<std::string> names(const Graph& g, const std::vector<NodeId>& ids)
{
    std::vector<std::string> out;
    for (NodeId id : ids)
        if (const scene::Node* n = g.node(id))
            out.push_back(n->path());
    return out;
}

/** The path from positional 0 or a flag. */
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
    o.upAxis = a.get<std::string>("upAxis", "auto");
    if (o.upAxis != "auto" && o.upAxis != "y" && o.upAxis != "z")
        throw CommandError(std::string(cmd) + ": upAxis must be auto, y or z");
    o.skins = a.get<bool>("skins", true);
    o.morphs = a.get<bool>("morphs", true);
    o.animation = a.get<bool>("animation", true);
    o.materials = a.get<bool>("materials", true);
    if (a.flags.contains("references") && a.flags["references"].is_boolean())
        o.references = a.flags["references"].get<bool>() ? "reference" : "none";
    else
        o.references = a.get<std::string>("references", "reference");
    if (o.references != "reference" && o.references != "import" && o.references != "none")
        throw CommandError(std::string(cmd) + ": references must be reference, import or none");
    o.history = a.get<bool>("history", true);
    o.setPlayback = a.get<bool>("setPlayback", true);
    return o;
}

json reportJson(const Graph& g, const ImportResult& r)
{
    return {{"nodes", names(g, r.nodes)},   {"roots", names(g, r.roots)},     {"joints", names(g, r.joints)},
            {"skinClusters", names(g, r.skins)}, {"morphs", names(g, r.morphs)}, {"cameras", names(g, r.cameras)}, {"imagePlanes", names(g, r.imagePlanes)},
            {"materials", r.materials},     {"displayLayers", r.displayLayers}, {"curves", r.curves},
            {"skipped", r.skipped},         {"warnings", r.warnings},          {"format", r.format},
            {"version", r.version},         {"upAxis", r.upAxis},              {"unitCm", r.unitCm},
            {"fps", r.fps},                 {"references", r.references},      {"converted", r.converted}};
}

void logReport(const char* cmd, const fs::path& path, const ImportResult& r)
{
    for (const std::string& s : r.skipped)
        log::info("{}: {}: not converted: {}", cmd, path.filename().string(), s);
    for (const std::string& w : r.warnings)
        log::warn("{}: {}: {}", cmd, path.filename().string(), w);
}

/**
 * Reads and parses a Maya file off the main thread: the slow part, and the only one that can be cancelled without
 * touching the scene. Throws CommandError; on success the root's Cancel button goes away.
 */
File readWithProgress(const fs::path& path, const char* cmd, progress::Scope& root)
{
    File f;
    try
    {
        progress::Scope read("Reading " + utf8(path.filename()), 0.0, progress::None, 2.0);
        progress::runOnWorker([&] { f = readFile(path); });
    }
    catch (const progress::Cancelled&)
    {
        throw CommandError(std::string(cmd) + ": cancelled");
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string(cmd) + ": " + e.what());
    }
    if (root.cancelled())
        throw CommandError(std::string(cmd) + ": cancelled");
    root.setCancellable(false);
    return f;
}

std::string convertingTitle(const fs::path& path)
{
    return "Converting " + utf8(path.filename()) + "...";
}

/** Replaces the scene with a Maya file, like opening a .lila: not undoable, untitled afterwards. */
ImportResult openAsScene(const fs::path& path, const File& file, const ImportOptions& opts)
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
        r = importMaya(g, path, file, opts);
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

const std::vector<FlagDef>& importFlags()
{
    using T = ArgType;
    static const std::vector<FlagDef> k = {
        {"namespace", "ns", T::String, "", "Put every imported name in this namespace."},
        {"upAxis", "", T::String, "auto", "auto (from Maya's default cameras), y or z: the file's up axis."},
        {"skins", "", T::Bool, true, "Rebuild skinClusters with their weights and bind matrices."},
        {"morphs", "", T::Bool, true, "Rebuild blendShapes as morphs, with their weight curves and set driven keys."},
        {"animation", "anim", T::Bool, true, "Import animation curves on translate, rotate and scale."},
        {"materials", "", T::Bool, true, "Create materials from shading groups and assign them per face."},
        {"references", "", T::Any, "reference",
         "reference: referenced Maya / FBX / OBJ files become Lila references (Maya and FBX ones converted to a .lila beside "
         "them) with the scene's reference edits as overrides; import: their nodes as ordinary scene nodes; none: skip them.",
         {"reference", "import", "none"}},
        {"history", "", T::Bool, true, "Keep a lone primitive creator (polyCube ...) as a live generator node."},
        {"setPlayback", "", T::Bool, true, "Set the scene fps and playback range from the file."},
    };
    return k;
}

std::vector<fs::path> mayaFilesIn(const fs::path& dir, bool recursive)
{
    std::vector<fs::path> out;
    std::error_code ec;
    if (recursive)
    {
        for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec))
            if (it->is_regular_file(ec) && isMayaFile(it->path()))
                out.push_back(it->path());
    }
    else
        for (const auto& e : fs::directory_iterator(dir, ec))
            if (e.is_regular_file(ec) && isMayaFile(e.path()))
                out.push_back(e.path());
    std::sort(out.begin(), out.end());
    return out;
}
} // namespace

void registerCommands()
{
    using T = ArgType;

    {
        std::vector<FlagDef> flags = {{"path", "", T::String, nullptr, "The .ma or .mb file (or pass it first)."},
                                      {"parent", "p", T::String, nullptr, "Parent the imported roots under this node."}};
        flags.insert(flags.end(), importFlags().begin(), importFlags().end());
        flags.push_back({"report", "", T::Bool, false, "Return the import report instead of just the names."});
        flags.push_back({"select", "", T::Bool, true, "Select the imported roots."});
        ops::registerCommand({
            .name = "mayaImport",
            .doc = "Import a Maya .ma or .mb file into the scene, without Maya: transforms, groups, meshes (uvs, colors, "
                   "normals, hard edges, creases), primitive history, joints, skinClusters, locators, materials with "
                   "file textures, animation curves, display layers, namespaces and references. Anything it can't convert is reported "
                   "(report=True returns the lists) and never fails the import.",
            .positionalDoc = "path",
            .flags = flags,
            .run = [](const Args& a) -> json {
                fs::path path = utf8Path(pathArg(a, "path", "mayaImport"));
                Graph& g = Graph::current();
                const ImportOptions opts = optionsFrom(a, g, "mayaImport");
                progress::Scope scope(convertingTitle(path), 10.0, progress::Cancellable);
                const File file = readWithProgress(path, "mayaImport", scope);
                ImportResult r;
                {
                    progress::Scope convert("", 0.0, progress::None, 8.0);
                    r = importMaya(g, path, file, opts);
                }
                if (!r.ok)
                    throw CommandError("mayaImport: " + r.error);
                logReport("mayaImport", path, r);
                if (a.get<bool>("select", true))
                    g.select(r.roots);
                return a.get<bool>("report", false) ? reportJson(g, r) : json(names(g, r.nodes));
            },
        });
    }

    {
        std::vector<FlagDef> flags = {{"path", "", T::String, nullptr, "The .ma or .mb file (or pass it first)."},
                                      {"force", "f", T::Bool, false, "Discard unsaved changes."}};
        flags.insert(flags.end(), importFlags().begin(), importFlags().end());
        ops::registerCommand({
            .name = "mayaOpen",
            .doc = "Open a Maya .ma or .mb file as a new, untitled scene (save it to keep it as .lila). Not undoable. "
                   "Returns the import report.",
            .positionalDoc = "path",
            .flags = flags,
            .undoable = false,
            .run = [](const Args& a) -> json {
                fs::path path = utf8Path(pathArg(a, "path", "mayaOpen"));
                if (io::modified() && !a.get<bool>("force", false))
                    throw CommandError("mayaOpen: the scene has unsaved changes; save it or pass force=True");
                Graph& g = Graph::current();
                const ImportOptions opts = optionsFrom(a, g, "mayaOpen");
                progress::Scope scope(convertingTitle(path), 10.0, progress::Cancellable);
                const File file = readWithProgress(path, "mayaOpen", scope);
                ImportResult r = openAsScene(path, file, opts);
                if (!r.ok)
                    throw CommandError("mayaOpen: " + r.error);
                logReport("mayaOpen", path, r);
                return reportJson(g, r);
            },
        });
    }

    {
        std::vector<FlagDef> flags = {
            {"source", "src", T::String, nullptr, "A .ma / .mb file, or a folder of them."},
            {"destination", "dst", T::String, "", "The .lila / .lilaa file, or a folder for folder mode; next to the source by default."},
            {"format", "", T::String, "binary", "binary (.lila) or ascii (.lilaa) when the destination is a folder."},
            {"recursive", "r", T::Bool, false, "Folder mode: also convert files in subfolders (mirrored under the destination)."},
            {"overwrite", "", T::Bool, true, "Replace existing destination files."},
            {"force", "f", T::Bool, false, "Discard unsaved changes in the current scene."},
        };
        flags.insert(flags.end(), importFlags().begin(), importFlags().end());
        ops::registerCommand({
            .name = "mayaConvert",
            .doc = "Convert Maya files to Lila scenes: a file to a .lila / .lilaa, or every .ma / .mb in a folder. Each "
                   "file is opened as its own scene and saved; the scene is left empty afterwards. Returns one entry per "
                   "file with its destination (or error), skipped features and warnings.",
            .positionalDoc = "source",
            .flags = flags,
            .undoable = false,
            .run = [](const Args& a) -> json {
                LILA_ZONE();

                fs::path src = utf8Path(pathArg(a, "source", "mayaConvert"));
                if (io::modified() && !a.get<bool>("force", false))
                    throw CommandError("mayaConvert: the scene has unsaved changes; save it or pass force=True");
                const std::string format = a.get<std::string>("format", "binary");
                if (format != "binary" && format != "ascii")
                    throw CommandError("mayaConvert: format must be binary or ascii");
                const std::string ext = format == "ascii" ? ".lilaa" : ".lila";
                const std::string dstArg = a.get<std::string>("destination", "");
                const bool overwrite = a.get<bool>("overwrite", true);
                ImportOptions opts = optionsFrom(a, Graph::current(), "mayaConvert");

                std::error_code ec;
                std::vector<std::pair<fs::path, fs::path>> jobs;
                if (fs::is_directory(src, ec))
                {
                    fs::path dstDir = dstArg.empty() ? src : utf8Path(dstArg);
                    for (const fs::path& f : mayaFilesIn(src, a.get<bool>("recursive", false)))
                    {
                        fs::path rel = fs::relative(f, src, ec);
                        jobs.push_back({f, (dstDir / rel).replace_extension(ext)});
                    }
                    if (jobs.empty())
                        throw CommandError("mayaConvert: no .ma or .mb files in " + utf8(src));
                    // name.ma and name.mb side by side would write the same .lila; both keep their kind in the name.
                    std::map<fs::path, int> uses;
                    for (const auto& j : jobs)
                        ++uses[j.second];
                    for (auto& [from, to] : jobs)
                        if (uses[to] > 1)
                            to.replace_filename(to.stem().string() + "_" + from.extension().string().substr(1) + ext);
                }
                else
                {
                    if (!fs::exists(src, ec))
                        throw CommandError("mayaConvert: " + utf8(src) + " not found");
                    fs::path dst = dstArg.empty() ? fs::path(src).replace_extension(ext) : utf8Path(dstArg);
                    if (fs::is_directory(dst, ec))
                        dst = (dst / src.filename()).replace_extension(ext);
                    else if (dst.extension() != ".lila" && dst.extension() != ".lilaa")
                        dst += ext;
                    jobs.push_back({src, dst});
                }

                json out = json::array();
                // Cancel stops between files (or while one is being read); finished files stay converted.
                progress::Scope scope(jobs.size() == 1 ? convertingTitle(src) : "Converting " + utf8(src.filename()) + " folder...",
                                      double(jobs.size()), progress::Cancellable);
                for (const auto& [from, to] : jobs)
                {
                    json entry = {{"source", utf8(from)}, {"destination", utf8(to)}};
                    if (scope.cancelled())
                    {
                        entry["cancelled"] = true;
                        out.push_back(entry);
                        continue;
                    }
                    progress::Scope one("Converting " + utf8(from.filename()), 10.0);
                    one.setStatus(std::format("Converting {} ({}/{})", utf8(from.filename()), out.size() + 1, jobs.size()));
                    if (!overwrite && fs::exists(to, ec))
                    {
                        entry["skippedExisting"] = true;
                        out.push_back(entry);
                        continue;
                    }
                    try
                    {
                        File file;
                        {
                            progress::Scope read("Reading " + utf8(from.filename()), 0.0, progress::None, 2.0);
                            progress::runOnWorker([&] { file = readFile(from); });
                        }
                        ImportResult r = openAsScene(from, file, opts);
                        if (!r.ok)
                            throw std::runtime_error(r.error);
                        logReport("mayaConvert", from, r);
                        fs::create_directories(to.parent_path(), ec);
                        io::save(to);
                        entry["nodes"] = r.nodes.size();
                        entry["skipped"] = r.skipped;
                        entry["warnings"] = r.warnings;
                        log::info("mayaConvert: {} -> {}", utf8(from), utf8(to));
                    }
                    catch (const progress::Cancelled&)
                    {
                        entry["cancelled"] = true;
                    }
                    catch (const std::exception& e)
                    {
                        entry["error"] = e.what();
                        log::error("mayaConvert: {}: {}", utf8(from), e.what());
                    }
                    out.push_back(entry);
                }
                io::refs::clear();
                Graph::current().clear();
                UndoStack::get().clear();
                io::setCurrentPath({});
                io::markClean();
                return out;
            },
        });
    }

    ops::registerCommand({
        .name = "mayaInfo",
        .doc = "Read a Maya file without importing it: format, version, units, fileInfo, references, node counts by "
               "type and read warnings.",
        .positionalDoc = "path",
        .flags = {{"path", "", T::String, nullptr, "The .ma or .mb file (or pass it first)."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            fs::path path = utf8Path(pathArg(a, "path", "mayaInfo"));
            File f;
            try
            {
                f = readFile(path);
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("mayaInfo: ") + e.what());
            }
            std::map<std::string, int> counts;
            for (const Node& n : f.nodes)
                if (!n.existing)
                    ++counts[n.type];
            json refs = json::array();
            for (const Reference& r : f.references)
                refs.push_back({{"path", r.path}, {"namespace", r.ns}, {"type", r.type}, {"loaded", r.loaded}});
            return {{"format", f.binary ? "binary" : "ascii"},
                    {"version", f.version},
                    {"linearUnit", f.linearUnit},
                    {"angleUnit", f.angleUnit},
                    {"timeUnit", f.timeUnit},
                    {"fileInfo", f.fileInfo},
                    {"requires", f.requires_},
                    {"references", refs},
                    {"nodeTypes", counts},
                    {"nodes", f.nodes.size()},
                    {"connections", f.connections.size()},
                    {"warnings", f.warnings}};
        },
    });

    ops::registerCommand({
        .name = "mayaReference",
        .doc = "Reference a Maya .ma or .mb file: it is converted to a .lila beside it (reused while the Maya file is unchanged) "
               "and that is referenced under a namespace, as createReference. Returns the namespace. Clears undo.",
        .positionalDoc = "path",
        .flags = {{"path", "", T::String, nullptr, "The .ma or .mb file (or pass it first)."},
                  {"namespace", "ns", T::String, nullptr, "Namespace; defaults to the file name."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            fs::path path = utf8Path(pathArg(a, "path", "mayaReference"));
            std::error_code ec;
            if (!fs::exists(path, ec))
                throw CommandError("mayaReference: " + utf8(path) + " not found");
            if (!isMayaFile(path))
                throw CommandError("mayaReference: " + utf8(path) + " is not a .ma or .mb file");
            ImportResult r;
            fs::path lila;
            {
                progress::Scope scope(convertingTitle(path), 1.0, progress::Cancellable);
                lila = lilaForReference(path, ImportOptions{}, r);
            }
            if (lila.empty())
                throw CommandError("mayaReference: " + r.error);
            for (const std::string& w : r.warnings)
                log::warn("mayaReference: {}", w);
            json call = {{"args", {utf8(lila)}}};
            call["namespace"] = a.has("namespace") ? a.get<std::string>("namespace") : path.stem().string();
            return ops::execute("createReference", call);
        },
    });
}
} // namespace lila::maya
