#pragma once

#include "core/Export.h"

#include "scene/Node.h"

#include <optional>
#include <string_view>
#include <vector>

namespace lila::scene
{
class Graph;

/**
 * Maya's display layers. A `displayLayer` node (non-DAG) holds the layer's switches; an object joins one by having the
 * layer's drawInfo connected to its displayLayer plug, as Maya wires drawInfo into drawOverride. An object is in at
 * most one layer, and a layer on a group governs everything under it unless a child has a layer of its own.
 *
 *   visibility       off hides the members, like their own visibility.
 *   hideOnPlayback   hides the members only while the animation plays (Maya's P).
 *   displayType      normal; template: grey wire that can't be picked; reference: drawn as usual but can't be picked.
 *   useColor/color   tints the members' wireframe (linear RGB). Off by default: no colour.
 *   displayOrder     row order in the Display Layers panel, lowest first.
 */
class DisplayLayer : public Node
{
public:
    enum Attr
    {
        kVisibility,
        kHideOnPlayback,
        kDisplayType,
        kUseColor,
        kColor,
        kDisplayOrder,
        kDrawInfo,
        kCount
    };
};

namespace displayLayers
{
inline constexpr const char* kTypeName = "displayLayer";
/** Maya's implicit layer: naming it as the target of a membership edit takes objects out of their layer. */
inline constexpr const char* kDefaultLayer = "defaultLayer";

enum class DisplayType : int64_t
{
    Normal,
    Template,
    Reference,
};

LILA_SCENE_API bool isLayer(const Node& n);
/** Every layer in display order (then name). */
LILA_SCENE_API std::vector<Node*> all(const Graph& g);
/** The layer the node itself is in, or null. */
LILA_SCENE_API Node* layerOf(const Graph& g, NodeId id);
/** Nodes whose own layer is this one, in node order. */
LILA_SCENE_API std::vector<NodeId> members(const Graph& g, NodeId layer);

/** True when the node's own layer hides it: visibility off, or hideOnPlayback while playing. */
LILA_SCENE_API bool hiddenByOwnLayer(const Graph& g, NodeId id);

/** How the layer governing a node (its own, else the nearest ancestor's) draws it. */
struct Display
{
    DisplayType type = DisplayType::Normal;
    std::optional<glm::dvec3> color; // linear RGB wire tint
};
LILA_SCENE_API Display display(const Graph& g, NodeId id);
/** False for template and reference members: the viewport doesn't pick them, though lists and scripts still can. */
LILA_SCENE_API bool pickable(const Graph& g, NodeId id);

/** All undoable. create takes the nodes out of any layer they were in. */
LILA_SCENE_API Node* create(Graph& g, std::string_view name, const std::vector<NodeId>& nodes);
LILA_SCENE_API void addMembers(Graph& g, NodeId layer, const std::vector<NodeId>& nodes);
LILA_SCENE_API void removeMembers(Graph& g, const std::vector<NodeId>& nodes);
/** Members go back to no layer; the node is deleted. */
LILA_SCENE_API void remove(Graph& g, NodeId layer);
/** Moves the layer to index among all() and renumbers every layer's displayOrder 0, 1, 2, ... */
LILA_SCENE_API void reorder(Graph& g, NodeId layer, size_t index);

/** { name, visibility, hideOnPlayback, displayType, color (null without one), order, members }. */
LILA_SCENE_API json info(const Graph& g, NodeId layer);

LILA_SCENE_API void registerTypes();
/**
 * createDisplayLayer, editDisplayLayerMembers, removeFromDisplayLayer, deleteDisplayLayer, displayLayer,
 * listDisplayLayers, reorderDisplayLayer, selectDisplayLayerMembers.
 */
LILA_SCENE_API void registerCommands();
} // namespace displayLayers
} // namespace lila::scene
