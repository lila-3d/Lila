#pragma once

#include "Terrain.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

namespace terrain
{
/** An Unreal landscape's component layout: vertices per side are components * sectionsPerComponent * quadsPerSection + 1. */
struct UnrealLayout
{
    int quadsPerSection = 63;
    int sectionsPerComponent = 1;
    int componentsX = 1;
    int componentsY = 1;

    int vertsX() const { return componentsX * sectionsPerComponent * quadsPerSection + 1; }
    int vertsY() const { return componentsY * sectionsPerComponent * quadsPerSection + 1; }
};

/** The layout that holds at least this many vertices per side with the least waste, preferring fewer components. */
UnrealLayout chooseUnrealLayout(int vertsX, int vertsY);

struct UnrealOptions
{
    /** Keep the cell size and let the landscape grow past the terrain; otherwise resample so it covers the same area. */
    bool pad = false;
    /** Smallest Z scale; 100 is Unreal's default, 256 m either side of the actor. */
    double minScaleZ = 1.0;
};

/**
 * Writes <name>.r16 (16-bit heights in Unreal's layout), <name>.png (the same as an image), <name>_<mask>.raw (8-bit
 * weightmaps) and <name>.json (the import settings), resampled to a landscape size Unreal accepts. Returns the
 * settings: resolution, section and component layout, actor location and scale (Unreal axes, cm), files.
 */
nlohmann::json exportUnreal(const Heightfield& h, const std::filesystem::path& folder, const std::string& name, const UnrealOptions& o = {});

/** Writes <name>.exr (heights in cm), <name>.png and <name>.r16 (16-bit, 0..1 over the height range), mask PNGs and <name>.json. */
nlohmann::json exportFiles(const Heightfield& h, const std::filesystem::path& folder, const std::string& name);
} // namespace terrain
