#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <vector>

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
const std::vector<Script>& all();
const Script* find(const std::string& name);

/** Adds or replaces by name, undoably. */
void set(const Script& script);
/** Undoable. False when there was no such script. */
bool remove(const std::string& name);
/** Drops every script without undo, as a new or opened scene does. */
void clear();

/** Saved with the scene as "scripts": [ { "name", "text", "runOnOpen" } ]. */
json toJson();
/** Replaces every script without undo; malformed entries are skipped. */
void fromJson(const json& j);

/** Notified after any change, undo and redo included. */
int addListener(std::function<void()> fn);
void removeListener(int id);

/**
 * Decides what happens to the run-on-open scripts of a scene just opened. The UI installs one that asks first;
 * without one nothing runs.
 */
void setOpenHandler(std::function<void(const std::vector<Script>&)> fn);
/** Called by the scene loader once a scene file has opened. */
void sceneOpened();

/** Commands: sceneScript. */
void registerCommands();
} // namespace lila::scene::scripts
