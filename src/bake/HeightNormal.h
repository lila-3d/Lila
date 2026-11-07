#pragma once

#include "core/Export.h"
#include "core/ImageIO.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

/**
 * Normals that agree with a height map. An engine that displaces by height (Nanite displacement, parallax) or derives
 * normals from it rebuilds the shape's normals from the height and lays only the detail on top, so the walls of a jump
 * light as walls: a baked or painted normal only knows the tops and bottoms of a step, and between them it
 * interpolates to pointing up.
 *
 * All images are RGBA floats, row 0 the top, tangent normals encoded n * 0.5 + 0.5; heights read the red channel.
 * Sampling wraps. slope converts a height step to a tangent-space slope per UV unit along u and v: the height's depth
 * (cm from 0 to 1) over the world size of the UV square (cm), so (5 / 100, 5 / 100) is 5 cm of relief on a 1 m tile.
 */
namespace lila::bake
{
/** The normals of the height's surface, OpenGL (green up) when greenUp, else DirectX. Any size; sampled bilinearly. */
LILA_SCENE_API image::Image heightNormals(const image::Image& height, glm::dvec2 slope, bool greenUp);

/**
 * The detail a normal map holds beyond its height's shape: normal with heightNormals(height) rotated out of it (inverse
 * reoriented normal mapping), so combineNormals(detail, height) gives the normal back. Where the height at or next to a
 * texel is steeper than fadeDegrees and the normal is well off it, the detail fades to flat, so the height's wall wins
 * over a normal that missed it (a bake looking straight down never sees vertical sides); 0 keeps all of it. covered (one byte per normal texel, optional) marks the
 * texels a bake reached: uncovered neighbours read as the texel itself, as the padding a bake dilates in will.
 */
LILA_SCENE_API image::Image detailNormals(const image::Image& normal, const image::Image& height, glm::dvec2 slope, double fadeDegrees,
                                          bool greenUp, const std::vector<uint8_t>* covered = nullptr);

/** heightNormals(height) with the detail laid on it (reoriented normal mapping): what an engine rebuilds, at detail's size. */
LILA_SCENE_API image::Image combineNormals(const image::Image& detail, const image::Image& height, glm::dvec2 slope, bool greenUp);

/** The detail blend both directions use: detail rotated onto base, unit vectors in, unit vector out. */
LILA_SCENE_API glm::vec3 reorientNormal(const glm::vec3& base, const glm::vec3& detail);
} // namespace lila::bake
