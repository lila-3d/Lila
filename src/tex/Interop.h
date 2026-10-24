#pragma once

#include "core/Export.h"

#include "tex/TextureSet.h"

#include <filesystem>
#include <string>
#include <vector>

/**
 * Texture sets to and from other tools' files: any channel or node output as an image, the layer stack as one layered PSD
 * per channel, and images or PSD layers brought in as paint or fill layers.
 */
namespace lila::tex
{
struct PsdOptions
{
    /** Channel names; all when empty. */
    std::vector<std::string> channels;
    /** Relative folders resolve as bake output folders do. Empty: textures/<set>. */
    std::string folder;
    int width = 0;
    int height = 0;
    std::string backend;
};

/**
 * Writes <folder>/<set>_<channel>[.<udim>].psd per channel and tile: a bottom "Base" layer (the stack's base value) and one layer
 * per stack layer that writes the channel, its values in RGB and its mask times its opacity (and its folders' masks) in alpha,
 * named after the layer (with the blend mode when it isn't normal). A set without a stack gets its composite as one layer.
 * Returns {files, warnings}. Throws.
 */
LILA_SCENE_API json exportLayerPsd(scene::Node& set, const PsdOptions& options);

/**
 * Files the scene's texture sets keep outside the scene file, for source control: every image a set's graph reads (paint
 * layer and painted mask images, imported layers, image parameters) under the workspace or the scene's folder, and the
 * scene's page store (<scene>.lilatex/) when there is one.
 */
LILA_SCENE_API std::vector<std::filesystem::path> sceneTextureFiles(scene::Graph& g, const std::filesystem::path& scene);

/** Save hook: opens sceneTextureFiles that are new for add in the scene's changelist (the bake outputs' rules). */
LILA_SCENE_API void onSceneSaved(const std::filesystem::path& scene);

/** texExportImage, texExportPsd, texImportImage (called by registerCommands). */
LILA_SCENE_API void registerInteropCommands();
} // namespace lila::tex
