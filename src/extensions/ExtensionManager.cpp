#include "extensions/ExtensionManager.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Events.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "core/Workspace.h"
#include "extensions/Library.h"
#include "lila/Extension.h"
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

bool isPackageDir(const fs::path& path)
{
    std::error_code ec;
    return fs::is_directory(path, ec) && fs::is_regular_file(path / ext::kManifestFile, ec);
}

/**
 * What kind of extension lives at path, if any. A package with a broken manifest comes back with its error set. On the
 * search paths packages are <Id>.lilaext folders; a path given explicitly may be any folder holding an extension.json.
 */
std::optional<ExtensionInfo> probe(const fs::path& path, bool explicitPath = false)
{
    std::error_code ec;
    std::string stem = path.stem().string();
    if (stem.empty() || stem[0] == '_' || stem[0] == '.')
        return std::nullopt;
    ExtensionInfo info;
    info.path = path;
    if (isPackageDir(path) && (explicitPath || path.extension() == ext::kPackageSuffix))
    {
        info.name = path.extension() == ext::kPackageSuffix ? stem : path.filename().string();
        try
        {
            info.manifest = ext::Manifest::load(path);
            info.name = info.manifest->id;
            info.kind = info.manifest->library() ? ExtensionKind::Native : ExtensionKind::Python;
        }
        catch (const ext::ManifestError& e)
        {
            info.error = e.what();
        }
        return info;
    }
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
    if (path.filename() == ext::kManifestFile)
        return probe(path.parent_path(), true);
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (extension != ".py")
        return std::nullopt;
    info.name = stem;
    info.kind = ExtensionKind::Python;
    return info;
}

void refreshPythonCommands()
{
    if (python::initialized())
        python::run("__import__('lila.ops', fromlist=['refresh']).refresh()", "<extensions>");
}

std::string pyString(const std::string& s)
{
    return json(s).dump();
}

std::vector<std::string> stringList(const char* key)
{
    std::vector<std::string> out;
    json list = PrefStore::get().value("general", key, json::array());
    if (list.is_array())
        for (const json& n : list)
            if (n.is_string())
                out.push_back(n.get<std::string>());
    return out;
}

bool contains(const std::vector<std::string>& v, const std::string& s)
{
    return std::find(v.begin(), v.end(), s) != v.end();
}
} // namespace

json ExtensionInfo::toJson() const
{
    json j = {{"name", name}, {"path", path.string()}, {"kind", kindName(kind)}, {"loaded", loaded}, {"autoload", autoload}};
    if (manifest)
    {
        j["displayName"] = manifest->name;
        j["version"] = manifest->version;
        j["vendor"] = manifest->vendor;
        j["description"] = manifest->description;
        j["sdk"] = manifest->sdk.str();
        j["enabledByDefault"] = manifest->enabledByDefault;
        json deps = json::array();
        for (const ext::Dependency& d : manifest->dependencies)
            deps.push_back(d.id);
        j["dependencies"] = deps;
    }
    if (!error.empty())
        j["error"] = error;
    return j;
}

struct ExtensionManager::Loaded
{
    ExtensionInfo info;
    std::unique_ptr<Library> lib;
    std::unique_ptr<ext::Context> ctx;
    bool python = false;
};

namespace
{
/** Loaded and loading extensions' contexts, for registrations made from their Python code. */
std::map<std::string, ext::Context*, std::less<>>& liveContexts()
{
    static std::map<std::string, ext::Context*, std::less<>> m;
    return m;
}
} // namespace

ext::Context* contextOf(std::string_view name)
{
    auto it = liveContexts().find(name);
    return it == liveContexts().end() ? nullptr : it->second;
}

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
    for (const std::string& p : stringList("/extensions/paths"))
        out.emplace_back(p);
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
    auto add = [&](ExtensionInfo info) {
        if (std::any_of(out.begin(), out.end(), [&](const ExtensionInfo& o) { return o.name == info.name; }))
            return;
        info.loaded = isLoaded(info.name);
        info.autoload = autoloads(info);
        if (info.error.empty())
            info.error = lastError(info.name);
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
        if (auto info = probe(fs::absolute(p, ec), true))
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
    std::set<std::string> visiting;
    loadInfo(*found, visiting);
    return found->name;
}

void ExtensionManager::loadInfo(const ExtensionInfo& found, std::set<std::string>& visiting)
{
    if (auto it = mLoaded.find(found.name); it != mLoaded.end())
    {
        if (pathKey(it->second->info.path) != pathKey(found.path))
            throw ExtensionError("a different extension named '" + found.name + "' is already loaded from " + it->second->info.path.string());
        return;
    }
    // A broken manifest; not a failed load, so it is not recorded as one.
    if (found.manifest == std::nullopt && !found.error.empty() && isPackageDir(found.path))
        throw ExtensionError(found.error);
    if (!visiting.insert(found.name).second)
        throw ExtensionError("extension " + found.name + " depends on itself through its dependencies");

    auto fail = [&](const std::string& why) {
        mErrors[found.name] = why;
        throw ExtensionError(why);
    };

    if (found.manifest)
    {
        const ext::Manifest& m = *found.manifest;
        if (std::string why = ext::sdkMismatch(m.sdk); !why.empty())
            fail("can't load " + m.id + " " + m.version + ": " + why);
        for (const ext::Dependency& d : m.dependencies)
        {
            std::optional<ExtensionInfo> dep;
            if (auto it = mLoaded.find(d.id); it != mLoaded.end())
                dep = it->second->info;
            else
                dep = find(d.id);
            if (!dep)
                fail(m.id + " needs the extension " + d.id + ", which is not on the search paths");
            const std::string depVersion = dep->manifest ? dep->manifest->version : std::string();
            if (!d.minVersion.empty() && ext::compareVersions(depVersion, d.minVersion) < 0)
                fail(m.id + " needs " + d.id + " " + d.minVersion + " or newer, found " + (depVersion.empty() ? "no version" : depVersion));
            try
            {
                loadInfo(*dep, visiting);
            }
            catch (const ExtensionError& e)
            {
                fail(m.id + " needs " + d.id + ": " + e.what());
            }
        }
    }

    auto rec = std::make_unique<Loaded>();
    rec->info = found;
    rec->info.loaded = true;
    rec->info.error.clear();
    const fs::path root = found.manifest ? found.manifest->root : found.path.parent_path();
    rec->ctx = std::make_unique<ext::Context>(found.name, root);
    liveContexts()[found.name] = rec->ctx.get();

    try
    {
        if (found.manifest)
        {
            if (found.manifest->library())
                loadNative(*rec);
            if (auto module = found.manifest->pythonModule())
                loadPython(*rec, *module);
        }
        else
            loadPython(*rec, found.path);
    }
    catch (const ExtensionError& e)
    {
        removeOwned(*rec);
        fail(e.what());
    }

    mErrors.erase(found.name);
    const ExtensionKind kind = rec->info.kind;
    mLoaded[found.name] = std::move(rec);
    mOrder.push_back(found.name);
    if (kind == ExtensionKind::Native)
        refreshPythonCommands();
    log::info("loaded extension {} ({})", found.name, found.path.string());
    events::send({.type = events::Event::ExtensionLoaded, .path = found.path, .name = found.name});
}

void ExtensionManager::loadNative(Loaded& rec)
{
    const ext::Manifest& m = *rec.info.manifest;
    const fs::path path = *m.library();
    std::error_code ec;
    if (!fs::is_regular_file(path, ec))
        throw ExtensionError(m.id + ": its library " + path.string() + " is missing (nothing built for " + ext::platformName() + "?)");
    std::string err;
    rec.lib = Library::open(path, &err);
    if (!rec.lib)
        throw ExtensionError("could not load " + path.string() + ": " + err);

    auto sdk = reinterpret_cast<LilaExtensionSdkVersionFn>(rec.lib->symbol("lilaExtensionSdkVersion"));
    if (!sdk)
        throw ExtensionError(path.string() + " does not export lilaExtensionSdkVersion: build it with the Lila SDK's lila/Extension.h");
    if (std::string why = ext::sdkMismatch(ext::SdkVersion::unpack(sdk())); !why.empty())
        throw ExtensionError("can't load " + m.id + ": its library was " + why);
    auto init = reinterpret_cast<LilaExtensionInitFn>(rec.lib->symbol("lilaExtensionInit"));
    if (!init)
        throw ExtensionError(path.string() + " does not export lilaExtensionInit");

    int rc = 0;
    try
    {
        owners::OwnerScope attribute(m.id);
        rc = init(*rec.ctx);
    }
    catch (const std::exception& e)
    {
        throw ExtensionError("extension " + m.id + " threw during init: " + e.what());
    }
    if (rc != 0)
        throw ExtensionError("extension " + m.id + " failed to initialize (code " + std::to_string(rc) + ")");
}

void ExtensionManager::loadPython(Loaded& rec, const fs::path& module)
{
    const std::string& name = rec.info.name;
    if (!python::initialized())
        throw ExtensionError("can't load the Python part of " + name + ": Python is not running");
    owners::OwnerScope attribute(name);
    python::Result r = python::run("__import__('lila.extension', fromlist=['_load'])._load(" + pyString(name) + ", " +
                                       pyString(module.string()) + ")",
                                   "<loadExtension>");
    if (!r.ok)
        throw ExtensionError("Python extension " + name + " failed to load:\n" + r.value);
    rec.python = true;
}

void ExtensionManager::checkUnloadable(const std::string& name) const
{
    for (const std::string& other : mOrder)
    {
        const ExtensionInfo& info = mLoaded.at(other)->info;
        if (info.manifest && std::any_of(info.manifest->dependencies.begin(), info.manifest->dependencies.end(),
                                         [&](const ext::Dependency& d) { return d.id == name; }))
            throw ExtensionError("can't unload " + name + ": " + other + " depends on it. Unload " + other + " first.");
    }

    scene::Graph& g = scene::Graph::current();
    std::vector<std::string> users;
    for (scene::NodeId id : g.nodes())
        if (scene::Node* n = g.node(id); n && n->type().owner == name)
            users.push_back(n->path());
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

void ExtensionManager::removeOwned(Loaded& rec)
{
    LILA_ZONE();

    const std::string& name = rec.info.name;
    if (rec.python && python::initialized())
    {
        python::run("__import__('lila.extension', fromlist=['_unload'])._unload(" + pyString(name) + ")", "<unloadExtension>");
        rec.python = false;
    }
    if (rec.lib)
    {
        if (auto uninit = reinterpret_cast<LilaExtensionUninitFn>(rec.lib->symbol("lilaExtensionUninit")))
        {
            try
            {
                uninit(*rec.ctx);
            }
            catch (const std::exception& e)
            {
                log::error("extension {} threw during uninit: {}", name, e.what());
            }
        }
    }
    if (rec.ctx)
        rec.ctx->unloadAll();
    liveContexts().erase(name);

    // Anything tagged with the owner that did not go through the context (Python registrations, for instance).
    ops::registry().removeOwned(name);
    for (const std::string& t : scene::types::registry().owned(name))
        scene::types::unregisterType(t);
    events::unsubscribeOwned(name);
    // Every other registry, before the library goes: entries made through calls that take no context still hold its code.
    owners::removeOwned(name);

    // Undo steps may hold actions whose code lives in the library.
    if (rec.lib)
        UndoStack::get().clear();
}

void ExtensionManager::unload(const std::string& name)
{
    LILA_ZONE();

    auto it = mLoaded.find(name);
    if (it == mLoaded.end())
        throw ExtensionError("extension '" + name + "' is not loaded");
    checkUnloadable(name);
    Loaded& rec = *it->second;
    removeOwned(rec);

    const ExtensionKind kind = rec.info.kind;
    const fs::path path = rec.info.path;
    mLoaded.erase(it);
    mOrder.erase(std::remove(mOrder.begin(), mOrder.end(), name), mOrder.end());
    if (kind == ExtensionKind::Native)
        refreshPythonCommands();
    log::info("unloaded extension {}", name);
    events::send({.type = events::Event::ExtensionUnloaded, .path = path, .name = name});
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
    std::vector<std::string> names = mOrder;
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
    return mOrder;
}

std::string ExtensionManager::lastError(const std::string& name) const
{
    auto it = mErrors.find(name);
    return it == mErrors.end() ? std::string() : it->second;
}

json ExtensionManager::describe(const std::string& name) const
{
    std::optional<ExtensionInfo> info;
    const Loaded* rec = nullptr;
    if (auto it = mLoaded.find(name); it != mLoaded.end())
    {
        rec = it->second.get();
        info = rec->info;
    }
    else
        info = find(name);
    if (!info)
        throw ExtensionError("no extension named '" + name + "'");
    info->autoload = autoloads(*info);
    if (info->error.empty())
        info->error = lastError(name);

    json j = info->toJson();
    json commands = json::array();
    for (const std::string& c : ops::registry().owned(info->name))
        commands.push_back(c);
    std::sort(commands.begin(), commands.end());
    json types = json::array();
    for (const std::string& t : scene::types::registry().owned(info->name))
        types.push_back(t);
    std::sort(types.begin(), types.end());
    j["commands"] = commands;
    j["types"] = types;
    j["registrations"] = rec && rec->ctx ? json(rec->ctx->registrations()) : json::array();
    return j;
}

std::vector<std::string> ExtensionManager::autoloadList() const
{
    return stringList("/extensions/autoload");
}

std::vector<std::string> ExtensionManager::disabledList() const
{
    return stringList("/extensions/disabled");
}

bool ExtensionManager::autoloads(const ExtensionInfo& info) const
{
    if (contains(autoloadList(), info.name))
        return true;
    return info.manifest && info.manifest->enabledByDefault && !contains(disabledList(), info.name);
}

void ExtensionManager::setAutoload(const std::string& name, bool on)
{
    std::vector<std::string> list = autoloadList();
    std::vector<std::string> off = disabledList();
    auto drop = [&](std::vector<std::string>& v) { v.erase(std::remove(v.begin(), v.end(), name), v.end()); };
    drop(list);
    drop(off);
    (on ? list : off).push_back(name);
    PrefStore::get().set("general", "/extensions/autoload", list);
    PrefStore::get().set("general", "/extensions/disabled", off);
}

void ExtensionManager::loadAutoload()
{
    LILA_ZONE();

    std::vector<ExtensionInfo> all = available();
    std::vector<std::string> ids;
    for (const ExtensionInfo& info : all)
        if (info.autoload && !info.loaded)
            ids.push_back(info.name);
    // Turned on by name but not found: say so rather than dropping it silently.
    for (const std::string& name : autoloadList())
        if (std::none_of(all.begin(), all.end(), [&](const ExtensionInfo& i) { return i.name == name; }))
            ids.push_back(name);

    auto depsOf = [&](const std::string& id) {
        std::vector<std::string> out;
        for (const ExtensionInfo& info : all)
            if (info.name == id && info.manifest)
                for (const ext::Dependency& d : info.manifest->dependencies)
                    out.push_back(d.id);
        return out;
    };
    for (const std::string& name : ext::dependencyOrder(ids, depsOf))
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

    ops::registerCommand({
        .name = "loadExtension",
        .doc = "Load an extension by id (from the extension search paths) or by the path of its package. Returns its id.",
        .positionalDoc = "id or path",
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

    ops::registerCommand({
        .name = "unloadExtension",
        .doc = "Unload an extension, removing everything it registered. Fails while the scene uses its node types or another "
               "loaded extension depends on it.",
        .positionalDoc = "id",
        .undoable = false,
        .run = [&pm](const Args& a) -> json {
            std::string name = nameArg(a, "unloadExtension");
            return guarded([&] {
                pm.unload(name);
                return json(name);
            });
        },
    });

    ops::registerCommand({
        .name = "reloadExtension",
        .doc = "Unload and load an extension again from the same package, picking up changes.",
        .positionalDoc = "id",
        .undoable = false,
        .run = [&pm](const Args& a) -> json {
            std::string name = nameArg(a, "reloadExtension");
            return guarded([&] { return json(pm.reload(name)); });
        },
    });

    ops::registerCommand({
        .name = "listExtensions",
        .doc = "Extensions on the search paths: [{name, path, kind, loaded, autoload, version, vendor, sdk, error, ...}].",
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

    ops::registerCommand({
        .name = "extensionInfo",
        .doc = "One extension: {name, path, kind, loaded, autoload, version, vendor, sdk, commands, types, registrations, error}.",
        .positionalDoc = "id",
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
