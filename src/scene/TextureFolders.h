#pragma once

#include "core/Export.h"
#include "core/ProjectTextures.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

/**
 * Which project images the texture pickers list: include and exclude globs kept per workspace in
 * .lila/settings.json (textures/include, textures/exclude) and overridable per scene in the scene's data. Each list
 * is overridden on its own, so a scene can add excludes and keep the workspace's includes.
 */
namespace lila::scene::textureFolders
{
using json = nlohmann::json;

inline constexpr const char* kPrefFile = "settings";
inline constexpr const char* kIncludeKey = "/textures/include";
inline constexpr const char* kExcludeKey = "/textures/exclude";
/** Scene data key holding {"include": [...], "exclude": [...]}; a missing list falls back to the workspace's. */
inline constexpr const char* kSceneKey = "textures.folders";

struct Rules
{
    projectTextures::Filter filter;
    /** Where each list came from: "scene", "workspace" or "" when neither sets it. */
    std::string includeFrom, excludeFrom;
};

/** Scene over workspace, list by list. Both are {"include": [...], "exclude": [...]}; anything else counts as unset. */
LILA_SCENE_API Rules resolve(const json& workspace, const json& scene);

/** The workspace's lists as stored in prefs, {"include", "exclude"} with only the set ones present. */
LILA_SCENE_API json workspaceRules();
/** The open scene's overrides, {"include", "exclude"} with only the set ones present. */
LILA_SCENE_API json sceneRules();
/** resolve(workspaceRules(), sceneRules()). */
LILA_SCENE_API Rules current();

/** Commands: textureFolders, projectTextures. */
LILA_SCENE_API void registerCommands();
} // namespace lila::scene::textureFolders
