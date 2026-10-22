#include "extensions/ExtensionManager.h"

#include "cmds/Command.h"
#include "cmds/Undo.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "core/Workspace.h"
#include "extensions/Library.h"
#include "extensions/LilaExtension.h"
#include "python/Python.h"
#include "scene/Graph.h"

#include <algorithm>
#include <cstdlib>

namespace lila::extensions
{
namespace
{
const char* kindName(ExtensionKind k)
{
    return k == ExtensionKind::Native ? "native" : "python";
}

std::string pathKey(const fs::path& p)
{
    std::string s = p.lexically_normal().generic_string();
#ifdef _WIN32
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
#endif
    return s;
}

/** What kind of extension lives at path, if any. */
std::optional<ExtensionInfo> probe(const fs::path& path)
{
    std::error_code ec;
    std::string stem = path.stem().string();
    if (stem.empty() || stem[0] == '_' || stem[0] == '.')
        return std::nullopt;
    ExtensionInfo info;
    info.path = path;
    if (fs::is_directory(path, ec))
    {
        if (!fs::is_regular_file(path / "__init__.py", ec))
            return std::nullopt;
        info.name = path.filename().string();
        info.kind = ExtensionKind::Python;
        return info;
    }
    if (!fs::is_regular_file(path, ec))
        return std::nullopt;
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    info.name = stem;
    if (ext == ".py")
    {
        info.kind = ExtensionKind::Python;
        return info;
    }
    if (ext == Library::extension())
    {
#ifndef _WIN32
        if (info.name.starts_with("lib"))
            info.name = info.name.substr(3);
#endif
        info.kind = ExtensionKind::Native;
        return info;
    }
    return std::nullopt;
}

void refreshPythonCommands()
{
    if (python::initialized())
        python::run("__import__('lila.cmds', fromlist=['refresh']).refresh()", "<extensions>");
}

std::string pyString(const std::string& s)
{
    return json(s).dump();
}

// Context callbacks: everything a native extension registers goes through here to get its owner tag.
const char* ownerOf(LilaExtensionContext* ctx)
{
    return ctx && ctx->extensionName ? ctx->extensionName : "";
}

bool hostRegisterCommand(LilaExtensionContext* ctx, CommandDef* def)
{
    if (!def)
        return false;
    if (const CommandDef* existing = cmds::find(def->name); existing && existing->owner != ownerOf(ctx))
    {
        log::error("extension {}: command '{}' already exists{}", ownerOf(ctx), def->name,
                   existing->owner.empty() ? std::string() : " (from extension " + existing->owner + ")");
        return false;
    }
    def->owner = ownerOf(ctx);
    cmds::registerCommand(std::move(*def));
    return true;
}

void hostUnregisterCommand(LilaExtensionContext* ctx, const char* name)
{
    if (const CommandDef* d = cmds::find(name ? name : ""); d && d->owner == ownerOf(ctx))
        cmds::unregisterCommand(name);
}

const scene::NodeType* hostRegisterType(LilaExtensionContext* ctx, scene::NodeType* type)
{
    if (!type)
        return nullptr;
    type->owner = ownerOf(ctx);
    if (type->doc.empty())
        log::warn("extension {}: node type '{}' has no doc, so menus and tooltips can't say what it does", ownerOf(ctx), type->name);
    try
    {
        return &scene::types::registerType(std::move(*type));
    }
    catch (const std::exception& e)
    {
        log::error("extension {}: {}", ownerOf(ctx), e.what());
        return nullptr;
    }
}

const scene::NodeType* hostFindType(LilaExtensionContext*, const char* name)
{
    return scene::types::find(name ? name : "");
}

bool hostRunPython(LilaExtensionContext* ctx, const char* code)
{
    if (!python::initialized())
    {
        log::warn("extension {}: Python is not running", ownerOf(ctx));
        return false;
    }
    return python::run(code ? code : "", std::string("<extension ") + ownerOf(ctx) + ">").ok;
}

void hostLog(LilaExtensionContext* ctx, int level, const char* text)
{
    LogLevel l = level >= 0 && level <= int(LogLevel::Echo) ? LogLevel(level) : LogLevel::Info;
    log::write(l, std::string("[") + ownerOf(ctx) + "] " + (text ? text : ""));
}
} // namespace

json ExtensionInfo::toJson() const
{
    return {{"name", name}, {"path", path.string()}, {"kind", kindName(kind)}, {"loaded", loaded}, {"autoload", autoload}};
}

struct ExtensionManager::Loaded
{
    ExtensionInfo info;
    std::unique_ptr<Library> lib;
    std::unique_ptr<LilaExtensionContext> ctx;
    std::string name;
    std::string path;
};

ExtensionManager& ExtensionManager::get()
{
    static ExtensionManager m;
    return m;
}

ExtensionManager::ExtensionManager() = default;

ExtensionManager::~ExtensionManager()
{
    // Static teardown: registries may still hold code from these libraries, so they stay mapped.
    for (auto& [n, l] : mLoaded)
        if (l->lib)
            l->lib->leak();
}

void ExtensionManager::addSearchPath(const fs::path& dir)
{
    mExtraPaths.push_back(dir);
}

std::vector<fs::path> ExtensionManager::searchPaths() const
{
    std::vector<fs::path> out = mExtraPaths;
    if (const char* env = std::getenv("LILA_EXTENSION_PATH"); env && *env)
    {
#ifdef _WIN32
        const char sep = ';';
#else
        const char sep = ':';
#endif
        std::string_view all(env);
        while (!all.empty())
        {
            size_t end = all.find(sep);
            if (std::string_view d = all.substr(0, end); !d.empty())
                out.emplace_back(d);
            all = end == std::string_view::npos ? std::string_view() : all.substr(end + 1);
        }
    }
    json prefPaths = PrefStore::get().value("general", "/extensions/paths", json::array());
    if (prefPaths.is_array())
        for (const json& p : prefPaths)
            if (p.is_string())
                out.emplace_back(p.get<std::string>());
    if (Workspace::get().isOpen())
        out.push_back(Workspace::get().root() / ".lila" / "extensions");
    out.push_back(paths::userConfigDir() / "extensions");
    out.push_back(paths::executableDir() / "extensions");

    std::vector<fs::path> unique;
    for (fs::path& p : out)
        p = p.lexically_normal().make_preferred();
    for (const fs::path& p : out)
        if (std::none_of(unique.begin(), unique.end(), [&](const fs::path& u) { return pathKey(u) == pathKey(p); }))
            unique.push_back(p);
    return unique;
}

std::vector<ExtensionInfo> ExtensionManager::available() const
{
    LILA_ZONE();

    std::vector<ExtensionInfo> out;
    std::vector<std::string> auto_ = autoloadList();
    auto add = [&](ExtensionInfo info) {
        if (std::any_of(out.begin(), out.end(), [&](const ExtensionInfo& o) { return o.name == info.name; }))
            return;
        info.loaded = isLoaded(info.name);
        info.autoload = std::find(auto_.begin(), auto_.end(), info.name) != auto_.end();
        out.push_back(std::move(info));
    };

    for (auto& [n, l] : mLoaded)
        add(l->info);
    for (const fs::path& dir : searchPaths())
    {
        std::error_code ec;
        if (!fs::is_directory(dir, ec))
            continue;
        std::vector<fs::path> entries;
        for (const auto& e : fs::directory_iterator(dir, ec))
            entries.push_back(e.path());
        std::sort(entries.begin(), entries.end());
        for (const fs::path& e : entries)
            if (auto info = probe(e))
                add(*info);
    }
    std::sort(out.begin(), out.end(), [](const ExtensionInfo& a, const ExtensionInfo& b) { return a.name < b.name; });
    return out;
}

std::optional<ExtensionInfo> ExtensionManager::find(const std::string& nameOrPath) const
{
    std::error_code ec;
    fs::path p(nameOrPath);
    bool looksLikePath = p.has_parent_path() || p.has_extension();
    if (looksLikePath && fs::exists(p, ec))
    {
        if (auto info = probe(fs::absolute(p, ec)))
            return info;
        throw ExtensionError("not an extension: " + nameOrPath);
    }
    for (const ExtensionInfo& info : available())
        if (info.name == nameOrPath)
            return info;
    return std::nullopt;
}

std::string ExtensionManager::load(const std::string& nameOrPath)
{
    LILA_ZONE();

    std::optional<ExtensionInfo> found = find(nameOrPath);
    if (!found)
    {
        std::string dirs;
        for (const fs::path& d : searchPaths())
            dirs += "\n  " + d.string();
        throw ExtensionError("no extension named '" + nameOrPath + "' on the search paths:" + dirs);
    }
    ExtensionInfo info = *found;
    if (auto it = mLoaded.find(info.name); it != mLoaded.end())
    {
        if (pathKey(it->second->info.path) != pathKey(info.path))
            throw ExtensionError("a different extension named '" + info.name + "' is already loaded from " + it->second->info.path.string());
        return info.name;
    }

    auto rec = std::make_unique<Loaded>();
    rec->info = info;
    rec->info.loaded = true;
    rec->name = info.name;
    rec->path = info.path.string();

    if (info.kind == ExtensionKind::Native)
    {
        std::string err;
        rec->lib = Library::open(info.path, &err);
        if (!rec->lib)
            throw ExtensionError("could not load " + info.path.string() + ": " + err);
        auto init = reinterpret_cast<LilaExtensionInitFn>(rec->lib->symbol("lilaExtensionInit"));
        if (!init)
            throw ExtensionError(info.path.string() + " does not export lilaExtensionInit");

        rec->ctx = std::make_unique<LilaExtensionContext>();
        LilaExtensionContext& c = *rec->ctx;
        c.extensionName = rec->name.c_str();
        c.extensionPath = rec->path.c_str();
        c.registerCommand = hostRegisterCommand;
        c.unregisterCommand = hostUnregisterCommand;
        c.registerType = hostRegisterType;
        c.findType = hostFindType;
        c.runPython = hostRunPython;
        c.log = hostLog;
        c.host = this;

        int rc = 0;
        try
        {
            rc = init(&c);
        }
        catch (const std::exception& e)
        {
            removeOwned(info.name);
            throw ExtensionError("extension " + info.name + " threw during init: " + e.what());
        }
        if (rc != 0)
        {
            removeOwned(info.name);
            throw ExtensionError("extension " + info.name + " failed to initialize (code " + std::to_string(rc) + ")");
        }
    }
    else
    {
        if (!python::initialized())
            throw ExtensionError("can't load Python extension " + info.name + ": Python is not running");
        python::Result r = python::run("__import__('lila.extension', fromlist=['_load'])._load(" + pyString(info.name) + ", " +
                                           pyString(info.path.string()) + ")",
                                       "<loadExtension>");
        if (!r.ok)
        {
            removeOwned(info.name);
            throw ExtensionError("Python extension " + info.name + " failed to load:\n" + r.value);
        }
    }

    mLoaded[info.name] = std::move(rec);
    if (info.kind == ExtensionKind::Native)
        refreshPythonCommands();
    log::info("loaded extension {} ({})", info.name, info.path.string());
    return info.name;
}

void ExtensionManager::checkUnloadable(const std::string& name) const
{
    scene::Graph& g = scene::Graph::current();
    std::vector<std::string> users;
    for (scene::NodeId id : g.nodes())
        if (scene::Node* n = g.node(id); n && n->type().owner == name)
            users.push_back(n->name());
    if (users.empty())
        return;
    std::string list;
    for (size_t i = 0; i < users.size() && i < 5; ++i)
        list += (i ? ", " : "") + users[i];
    if (users.size() > 5)
        list += ", ...";
    throw ExtensionError("can't unload " + name + ": the scene has " + std::to_string(users.size()) + " node(s) of its types (" + list +
                      "). Delete them first.");
}

void ExtensionManager::removeOwned(const std::string& name)
{
    LILA_ZONE();

    std::vector<std::string> commands;
    for (const CommandDef* d : cmds::all())
        if (d->owner == name)
            commands.push_back(d->name);
    for (const std::string& c : commands)
        cmds::unregisterCommand(c);

    std::vector<std::string> types;
    for (const scene::NodeType* t : scene::types::all())
        if (t->owner == name)
            types.push_back(t->name);
    for (const std::string& t : types)
        scene::types::unregisterType(t);
}

void ExtensionManager::unload(const std::string& name)
{
    LILA_ZONE();

    auto it = mLoaded.find(name);
    if (it == mLoaded.end())
        throw ExtensionError("extension '" + name + "' is not loaded");
    checkUnloadable(name);
    Loaded& rec = *it->second;

    if (rec.info.kind == ExtensionKind::Native)
    {
        if (auto uninit = reinterpret_cast<LilaExtensionUninitFn>(rec.lib->symbol("lilaExtensionUninit")))
        {
            try
            {
                uninit(rec.ctx.get());
            }
            catch (const std::exception& e)
            {
                log::error("extension {} threw during uninit: {}", name, e.what());
            }
        }
        removeOwned(name);
        // Undo steps may hold actions whose code lives in the library.
        UndoStack::get().clear();
    }
    else if (python::initialized())
    {
        python::run("__import__('lila.extension', fromlist=['_unload'])._unload(" + pyString(name) + ")", "<unloadExtension>");
        removeOwned(name);
    }

    ExtensionKind kind = rec.info.kind;
    mLoaded.erase(it);
    if (kind == ExtensionKind::Native)
        refreshPythonCommands();
    log::info("unloaded extension {}", name);
}

std::string ExtensionManager::reload(const std::string& name)
{
    auto it = mLoaded.find(name);
    if (it == mLoaded.end())
        return load(name);
    fs::path path = it->second->info.path;
    unload(name);
    return load(path.string());
}

void ExtensionManager::unloadAll()
{
    std::vector<std::string> names = loaded();
    for (auto n = names.rbegin(); n != names.rend(); ++n)
    {
        try
        {
            unload(*n);
        }
        catch (const ExtensionError& e)
        {
            log::warn("{}", e.what());
        }
    }
}

bool ExtensionManager::isLoaded(const std::string& name) const
{
    return mLoaded.contains(name);
}

std::vector<std::string> ExtensionManager::loaded() const
{
    std::vector<std::string> out;
    for (auto& [n, l] : mLoaded)
        out.push_back(n);
    return out;
}

json ExtensionManager::describe(const std::string& name) const
{
    std::optional<ExtensionInfo> info;
    if (auto it = mLoaded.find(name); it != mLoaded.end())
        info = it->second->info;
    else
        info = find(name);
    if (!info)
        throw ExtensionError("no extension named '" + name + "'");
    std::vector<std::string> auto_ = autoloadList();
    info->autoload = std::find(auto_.begin(), auto_.end(), info->name) != auto_.end();

    json j = info->toJson();
    json commands = json::array();
    for (const CommandDef* d : cmds::all())
        if (d->owner == info->name)
            commands.push_back(d->name);
    json types = json::array();
    for (const scene::NodeType* t : scene::types::all())
        if (t->owner == info->name)
            types.push_back(t->name);
    j["commands"] = commands;
    j["types"] = types;
    return j;
}

std::vector<std::string> ExtensionManager::autoloadList() const
{
    std::vector<std::string> out;
    json list = PrefStore::get().value("general", "/extensions/autoload", json::array());
    if (list.is_array())
        for (const json& n : list)
            if (n.is_string())
                out.push_back(n.get<std::string>());
    return out;
}

void ExtensionManager::setAutoload(const std::string& name, bool on)
{
    std::vector<std::string> list = autoloadList();
    auto it = std::find(list.begin(), list.end(), name);
    if (on == (it != list.end()))
        return;
    if (on)
        list.push_back(name);
    else
        list.erase(it);
    PrefStore::get().set("general", "/extensions/autoload", list);
}

void ExtensionManager::loadAutoload()
{
    LILA_ZONE();

    for (const std::string& name : autoloadList())
    {
        try
        {
            load(name);
        }
        catch (const ExtensionError& e)
        {
            log::error("autoload: {}", e.what());
        }
    }
}

namespace
{
std::string nameArg(const Args& a, const char* cmd)
{
    for (const json& v : a.positional)
        if (v.is_string() && !v.get<std::string>().empty())
            return v.get<std::string>();
    throw CommandError(std::string(cmd) + ": give an extension name");
}

template <class F> json guarded(F&& f)
{
    try
    {
        return f();
    }
    catch (const ExtensionError& e)
    {
        throw CommandError(e.what());
    }
}
} // namespace

void registerCommands()
{
    using T = ArgType;
    ExtensionManager& pm = ExtensionManager::get();

    cmds::registerCommand({
        .name = "loadExtension",
        .doc = "Load an extension by name (from the extension search paths) or by path. Returns its name.",
        .positionalDoc = "name or path",
        .flags = {{"autoload", "", T::Bool, nullptr, "Also load it at every startup (or stop doing so with False)."}},
        .undoable = false,
        .run = [&pm](const Args& a) -> json {
            std::string target = nameArg(a, "loadExtension");
            return guarded([&] {
                std::string name = pm.load(target);
                if (a.has("autoload"))
                    pm.setAutoload(name, a.get<bool>("autoload"));
                return json(name);
            });
        },
    });

    cmds::registerCommand({
        .name = "unloadExtension",
        .doc = "Unload an extension, removing its commands and node types. Fails while the scene uses its node types.",
        .positionalDoc = "name",
        .undoable = false,
        .run = [&pm](const Args& a) -> json {
            std::string name = nameArg(a, "unloadExtension");
            return guarded([&] {
                pm.unload(name);
                return json(name);
            });
        },
    });

    cmds::registerCommand({
        .name = "reloadExtension",
        .doc = "Unload and load an extension again from the same file, picking up changes.",
        .positionalDoc = "name",
        .undoable = false,
        .run = [&pm](const Args& a) -> json {
            std::string name = nameArg(a, "reloadExtension");
            return guarded([&] { return json(pm.reload(name)); });
        },
    });

    cmds::registerCommand({
        .name = "listExtensions",
        .doc = "Extensions on the search paths: [{name, path, kind, loaded, autoload}].",
        .flags = {
            {"loaded", "", T::Bool, false, "Only loaded extensions."},
            {"searchPaths", "", T::Bool, false, "Return the search paths instead."},
        },
        .undoable = false,
        .run = [&pm](const Args& a) -> json {
            json out = json::array();
            if (a.get<bool>("searchPaths", false))
            {
                for (const fs::path& p : pm.searchPaths())
                    out.push_back(p.string());
                return out;
            }
            bool onlyLoaded = a.get<bool>("loaded", false);
            for (const ExtensionInfo& p : pm.available())
                if (!onlyLoaded || p.loaded)
                    out.push_back(p.toJson());
            return out;
        },
    });

    cmds::registerCommand({
        .name = "extensionInfo",
        .doc = "One extension: {name, path, kind, loaded, autoload, commands, types}.",
        .positionalDoc = "name",
        .flags = {{"autoload", "", T::Bool, nullptr, "Set whether it loads at startup."}},
        .undoable = false,
        .run = [&pm](const Args& a) -> json {
            std::string name = nameArg(a, "extensionInfo");
            return guarded([&] {
                if (a.has("autoload"))
                    pm.setAutoload(name, a.get<bool>("autoload"));
                return pm.describe(name);
            });
        },
    });
}

void autoload()
{
    ExtensionManager::get().loadAutoload();
}
} // namespace lila::extensions
