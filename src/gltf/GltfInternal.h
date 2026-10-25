#pragma once

#include "fbx/Fbx.h"
#include "math/MathCore.h"

#include <glm/glm.hpp>

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/** Pieces shared by the glTF reader and writer. */
namespace lila::gltf
{
namespace fs = std::filesystem;

/** Lila axes to glTF axes for the scene's up axis: a rotation (signed axis permutation), no unit scale. */
inline glm::dmat3 lilaToFile(math::UpAxis up) { return fbx::profileBasis(fbx::AxisProfile::Ue6, up); }

fs::path utf8Path(std::string_view s);
std::string utf8(const fs::path& p);

/** Whole file as bytes; false when it can't be read. */
bool readFile(const fs::path& path, std::string& out);
bool writeFile(const fs::path& path, std::string_view bytes);

/** "image/png", "image/jpeg", ... from the leading bytes; empty when unknown. */
std::string sniffMime(std::string_view bytes);
/** ".png" for "image/png" and so on; ".bin" when unknown. */
std::string extensionForMime(std::string_view mime);
/** "image/png" or "image/jpeg" by extension, empty for anything glTF can't carry as is. */
std::string gltfMimeForPath(const fs::path& path);

/** RGBA texels 0 to 1, row 0 at the top, as stored (no color space conversion). */
struct Pixels
{
    int width = 0;
    int height = 0;
    int bits = 8;
    std::vector<float> rgba;

    bool empty() const { return width <= 0 || height <= 0; }
    glm::vec4 at(int x, int y) const;
    /** Bilinear, wrapping, u right and v down from the top left (glTF's image space). */
    glm::vec4 sample(float u, float v) const;
};

/** PNG, JPEG, TGA, BMP, GIF, PSD and HDR from memory (stb_image). */
bool decodeImage(std::string_view bytes, Pixels& out, std::string* error);
/** Any of the above, plus EXR and TIFF through Lila's own readers. */
bool loadImage(const fs::path& path, Pixels& out, std::string* error);
/** 8 or 16 bit PNG bytes. */
std::string encodePng(const Pixels& p);

/** The base64 data URI of bytes. */
std::string dataUri(std::string_view mime, std::string_view bytes);
} // namespace lila::gltf
