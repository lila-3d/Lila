#pragma once

#include "core/Export.h"

#include "ops/Command.h"
#include "geo/Mesh.h"
#include "geo/Symmetry.h"
#include "scene/Components.h"
#include "scene/Node.h"
#include "uv/FinalMesh.h"
#include "uv/Pack.h"

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

/** Helpers the uv commands share across files (defined in UvCommands.cpp). */
namespace lila::uv
{
using UvMeshEditFn = std::function<json(geo::Mesh&, const scene::components::Ids& ids, scene::Node&)>;

/** The uvSet flag the uv commands take: a set name, else the current set. */
inline FlagDef uvSetFlag() { return {"uvSet", "", ArgType::String, nullptr, "UV set; the current one (uvSetCurrent) when not given."}; }

/**
 * Runs fn on a copy of every target mesh (the component selection converted to kind, or whole selected meshes when
 * allowWhole) and writes it back to inMesh as one undo step. Returns fn's results keyed by mesh name.
 */
LILA_SCENE_API json editUvMeshes(const char* label, scene::ComponentMode kind, const UvMeshEditFn& fn, bool allowWhole = true);

/** The finalMesh flag: uv tools measure and solve on the mesh as exported, after its modifiers. */
inline FlagDef finalMeshFlag()
{
    return {"finalMesh", "fm", ArgType::Bool, true,
            "Measure and solve on the mesh as exported, after modifiers that move its surface (bevel, subdivision); False works on the "
            "base mesh alone."};
}

/** The mesh's modifiers as exported, from its cage up. Empty when it has none. */
LILA_SCENE_API Stack exportStack(scene::Node& mesh);

/**
 * With on, re-solves the uv islands of the faces against the exported mesh and returns what happened for the command's
 * result (null when the modifiers leave the surface alone). A modifier the solve can't follow is logged and reported.
 * usedMap, when given, gets the map the solve used.
 */
LILA_SCENE_API json refineOnExport(geo::Mesh& m, std::span<const geo::Index> faces, std::string_view uvSet, scene::Node& n,
                                   const FinalSolveOptions& opts, bool on, std::shared_ptr<const FinalMap>* usedMap = nullptr);

/**
 * Other meshes' uv triangles in the set (as exported) on a texture this mesh also uses: faces under a material slot
 * both use. The default slot is each mesh's own texture. For tools that must keep islands off them.
 */
LILA_SCENE_API std::vector<std::array<glm::dvec2, 3>> sharedTextureTriangles(scene::Node& n, const geo::Mesh& m, std::string_view uvSet);

/** With on, the exported mesh traced back to the cage's faces (traceFaces), for measuring uvs where they are exported. */
LILA_SCENE_API FinalMap exportTraceOf(const geo::Mesh& m, scene::Node& n, bool on);

/**
 * The end of a uv tool that lays out islands: with on and modifiers that move the surface, re-solves the faces' islands
 * against the exported mesh, then repacks them with the export's density when the tool packed. Returns refineOnExport's
 * report.
 */
LILA_SCENE_API json finishOnExport(geo::Mesh& m, std::span<const geo::Index> faces, std::string_view uvSet, scene::Node& n, bool on,
                                   const PackOptions* repack = nullptr);

/** With on, sets each chart's densityArea from the exported mesh. Logs and returns a warning when the cage is used instead. */
LILA_SCENE_API std::string densityOnExport(const geo::Mesh& m, std::span<Chart> charts, std::string_view uvSet, scene::Node& n, bool on);

/** The base mesh's plug under any modifiers (modifiers::editPlug): uv edits go there so the stack survives. */
LILA_SCENE_API scene::PlugRef uvEditPlug(scene::Node& n);

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

/** The corners plus their images under the map, sorted and unique. */
LILA_SCENE_API std::vector<geo::Index> mirrorCorners(const geo::SymmetryMap& map, const geo::Mesh& m, std::span<const geo::Index> corners);

/**
 * The uv corners a component selection covers: the uv vertices picked in the UV Editor when the vertex selection came
 * from there, else faces' uv wedges, or every corner of the selected points or edges.
 */
LILA_SCENE_API std::vector<geo::Index> selectionCorners(const geo::Mesh& m, scene::NodeId node, scene::ComponentMode kind,
                                                        std::span<const uint32_t> ids, std::string_view uvSet);

/** The corners, edges or points plus their mirror images when the node's symmetry is on, sorted and unique. */
LILA_SCENE_API std::vector<geo::Index> withMirrorCorners(const scene::Node& n, const geo::Mesh& m, std::span<const geo::Index> corners);
LILA_SCENE_API std::vector<geo::Index> withMirrorEdges(const scene::Node& n, const geo::Mesh& m, std::span<const geo::Index> edges);
} // namespace lila::uv
