#pragma once

#include "core/Export.h"

#include "scene/Plug.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace lila::scene
{
class Graph;
class Node;

/**
 * Lighting Channels, as in Unreal: a light lights the objects that share a channel with it. Channels are named and as many
 * as a scene needs ("default" always exists, Unreal's channel 0). The scene lists them (scene data "lightingChannels");
 * each light and object holds its channels in a hidden lightingChannels string array, and one without it is in "default".
 */
namespace lightingChannels
{
inline constexpr const char* kDefault = "default";
inline constexpr const char* kAttr = "lightingChannels";
/** How many channels the viewport tells apart; later ones draw as if lit by every light. */
inline constexpr int kViewportMax = 20;

/** Every channel, "default" first. */
LILA_SCENE_API std::vector<std::string> names();
/** Adds a channel to the scene's list (undoable). Throws std::invalid_argument for an empty or taken name. */
LILA_SCENE_API void create(const std::string& name);
/** Renames a channel everywhere: the list and every node holding it. */
LILA_SCENE_API void rename(Graph& g, const std::string& from, const std::string& to);
/** Removes a channel from the list and every node; nodes left with none go back to "default". */
LILA_SCENE_API void remove(Graph& g, const std::string& name);

/** A node's channels; {"default"} when it has none set. */
LILA_SCENE_API std::vector<std::string> of(const Node& n);
/** Sets a node's channels (undoable); unknown names are created. An empty list or just "default" clears the attr. */
LILA_SCENE_API void set(Graph& g, NodeId node, std::vector<std::string> channels);
/** A node's channels as viewport bits, bit i for names()[i]; channels past kViewportMax set every bit. */
LILA_SCENE_API uint32_t mask(const Node& n);
/** Whether light lights object. */
LILA_SCENE_API bool lights(const Node& light, const Node& object);
/** Nodes with channel data other than "default" alone: what exports must report. */
LILA_SCENE_API std::vector<NodeId> nodesWithChannels(Graph& g);

/**
 * Bakes light links into channels: links maps each light to the objects it lights. Lights with the same set of objects
 * share one channel, named after the first such light; every object gets the channels of the lights that light it.
 * Lights and objects not mentioned keep "default". Returns the channels made.
 */
LILA_SCENE_API std::vector<std::string> bakeLinks(Graph& g, const std::vector<std::pair<NodeId, std::vector<NodeId>>>& links);

/** What an export carries: the channels other than "default" its nodes use, in names() order, and the nodes holding them. */
struct Usage
{
    std::vector<std::string> channels;
    std::vector<NodeId> lights;
    std::vector<NodeId> objects;
    bool empty() const { return lights.empty() && objects.empty(); }
};
/** The channel data among nodes and, with children, their descendants. */
LILA_SCENE_API Usage usage(Graph& g, const std::vector<NodeId>& nodes, bool children = false);

/** Unreal's three Lighting Channels: "default" is 0, the next two channels used by scene order 1 and 2, the rest dropped. */
struct UnrealChannels
{
    std::array<std::string, 3> slots;
    std::vector<std::string> dropped;
};
LILA_SCENE_API UnrealChannels unrealChannels(const Usage& u);
/** A node's bLightingChannels 0/1/2. One left with none (only dropped channels) goes on channel 0. */
LILA_SCENE_API std::array<bool, 3> unrealBits(const Node& n, const UnrealChannels& c);

/**
 * An export report's lightingChannels entry: {channels, lights, objects (paths), written}, plus a note when given. With
 * unreal, also {unreal: [channel per slot], dropped}.
 */
LILA_SCENE_API json report(const Graph& g, const Usage& u, bool written, const std::string& note = {}, const UnrealChannels* unreal = nullptr);
/** "rim, back on 2 light(s) and 3 object(s)", for logs and option boxes. */
LILA_SCENE_API std::string summary(const Usage& u);
/** One line for an export's option box: what exporting the selection with its descendants would carry. */
LILA_SCENE_API std::string describeSelection(Graph& g, bool unreal = false);

LILA_SCENE_API void registerCommands();
} // namespace lightingChannels
} // namespace lila::scene
