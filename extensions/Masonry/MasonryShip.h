#pragma once

#include "MasonryCore.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

namespace masonry
{
struct ShipOptions
{
    std::string folder = "/Game/Lila/Masonry"; // content folder; each wall's assets go in <folder>/<name>
    double chunkSize = 400.0;                  // cm, ground cell per instanced component
    std::string mode = "hism";                 // hism | ism | merged
};

/**
 * What masonryShip sends for a wall built with lods. Instanced (hism, ism): an asset per stone mesh (its LOD chain) and
 * one instanced group per chunk and stone mesh, keyed "<wallKey>:<cx>_<cz>:<mesh>" so a resend replaces it in place;
 * customData is [random, height 0 at the bottom course to 1 at the top]. Merged: an asset per chunk holding its stones
 * at three LODs, placed as one actor keyed "<wallKey>:<cx>_<cz>". The core is an asset and actor of its own either way.
 * Placements are in Unreal space.
 * {assets: [{name, contentPath, objectPath, kind (stone, chunk, core), index}], groups: [{key, label, asset, mode,
 * placements}], chunks: [[cx, cz], ...], origins: [[x, y, z] per chunk, wall space], members: [[stone, ...] per chunk]}.
 */
nlohmann::json shipPlan(const Built& b, const glm::dmat4& world, const std::string& wallKey, const std::string& name, const ShipOptions& o);

/**
 * An FBX of one static mesh named name: an LOD group when there are several levels (LOD0 first, the way Unreal imports
 * a mesh with LODs), plus each collision hull as UCX_<name>_<nn>.
 */
bool writeLodFbx(std::span<const geo::Mesh> levels, const std::string& name, const std::filesystem::path& path, std::string* err,
                 std::span<const geo::Mesh> collision = {});
} // namespace masonry
