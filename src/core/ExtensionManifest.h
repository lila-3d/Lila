#pragma once

#include "core/ExtensionContext.h"
#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace lila::ext
{
/** Thrown with every problem found in an extension.json. */
struct ManifestError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

struct Dependency
{
    std::string id;
    std::string minVersion; // empty = any
};

/**
 * An extension package: a folder <Id>.lilaext holding extension.json.
 *
 *     {
 *       "id": "Hello", "name": "Hello", "version": "1.0.0", "vendor": "Lila", "sdk": "1.0",
 *       "description": "...",
 *       "entry": {"windows": "Hello.dll", "linux": "libHello.so", "macos": "libHello.dylib"},
 *       "python": "hello",                  // optional: module (folder or .py) relative to the package
 *       "resources": "resources",           // optional, the default; alone (no entry, no python) for a resource pack
 *       "usdPlugins": ["usd"],              // optional: folders of USD plugins (Hydra render delegates), see lila::usdPlugins
 *       "dependencies": ["Other", {"id": "Base", "version": "1.2"}],
 *       "enabledByDefault": true,
 *       "license": {}                       // reserved
 *     }
 */
struct Manifest
{
    std::string id;
    std::string name;
    std::string version;
    std::string vendor;
    std::string description;
    SdkVersion sdk;
    std::map<std::string, std::string> entry; // platform -> library, relative to the package
    std::string python;
    std::string resources = "resources";
    std::vector<std::string> usdPlugins; // relative to the package
    std::vector<Dependency> dependencies;
    bool enabledByDefault = false;
    nlohmann::json license;
    std::filesystem::path root; // the package folder

    /** root names the package folder; its name must be <id>.lilaext when it has that extension. Throws ManifestError. */
    LILA_FOUNDATION_API static Manifest parse(const nlohmann::json& j, const std::filesystem::path& root);
    /** Reads <dir>/extension.json. Throws ManifestError. */
    LILA_FOUNDATION_API static Manifest load(const std::filesystem::path& dir);

    /** This platform's library, absolute; nullopt for a Python-only extension. */
    LILA_FOUNDATION_API std::optional<std::filesystem::path> library() const;
    /** The Python module's file or package folder, absolute; nullopt when there is none. */
    LILA_FOUNDATION_API std::optional<std::filesystem::path> pythonModule() const;
    /** Ships only resources (materials, nodes, presets): no library and no Python module. */
    bool resourcesOnly() const { return entry.empty() && python.empty(); }
    LILA_FOUNDATION_API nlohmann::json toJson() const;
};

inline constexpr const char* kPackageSuffix = ".lilaext";
inline constexpr const char* kManifestFile = "extension.json";

/** "windows", "linux" or "macos": the key into Manifest::entry. */
LILA_FOUNDATION_API const char* platformName();

/**
 * Orders ids so each comes after the dependencies it has in the set. depsOf gives an id's dependency ids (unknown
 * ones are ignored). Cycles are broken and reported in cycleOut.
 */
LILA_FOUNDATION_API std::vector<std::string> dependencyOrder(const std::vector<std::string>& ids,
                                                         const std::function<std::vector<std::string>(const std::string&)>& depsOf,
                                                         std::vector<std::string>* cycleOut = nullptr);

/** Compares dotted versions numerically: <0, 0, >0. */
LILA_FOUNDATION_API int compareVersions(std::string_view a, std::string_view b);
} // namespace lila::ext
