#include "fbx/GameExport.h"

#include "ops/Command.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "core/Progress.h"
#include "fbx/Fbx.h"
#include "scene/Graph.h"
#include "scene/ObjectSets.h"

#include <algorithm>

namespace lila::fbx::game
{
namespace
{
namespace fs = std::filesystem;
using scene::Graph;
using scene::Node;
using scene::NodeId;
using T = ArgType;

constexpr const char* kPresetType = "gameExportPreset";
constexpr const char* kPrefFile = "gameExporter";

const std::vector<std::string> kModes = {"all", "selection", "sets"};
const std::vector<std::string> kSeparate = {"none", "perSet", "perMember"};
const std::vector<std::string> kFileTypes = {"fbx", "obj"};
const std::vector<std::string> kUnits = {"cm", "m", "mm", "in", "ft"};

std::string oneOf(const json& s, const char* key, const std::vector<std::string>& allowed)
{
    std::string v = s.value(key, allowed.front());
    if (std::ranges::find(allowed, v) == allowed.end())
    {
        std::string list;
        for (const std::string& a : allowed)
            list += (list.empty() ? "" : ", ") + a;
        throw std::invalid_argument(std::string(key) + " must be one of " + list + ", not '" + v + "'");
    }
    return v;
}

/** File names can't hold namespace colons or path separators. */
std::string fileSafe(std::string name)
{
    for (char& c : name)
        if (c == ':' || c == '|' || c == '/' || c == '\\' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>')
            c = '_';
    return name;
}

void addUnique(std::vector<NodeId>& out, NodeId id)
{
    if (std::ranges::find(out, id) == out.end())
        out.push_back(id);
}

/** Top-level things worth exporting: transforms other than cameras. */
std::vector<NodeId> exportableRoots(const Graph& g)
{
    std::vector<NodeId> out;
    for (NodeId id : g.roots())
        if (const Node* n = g.node(id); n && n->type().isA("transform") && !n->type().isA("camera"))
            out.push_back(id);
    return out;
}

enum PresetAttr
{
    kPresetName,
    kPresetSettings,
};

std::vector<Node*> presetNodes(const Graph& g)
{
    std::vector<NodeId> ids = g.nodes();
    std::ranges::sort(ids);
    std::vector<Node*> out;
    for (NodeId id : ids)
        if (Node* n = g.node(id); n->type().isA(kPresetType))
            out.push_back(n);
    return out;
}

Node* presetNode(const Graph& g, const std::string& name)
{
    for (Node* n : presetNodes(g))
        if (n->as<std::string>(kPresetName) == name)
            return n;
    return nullptr;
}

json scenePresets(const Graph& g)
{
    json out = json::object();
    for (const Node* n : presetNodes(g))
    {
        json s = json::parse(n->as<std::string>(kPresetSettings), nullptr, false);
        out[n->as<std::string>(kPresetName)] = s.is_object() ? s : json::object();
    }
    return out;
}

json prefPresets()
{
    json p = PrefStore::get().value(kPrefFile, "/presets", json::object());
    return p.is_object() ? p : json::object();
}

/** Flags shared by gameExport and the preset commands, one per setting. */
std::vector<FlagDef> settingFlags()
{
    return {
        {"path", "p", T::String, nullptr, "Folder to write into; relative to the scene's folder. The scene's folder by default."},
        {"filename", "fn", T::String, nullptr, "File name without extension for a single-file export; export by default."},
        {"fileType", "ft", T::String, nullptr, "fbx or obj.", kFileTypes},
        {"mode", "m", T::String, nullptr, "What to export: all, selection or sets.", kModes},
        {"sets", "s", T::List, nullptr, "Object sets to export with mode=sets."},
        {"separate", "sep", T::String, nullptr,
         "none: one file; perSet: a file per set, named after it; perMember: a file per object, named after it.", kSeparate},
        {"moveToOrigin", "mo", T::Bool, nullptr, "Drop the exported roots' own transforms so they sit at the origin."},
        {"profile", "", T::String, nullptr, "FBX axes: ue5 (Z up), ue6 (Y up) or generic (the scene's own).", {"ue5", "ue6", "generic"}},
        {"triangulate", "", T::Bool, nullptr, "Write triangles exactly as the viewport splits faces."},
        {"smoothingGroups", "sg", T::Bool, nullptr, "Write edge smoothing (hard and soft edges)."},
        {"tangents", "", T::Bool, nullptr, "Write MikkTSpace tangents and binormals from uv0."},
        {"units", "u", T::String, nullptr, "File units: cm, m, mm, in or ft. Lengths are scaled to them.", kUnits},
        {"embedMedia", "em", T::Bool, nullptr, "Embed textures in the file. The FBX writer doesn't write textures yet, so this has no effect."},
    };
}

json settingsFrom(const Args& a)
{
    json s = json::object();
    for (const FlagDef& f : settingFlags())
        if (a.has(f.name))
            s[f.name] = a.flags[f.name];
    return s;
}

std::vector<NodeId> setsByName(const Graph& g, const json& names)
{
    std::vector<NodeId> out;
    if (!names.is_array())
        throw std::invalid_argument("sets must be a list of set names");
    for (const json& v : names)
    {
        const Node* n = v.is_string() ? g.find(v.get<std::string>()) : nullptr;
        if (!n || !scene::objectSets::isSet(*n))
            throw std::invalid_argument((v.is_string() ? v.get<std::string>() : v.dump()) + " is not an object set");
        out.push_back(n->id());
    }
    return out;
}

void registerPresetType()
{
    if (scene::types::find(kPresetType))
        return;
    scene::NodeType t;
    t.name = kPresetType;
    t.icon = "file-export";
    t.doc = "Game exporter settings kept with the scene (File > Game Exporter).";
    t.ownAttrs = {{.name = "presetName", .shortName = "pn", .type = scene::PlugType::String, .defaultValue = std::string(),
                   .flags = scene::AttrInput | scene::AttrStorable, .doc = "The name the Game Exporter lists it under."},
                  {.name = "settings", .shortName = "st", .type = scene::PlugType::String, .defaultValue = std::string("{}"),
                   .flags = scene::AttrInput | scene::AttrStorable, .doc = "The preset's settings as JSON."}};
    t.create = [] { return std::make_unique<Node>(); };
    scene::types::registerType(std::move(t));
}
} // namespace

json defaults()
{
    return {
        {"path", ""},
        {"filename", "export"},
        {"fileType", "fbx"},
        {"mode", "all"},
        {"sets", json::array()},
        {"separate", "none"},
        {"moveToOrigin", false},
        {"profile", "ue5"},
        {"triangulate", true},
        {"smoothingGroups", true},
        {"tangents", true},
        {"units", "cm"},
        {"embedMedia", false},
    };
}

json resolved(const json& settings)
{
    json out = defaults();
    if (settings.is_object())
        for (auto& [k, v] : settings.items())
            if (!v.is_null())
                out[k] = v;
    return out;
}

double cmPerUnit(const std::string& units)
{
    if (units == "cm")
        return 1.0;
    if (units == "m")
        return 100.0;
    if (units == "mm")
        return 0.1;
    if (units == "in")
        return 2.54;
    if (units == "ft")
        return 30.48;
    return 0.0;
}

std::vector<Job> plan(Graph& g, const json& settingsIn)
{
    LILA_ZONE();

    const json s = resolved(settingsIn);
    const std::string mode = oneOf(s, "mode", kModes);
    const std::string separate = oneOf(s, "separate", kSeparate);
    const std::string ext = "." + oneOf(s, "fileType", kFileTypes);
    oneOf(s, "units", kUnits);

    fs::path dir = paths::resolvePath(s.value("path", std::string()));
    if (dir.empty())
        dir = paths::currentSceneDir();
    std::string base = s.value("filename", std::string());
    if (base.empty())
        base = "export";
    if (fs::path(paths::fromUtf8(base)).extension() == ext)
        base = paths::toUtf8(fs::path(paths::fromUtf8(base)).stem());

    // Groups of nodes, each named for perSet.
    std::vector<std::pair<std::string, std::vector<NodeId>>> groups;
    if (mode == "sets")
    {
        const std::vector<NodeId> sets = setsByName(g, s["sets"]);
        if (sets.empty())
            throw std::invalid_argument("mode=sets needs at least one set");
        for (NodeId set : sets)
            groups.push_back({g.node(set)->name(), scene::objectSets::nodesOf(g, set)});
    }
    else if (mode == "selection")
    {
        std::vector<NodeId> nodes;
        for (NodeId id : g.selection())
        {
            const Node* n = g.node(id);
            if (n && scene::objectSets::isSet(*n))
                for (NodeId m : scene::objectSets::nodesOf(g, id))
                    addUnique(nodes, m);
            else if (n && n->type().dag)
                addUnique(nodes, id);
        }
        groups.push_back({base, nodes});
    }
    else
        groups.push_back({base, exportableRoots(g)});

    std::vector<Job> jobs;
    auto job = [&](const std::string& name, std::vector<NodeId> nodes) {
        if (nodes.empty())
            return;
        fs::path file = dir / paths::fromUtf8(fileSafe(name) + ext);
        auto it = std::ranges::find_if(jobs, [&](const Job& j) { return j.file == file; });
        if (it == jobs.end())
            jobs.push_back({file, std::move(nodes)});
        else
            for (NodeId id : nodes)
                addUnique(it->nodes, id);
    };
    if (separate == "perMember")
    {
        for (const auto& [name, nodes] : groups)
            for (NodeId id : nodes)
                job(g.node(id)->name(), {id});
    }
    else if (separate == "perSet" && mode == "sets")
    {
        for (const auto& [name, nodes] : groups)
            job(name, nodes);
    }
    else
    {
        std::vector<NodeId> all;
        for (const auto& [name, nodes] : groups)
            for (NodeId id : nodes)
                addUnique(all, id);
        job(base, all);
    }
    return jobs;
}

std::vector<fs::path> run(Graph& g, const json& settingsIn)
{
    LILA_ZONE();

    const json s = resolved(settingsIn);
    std::vector<Job> jobs = plan(g, s);
    if (jobs.empty())
        throw std::runtime_error("nothing to export");

    ExportOptions opts;
    if (!parseProfile(s.value("profile", std::string("ue5")), &opts.profile))
        throw std::invalid_argument("profile must be ue5, ue6 or generic");
    opts.kind = ExportKind::Static;
    opts.triangulate = s.value("triangulate", true);
    opts.tangents = s.value("tangents", true);
    opts.smoothingGroups = s.value("smoothingGroups", true);
    opts.atOrigin = s.value("moveToOrigin", false);
    opts.unitScale = cmPerUnit(s.value("units", std::string("cm")));
    if (s.value("embedMedia", false))
        log::warn("gameExport: embedMedia has no effect, the FBX writer doesn't write textures yet");
    const bool obj = s.value("fileType", std::string("fbx")) == "obj";

    std::vector<fs::path> written;
    progress::Scope scope("Game export", double(jobs.size()));
    for (const Job& j : jobs)
    {
        std::error_code ec;
        fs::create_directories(j.file.parent_path(), ec);
        scope.setStatus("Exporting " + paths::toUtf8(j.file.filename()));
        if (obj)
        {
            json names = json::array();
            for (NodeId id : j.nodes)
            {
                std::vector<NodeId> all{id};
                for (NodeId d : g.descendants(id))
                    all.push_back(d);
                for (NodeId n : all)
                    if (g.node(n)->type().isA("mesh"))
                        names.push_back(g.node(n)->name());
            }
            if (names.empty())
            {
                scope.step();
                continue;
            }
            names.insert(names.begin(), paths::toUtf8(j.file));
            ops::execute("exportObj", {{"args", names}});
        }
        else
        {
            ExportScene snapshot = gatherExport(g, j.nodes, opts);
            for (const std::string& w : snapshot.warnings)
                log::warn("gameExport: {}", w);
            if (snapshot.nodes.empty())
            {
                scope.step();
                continue;
            }
            std::string err;
            bool ok = false;
            progress::runOnWorker([&] { ok = writeFbx(snapshot, j.file, opts, &err); });
            if (!ok)
                throw std::runtime_error(paths::toUtf8(j.file) + ": " + err);
        }
        written.push_back(j.file);
        scope.step();
    }
    return written;
}

void registerCommands()
{
    registerPresetType();
    if (ops::find("gameExport"))
        return;

    std::vector<FlagDef> exportFlags = settingFlags();
    exportFlags.push_back({"preset", "pr", T::String, nullptr, "Start from a preset (the scene's first, then the prefs'); flags override it."});
    exportFlags.push_back({"dryRun", "dr", T::Bool, false, "Only list the files and what goes in each, writing nothing."});
    ops::registerCommand({
        .name = "gameExport",
        .doc = "Maya's Game Exporter (Model tab): export everything, the selection, or object sets to FBX or OBJ, as one "
               "file, a file per set or a file per object. Returns the paths written, or with dryRun=True "
               "[{file, nodes}] without writing.",
        .flags = exportFlags,
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            json s = json::object();
            if (a.has("preset"))
            {
                const std::string name = a.get<std::string>("preset");
                json scene = scenePresets(g), prefs = prefPresets();
                if (scene.contains(name))
                    s = scene[name];
                else if (prefs.contains(name))
                    s = prefs[name];
                else
                    throw CommandError("gameExport: no preset named " + name);
            }
            const json given = settingsFrom(a);
            for (auto& [k, v] : given.items())
                s[k] = v;
            try
            {
                if (a.get<bool>("dryRun", false))
                {
                    json out = json::array();
                    for (const Job& j : plan(g, s))
                    {
                        json nodes = json::array();
                        for (NodeId id : j.nodes)
                            nodes.push_back(g.node(id)->name());
                        out.push_back({{"file", paths::toUtf8(j.file)}, {"nodes", nodes}});
                    }
                    return out;
                }
                json out = json::array();
                for (const fs::path& p : run(g, s))
                    out.push_back(paths::toUtf8(p));
                return out;
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("gameExport: ") + e.what());
            }
        },
    });

    ops::registerCommand({
        .name = "gameExportPresets",
        .doc = "Game exporter presets: {\"scene\": {name: settings}, \"prefs\": {name: settings}, \"defaults\": settings}.",
        .undoable = false,
        .run = [](const Args&) -> json {
            return {{"scene", scenePresets(Graph::current())}, {"prefs", prefPresets()}, {"defaults", defaults()}};
        },
    });

    std::vector<FlagDef> saveFlags = settingFlags();
    saveFlags.push_back({"settings", "", T::Any, nullptr, "The whole settings object; the other flags are laid over it."});
    saveFlags.push_back({"scene", "", T::Bool, false, "Keep it in the scene (a gameExportPreset node) instead of the prefs."});
    ops::registerCommand({
        .name = "gameExportSavePreset",
        .doc = "Save game exporter settings under a name, in the prefs or (scene=True) in the scene. Replaces a preset of "
               "that name. Returns the settings saved.",
        .positionalDoc = "name",
        .flags = saveFlags,
        .run = [](const Args& a) -> json {
            if (a.positional.empty() || !a.positional[0].is_string() || a.positional[0].get<std::string>().empty())
                throw CommandError("gameExportSavePreset: name the preset");
            const std::string name = a.positional[0].get<std::string>();
            json s = a.has("settings") && a.flags["settings"].is_object() ? a.flags["settings"] : json::object();
            const json given = settingsFrom(a);
            for (auto& [k, v] : given.items())
                s[k] = v;
            Graph& g = Graph::current();
            if (a.get<bool>("scene", false))
            {
                Node* n = presetNode(g, name);
                if (!n)
                {
                    n = g.createNode(*scene::types::find(kPresetType), "gameExportPreset1");
                    g.setValue({n->id(), kPresetName, -1}, name);
                }
                g.setValue({n->id(), kPresetSettings, -1}, s.dump());
            }
            else
                PrefStore::get().set(kPrefFile, "/presets/" + name, s);
            return s;
        },
    });

    ops::registerCommand({
        .name = "gameExportDeletePreset",
        .doc = "Delete a game exporter preset from the prefs or (scene=True) the scene.",
        .positionalDoc = "name",
        .flags = {{"scene", "", T::Bool, false, "Delete the scene's preset instead of the prefs'."}},
        .run = [](const Args& a) -> json {
            if (a.positional.empty() || !a.positional[0].is_string())
                throw CommandError("gameExportDeletePreset: name the preset");
            const std::string name = a.positional[0].get<std::string>();
            Graph& g = Graph::current();
            if (a.get<bool>("scene", false))
            {
                Node* n = presetNode(g, name);
                if (!n)
                    throw CommandError("gameExportDeletePreset: no scene preset named " + name);
                g.deleteNode(n->id());
            }
            else
            {
                if (!prefPresets().contains(name))
                    throw CommandError("gameExportDeletePreset: no preset named " + name);
                PrefStore::get().remove(kPrefFile, "/presets/" + name);
            }
            return nullptr;
        },
    });
}
} // namespace lila::fbx::game
