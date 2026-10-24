#pragma once

#include "core/Export.h"

#include "core/ImageIO.h"
#include "tex/Types.h"

#include <functional>

#include <string>
#include <string_view>
#include <vector>

namespace lila::scene
{
class Graph;
class Node;
} // namespace lila::scene

/**
 * Generators: mask graphs (resources/texgenerators, category Generator) whose port inputs are mesh map roles. A layer mask
 * using one binds each input by name to a mesh input node (meshCurvature, meshAo, ...) reading the set's baked map, or to
 * an anchor, so the same generator adapts to any mesh the set bakes from.
 */
namespace lila::tex
{
/** A mesh map a generator input can bind to. */
struct MeshRole
{
    /** The binding name: curvature, ao, worldNormal... */
    std::string name;
    /** The input node that reads it and its output. */
    std::string nodeType;
    std::string socket;
    PortType type = PortType::Gray;
    /** The bake map id the node reads (its external). */
    std::string bakeMap;
};

LILA_SCENE_API const std::vector<MeshRole>& meshRoles();
LILA_SCENE_API const MeshRole* findMeshRole(std::string_view name);

/** Node types in the Generator category. */
LILA_SCENE_API std::vector<std::string> generatorTypes();

/** The bake map an external map name reads: worldNormal is the objectNormal map (bake space is world space), others are themselves. */
LILA_SCENE_API std::string bakeMapName(std::string_view external);

/**
 * A bake project's live mesh map (render/MeshMaps.h) for a tile, read back as RGBA floats, for CPU evaluation. Empty when the
 * project has no live map of that name or no renderer runs. The renderer installs it.
 */
using LiveMeshMapReader = std::function<image::Image(const std::string& bakeProject, const std::string& map, int udim)>;
LILA_SCENE_API void setLiveMeshMapReader(LiveMeshMapReader fn);
LILA_SCENE_API image::Image readLiveMeshMap(const std::string& bakeProject, const std::string& map, int udim);

/** Of the maps (externals) given, those that are bake maps the set's bake project doesn't make. Empty without a project. */
LILA_SCENE_API std::vector<std::string> missingBakeMaps(scene::Graph& g, const std::string& bakeProject, const std::vector<std::string>& maps);
/** Adds the maps to the bake project (undoable) and returns those added. */
LILA_SCENE_API std::vector<std::string> addBakeMaps(scene::Graph& g, const std::string& bakeProject, const std::vector<std::string>& maps);
} // namespace lila::tex
