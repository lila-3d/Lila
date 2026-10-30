#pragma once

#include "TrimSheet.h"

namespace trims
{
/** The flat low poly and the bevelled high poly a sheet bakes from. Both lie on the XZ plane facing +Y, the sheet's top-left at the origin. */
struct Generated
{
    geo::Mesh low;
    geo::Mesh high;
    /** How far the high rises above and sinks below the low, cm: the bake's frontal and rear distances. */
    double above = 0.0;
    double below = 0.0;
};

struct GenerateOptions
{
    /** Repeat tiling strips one sheet width to each side, so occlusion and bevel rays wrap like the texture will. */
    bool tile = true;
    /** Only these trims (indices); all when empty. The low still covers the whole sheet. */
    std::vector<size_t> only;
};

/** Sheet pixel to the plane: x right, z down the texture. */
glm::dvec3 sheetPoint(const Sheet& sheet, glm::dvec2 px, double height = 0.0);

/** The sheet's low plane, uv0 over 0-1 with V up. */
geo::Mesh lowPlane(const Sheet& sheet);

/** Builds the high poly from every trim's profile (corner "color" per trim, as trimColor) and the low plane. */
Generated generate(const Sheet& sheet, const GenerateOptions& opts = {});
} // namespace trims
