#pragma once

#include "core/Export.h"
#include "core/ImageIO.h"

#include "scene/Deformers.h"

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace lila::geo
{
class Mesh;
}

/** The texture deformer and the tension bakes (TextureDeformers.cpp). */
namespace lila::scene::deformers
{
/**
 * A texture set's channel or node.socket as an image, for textureDeformer: the set's name, the source, and whether to
 * drop a cached evaluation first. Null when there is no such set. Throws when it fails to evaluate. tex installs it.
 */
using TextureSetSource = std::function<std::shared_ptr<const image::Image>(const std::string& set, const std::string& source, bool refresh)>;
LILA_SCENE_API void setTextureSetSource(TextureSetSource fn);

/** RGBA at a uv, bilinear and wrapping; V up, so row 0 is v = 1. */
LILA_SCENE_API glm::vec4 sampleUv(const image::Image& img, const glm::dvec2& uv);

/**
 * Per-point colours painted into a width x height image through a uv set, interpolated across each triangle, then
 * grown `padding` texels past the uv islands so filtering never reads the background. Empty when the set is missing.
 */
LILA_SCENE_API image::Image rasterizeByUv(const geo::Mesh& mesh, std::string_view uvSet, std::span<const glm::vec4> pointColours, int width,
                                          int height, int padding = 4);

void registerTextureTypes();
void registerTextureCommands();

/** Drops a textureDeformer's cached image so it reads its file or texture set again. False for other types. */
bool rebindTexture(Graph& g, DeformerNode& d);
} // namespace lila::scene::deformers
