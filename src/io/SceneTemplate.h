#pragma once

#include "core/Export.h"

#include <filesystem>

/**
 * The New Scene template: a scene File > New and launch start from instead of an empty one. The pref holds its path, so
 * a studio or workspace pref can supply one; a user's own template is a copy saved in the user config folder.
 */
namespace lila::io::sceneTemplate
{
namespace fs = std::filesystem;

inline constexpr const char* kPref = "/newScene/template";

/** The template the prefs name, empty when none. */
LILA_SCENE_API fs::path path();

/** Where the user's own template is saved. */
LILA_SCENE_API fs::path userPath();

/**
 * Replaces the scene with the template, untitled and unchanged. False when there is none; a missing or unreadable one
 * leaves an empty scene and logs a warning naming it.
 */
LILA_SCENE_API bool apply();

/** Saves the scene as the user's template and points the user pref at it. Returns where it went. */
LILA_SCENE_API fs::path saveCurrent();

/** Drops the user's template, so New Scene uses the studio's or workspace's, or starts empty. */
LILA_SCENE_API void clear();

/** Commands: saveSceneTemplate, clearSceneTemplate, sceneTemplate. */
LILA_SCENE_API void registerCommands();
} // namespace lila::io::sceneTemplate
