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
    std::string mode = "hism";                 // hism | ism
};

/**
 * What masonryShip sends for a wall built with lods: an asset per stone mesh (its LOD chain) plus the core, and one
 * instanced group per chunk and stone mesh, keyed "<wallKey>:<cx>_<cz>:<mesh>" so a resend replaces it in place.
 * Placements are in Unreal space; customData is [random, height 0 at the bottom course to 1 at the top].
 * {assets: [{name, contentPath, objectPath, mesh (-1 for the core)}], groups: [{key, label, asset, mode, placements}],
 * chunks: [[cx, cz], ...]}.
 */
nlohmann::json shipPlan(const Built& b, const glm::dmat4& world, const std::string& wallKey, const std::string& name, const ShipOptions& o);

/** An FBX holding one LOD group, LOD0 first, the way Unreal imports a mesh with LODs. */
bool writeLodFbx(std::span<const geo::Mesh> levels, const std::string& name, const std::filesystem::path& path, std::string* err);
} // namespace masonry
