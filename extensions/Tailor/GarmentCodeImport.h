#pragma once

// GarmentCode / ChatGarment sewing-pattern specification import (pygarment JSON, MIT licensed format).

#include "TailorCore.h"

#include <string>

namespace tailor
{

struct GarmentCodeImportOptions
{
    float curveSpacing = 0.01f; // metres between samples on curved edges; straight edges stay one segment
};

/**
 * Parse a pattern specification (the file pygarment writes: {"pattern": {"panels", "stitches"}, "properties"}, or the
 * bare "pattern" object) into `out`. Panels keep file order; outlines are made counter-clockwise; placement follows
 * the panel's translation and Euler XYZ rotation (degrees) as pygarment's box mesh generator applies them.
 * Returns false with a message in `error` on malformed input; `out` is left untouched then.
 */
bool importGarmentCode(const std::string& jsonText, Garment& out, std::string* error = nullptr,
                       const GarmentCodeImportOptions& options = {});

bool importGarmentCodeFile(const std::string& path, Garment& out, std::string* error = nullptr,
                           const GarmentCodeImportOptions& options = {});

} // namespace tailor
