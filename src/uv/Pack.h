#pragma once

#include "core/Export.h"

#include "uv/Chart.h"

#include <glm/glm.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lila::uv
{
/** Where islands go when there is more than one place to put them. */
enum class PackLayout : uint8_t
{
    Fill,         // fill the UDIM tiles in order (1001, 1002...), spilling over when one is full
    GroupTiles,   // each group gets its own UDIM tile, in group order, rows of tilesU
    GroupRegions, // each group gets its own part of the region in tile 1001, sized by its area
    KeepTile,     // each island stays in the UDIM tile its centre is in now
};

/** What makes a group, for the group layouts. */
enum class PackGroupBy : uint8_t
{
    None,
    Object,    // one group per mesh
    Material,  // the face "material" slot
    Attribute, // a face int attribute (PackOptions::groupAttr)
};

struct PackOptions
{
    /** Target texture size in pixels; padding and margin are measured in its texels. */
    int resolution = 1024;
    /** Texels between islands. */
    double padding = 4.0;
    /** Texels from the tile (or region) border; negative means half the padding. */
    double margin = -1.0;
    /** Padding and margin hold down to this mip level: both are multiplied by 2^paddingMips. */
    int paddingMips = 0;

    /** 0 keeps orientation, 90 tries quarter turns, smaller steps (45, 15...) try more angles. */
    int rotateStep = 90;
    /** Angles to try in degrees; replaces rotateStep when not empty. */
    std::vector<double> angles;
    /** Any angle: each island also tries its three best-fitting angles (smallest bounding box, 5 degree steps and hull edges) and their quarter turns. */
    bool freeRotation = false;
    /** Also try each island mirrored (flips its uvs, so the texture reads mirrored on it). */
    bool flip = false;
    /** Turn each island to its smallest bounding rectangle before trying rotations. */
    bool preOrient = true;

    /** Scale every island to its surface area first, so texel density is even across islands and objects. */
    bool normalizeScale = true;
    /** Islands keep their exact uv size (their texel density): they are only moved and turned. Overrides normalizeScale. */
    bool lockScale = false;
    /**
     * Texels per surface unit to pack at instead of the largest scale that fits (texel density lock).
     * Islands that do not fit spill into further tiles; the pack fails when they run out.
     */
    double texelsPerUnit = 0.0;

    /** Islands with the same shape share one spot (duplicates, and mirror images with stackMirrored). */
    bool stack = false;
    bool stackMirrored = true;
    /** Largest vertex mismatch for two islands to stack, as a fraction of the island's size. */
    double stackTolerance = 0.002;

    PackLayout layout = PackLayout::Fill;
    PackGroupBy groupBy = PackGroupBy::None;
    std::string groupAttr = "uvGroup";
    /** UDIM tiles to fill, row by row from 1001. */
    int tilesU = 1;
    int tilesV = 1;
    /** Part of each tile to pack into, in tile uvs. */
    glm::dvec2 regionMin{0.0};
    glm::dvec2 regionMax{1.0};
    /** Islands with a pinned uv stay where they are, and the others pack around them. */
    bool fixPinned = false;

    /**
     * Raster cells across a tile for the search, up to 4096. 0 picks the texture resolution capped by effort (1024, 2048,
     * then 4096). Finer packs denser, coarser is faster; padding is exact either way.
     */
    int workingResolution = 0;
    /** Search effort 0-3: island orders tried per scale (1, 3, 6, 12) and the default search grid. Denser but slower as it rises. */
    int effort = 1;
    /** Stop refining after this many seconds (0 never). With a limit, results can vary from run to run. */
    double timeLimit = 0.0;
};

/** Per chart extras for packCharts. */
struct PackChartInfo
{
    /** Group for the group layouts. */
    int group = 0;
    /** Left where it is and packed around. */
    bool fixed = false;
};

struct PackResult
{
    bool ok = false;
    bool cancelled = false;
    /** Texels per surface unit the islands ended up at (with normalizeScale), else texels per old uv unit. */
    double texelsPerUnit = 0.0;
    /** Fraction of the used tiles' (or regions') area covered by islands: the packing density. */
    double coverage = 0.0;
    int tilesUsed = 0;
    size_t stacked = 0;
    /** Per chart: the chart it is stacked on, or -1. */
    std::vector<int> stackedOn;
    /** Per chart: the UDIM tile it landed in, 0 for 1001. Fixed charts keep -1. */
    std::vector<int> tile;
    double seconds = 0.0;
    /** Layouts tried. */
    int attempts = 0;
};

/** Packs the charts' uvs. Deterministic (without a time limit); parallel; cancellable through the current progress scope. */
LILA_SCENE_API PackResult packCharts(std::span<Chart> charts, const PackOptions& opts = {}, std::span<const PackChartInfo> info = {});

/** One mesh for packMeshes. */
struct PackTarget
{
    geo::Mesh* mesh = nullptr;
    /** Faces whose islands are packed; all faces when empty. */
    std::vector<Index> faces;
    /** The mesh's other islands stay where they are and the packed ones go around them. */
    bool othersFixed = false;
    /** Islands touching these faces stay where they are and the rest go around them. */
    std::vector<Index> fixedFaces;
    /** Object to world, so surface area (and so texel density) is measured in world space across objects. */
    glm::dmat3 linear{1.0};
};

/** Packs the islands of several meshes into one layout and writes them back; nothing is written unless it succeeds. */
LILA_SCENE_API PackResult packMeshes(std::span<PackTarget> targets, std::string_view uvSet, const PackOptions& opts = {});

/** Packs the uv islands of the faces (all when empty) and writes them back. */
LILA_SCENE_API PackResult packIslands(geo::Mesh& mesh, std::span<const Index> faces, std::string_view uvSet, const PackOptions& opts = {});

/** Islands lying on top of one another with the same shape, as face lists, one entry per stack of two or more. */
LILA_SCENE_API std::vector<std::vector<std::vector<Index>>> stackedIslands(const geo::Mesh& mesh, std::string_view uvSet, double tolerance = 0.002);
} // namespace lila::uv
