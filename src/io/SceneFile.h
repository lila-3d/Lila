#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <string>

namespace lila::io
{
/**
 * Scenes save as binary .lila or text .lilaa (see SceneFormats.h); both carry this document:
 *   { "lila": 1, "upAxis": "y", "units": "cm", "time": 0.0,
 *     "nodes": [ { "name", "type", "parent", "attrs": { name: value } } ],   parents before children
 *     "connections": [ ["src.attr", "dst.attr"] ],
 *     "selection": [ names ],
 *     "references": [ { "namespace", "path", "loaded", "edits": [ { "node", "attrs", "parent" } ] } ],
 *     "ui": { section: value } }
 * Only storable attrs that differ from their defaults are written.
 */
/**
 * `sceneDir` is where the file will live, so reference paths can be written relative to it. packed leaves meshes' big
 * arrays as packed:: binary values, which only encodeBinary takes; packed::unpackAll turns them back into plain json.
 */
nlohmann::json serialize(const std::filesystem::path& sceneDir = {}, bool packed = false);
/**
 * Replaces the current scene. Throws std::runtime_error with a readable message. ownBytes weighs the document's own
 * nodes against its references' file sizes on the progress bar (the scene file's size, typically).
 */
void deserialize(const nlohmann::json& doc, const std::filesystem::path& sceneDir = {}, double ownBytes = 0.0);

/**
 * A part of the scene document owned by a layer above the scene, such as the viewports' look-through cameras. It is
 * written as "ui"/key from save() (null leaves it out) and handed to load() after a scene opens, null when the file has
 * none. Sections nobody registered are kept and written back, so batch mode saves them unchanged.
 */
void registerUiSection(const std::string& key, std::function<nlohmann::json()> save, std::function<void(const nlohmann::json&)> load);

void save(const std::filesystem::path& path);
/** Reading the file can be cancelled (progress::Cancelled), which leaves the current scene alone. */
void load(const std::filesystem::path& path);

/** What a scene load shows instead of "Loading": one of a dozen verbs, picked at random ("Unfurling"). */
std::string loadingVerb();

/** The file the scene was last saved to or opened from; empty for an untitled scene. */
const std::filesystem::path& currentPath();
void setCurrentPath(const std::filesystem::path& path);
/** True when the scene changed since the last save/open/new. */
bool modified();
/** True when saving to path would write what is already there: it is the current file, it exists and nothing changed. */
bool upToDate(const std::filesystem::path& path);
void markClean();
void onStateChanged(std::function<void()> fn);
/** Called after every successful save (not autosaves). */
void onSaved(std::function<void(const std::filesystem::path&)> fn);

/** Commands: saveScene, openScene, sceneInfo, plus autosave scheduling. */
void registerCommands();

/**
 * Writes a timestamped autosave into history::autosaveFolder() if the scene changed since the last one, keeping the
 * newest /autosave/keep (default 8). Never touches the scene file or the modified flag. Called on a timer by the host.
 */
void autosave();

/** Marks the scene as changed, so Save is offered and the next autosave writes. */
void markModified();

/**
 * Replaces the scene with a history file (autosave, backup, revision copy) whose references resolve against scene's
 * folder. keepPath: it becomes scene, modified, so Save writes the real file. Otherwise it opens untitled and
 * modified, shown as title.
 */
void loadVersion(const std::filesystem::path& file, const std::filesystem::path& scene, bool keepPath,
                 const std::string& title = {});

/** What the window title shows: the version title from loadVersion, the file name, or "untitled". */
std::string displayName();

/** Serializes the scene marked as an emergency save into its autosave folder. Never throws; empty on failure. */
std::filesystem::path emergencySave();
} // namespace lila::io
