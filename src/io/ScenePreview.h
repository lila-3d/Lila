#pragma once

#include "core/Export.h"

#include <glm/glm.hpp>

#include <filesystem>
#include <vector>

namespace lila::io
{
/**
 * The meshes a .lila or .lilaa file stores, triangulated in world space with Y up, three corners per triangle: each
 * visible mesh node's own mesh before modifiers, instances, and loaded references. Meshes only made by evaluation
 * (geometry nodes, curves) are absent. Any thread, no scene; for previews. Empty when the file can't be read.
 */
LILA_SCENE_API std::vector<glm::vec3> previewTriangles(const std::filesystem::path& path);
} // namespace lila::io
