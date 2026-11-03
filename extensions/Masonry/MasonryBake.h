#pragma once

#include "MasonryCore.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

namespace masonry
{
/** Slot of the far LODs (clean and box), which take the atlas normal map that stands in for the worn geometry. */
inline constexpr const char* kStoneFarSlot = "MasonryStoneFar";
/** Slot of the low poly shells. */
inline constexpr const char* kShellSlot = "MasonryShell";

/** Rectangles packed on shelves into the unit square at one shared scale: rectangle k spans origin[k] + size[k] * scale. */
struct Packed
{
    std::vector<glm::dvec2> origin;
    double scale = 0.0;
};

/** Largest scale whose shelf packing fits, with pad (a fraction of the square) around every rectangle. Deterministic. */
Packed shelfPack(std::span<const glm::dvec2> sizes, double pad);

/**
 * Gives every stone mesh's LOD chain one place in a shared atlas: each mesh's six box faces are packed at one texel
 * density, and every level maps through the same box faces (a point goes to the face its polygon mostly faces), so the
 * levels read the same texels. The atlas goes in uv0 and the tiling projection moves to uv1; the far levels go on
 * kStoneFarSlot. Needs b.lods. Returns texels per cm at the given atlas size.
 */
double atlasStones(Built& b, int size, int padding);

/** One ground chunk of a wall as a low poly box shell: the wall's full volume, uv0 unwrapped on its own. */
struct Shell
{
    glm::ivec2 cell{0};
    glm::dvec3 origin{0.0};       // wall space, where the chunk's mesh is placed from
    geo::Mesh mesh;               // wall space
    std::vector<geo::Mesh> hulls; // convex pieces, wall space
};

/** The walls' shells, split where the footprints cross the size cm ground grid, in cell order. */
std::vector<Shell> lowPolyShells(std::span<const Footprint> footprints, const WallParams& p, double size, int texture, int padding);

/** Shells from convex cells (Built::shellCells: the walls as they stand, ruined or not), grouped by the size cm ground grid. */
std::vector<Shell> shellsFromCells(std::span<const geo::Mesh> cells, double size, int texture, int padding);

/** A bake of the stones' atlas or the shells, through the Baker. */
struct BakeSettings
{
    std::filesystem::path folder; // absolute
    int size = 2048;
    int padding = 8; // texels between islands; maps dilate half of it
    std::string backend = "auto"; // auto (gpu when registered, else cpu), cpu, gpu
    std::vector<std::string> maps = {"normal", "ao", "curvature"};
};

/**
 * Bakes the worn stones (LOD0) onto their boxes (LOD2) into one atlas texture set, T_<project>_<map>. After atlasStones.
 * Returns the Baker's result (BakeResult::toJson).
 */
nlohmann::json bakeAtlas(const Built& b, double bevel, const std::string& project, const BakeSettings& s);

/** Bakes each chunk's stones and the core onto its shell, a texture set per chunk, T_<project>_<cx>_<cz>_<map>. */
nlohmann::json bakeShells(const Built& b, std::span<const Shell> shells, const WallParams& p, const std::string& project, const BakeSettings& s);
} // namespace masonry
