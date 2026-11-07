#pragma once

#include "Terrain.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace terrain
{
/** terrainSpec, its schema and presets as MCP resources, and the make_terrain prompt. */
void registerSpec(lila::ext::Context& ctx);

/** The JSON schema of a terrain spec. */
const nlohmann::json& terrainSpecSchema();

/** Preset names, and one preset's spec; throws CommandError for an unknown name. */
std::vector<std::string> terrainSpecPresets();
nlohmann::json terrainSpecPreset(const std::string& name);

/** base with overrides merged in: objects key by key, anything else replaced. */
nlohmann::json mergeSpec(nlohmann::json base, const nlohmann::json& overrides);

/** Builds a terrain from a spec as terrainSpec does: {ok, terrain, steps, stats, check, ...}. */
nlohmann::json buildTerrainSpec(const nlohmann::json& spec, bool replace, bool stopOnError, bool full);

/** A spec that rebuilds the terrain: {spec, from: "spec" | "graph", notes}. */
nlohmann::json readTerrainSpec(const std::string& terrain);
} // namespace terrain
