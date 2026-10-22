#include "io/SceneFile.h"

#include "cmds/Command.h"
#include "cmds/Undo.h"
#include "core/Log.h"
#include "core/PackedArray.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "core/Progress.h"
#include "core/Workspace.h"
#include "io/References.h"
#include "io/SceneHistory.h"
#include "io/SceneFormats.h"
#include "scene/Anim.h"
#include "scene/Camera.h"
#include "scene/Graph.h"
#include "scene/RenderSettings.h"
#include "scene/SceneScripts.h"

#include <format>
#include <future>
#include <map>
#include <fstream>
#include <optional>
#include <random>
#include <sstream>
#include <thread>

namespace lila::io
{
namespace fs = std::filesystem;
using scene::Graph;
using scene::NodeId;

namespace
{
constexpr int kVersion = 1;

/** .lilaa is ASCII; everything else is binary. */
void encodeFor(const fs::path& path, json doc, const std::function<void(std::string_view)>& sink)
{
    if (path.extension() != ".lilaa")
        return encodeBinary(std::move(doc), sink);
    packed::unpackAll(doc);
    sink(encodeAscii(doc));
}

struct State
{
    fs::path path;
    bool modified = false;
    bool autosaveDirty = false;
    bool loading = false;
    uint64_t cleanStateId = 0;
    bool untracked = false; // changed outside the undo stack, so no undo can return to the saved state
    std::optional<uint64_t> autosavedStateId; // undo state the last autosave wrote, while no untracked edit followed

    int listener = 0;
    std::string title;
    std::vector<std::function<void()>> callbacks;
    std::vector<std::function<void(const fs::path&)>> savedCallbacks;

    struct UiSection
    {
        std::function<json()> save;
        std::function<void(const json&)> load;
    };
    std::map<std::string, UiSection> uiSections;
    json ui = json::object(); // the open scene's "ui" document, unregistered sections included
};

State& state()
{
    static State s;
    return s;
}

void notify()
{
    for (auto& fn : state().callbacks)
        fn();
}

void setModified(bool m)
{
    State& s = state();
    if (m)
        s.autosaveDirty = true;
    if (s.modified == m)
        return;
    s.modified = m;
    notify();
}

void ensureListener()
{
    State& s = state();
    if (s.listener)
        return;
    s.listener = Graph::current().addListener([](scene::Change c, NodeId) {
        if (state().loading || c == scene::Change::Selection)
            return;
        if (c == scene::Change::Cleared)
        {
            state().title.clear();
            state().ui = json::object();
            refs::clear();
            scene::renderSettings().fromJson({});
            scene::renderSettingsChanged();
            scene::scripts::clear();
        }
        if (c == scene::Change::Cleared)
            return setModified(false);
        UndoStack& u = UndoStack::get();
        if (u.replaying())
        {
            state().autosaveDirty = true;
            return;
        }
        if (!u.enabled())
            state().untracked = true;
        setModified(true);
    }, true);
    scene::addRenderSettingsListener([] {
        if (state().loading || UndoStack::get().replaying())
            return;
        state().untracked = true;
        setModified(true);
    });
    UndoStack::get().addListener([] {
        State& s = state();
        if (!s.loading)
            setModified(s.untracked || UndoStack::get().stateId() != s.cleanStateId);
    });
}

using Sink = std::function<void(std::string_view)>;

/**
 * Writes path.tmp through fill(sink), then swaps it in. beforeReplace runs between the two, for work that must finish
 * before the old file goes away.
 */
void writeAtomic(const fs::path& path, const std::function<void(const Sink&)>& fill, const std::function<void()>& beforeReplace = {})
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            throw std::runtime_error("can't write " + tmp.string());
        fill([&](std::string_view piece) {
            if (!out.write(piece.data(), std::streamsize(piece.size())))
                throw std::runtime_error("can't write " + tmp.string());
        });
        if (!out.flush())
            throw std::runtime_error("can't write " + tmp.string());
    }
    if (beforeReplace)
        beforeReplace();
    fs::rename(tmp, path, ec);
    if (ec)
        throw std::runtime_error("can't replace " + path.string() + ": " + ec.message());
}

void writeAtomic(const fs::path& path, const std::string& text)
{
    writeAtomic(path, [&](const Sink& sink) { sink(text); });
}

/** Depth-first so every parent is written before its children. */
void collectDag(Graph& g, NodeId id, std::vector<NodeId>& out)
{
    out.push_back(id);
    for (NodeId c : g.children(id))
        collectDag(g, c, out);
}
} // namespace

nlohmann::json serialize(const fs::path& sceneDir, bool packed)
{
    LILA_ZONE();

    Graph& g = Graph::current();
    std::vector<NodeId> order;
    for (NodeId r : g.roots())
        collectDag(g, r, order);
    for (NodeId id : g.nodes())
        if (!g.node(id)->type().dag)
            order.push_back(id);

    // Values first, all at once, so meshes convert in parallel.
    std::erase_if(order, [&](NodeId id) { return g.node(id)->referenceId() != 0; }); // saved as the reference plus its edits
    std::vector<const scene::PlugValue*> values;
    std::vector<std::vector<int>> stored(order.size());
    for (size_t k = 0; k < order.size(); ++k)
    {
        scene::Node* n = g.node(order[k]);
        const auto& defs = n->type().attrs;
        for (int i = 0; i < int(defs.size()); ++i)
        {
            const scene::AttrDef& a = defs[i];
            if (!(a.flags & scene::AttrStorable) || !(a.flags & scene::AttrInput))
                continue;
            if (g.source({order[k], i, -1}))
                continue;
            if (n->value(i) != a.storedDefault())
            {
                stored[k].push_back(i);
                values.push_back(&n->value(i));
            }
        }
    }
    std::vector<json> converted = scene::toJsonAll(values, packed);

    const fs::path dir = sceneDir.empty() ? paths::currentSceneDir() : sceneDir;
    json nodes = json::array();
    size_t next = 0;
    for (size_t k = 0; k < order.size(); ++k)
    {
        const NodeId id = order[k];
        scene::Node* n = g.node(id);
        json attrs = json::object();
        for (int i : stored[k])
        {
            const scene::AttrDef& a = n->type().attrs[i];
            json& v = attrs[a.name] = std::move(converted[next++]);
            if ((a.flags & scene::AttrFile) && v.is_string())
                v = paths::storedPath(paths::fromUtf8(v.get<std::string>()), dir);
        }
        json node = {{"name", n->name()}, {"type", n->type().name}};
        if (NodeId p = g.parent(id); p != scene::kNoNode)
            node["parent"] = g.node(p)->name();
        if (!attrs.empty())
            node["attrs"] = std::move(attrs);
        json locked = json::array();
        for (int i = 0; i < int(n->type().attrs.size()); ++i)
            if (uint8_t mask = n->lockMask(i))
            {
                const scene::AttrDef& a = n->type().attrs[i];
                if (a.type != scene::PlugType::Vec3)
                    locked.push_back(a.name);
                else
                    for (int c = 0; c < 3; ++c)
                        if (mask & (1u << c))
                            locked.push_back(a.childNames[c]);
            }
        if (!locked.empty())
            node["locked"] = std::move(locked);
        nodes.push_back(std::move(node));
    }

    json connections = json::array();
    for (auto& [dst, src] : g.connections())
        if (!refs::isBaselineConnection(g.plugName(src), g.plugName(dst)))
            connections.push_back({g.plugName(src), g.plugName(dst)});

    json selection = json::array();
    for (NodeId id : g.selection())
        selection.push_back(g.node(id)->name());

    json doc = {
        {"lila", kVersion},
        {"upAxis", PrefStore::get().value<std::string>("general", "/scene/upAxis", "y")},
        {"units", "cm"},
        {"time", g.time()},
        {"nodes", std::move(nodes)},
        {"connections", std::move(connections)},
        {"selection", std::move(selection)},
        {"playback", scene::anim::playback().toJson()},
        {"renderSettings", scene::renderSettings().toJson()},
        {"references", refs::serialize(dir)},
    };
    if (!scene::scripts::all().empty())
        doc["scripts"] = scene::scripts::toJson();
    json ui = state().ui;
    for (auto& [key, section] : state().uiSections)
        if (json v = section.save(); v.is_null())
            ui.erase(key);
        else
            ui[key] = std::move(v);
    if (!ui.empty())
        doc["ui"] = std::move(ui);
    return doc;
}

void deserialize(const nlohmann::json& doc, const fs::path& sceneDir, double ownBytes)
{
    LILA_ZONE();

    if (!doc.is_object() || !doc.contains("lila"))
        throw std::runtime_error("not a Lila scene");
    if (doc["lila"].get<int>() > kVersion)
        throw std::runtime_error("scene was saved by a newer Lila (format " + doc["lila"].dump() + ")");

    Graph& g = Graph::current();
    State& s = state();
    s.loading = true;
    std::vector<std::string> warnings;
    const json references = doc.value("references", json::array());
    const double refBytes = refs::loadedBytes(references, sceneDir);
    progress::Scope scope("", refBytes + (ownBytes > 0.0 ? ownBytes : 1e6));
    Graph::Batch batch(g);
    try
    {
        g.clear();
        UndoStack::get().setEnabled(false);
        // References first, so scene nodes parented under referenced ones find their parents.
        refs::deserialize(references, sceneDir, warnings, refBytes);
        {
            progress::Scope own("", 0.0, progress::None, ownBytes > 0.0 ? ownBytes : 1e6);
            instantiate(doc, "", 0, sceneDir, warnings);
        }
        refs::resolvePendingParents(warnings);

        std::vector<NodeId> sel;
        for (const json& n : doc.value("selection", json::array()))
            if (scene::Node* node = g.find(n.get<std::string>()))
                sel.push_back(node->id());
        g.select(sel);
        g.setTime(doc.value("time", 0.0));
        if (doc.contains("playback"))
        {
            scene::anim::playback().fromJson(doc["playback"]);
            scene::anim::playbackChanged();
        }
        scene::renderSettings().fromJson(doc.value("renderSettings", json::object()));
        scene::camera::migrateNamedSettings(g, scene::renderSettings());
        scene::renderSettingsChanged();
        scene::scripts::fromJson(doc.value("scripts", json::array()));
    }
    catch (...)
    {
        UndoStack::get().setEnabled(true);
        s.loading = false;
        throw;
    }
    UndoStack::get().setEnabled(true);
    UndoStack::get().clear();
    s.loading = false;

    json ui = doc.value("ui", json::object());
    s.ui = ui.is_object() ? ui : json::object();
    for (auto& [key, section] : s.uiSections)
        section.load(s.ui.value(key, json()));

    for (const std::string& w : warnings)
        log::warn("{}", w);
}

void registerUiSection(const std::string& key, std::function<json()> save, std::function<void(const json&)> load)
{
    state().uiSections[key] = {std::move(save), std::move(load)};
}

void save(const fs::path& path)
{
    LILA_ZONE();

    progress::Scope scope(std::format("Saving {}...", path.filename().string()), 3.0);
    // The backup copies the file as it is now, so it runs alongside gathering and encoding the new one.
    const int keep = PrefStore::get().value<int>("general", "/backups/keep", 10);
    std::future<void> backup;
    if (keep > 0)
        backup = std::async(std::launch::async, [path, folder = history::backupFolder(path), keep] {
            history::backup(path, folder, keep);
        });
    json doc;
    {
        progress::Scope s("Gathering the scene");
        doc = serialize(path.parent_path(), path.extension() != ".lilaa");
    }
    progress::Scope s("Writing " + path.filename().string(), 0.0, progress::None, 2.0);
    progress::runOnWorker([&] {
        writeAtomic(path, [&](const Sink& sink) { encodeFor(path, std::move(doc), sink); }, [&] {
            if (backup.valid())
                backup.get();
        });
    });
    setCurrentPath(path);
    markClean();
    log::info("saved {}", path.string());
    for (auto& fn : state().savedCallbacks)
        fn(path);
}

void load(const fs::path& path)
{
    LILA_ZONE();

    const std::string name = path.filename().string();
    progress::Scope scope(loadingVerb() + "...", 10.0, progress::Cancellable);
    scope.setStatus(name);
    std::vector<std::string> warnings;
    json doc;
    {
        progress::Scope read("Reading " + name, 0.0, progress::None, 3.0);
        progress::runOnWorker([&] { doc = readSceneFile(path, &warnings); });
    }
    scope.checkCancel();
    scope.setCancellable(false);
    for (const std::string& w : warnings)
        log::warn("{}: {}", name, w);
    {
        std::error_code ec;
        const auto size = fs::file_size(path, ec);
        progress::Scope build("", 0.0, progress::None, 7.0);
        deserialize(doc, path.parent_path(), ec ? 0.0 : double(size));
    }
    setCurrentPath(path);
    markClean();
    if (!Workspace::get().isOpen())
        if (auto ws = Workspace::detect(path))
            Workspace::get().open(*ws);
    log::info("opened {}", path.string());
    scene::scripts::sceneOpened();
}

std::string loadingVerb()
{
    static const char* const kVerbs[] = {
        "Perambulating", "Unfurling",          "Assembling",        "Summoning polygons", "Gathering vertices", "Untangling edges",
        "Rehydrating",   "Marshalling meshes", "Coaxing normals",   "Waking the scene",   "Stitching faces",    "Arranging things",
    };
    static std::mt19937 rng{std::random_device{}()};
    return kVerbs[std::uniform_int_distribution<size_t>(0, std::size(kVerbs) - 1)(rng)];
}

const fs::path& currentPath()
{
    return state().path;
}

void setCurrentPath(const fs::path& path)
{
    state().path = path;
    paths::setCurrentSceneDir(path.empty() ? fs::path() : path.parent_path());
    state().title.clear();
    notify();
}

bool modified()
{
    return state().modified;
}

bool upToDate(const fs::path& path)
{
    std::error_code ec;
    return !state().modified && !path.empty() && !state().path.empty() && fs::equivalent(path, state().path, ec);
}

void markClean()
{
    ensureListener();
    State& s = state();
    s.autosaveDirty = false;
    s.autosavedStateId.reset();
    s.cleanStateId = UndoStack::get().stateId();
    s.untracked = false;
    setModified(false);
}

void onSaved(std::function<void(const fs::path&)> fn)
{
    state().savedCallbacks.push_back(std::move(fn));
}

void onStateChanged(std::function<void()> fn)
{
    ensureListener();
    state().callbacks.push_back(std::move(fn));
}

void autosave()
{
    LILA_ZONE();

    State& s = state();
    // A timer can fire while a long load pumps events; the scene is half built then.
    if (!s.autosaveDirty || progress::modal())
        return;
    s.autosaveDirty = false;
    // Unmodified means the scene matches its saved file, and an equal state id means undo/redo led back to what the
    // last autosave already holds.
    const uint64_t stateId = UndoStack::get().stateId();
    if (!s.modified || (!s.untracked && s.autosavedStateId == stateId))
        return;
    s.autosavedStateId = s.untracked ? std::nullopt : std::optional(stateId);

    fs::path folder = history::autosaveFolder(s.path);
    std::string stem = history::stemFor(s.path);
    int keep = std::max(1, PrefStore::get().value<int>("general", "/autosave/keep", 8));
    json doc = serialize({}, true);
    doc["recovery"] = {{"kind", "autosave"}, {"scene", s.path.generic_string()}};
    // Encoding and the disk write happen off the main thread.
    std::thread([folder, stem, keep, doc = std::move(doc)]() mutable {
        progress::Scope scope("Autosaving", 0.0, progress::Silent);
        try
        {
            std::string text = encodeBinary(std::move(doc));
            std::error_code ec;
            fs::create_directories(folder, ec);
            writeAtomic(history::uniqueName(folder, stem, history::timestamp(), "autosave"), text);
            history::prune(folder, stem, "autosave", keep);
        }
        catch (const std::exception& e)
        {
            log::warn("autosave failed: {}", e.what());
        }
    }).detach();
}

void markModified()
{
    ensureListener();
    state().untracked = true;
    setModified(true);
}

void loadVersion(const fs::path& file, const fs::path& scene, bool keepPath, const std::string& title)
{
    LILA_ZONE();

    progress::Scope scope(loadingVerb() + "...", 10.0);
    scope.setStatus(file.filename().string());
    std::vector<std::string> warnings;
    json doc;
    {
        progress::Scope read("Reading " + file.filename().string(), 0.0, progress::None, 3.0);
        progress::runOnWorker([&] { doc = readSceneFile(file, &warnings); });
    }
    for (const std::string& w : warnings)
        log::warn("{}: {}", file.filename().string(), w);
    {
        std::error_code ec;
        const auto size = fs::file_size(file, ec);
        progress::Scope build("", 0.0, progress::None, 7.0);
        deserialize(doc, scene.empty() ? file.parent_path() : scene.parent_path(), ec ? 0.0 : double(size));
    }
    setCurrentPath(keepPath ? scene : fs::path());
    if (!keepPath)
    {
        state().title = title;
        // Paths in the version are stored for the scene it came from.
        if (!scene.empty())
            paths::setCurrentSceneDir(scene.parent_path());
    }
    markClean();
    markModified();
    if (!Workspace::get().isOpen() && !scene.empty())
        if (auto ws = Workspace::detect(scene))
            Workspace::get().open(*ws);
    log::info("opened {} as {}", file.string(), keepPath ? scene.string() : title);
}

std::string displayName()
{
    const State& s = state();
    if (!s.title.empty())
        return s.title;
    return s.path.empty() ? std::string("untitled") : s.path.filename().string();
}

fs::path emergencySave()
{
    try
    {
        State& s = state();
        if (!s.modified)
            return {};
        fs::path folder = history::autosaveFolder(s.path);
        json doc = serialize({}, true);
        doc["recovery"] = {{"kind", "emergency"}, {"scene", s.path.generic_string()}};
        std::error_code ec;
        fs::create_directories(folder, ec);
        fs::path target = history::uniqueName(folder, history::stemFor(s.path), history::timestamp(), "emergency");
        writeAtomic(target, encodeBinary(std::move(doc)));
        return target;
    }
    catch (...)
    {
        return {};
    }
}

void registerCommands()
{
    using T = ArgType;
    ensureListener();
    refs::registerCommands();

    cmds::registerCommand({
        .name = "saveScene",
        .doc = "Save the scene. With no path, saves to the current file. .lila is binary, .lilaa is text (diffable and "
               "repairable by hand).",
        .positionalDoc = "[path]",
        .flags = {{"ifModified", "im", T::Bool, false, "Skip the write when the file is already up to date: the current "
                                                      "file, still on disk, with no changes since."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            fs::path path = a.positional.empty() ? currentPath() : fs::path(a.positional[0].get<std::string>());
            if (path.empty())
                throw CommandError("saveScene: the scene has no file yet; pass a path");
            if (path.extension().empty())
                path += PrefStore::get().value<std::string>("general", "/scene/defaultFormat", "binary") == "ascii" ? ".lilaa" : ".lila";
            if (a.get<bool>("ifModified") && upToDate(path))
            {
                log::info("No changes to save");
                return path.generic_string();
            }
            try
            {
                save(path);
            }
            catch (const std::exception& e)
            {
                throw CommandError(e.what());
            }
            return path.generic_string();
        },
    });

    cmds::registerCommand({
        .name = "openScene",
        .doc = "Open a .lila or .lilaa scene, replacing the current one. Not undoable.",
        .positionalDoc = "path",
        .flags = {{"force", "f", T::Bool, false, "Discard unsaved changes without asking."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.positional.empty())
                throw CommandError("openScene: path required");
            try
            {
                load(fs::path(a.positional[0].get<std::string>()));
            }
            catch (const std::exception& e)
            {
                throw CommandError(e.what());
            }
            return currentPath().generic_string();
        },
    });

    cmds::registerCommand({
        .name = "sceneInfo",
        .doc = "Current scene file and whether it has unsaved changes.",
        .undoable = false,
        .run = [](const Args&) -> json {
            return {{"path", currentPath().empty() ? json(nullptr) : json(currentPath().generic_string())}, {"modified", modified()}};
        },
    });

    cmds::registerCommand({
        .name = "incrementAndSave",
        .doc = "Save to the next free incremental name (scene.lila -> scene_v002.lila -> scene_v003.lila) and make it "
               "the current file. Returns the new path.",
        .undoable = false,
        .run = [](const Args&) -> json {
            if (currentPath().empty())
                throw CommandError("incrementAndSave: the scene has no file yet; save it first");
            fs::path path = history::nextIncrement(currentPath());
            try
            {
                save(path);
            }
            catch (const std::exception& e)
            {
                throw CommandError(e.what());
            }
            return path.generic_string();
        },
    });

    cmds::registerCommand({
        .name = "sceneVersions",
        .doc = "Backups, autosaves, emergency saves and incremental versions of a scene on disk, newest first.",
        .positionalDoc = "[path]",
        .undoable = false,
        .run = [](const Args& a) -> json {
            fs::path scene = a.positional.empty() ? currentPath() : fs::path(a.positional[0].get<std::string>());
            json out = json::array();
            for (const history::Version& v : history::list(scene))
                out.push_back({{"path", v.path.generic_string()}, {"kind", history::kindName(v.kind)}, {"time", v.time}, {"size", v.size}});
            return out;
        },
    });
}
} // namespace lila::io
