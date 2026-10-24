#pragma once

#include "core/Export.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

/**
 * The image files of the project folder: the workspace root, else the open scene's folder. Scanned on a worker
 * thread, cached, and rescanned when files under the root are added, removed or renamed.
 */
namespace lila::projectTextures
{
namespace fs = std::filesystem;

/** Whether the extension (with or without the dot, any case) is one the index lists. */
LILA_FOUNDATION_API bool isImageExtension(std::string_view ext);

/** Folders never scanned: hidden ones (.lila, .git, ...) and build or cache output. */
LILA_FOUNDATION_API bool skipsDirectory(std::string_view name);

/**
 * A glob against a '/' separated relative path, ASCII case-insensitive: `*` and `?` stay within a segment, `**`
 * spans segments (`a/**` is everything under a, `**` + `/x` is x at any depth). A pattern without '/' is tried
 * against every segment, so `*_old.*` or `Saved` match at any depth. A pattern also matches a path under any folder
 * it matches, so `Textures` and `Textures/` include everything in Textures.
 */
LILA_FOUNDATION_API bool globMatch(std::string_view pattern, std::string_view relPath);

/** Include and exclude globs. An empty include list takes everything; excludes win over includes. */
struct LILA_FOUNDATION_API Filter
{
    std::vector<std::string> include;
    std::vector<std::string> exclude;

    bool accepts(std::string_view relPath) const;
    std::vector<std::string> apply(const std::vector<std::string>& relPaths) const;
};

struct ScanLimits
{
    /** Directory entries visited before the scan stops, so a project on a drive root stays bounded. */
    size_t maxEntries = 250000;
    int maxDepth = 24;
};

/**
 * Every image under root as sorted, '/' separated relative paths. Blocking; `cancel` stops it early and `truncated`
 * reports whether the limits cut it short.
 */
LILA_FOUNDATION_API std::vector<std::string> scan(const fs::path& root, const ScanLimits& limits = {},
                                                  const std::atomic<bool>* cancel = nullptr, bool* truncated = nullptr);

/** The workspace root when one is open, else the open scene's folder; empty for an untitled scene. */
LILA_FOUNDATION_API fs::path projectRoot();

/**
 * The cached scan of one root. Main thread for everything except snapshot(), which any thread may call. Listeners
 * run on the main thread after each scan lands.
 */
class LILA_FOUNDATION_API Index
{
public:
    static Index& get();

    /** Points the index at root, scanning it in the background when it changed or has never been scanned. */
    void setRoot(const fs::path& root);
    /** setRoot(projectRoot()). */
    void follow();
    fs::path root() const;

    /** Rescan in the background. */
    void refresh();

    struct Snapshot
    {
        fs::path root;
        std::vector<std::string> files;
        bool scanning = false;
        /** True when the scan hit ScanLimits and the list is partial. */
        bool truncated = false;
        uint64_t generation = 0;
    };
    /** The last finished scan. With wait, blocks until a pending scan lands first. */
    Snapshot snapshot(bool wait = false) const;

    int addListener(std::function<void()> fn);
    void removeListener(int id);

private:
    Index();
    struct Impl;
    Impl* m;
};
} // namespace lila::projectTextures
