#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace lila
{
using json = nlohmann::json;

enum class PrefScope : uint8_t
{
    User,
    Workspace,
};

/**
 * Layered JSON prefs: built-in defaults < studio folders < user dir < workspace .lila/.
 *
 * Studio folders (LILA_STUDIO_PREFS, or the "prefs" list in studio.json beside the exe; most specific first) hold the
 * same files as the user dir, usually on a network share. Their values are defaults the user can override, except
 * keys listed in a file's "$locked" array (JSON pointers; "/" locks the whole file), which always take the studio
 * value and ignore set(). A local copy keeps them applying while the share is offline.
 *
 * A "file" is a name like "general" or "markingMenus/modeling"; keys are JSON pointers ("/viewport/fov").
 * Every set() is written to disk shortly after on the IO thread, and edits made to the files outside Lila are
 * picked up and reported through onChanged. Nothing waits for shutdown.
 */
class LILA_FOUNDATION_API PrefStore
{
public:
    using ChangeFn = std::function<void(const std::string& file)>;

    static PrefStore& get();

    /** Starts the IO thread. Change callbacks are delivered on the main thread via mainq. */
    void start();
    void stop();

    /** Point the workspace layer at <root>/.lila, or detach it with an empty path. */
    void setWorkspaceDir(const std::filesystem::path& lilaDir);

    json value(std::string_view file, std::string_view key, const json& fallback = nullptr) const;

    template <class T> T value(std::string_view file, std::string_view key, const T& fallback) const
    {
        json v = value(file, key, json(nullptr));
        if (v.is_null())
            return fallback;
        try
        {
            return v.get<T>();
        }
        catch (const json::exception&)
        {
            return fallback;
        }
    }

    /** Whole merged document for a file. */
    json merged(std::string_view file) const;

    void set(std::string_view file, std::string_view key, json value, PrefScope scope = PrefScope::User);
    void remove(std::string_view file, std::string_view key, PrefScope scope = PrefScope::User);

    /** The user dir's own document for a file, without the layers around it. */
    json userDoc(std::string_view file) const;
    /** Replaces the user dir's document for a file; an empty object leaves the file to its defaults. */
    void setUserDoc(std::string_view file, json data);

    /**
     * Defaults contributed at runtime, by extensions and modes: they sit above the built-in defaults and below the
     * user's, so a user edit still wins and removing them brings the built-ins back. Each is kept under a layer id
     * ("<owner>:<what>") so it can be taken away again; later layers win over earlier ones. Never written to disk.
     */
    void setDefaults(std::string_view file, std::string layer, json data);
    /** False when that layer contributed nothing to file. */
    bool removeDefaults(std::string_view file, std::string_view layer);
    /** Removes every contribution whose layer id starts with prefix. Returns the count. */
    size_t removeDefaultsWithPrefix(std::string_view prefix);
    /** Files with contributed defaults whose name starts with prefix ("markingMenus/"), sorted. */
    std::vector<std::string> contributedFiles(std::string_view prefix = {}) const;
    /** Whether any layer contributed to file. */
    bool hasContributed(std::string_view file) const;

    /** Block until pending writes hit disk. */
    void flush();

    int onChanged(ChangeFn fn);
    void removeOnChanged(int id);

    std::filesystem::path userDir() const;

    /** Whether a studio folder locks this key (or a parent of it). */
    bool isLocked(std::string_view file, std::string_view key) const;
    /** Replaces the studio folders, most specific first; the environment sets them at startup. */
    void setStudioDirs(std::vector<std::filesystem::path> dirs);
    std::vector<std::filesystem::path> studioDirs() const;
    /** Files the studio folders provide whose name starts with prefix ("shelves/"), sorted, from the local copy when offline. */
    std::vector<std::string> studioFiles(std::string_view prefix = {}) const;

    PrefStore();
    ~PrefStore();

private:
    /** value() without the memo; a discarded json when no layer has the key. Holds the lock. */
    json valueUncached(std::string_view file, std::string_view key) const;
    struct Impl;
    std::unique_ptr<Impl> m;
};
} // namespace lila
