#pragma once

#include "core/Export.h"

#include "gn/Group.h"
#include "scene/Graph.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

/**
 * Geometry nodes: Blender-style node groups that run as modifiers in a mesh's stack.
 *
 * A group lives in the scene on a "gnGroup" node (its definition is a JSON string attr, so scenes save it and edits
 * undo). Each group registers a modifier node type "gn_<Group>" (versioned "gn_<Group>_v2"... when its interface
 * changes) with one keyable attr per exposed input. Instances get the definition through a connection from the
 * group node, so editing the group re-evaluates every mesh using it.
 */
namespace lila::gn
{
inline constexpr const char* kGroupNodeType = "gnGroup";

/** Node types (gnGroup) and the scene listener that registers modifier types as groups appear. Call after the scene's builtins. */
LILA_SCENE_API void registerTypes();
/** gnCreateGroup, gnAddNode, gnConnect, ... and exportMeshSequence. */
LILA_SCENE_API void registerCommands();

LILA_SCENE_API scene::Node* findGroupNode(scene::Graph& g, std::string_view group);
LILA_SCENE_API std::vector<std::string> groupNames(scene::Graph& g);
/** Throws std::invalid_argument when the definition is unreadable. */
LILA_SCENE_API Group readGroup(const scene::Node& groupNode);
/** Modifier node type name for the group's current interface version. */
LILA_SCENE_API std::string typeNameFor(const Group& g);
/** Modifiers using a group. */
LILA_SCENE_API std::vector<scene::NodeId> instancesOf(scene::Graph& g, scene::NodeId groupNode);
LILA_SCENE_API bool isGnModifier(const scene::Node& n);
/** The group node feeding a gn modifier, or null. */
LILA_SCENE_API scene::Node* groupOfModifier(scene::Graph& g, const scene::Node& modifier);

/**
 * Writes a changed definition back undoably: bumps the type version when the interface changed, drops links to
 * sockets that no longer exist, then migrates and reconnects the group's modifiers.
 */
LILA_SCENE_API void commitGroup(scene::Graph& g, scene::Node& groupNode, Group next);

/** User library (prefs) and workspace library folders, user first. Missing folders are skipped. */
LILA_SCENE_API std::vector<std::filesystem::path> libraryDirs();
} // namespace lila::gn
