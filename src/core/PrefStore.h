#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace lila
{
using json = nlohmann::json;

enum class PrefScope : uint8_t
{
    User,
    Workspace,
};

/**
 * Layered JSON prefs: built-in defaults < user dir < workspace .lila/.
 *
 * A "file" is a name like "general" or "markingMenus/modeling"; keys are JSON pointers ("/viewport/fov").
 * Every set() is written to disk shortly after on the IO thread, and edits made to the files outside Lila are
 * picked up and reported through onChanged. Nothing waits for shutdown.
 */
class PrefStore
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

    /** Block until pending writes hit disk. */
    void flush();

    int onChanged(ChangeFn fn);
    void removeOnChanged(int id);

    std::filesystem::path userDir() const;

    PrefStore();
    ~PrefStore();

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
} // namespace lila
