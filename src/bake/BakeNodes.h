#pragma once

#include "core/Export.h"

#include "bake/BakeTypes.h"
#include "scene/Node.h"

#include <string>
#include <vector>

namespace lila::scene
{
class Graph;
}

/**
 * The baker's DG nodes. Settings live in one storable Data plug per node holding versioned JSON (kinds
 * "bakeProjectSettings" and "bakeGroupSettings"), so scene files round trip them and every edit is one undoable setValue.
 *
 *   bakeProject  settings: ProjectSettings (output, maps, target)
 *   bakeGroup    project:  the bakeProject's name when it was last set (String, informational)
 *                settings: GroupSettings (lows, highs with bake-only modifier stacks, cage, distances) and the project
 *
 * A group holds its lows, highs, cage and project by node id, so renames follow, the way object sets hold members:
 * groupSettings() reports their current paths, a deleted node drops out (undo brings it back), and files store paths
 * that bind by name on load (relative to the group's namespace first). Version 1 files, which named the project only in
 * the project attr, load unchanged.
 */
namespace lila::bake
{
inline constexpr const char* kProjectType = "bakeProject";
inline constexpr const char* kGroupType = "bakeGroup";

LILA_SCENE_API void registerTypes();

LILA_SCENE_API bool isProject(const scene::Node& n);
LILA_SCENE_API bool isGroup(const scene::Node& n);

LILA_SCENE_API ProjectSettings projectSettings(const scene::Node& project);
/** Undoable. */
LILA_SCENE_API void setProjectSettings(scene::Graph& g, scene::NodeId project, const ProjectSettings& s);

LILA_SCENE_API GroupSettings groupSettings(const scene::Node& group);
/** Undoable. */
LILA_SCENE_API void setGroupSettings(scene::Graph& g, scene::NodeId group, const GroupSettings& s);
/** The project a group names (by its project attr), or null. */
LILA_SCENE_API scene::Node* groupProject(scene::Graph& g, const scene::Node& group);
/** Undoable. */
LILA_SCENE_API void setGroupProject(scene::Graph& g, scene::NodeId group, const std::string& project);

/** Every bakeProject, in creation order. */
LILA_SCENE_API std::vector<scene::NodeId> projects(const scene::Graph& g);
/** The project's groups, in creation order. */
LILA_SCENE_API std::vector<scene::NodeId> groupsOf(scene::Graph& g, scene::NodeId project);

/** Creates a bakeProject (undoable) with default settings. name empty gives bakeProject1. */
LILA_SCENE_API scene::Node* createProject(scene::Graph& g, const std::string& name, const ProjectSettings& s = ProjectSettings::defaults());
/** Creates a bakeGroup in project (undoable), filled in from the project's group defaults. */
LILA_SCENE_API scene::Node* createGroup(scene::Graph& g, scene::NodeId project, const std::string& name, const GroupSettings& s = {});
} // namespace lila::bake
