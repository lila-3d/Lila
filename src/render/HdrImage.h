#pragma once

#include "core/Export.h"

#include <glm/glm.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace lila::render
{
/** Linear float RGB image, rows top to bottom. */
struct HdrImage
{
    int width = 0;
    int height = 0;
    std::vector<glm::vec3> pixels;

    glm::vec3 at(int x, int y) const { return pixels[size_t(y) * size_t(width) + size_t(x)]; }
};

/** Reads a Radiance .hdr (RGBE, flat or run-length encoded). Returns false and fills error on failure. */
LILA_UI_API bool loadHdr(const std::filesystem::path& path, HdrImage& out, std::string& error);
} // namespace lila::render
