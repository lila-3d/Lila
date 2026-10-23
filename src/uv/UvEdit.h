#pragma once

#include "core/Export.h"

#include "ops/Command.h"
#include "geo/Mesh.h"
#include "geo/Symmetry.h"
#include "scene/Components.h"
#include "scene/Node.h"

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

/** Helpers the uv commands share across files (defined in UvCommands.cpp). */
namespace lila::uv
{
using UvMeshEditFn = std::function<json(geo::Mesh&, const scene::components::Ids& ids, scene::Node&)>;

/**
 * Runs fn on a copy of every target mesh (the component selection converted to kind, or whole selected meshes when
 * allowWhole) and writes it back to inMesh as one undo step. Returns fn's results keyed by mesh name.
 */
LILA_SCENE_API json editUvMeshes(const char* label, scene::ComponentMode kind, const UvMeshEditFn& fn, bool allowWhole = true);

/** Direct edits work on the base mesh, so procedural history is baked first. */
LILA_SCENE_API void bakeUvHistory(scene::Node& n);

/** Unmarks the seams and welds the uvs across them; with move, each smaller island first travels onto its neighbour. */
LILA_SCENE_API json sewUvEdges(geo::Mesh& m, std::span<const geo::Index> edges, std::string_view uvSet, bool move = true);

/** 0, 1 or 2 when the mesh node has modeling symmetry on (setSymmetry, toggleSymmetry), else -1. */
LILA_SCENE_API int uvSymmetryAxis(const scene::Node& n);

/** The node's persistent symmetry pairs, read from a copy of the mesh; nothing with symmetry off. */
LILA_SCENE_API std::optional<geo::SymmetryMap> uvSymmetryMap(const scene::Node& n, const geo::Mesh& mesh);

/** How an edit's result is mirrored onto the partner uvs: see uvFollowRules (Delta) and symmetrizeUvIslands (Absolute). */
enum class UvSymmetry
{
    Delta,
    Absolute,
};

/**
 * Runs edit on the mesh; with the node's symmetry on, the partner uvs then follow what it did, mirrored. moved: the
 * corners the edit means to move (those that changed count too).
 */
LILA_SCENE_API json withUvSymmetry(geo::Mesh& m, const scene::Node& n, std::string_view uvSet, UvSymmetry mode, std::span<const geo::Index> moved,
                                   const std::function<json()>& edit);

/** The vertices, edges or faces (by mode) plus their mirror images, sorted and unique: what a uv editor pick selects with symmetry on. */
LILA_SCENE_API std::vector<geo::Index> mirrorComponents(const geo::SymmetryMap& map, const geo::Mesh& m, scene::ComponentMode mode,
                                                        std::span<const geo::Index> ids);

/** fn under withUvSymmetry (Absolute): islands it re-solves come out symmetric when the node's symmetry is on. */
LILA_SCENE_API UvMeshEditFn symmetricUvEdit(std::string uvSet, UvMeshEditFn fn);

/** The corners, edges or points plus their mirror images when the node's symmetry is on, sorted and unique. */
LILA_SCENE_API std::vector<geo::Index> withMirrorCorners(const scene::Node& n, const geo::Mesh& m, std::span<const geo::Index> corners);
LILA_SCENE_API std::vector<geo::Index> withMirrorEdges(const scene::Node& n, const geo::Mesh& m, std::span<const geo::Index> edges);
} // namespace lila::uv
