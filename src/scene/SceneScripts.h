#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <vector>

namespace lila::ext
{
class Context;
}

namespace lila::scene::scripts
{
using json = nlohmann::json;

/** Python source saved inside the scene file. Nothing runs on its own unless runOnOpen is set and the host agrees. */
struct Script
{
    std::string name;
    std::string text;
    bool runOnOpen = false;

    bool operator==(const Script&) const = default;
};

/** In the order they were added. */
LILA_SCENE_API const std::vector<Script>& all();
LILA_SCENE_API const Script* find(const std::string& name);

/** Adds or replaces by name, undoably. */
LILA_SCENE_API void set(const Script& script);
/** Undoable. False when there was no such script. */
LILA_SCENE_API bool remove(const std::string& name);
/** Drops every script without undo, as a new or opened scene does. */
LILA_SCENE_API void clear();

/** Saved with the scene as "scripts": [ { "name", "text", "runOnOpen" } ]. */
LILA_SCENE_API json toJson();
/** Replaces every script without undo; malformed entries are skipped. */
LILA_SCENE_API void fromJson(const json& j);

/** Notified after any change, undo and redo included. */
LILA_SCENE_API int addListener(std::function<void()> fn);
/** For extensions: removed when ctx unloads. */
LILA_SCENE_API int addListener(ext::Context& ctx, std::function<void()> fn);
LILA_SCENE_API void removeListener(int id);

/**
 * Decides what happens to the run-on-open scripts of a scene just opened. The UI installs one that asks first;
 * without one nothing runs.
 */
LILA_SCENE_API void setOpenHandler(std::function<void(const std::vector<Script>&)> fn);
/** Called by the scene loader once a scene file has opened. */
LILA_SCENE_API void sceneOpened();

/** Commands: sceneScript. */
LILA_SCENE_API void registerCommands();
} // namespace lila::scene::scripts
