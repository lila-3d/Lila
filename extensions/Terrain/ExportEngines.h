#pragma once

#include "Terrain.h"

#include <lila/Command.h>
#include <lila/Mesh.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace terrain
{
/** One tile of a grid of samples: its column and row, and the samples it covers. */
struct TileRect
{
    int ix = 0;
    int iz = 0;
    int x0 = 0;
    int z0 = 0;
    int nx = 0;
    int nz = 0;
};

/**
 * Tiles of tileSize samples a side over an nx by nz grid, row by row. With shared, neighbours repeat their common edge
 * (tiles step tileSize - 1 samples, as Unity and most tile tools want); else they abut (Unreal's tiled import).
 * The last row and column of tiles may be smaller.
 */
std::vector<TileRect> tileGrid(int nx, int nz, int tileSize, bool shared);
/** "_x<ix>_y<iz>", the tile naming Unreal, World Machine and Gaea read. */
std::string tileSuffix(int ix, int iz);
/** The samples of a tile as a heightfield of their own, masks included, at the same place. */
Heightfield cropHeightfield(const Heightfield& h, const TileRect& r);

/** Masks that become paint layers (empty takes every mask) and their names in the engine. */
struct EngineLayers
{
    std::vector<std::string> masks;
    std::map<std::string, std::string> names;
    /** Unity and Godot: a "Base" layer first in the list takes the weight the masks leave below 1. */
    bool baseLayer = true;
};

/**
 * Tiled files: per tile <name>_x<N>_y<M>.exr (heights in cm, 32-bit float), .png and .r16 (16-bit over the whole
 * terrain's height range, so tiles join) and a .json sidecar, mask PNGs per tile, plus <name>.json listing the tiles.
 */
nlohmann::json exportFilesTiled(const Heightfield& h, const std::filesystem::path& folder, const std::string& name, int tileSize, bool shared);

/** RGBA-packed splat PNGs over the whole grid, four masks each: <name>_splat<k>.png. Returns [{file, channels}]. */
nlohmann::json writeSplatPngs(const Heightfield& h, const std::filesystem::path& folder, const std::string& name, const EngineLayers& layers);

/** Unity's heightmap resolutions: 2^n + 1 from 33 to 4097. */
bool validUnityResolution(int r);

struct UnityOptions
{
    /** Heightmap resolution per tile, 2^n + 1; 0 picks the smallest that keeps the terrain's detail, up to maxResolution. */
    int resolution = 0;
    int maxResolution = 4097;
    EngineLayers layers;
    /** The importer script to copy next to the export; empty skips it. */
    std::filesystem::path script;
};

/**
 * Unity terrains: per tile <name>_x<N>_y<M>.raw (16-bit little-endian, resolution x resolution, rows along Unity +Z),
 * <name>_x<N>_y<M>_splat.raw (8-bit alphamaps, [row][column][layer]), <name>.json and LilaTerrainImporter.cs, the
 * editor script that builds the terrains from them. Tiles share their edge rows, so neighbours join exactly.
 */
nlohmann::json exportUnity(const Heightfield& h, const glm::dmat4& world, const std::filesystem::path& folder, const std::string& name,
                           const UnityOptions& o);

struct GodotOptions
{
    /** Terrain3D region size, 64 to 2048 (a power of two); 0 picks the smallest that fits Terrain3D's 32 x 32 region map from 256. */
    int regionSize = 0;
    EngineLayers layers;
    /** The importer script to copy next to the export; empty skips it. */
    std::filesystem::path script;
};

/**
 * Terrain3D regions: per region on Terrain3D's grid <name>_x<N>_y<M>.exr (heights in metres, 32-bit float) and
 * <name>_x<N>_y<M>_splat<k>.png (the layers' weights, four per PNG), <name>.json and <name>_terrain3d.gd, the
 * EditorScript that imports them and writes the control map through Terrain3D's API.
 */
nlohmann::json exportGodot(const Heightfield& h, const glm::dmat4& world, const std::filesystem::path& folder, const std::string& name,
                           const GodotOptions& o);

struct MeshTileOptions
{
    /** Samples per tile side; neighbours share their edge row. */
    int tileSize = 257;
    /** Levels per tile, LOD0 at full resolution. */
    int lods = 3;
    /** Triangles each level keeps of the one before. */
    double lodRatio = 0.25;
    /** Depth of the skirt hung from every tile edge, cm; 0 leaves it off. */
    double skirt = 0.0;
    bool collision = true;
    /** Triangles the collision mesh keeps of LOD0. */
    double collisionRatio = 0.1;
};

struct MeshTile
{
    TileRect rect;
    std::vector<lila::geo::Mesh> lods;
    lila::geo::Mesh collision;
    /** Each level's largest height difference from the samples, cm. */
    std::vector<double> errors;
};

/**
 * Mesh tiles in the heightfield's own space: LOD0 is the samples as quads (uv over the whole terrain, masks as point
 * attributes), the other levels and the collision mesh reduced with their borders kept, so every level of a tile meets
 * every level of its neighbours without a crack. Skirts hang straight down from the edges.
 */
std::vector<MeshTile> buildMeshTiles(const Heightfield& h, const MeshTileOptions& o);

struct MeshTerrainOptions
{
    /** Quads per Mesh Partition section side. */
    int sectionQuads = 256;
};

/**
 * Unreal Mesh Terrain (UE 5.8 Mesh Partition): <name>_meshterrain.png, a 16-bit greyscale heightmap laid out so the
 * engine's FHeightmapImporter puts every mesh vertex exactly on a terrain sample, and <name>_meshterrain.json with
 * the mesh resolution, sections, size and the placement (Unreal axes, cm) LilaLink applies after the import.
 */
nlohmann::json exportUnrealMeshTerrain(const Heightfield& h, const glm::dmat4& world, const std::filesystem::path& folder, const std::string& name,
                                       const MeshTerrainOptions& o);
/** A 16-bit greyscale PNG. Throws std::runtime_error. */
void writeGray16Png(const std::filesystem::path& path, int width, int height, const std::vector<uint16_t>& values);

/**
 * Splits an exportUnreal result into regions of regionQuads quads a side for a region import: <name>_x<N>_y<M>.r16 and
 * <name>_<layer>_x<N>_y<M>.raw, each region including its far edge vertex. Adds "regions" [{x, y, rect: [x0, y0, x1, y1],
 * heightmap, layers: [{name, weightmap}]}] to the settings and returns them.
 */
nlohmann::json splitUnrealRegions(nlohmann::json settings, int regionQuads);

/** Flags terrainExport takes for these targets, appended to its own. */
std::vector<lila::FlagDef> engineExportFlags(std::vector<lila::FlagDef> flags);
/** Flags terrainSendToUnreal takes for Mesh Terrain and region sends, appended to its own. */
std::vector<lila::FlagDef> engineSendFlags(std::vector<lila::FlagDef> flags);

/**
 * terrainExport for target unity, godot or mesh, and for files with tileSize or splats; null for the targets
 * Commands.cpp writes itself.
 */
nlohmann::json exportEngineTarget(const lila::Args& a, const Heightfield& local, const glm::dmat4& world, const std::filesystem::path& folder,
                                  const std::string& name);
/**
 * terrainSendToUnreal as Mesh Terrain or a region import; null for a plain landscape send. id is the terrain's path,
 * name its file stem.
 */
nlohmann::json sendEngineVariant(const lila::Args& a, const Heightfield& local, const glm::dmat4& world, const std::string& id, const std::string& name,
                                 const std::filesystem::path& cache);

/** Where the Unity and Godot importer scripts are; a missing folder is logged and the exports go without them. */
void setEngineScripts(const std::filesystem::path& folder);
/** terrainTiles' work: the tiles as scene meshes under one group, and their stats. */
nlohmann::json makeTerrainTiles(const lila::Args& a, const Heightfield& local, const glm::dmat4& world, const std::string& name);
/** terrainTiles, and the importer scripts' location. */
void registerExportEngines(lila::ext::Context& ctx);
} // namespace terrain
