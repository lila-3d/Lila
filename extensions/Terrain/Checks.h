#pragma once

#include "Analysis.h"

#include <nlohmann/json.hpp>

#include <string>

namespace terrain
{
/**
 * terrainCheck's road and water issues for a terrain: roads steeper or more banked than their design (roadGrade,
 * roadBank), roads over water (authored, or a river of order 3 and up) outside a bridge span or culvert (roadWater),
 * rivers whose bed or water level rises downstream (riverUphill), lakes held below their spill height with no way out (lakeNoOutlet)
 * and lakes above their rim with no dam (lakeOverflow). an is the analysis of the terrain's heightfield in its own
 * space; world places it.
 */
nlohmann::json featureIssues(const std::string& terrain, const Analysis& an, const glm::dmat4& world, double gradeTolerance);
} // namespace terrain
