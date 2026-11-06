#pragma once

#include "Terrain.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <memory>
#include <string>

namespace lila::scene
{
class Node;
}

namespace lila::link
{
class RpcClient;
}

namespace terrain
{
/** Terrain Biomes, Terrain Layers and Terrain Ground Cover. */
bool registerSurfaceNodes(lila::ext::Context& ctx);
/** terrainBiomes, terrainMaterialRules, terrainMaterialPreview, terrainMaskImages, terrainSatmap, terrainGroundCover. */
void registerSurfaceCommands(lila::ext::Context& ctx);
/** Their actions and menu items. */
void registerSurfaceUi(lila::ext::Context& ctx);

/** What a surface node published on its last run under this modifier: coverage, missing inputs, scatter counts; null before. */
nlohmann::json surfaceResult(uint64_t modifier, const std::string& node);

/**
 * terrainExport(target='files'): RGBA splat images of the terrain's material layers (bottom first, four to an image) next
 * to the other files. Null when the terrain has no Terrain Layers node.
 */
nlohmann::json surfaceSplats(const Heightfield& h, lila::scene::Node* modifier, const std::filesystem::path& folder, const std::string& name);

/**
 * terrainSendToUnreal: the material layers in setLandscape's params get their Unreal names and weight blending, and the
 * landscape material is left to sendSurfaceMaterial. Returns what sendSurfaceMaterial sends (null when the terrain has
 * no material layers).
 */
nlohmann::json surfaceLandscapeParams(nlohmann::json& params, lila::scene::Node* modifier, bool rvt);

/** Has LilaLink build the generated landscape material (and RVT) for those layers; the result, or {error}. */
nlohmann::json sendSurfaceMaterial(const std::shared_ptr<lila::link::RpcClient>& client, const nlohmann::json& surface, int timeoutMs);
} // namespace terrain
