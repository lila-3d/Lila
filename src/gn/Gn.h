#pragma once

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
void registerTypes();
/** gnCreateGroup, gnAddNode, gnConnect, ... and exportMeshSequence. */
void registerCommands();

scene::Node* findGroupNode(scene::Graph& g, std::string_view group);
std::vector<std::string> groupNames(scene::Graph& g);
/** Throws std::invalid_argument when the definition is unreadable. */
Group readGroup(const scene::Node& groupNode);
/** Modifier node type name for the group's current interface version. */
std::string typeNameFor(const Group& g);
/** Modifiers using a group. */
std::vector<scene::NodeId> instancesOf(scene::Graph& g, scene::NodeId groupNode);
bool isGnModifier(const scene::Node& n);
/** The group node feeding a gn modifier, or null. */
scene::Node* groupOfModifier(scene::Graph& g, const scene::Node& modifier);

/**
 * Writes a changed definition back undoably: bumps the type version when the interface changed, drops links to
 * sockets that no longer exist, then migrates and reconnects the group's modifiers.
 */
void commitGroup(scene::Graph& g, scene::Node& groupNode, Group next);

/** User library (prefs) and workspace library folders, user first. Missing folders are skipped. */
std::vector<std::filesystem::path> libraryDirs();
} // namespace lila::gn
