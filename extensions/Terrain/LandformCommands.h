#pragma once

#include "Landform.h"

#include <lila/Command.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <set>
#include <string>

namespace lila::ext
{
class Context;
}

namespace lila::gn
{
class ExecContext;
}

namespace terrain
{
/** The landform, shaping and preset nodes (LandformNodes.cpp). */
bool registerLandformNodes(lila::ext::Context& ctx);
/** terrainLandform, terrainStroke, terrainSculptRegion and terrainStampLibrary (LandformCommands.cpp). */
void registerLandformCommands(lila::ext::Context& ctx);

/** terrainErode with preset= or age=: adds or sets the terrain's Erode Preset node and returns terrainInfo plus its report. */
nlohmann::json erodePresetCommand(const std::string& terrain, const lila::Args& a);

/**
 * terrainSculpt's brush flags as a stroke's settings, in the heightfield's own space: localHeight turns a height along
 * the scene's up axis into one, vertical is the terrain's scale along its up. Null when no brush flag is given.
 */
nlohmann::json brushSettings(const lila::Args& a, const std::function<double(double)>& localHeight, double vertical);

/** types plus every landform node type: the steps that sit after Erode Terrain, which a base-stage sculpt goes before. */
std::set<std::string> withLandformTypes(std::set<std::string> types);

/** The Thermal Erosion node's run: one talus angle, or a material mask's angle mixed in; writes talus. */
void runThermalNode(lila::gn::ExecContext& c, Heightfield& h);

/** What an Erode Preset node (by name) of a modifier did on its last evaluation: its step timings and fallbacks. */
void publishPresetReport(uint64_t modifier, const std::string& node, nlohmann::json report);
nlohmann::json presetReport(uint64_t modifier, const std::string& node);
} // namespace terrain
