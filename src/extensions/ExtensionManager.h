#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

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
    Native, // shared library exporting lilaExtensionInit
    Python, // .py file or package defining initialize_extension()
};

struct ExtensionInfo
{
    std::string name;
    fs::path path;
    ExtensionKind kind = ExtensionKind::Native;
    bool loaded = false;
    bool autoload = false;

    json toJson() const;
};

/**
 * Finds, loads and unloads extensions. Main thread only.
 *
 * Search order, first match per name wins: addSearchPath() dirs, LILA_EXTENSION_PATH, general.json /extensions/paths,
 * <workspace>/.lila/extensions, <user config>/extensions, <exe dir>/extensions.
 */
class ExtensionManager
{
public:
    static ExtensionManager& get();

    ExtensionManager();
    ~ExtensionManager();

    std::vector<fs::path> searchPaths() const;
    void addSearchPath(const fs::path& dir);

    /** Everything on the search paths, plus loaded extensions that came from elsewhere. */
    std::vector<ExtensionInfo> available() const;

    /** A name on the search paths or a path to an extension. Loading a loaded extension does nothing. Returns the name. */
    std::string load(const std::string& nameOrPath);
    /** Refuses while scene nodes use the extension's node types. */
    void unload(const std::string& name);
    std::string reload(const std::string& name);
    void unloadAll();

    bool isLoaded(const std::string& name) const;
    std::vector<std::string> loaded() const;
    /** Info plus the commands and node types the extension owns. Throws for unknown names. */
    json describe(const std::string& name) const;

    /** general.json /extensions/autoload. */
    std::vector<std::string> autoloadList() const;
    void setAutoload(const std::string& name, bool on);
    /** Loads the autoload list, logging failures. */
    void loadAutoload();

private:
    struct Loaded;

    std::optional<ExtensionInfo> find(const std::string& nameOrPath) const;
    void removeOwned(const std::string& name);
    void checkUnloadable(const std::string& name) const;

    std::map<std::string, std::unique_ptr<Loaded>> mLoaded;
    std::vector<fs::path> mExtraPaths;
};

/** loadExtension, unloadExtension, reloadExtension, listExtensions, extensionInfo. */
void registerCommands();

/** Startup: load the extensions in the autoload pref. After python::init so Python extensions can load. */
void autoload();
} // namespace lila::extensions
