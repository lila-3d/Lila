#pragma once

#include "scene/Plug.h"

#include <functional>
#include <map>
#include <vector>

namespace lila::scene
{
enum class ComponentMode : uint8_t
{
    Object,
    Vertex,
    Edge,
    Face,
};

/**
 * Selected mesh components per node, Maya style: the object selection decides which meshes are live (hilited) and
 * the mode decides which component kind clicks pick. Ids are those of the mesh's outMesh. Changes record undo.
 */
namespace components
{
using Ids = std::vector<uint32_t>;

ComponentMode mode();
/** Not undoable, like Maya's selectMode. */
void setMode(ComponentMode m);
const char* modeName(ComponentMode m);
ComponentMode modeFromName(const std::string& name); // throws std::invalid_argument

const std::map<NodeId, Ids>& selected(ComponentMode kind);
const Ids& selected(NodeId node, ComponentMode kind);

enum class Op
{
    Replace,
    Add,
    Remove,
    Toggle,
};
/** Changes one node's selection of one kind. Replace clears that kind on every other node first. */
void select(NodeId node, ComponentMode kind, const Ids& ids, Op op = Op::Replace);
void clear();
/** Replaces everything at once, as one undo step. */
void setAll(std::map<NodeId, Ids> points, std::map<NodeId, Ids> edges, std::map<NodeId, Ids> faces);

/** Points touched by the node's selection of the current mode (verts as is, edge ends, face corners). */
Ids pointsOf(NodeId node);

/** Maya's polyListComponentConversion: edges/faces to their points, points to edges/faces they fully cover. */
Ids convert(const geo::Mesh& mesh, ComponentMode from, const Ids& ids, ComponentMode to);

int addListener(std::function<void()> fn);
void removeListener(int id);

void registerCommands();
} // namespace components

/**
 * Soft-selection weight per point (1 for selected points, then the falloff curve out to 0 at the radius), from the general
 * prefs /softSelect/enabled, /softSelect/radius (cm, world space), /softSelect/curve and /softSelect/falloffMode (see
 * scene/SoftSelect.h). All zero but the selection when it's off.
 */
std::vector<float> softWeights(const geo::Mesh& mesh, const glm::dmat4& world, const components::Ids& selectedPoints);

/** World-space centre of the selected components' points. */
glm::dvec3 selectionCenter();
/** Applies a world-space transform to the selected components' points (with symmetry), as one undo step. */
void transformSelectedComponents(const char* label, const glm::dmat4& worldXform);
} // namespace lila::scene
