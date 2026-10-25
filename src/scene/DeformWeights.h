#pragma once

#include "core/Export.h"

#include "scene/Node.h"
#include "scene/Skin.h"

#include <glm/glm.hpp>

#include <span>
#include <string>
#include <utility>
#include <vector>

namespace lila::geo
{
class Mesh;
}

namespace lila::scene
{
class DeformerNode;
class Graph;

/** The points a deformer may move (its membership), sorted. */
struct LILA_SCENE_API PointIds final : Data
{
    std::vector<uint32_t> points;
    const char* kind() const override { return "deformerMembership"; }
    size_t bytes() const override { return points.size() * sizeof(uint32_t); }
    json toJson() const override;
    static DataPtr fromJson(const json& j);
};

/**
 * Deformer weights beyond the painted map: fields, membership, painting and transfer of float maps (a deformer's
 * weights or a field's painted layer), and the correction deformer.
 */
namespace deformWeights
{
/**
 * Each point's weight before the envelope: the painted map (1 where unpainted), times or replaced by the connected
 * field (by fieldMode), and 0 outside the membership. One value per point of in.
 */
LILA_SCENE_API std::vector<float> effective(const DeformerNode& d, const geo::Mesh& in);

/** A paintable float map: a deformer's weights (layer -1) or a field's painted layer, on the points of mesh. */
struct MapTarget
{
    NodeId node = kNoNode;
    int layer = -1;
    NodeId meshNode = kNoNode; // the mesh it is painted on
    MeshPtr mesh;              // the points the map indexes (the deformer's input), object space
    glm::dmat4 world{1.0};     // places mesh in the world
    std::string label;
};

/**
 * The maps paintable on a mesh: each deformer in its stack, then each painted layer of a field connected to them.
 * Evaluates the graph.
 */
LILA_SCENE_API std::vector<MapTarget> paintable(Graph& g, NodeId mesh);
/** Resolves a deformer, or a field's painted layer on mesh (default: the mesh of the first deformer reading the field). Throws CommandError. */
LILA_SCENE_API MapTarget resolve(Graph& g, NodeId node, int layer = -1, NodeId mesh = kNoNode);
/** One value per point of the target's mesh: unpainted deformer points read 1, unpainted field points 0. */
LILA_SCENE_API std::vector<float> read(Graph& g, const MapTarget& t);
/** Stores the map; undoable. */
LILA_SCENE_API void write(Graph& g, const MapTarget& t, std::vector<float> values);

/** One brush sample of amount (0..1) on a weight: add (invert subtracts), replace and scale toward value. Clamped 0..1. */
LILA_SCENE_API void paint(float& w, skin::PaintMode mode, bool invert, float value, float amount);
/** Blends each (point, amount) toward its edge neighbours' average, Jacobi style. */
LILA_SCENE_API void smooth(const geo::Mesh& mesh, std::vector<float>& values, const std::vector<std::pair<uint32_t, float>>& points);
/** Copies one side's values to the other across the world plane through the origin normal to axis (0 x, 1 y, 2 z). */
LILA_SCENE_API void mirror(const geo::Mesh& worldMesh, std::vector<float>& values, int axis, bool positiveToNegative);
/** Values for dst's points from src's by closest surface point (both in world space); same point counts copy by id. */
LILA_SCENE_API std::vector<float> transfer(const geo::Mesh& srcWorld, std::span<const float> src, const geo::Mesh& dstWorld, bool byId = true);

LILA_SCENE_API void registerTypes();
LILA_SCENE_API void registerCommands();
} // namespace deformWeights
} // namespace lila::scene
