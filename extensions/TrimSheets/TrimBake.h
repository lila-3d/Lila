#pragma once

#include "TrimSheet.h"

#include <filesystem>
#include <string>
#include <vector>

namespace trims
{
struct SheetBakeOptions
{
    /** auto (gpu when registered, else cpu), cpu or gpu. */
    std::string backend = "auto";
    /** Rebake only these trims into the existing textures; all when empty. */
    std::vector<size_t> only;
    /** Wrap tiling strips (see GenerateOptions::tile). */
    bool tile = true;
};

/**
 * Generates the sheet's high and low and bakes its maps into the sheet's texture folder (T_<sheet>_<map>), plus an exact
 * ID map with one colour per trim. With only, the bake runs into a scratch folder and just those trims' rectangles
 * (and their padding) are copied over the existing textures. Returns the Baker's result plus folder, idMap, seamError
 * (mean normal difference across the U wrap of tiling strips, 0 is seamless) and trims.
 */
nlohmann::json bakeSheet(const Sheet& sheet, const std::filesystem::path& sheetFile, const SheetBakeOptions& opts = {});

/** The texture a sheet's map was baked to, whether it exists or not. */
std::filesystem::path mapFile(const Sheet& sheet, const std::filesystem::path& sheetFile, std::string_view map);

/** Mean absolute difference between the first and last column of the tiling strips' rows, 0 to 1. */
double seamError(const lila::image::Image& image, const Sheet& sheet);
} // namespace trims
