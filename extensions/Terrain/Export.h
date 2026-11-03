#pragma once

#include "Terrain.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

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
    /** Masks that become paint layers; empty takes every mask. */
    std::vector<std::string> masks;
    /** Paint layer name per mask; a mask not named here keeps its own name. */
    std::map<std::string, std::string> layerNames;
};

/** A landscape paint layer name Unreal accepts: letters, digits and underscores, starting with a letter. */
std::string unrealLayerName(const std::string& mask);

/**
 * Writes <name>.r16 (16-bit heights in Unreal's layout), <name>.png (the same as an image), <name>_<layer>.raw (8-bit
 * weightmaps, one per paint layer) and <name>.json (the import settings), resampled to a landscape size Unreal accepts.
 * h is in its own space and world places it (rotation and Z-up scenes included). Returns the settings: resolution,
 * section and component layout, actor location, rotation (pitch, yaw, roll, degrees) and scale (Unreal axes, cm), the
 * paint layers [{mask, layer, file}] and the files.
 */
nlohmann::json exportUnreal(const Heightfield& h, const glm::dmat4& world, const std::filesystem::path& folder, const std::string& name,
                            const UnrealOptions& o = {});

/** What LilaLink's setLandscape needs beyond the export settings. */
struct LandscapeSend
{
    std::string id;
    std::string label;
    /** The user's landscape material; empty lets LilaLink generate one on MF_LilaLandscapeLayer. */
    std::string material;
    bool makeMaterial = true;
    int gridSize = 0;
};

/** setLandscape's parameters from exportUnreal's settings: placement, layout, heightmap and paint layers. */
nlohmann::json landscapeParams(const nlohmann::json& settings, const LandscapeSend& s);

/** Writes <name>.exr (heights in cm), <name>.png and <name>.r16 (16-bit, 0..1 over the height range), mask PNGs and <name>.json. */
nlohmann::json exportFiles(const Heightfield& h, const std::filesystem::path& folder, const std::string& name);
} // namespace terrain
