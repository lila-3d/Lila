#pragma once

#include "core/ExtensionManifest.h"
#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace lila::ext
{
class Context;
}

namespace lila::extensions
{
using json = nlohmann::json;
namespace fs = std::filesystem;

struct ExtensionError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

enum class ExtensionKind : uint8_t
{
    Native, // a package with a library exporting lilaExtensionInit (and maybe a Python module too)
    Python, // a package with only a Python module, or a bare .py file or package defining initialize_extension()
    Resources, // a package with only resources (materials, nodes, presets) that Lila's libraries scan
};

struct LILA_SCENE_API ExtensionInfo
{
    std::string name; // the id
    fs::path path;    // the package folder, or the bare Python file or folder
    ExtensionKind kind = ExtensionKind::Native;
    bool loaded = false;
    bool autoload = false;
    /** From extension.json; empty for a bare Python extension. */
    std::optional<ext::Manifest> manifest;
    /** Why the manifest could not be read, or why the last load failed. */
    std::string error;

    json toJson() const;
};

/**
 * Finds, loads and unloads extensions. Main thread only.
 *
 * An extension is a package folder <Id>.lilaext with an extension.json (see ext::Manifest), or a bare Python file or
 * package. Search order, first match per id wins: addSearchPath() dirs, LILA_EXTENSION_PATH, general.json
 * /extensions/paths, <workspace>/.lila/extensions, <user config>/extensions, <exe dir>/extensions.
 *
 * Lila checks a package's SDK version (its manifest, then what its library was built against) before running any of
 * its code, and loads its dependencies first.
 */
class LILA_SCENE_API ExtensionManager
{
public:
    static ExtensionManager& get();

    ExtensionManager();
    ~ExtensionManager();
    ExtensionManager(const ExtensionManager&) = delete;
    ExtensionManager& operator=(const ExtensionManager&) = delete;

    std::vector<fs::path> searchPaths() const;
    void addSearchPath(const fs::path& dir);

    /** Everything on the search paths, plus loaded extensions that came from elsewhere. */
    std::vector<ExtensionInfo> available() const;

    /** An id on the search paths or a path to a package (or its extension.json). Loading a loaded extension does nothing. Returns the id. */
    std::string load(const std::string& nameOrPath);
    /** Refuses while scene nodes use the extension's node types or a loaded extension depends on it. */
    void unload(const std::string& name);
    std::string reload(const std::string& name);
    void unloadAll();

    bool isLoaded(const std::string& name) const;
    /** In load order. */
    std::vector<std::string> loaded() const;
    /** Info plus what the extension registered. Throws for unknown names. */
    json describe(const std::string& name) const;
    /** The last load failure, empty when there was none. */
    std::string lastError(const std::string& name) const;

    /** general.json /extensions/autoload: extensions the user turned on. */
    std::vector<std::string> autoloadList() const;
    /** Whether the extension loads at startup: turned on by the user, or enabledByDefault and not turned off. */
    bool autoloads(const ExtensionInfo& info) const;
    void setAutoload(const std::string& name, bool on);
    /** Loads every extension that autoloads, dependencies first, logging failures. */
    void loadAutoload();

private:
    struct Loaded;

    std::optional<ExtensionInfo> find(const std::string& nameOrPath) const;
    std::vector<std::string> disabledList() const;
    void loadInfo(const ExtensionInfo& info, std::set<std::string>& visiting);
    void loadNative(Loaded& rec);
    void loadPython(Loaded& rec, const fs::path& module);
    void removeOwned(Loaded& rec);
    void checkUnloadable(const std::string& name) const;

    std::map<std::string, std::unique_ptr<Loaded>> mLoaded;
    std::vector<std::string> mOrder; // load order
    std::map<std::string, std::string> mErrors;
    std::vector<fs::path> mExtraPaths;
};

/** The context of an extension that is loaded or loading, for registrations made on its behalf (from its Python code). */
LILA_SCENE_API ext::Context* contextOf(std::string_view name);

/** loadExtension, unloadExtension, reloadExtension, listExtensions, extensionInfo. */
LILA_SCENE_API void registerCommands();

/** Startup: load the extensions that autoload. After python::init so Python extensions can load. */
LILA_SCENE_API void autoload();
} // namespace lila::extensions
