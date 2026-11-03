#pragma once

#include "MasonryBake.h"
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
    std::string mode = "hism";                 // hism | ism | merged | lowPoly | assembly
};

/**
 * What masonryShip sends for a wall built with lods. Instanced (hism, ism): an asset per stone mesh (its LOD chain) and
 * one instanced group per chunk and stone mesh, keyed "<wallKey>:<cx>_<cz>:<mesh>" so a resend replaces it in place;
 * customData is [random, height 0 at the bottom course to 1 at the top, age, damage, wet, moss]. Assembly: the stone
 * assets, plus one USD Nanite Assembly asset placing every stone, as one actor. Merged: an asset per chunk holding its stones
 * at three LODs, placed as one actor keyed "<wallKey>:<cx>_<cz>". The core is an asset and actor of its own in all three.
 * Low poly: an asset per shell (its chunk's whole wall as a box shell, the stones baked onto it), placed like merged,
 * and no core. Placements are in Unreal space.
 * {assets: [{name, contentPath, objectPath, kind (stone, chunk, core, shell), index}], groups: [{key, label, asset, mode,
 * placements}], chunks: [[cx, cz], ...], origins: [[x, y, z] per chunk, wall space], members: [[stone, ...] per chunk]}.
 */
nlohmann::json shipPlan(const Built& b, const glm::dmat4& world, const std::string& wallKey, const std::string& name, const ShipOptions& o,
                        std::span<const Shell> shells = {});

/**
 * An FBX of one static mesh named name: an LOD group when there are several levels (LOD0 first, the way Unreal imports
 * a mesh with LODs), plus each collision hull as UCX_<name>_<nn>.
 */
bool writeLodFbx(std::span<const geo::Mesh> levels, const std::string& name, const std::filesystem::path& path, std::string* err,
                 std::span<const geo::Mesh> collision = {});

/**
 * A USD Nanite Assembly (UE 5.8) of the plan's stones: a root with NaniteAssemblyRootAPI holding a point instancer whose
 * prototypes are parts with NaniteAssemblyExternalRefAPI pointing at the plan's stone mesh assets. Lila space (Y up, cm).
 */
bool writeAssemblyUsda(const Built& b, const nlohmann::json& plan, const std::filesystem::path& path, std::string* err);
} // namespace masonry
