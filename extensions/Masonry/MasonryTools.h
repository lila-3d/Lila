#pragma once

#include "MasonryCore.h"

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace masonry
{
/**
 * Wall footprints from a blockout mesh (world space): sliced with level planes every band cm, each section's outline
 * offset inward to the wall's centre line. A hollow section (an outline round a hole) takes its thickness from the gap;
 * a solid one uses thickness. Bands with the same outline merge into one taller wall. thicknessUsed gets the thickness
 * the walls were laid at.
 */
std::vector<Footprint> footprintsFromBlockout(const geo::Mesh& mesh, double thickness, double band, double* thicknessUsed);

/** Two stones touching, for a destruction's links: where, and over how much area, cm2. b is -1 for the ground. */
struct StoneLink
{
    int a = -1, b = -1;
    glm::dvec3 at{0.0};
    double area = 0.0;
};
/** Every pair of stones touching within pad, and every grounded stone to the ground. */
std::vector<StoneLink> stoneLinks(std::span<const Stone> stones, double pad);

/** A contact sheet: each set of stones drawn in elevation (looking along view) in its own tile, tiles in a grid, as a PNG. */
bool writeContactSheet(std::span<const std::vector<Stone>> variants, const glm::dvec3& view, int tile, const std::filesystem::path& path,
                       std::string* err);
} // namespace masonry
