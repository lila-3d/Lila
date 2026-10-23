#pragma once

#include "core/Export.h"

#include "render/RenderMesh.h"

#include <QImage>

#include <array>
#include <functional>
#include <string>

namespace lila::ui::textures
{
/**
 * Texture images for the viewport, read with QImage (.hdr through the renderer's loader) on worker threads and
 * handed to the renderer as RGBA8. Main thread only. Every getter returns null while its file loads (the viewport
 * redraws once it arrives) or when it can't be read, and reloads a file that changes on disk.
 */
LILA_UI_API render::TexturePtr get(const std::string& path, bool srgb);

/**
 * One linear image with occlusion in R, roughness in G and metallic in B: the packed map when given, its channels
 * picked by `channels` (the source channel of occlusion, roughness and metallic), else the red channels of the
 * separate maps, white where one is missing.
 */
LILA_UI_API render::TexturePtr packed(const std::string& orm, const std::string& occlusion, const std::string& roughness,
                          const std::string& metallic, std::array<int, 3> channels = {0, 1, 2});

/** Square preview of an image for the material editor, null while loading. */
LILA_UI_API QImage thumbnail(const std::string& path, int size);

/** Drops a get() image from the cache, e.g. a frame of an image sequence that is no longer shown. */
LILA_UI_API void release(const std::string& path, bool srgb);

/** Why a path failed to load, empty when it loaded or is still loading. */
LILA_UI_API std::string error(const std::string& path);

/** Called on the main thread whenever an image finishes loading. */
LILA_UI_API int addListener(std::function<void()> fn);
LILA_UI_API void removeListener(int id);
} // namespace lila::ui::textures
