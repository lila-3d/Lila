#pragma once

#include "core/Export.h"

#include "bake/BakeTypes.h"
#include "bake/Naming.h"
#include "core/ImageIO.h"

#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

/**
 * How a bake's resolved textures reach disk: the output format and bit depth, triangular dither at 8 bit (never on exact
 * maps such as IDs), layered PSDs that gather a texture set's maps as layers, and the optional metadata sidecar.
 */
namespace lila::bake
{
struct TextureBuffer;

/** The file extension of an output format: png, exr, tga, tif (tiff too) or psd; anything else gives png. */
LILA_SCENE_API std::string formatExt(std::string_view format);

/** The depth a format writes for a requested one: png and psd 8 or 16, exr 16 (half) or 32 (float), tif 8, 16 or 32, tga 8. */
LILA_SCENE_API int formatBits(std::string_view ext, int bits);

/**
 * The layered document a name template gives: the template without {map} and one separator beside it, so
 * "T_{mesh}_{map}" names "T_Crate.psd" and every map of the set becomes one of its layers.
 */
LILA_SCENE_API std::string layeredName(std::string_view tmpl, NameTokens tokens, bool forceUdim);

/** What a texture's sidecar and an importer need: map, suffix, colorSpace (srgb | linear), normal convention, bits, decode meta. */
LILA_SCENE_API json textureInfo(const TextureBuffer& tex);

class LILA_SCENE_API TextureWriter
{
public:
    /** inputHash goes into every sidecar so a reader can tell which inputs a texture came from. */
    TextureWriter(const OutputSettings& output, std::string inputHash = {});

    /**
     * Writes tex's resolved image now, or keeps it as a layer of `layered` (a PSD) when that is set, layers in the order
     * added. Thread safe, so textures can encode in parallel. Throws std::runtime_error.
     */
    void add(const TextureBuffer& tex, image::Image img, const std::filesystem::path& layered = {});
    /** Writes the layered documents, their layers in the order they were added (the first on top). */
    void finish();

private:
    struct Pending
    {
        int bits = 8;
        bool dither = false;
        std::vector<std::pair<std::string, image::Image>> layers;
        json info = json::array();
    };

    image::WriteOptions optionsFor(const TextureBuffer& tex) const;

    OutputSettings mOutput;
    std::string mHash;
    std::mutex mMutex;
    std::map<std::filesystem::path, Pending> mLayered;
    std::vector<std::filesystem::path> mLayeredOrder;
};
} // namespace lila::bake
