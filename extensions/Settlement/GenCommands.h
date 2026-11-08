#pragma once

#include "Generators.h"

#include <lila/Command.h>

namespace lila::ext
{
class Context;
}
namespace lila::scene
{
class Graph;
}

namespace settlement
{
/** settlementCreate's generator flags: tensor, eras, walls, districts, osm, osmFile, osmOrigin, example, contourStreets, levels. */
std::vector<lila::FlagDef> generatorFlags();
/**
 * Folds the generator flags of a call into values["generators"] (JSON text): curves and files read, terrain commands run
 * by name, another settlement measured. Sets values["pattern"] to the generator's pattern when the call names none.
 * region is the site's outline for contour streets. Throws CommandError naming cmd.
 */
void resolveGeneratorFlags(lila::scene::Graph& g, const lila::Args& a, json& values, std::span<const Vec2> region, const std::string& ground,
                           const char* cmd);
/** settlementGenerators, settlementEras, settlementWalls, settlementLevels. */
void registerGeneratorCommands(lila::ext::Context& ctx);
} // namespace settlement
