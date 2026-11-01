#pragma once

#include <lila/Mesh.h>

#include <span>
#include <string>
#include <string_view>

/**
 * ZBrush's .GoZ binary mesh files: points, triangles and quads, per-face UVs, polypaint, mask and polygroups.
 *
 * Mapped onto Lila meshes as uv0, the current color set (sRGB in the file, linear in Lila), the point layer
 * "sculptMask" (1 masked) and the face int layer "polygroup". Faces past four corners are fanned into triangles.
 */
namespace goz
{
inline constexpr std::string_view kPolygroupLayer = "polygroup";
inline constexpr std::string_view kMaskLayer = "sculptMask";
inline constexpr std::string_view kPolypaintSet = "polypaint";

struct File
{
    std::string name;
    lila::geo::Mesh mesh;
};

/** The file's bytes for mesh, positions as they are. */
std::string write(const lila::geo::Mesh& mesh, std::string_view name);

/** Throws std::runtime_error on a file that is not GoZ or is cut short. */
File read(std::span<const char> bytes);
} // namespace goz
