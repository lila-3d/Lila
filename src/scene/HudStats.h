#pragma once

#include "core/Export.h"

#include "scene/Graph.h"

#include <cstdint>
#include <functional>
#include <string_view>

namespace lila::geo
{
class Mesh;
}

/** The counts behind the viewport's heads-up display, cheap enough to ask for every frame. */
namespace lila::scene::hud
{
struct PolyCount
{
    uint64_t verts = 0;
    uint64_t edges = 0;
    uint64_t faces = 0;
    uint64_t tris = 0;
    uint64_t uvs = 0;

    PolyCount& operator+=(const PolyCount& o);
    bool operator==(const PolyCount&) const = default;
};

/** Maya's three Poly Count columns. */
struct PolyStats
{
    PolyCount scene;      // every visible mesh
    PolyCount objects;    // visible meshes that are selected or under a selected transform
    PolyCount components; // selected components of the current mode; faces also give their triangles
    /** Some uv counts were carried over from a mesh's previous snapshot; ask again shortly for exact ones. */
    bool pending = false;
};

/**
 * Distinct uvs of a uv set, numbered the way Maya numbers map uvs: one per point per different uv among the point's
 * corners, so a seam point counts once for each side. 0 when the set is missing.
 */
LILA_SCENE_API uint64_t countUvs(const geo::Mesh& mesh, std::string_view uvSet);

/** One mesh's counts, uncached. */
LILA_SCENE_API PolyCount countMesh(const geo::Mesh& mesh, std::string_view uvSet);

/** effectivelyVisible(), for the HUD. */
LILA_SCENE_API bool visibleInHierarchy(const Graph& g, NodeId id);

/**
 * Counts for the HUD. Meshes are immutable snapshots, so each mesh's counts are cached per snapshot; a new snapshot
 * with the same element counts (a deform or drag) carries its uv count over until it settles. shown narrows the scene
 * further, as a view's isolate does; null shows everything. Instances sharing one mesh count once each.
 */
LILA_SCENE_API PolyStats polyStats(Graph& g, std::string_view uvSet = "uv0", const std::function<bool(NodeId)>& shown = {});
} // namespace lila::scene::hud
