#pragma once

#include "core/Export.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace lila::dds
{
namespace fs = std::filesystem;

/** What a DDS file's texels are stored as. BC1 to BC5 and BC7 are 4x4 blocks; BC6H is HDR. */
enum class Format : uint8_t
{
    RGBA8,
    BGRA8,
    BC1,
    BC2,
    BC3,
    BC4,
    BC5,
    BC6H,
    BC7,
};

struct Level
{
    int width = 0;
    int height = 0;
    size_t offset = 0; // into File::data
    size_t size = 0;
};

/** A 2D DDS texture: its texels and mip chain as stored, largest level first. */
struct File
{
    int width = 0;
    int height = 0;
    Format format = Format::RGBA8;
    bool srgb = false; // the file says its colour is sRGB (DX10 *_SRGB formats)
    std::vector<uint8_t> data;
    std::vector<Level> levels;
};

LILA_FOUNDATION_API bool isBlockCompressed(Format f);
/** Bytes per 4x4 block (8 or 16), or 0 for an uncompressed format. */
LILA_FOUNDATION_API int blockBytes(Format f);
/** Bytes one level of w x h takes. */
LILA_FOUNDATION_API size_t levelBytes(Format f, int w, int h);

/**
 * Reads a 2D DDS: legacy DXT1-5, ATI1/ATI2 and 32-bit RGB headers, and DX10 headers for BC1-7 and RGBA8/BGRA8.
 * False with err set for anything else (cube maps, volumes, arrays, signed or other formats) or a truncated file.
 */
LILA_FOUNDATION_API bool read(const fs::path& path, File& out, std::string* err = nullptr);

/**
 * One level as RGBA floats (width * height * 4), sRGB values left as stored. BC4 reads as gray. BC5 is taken as a
 * tangent-space normal map: X and Y in red and green, Z rebuilt into blue. BC6H gives its HDR values.
 */
LILA_FOUNDATION_API std::vector<float> decodeFloat(const File& f, size_t level = 0);
/** One level's blocks of format f (any but BC6H) as RGBA8, with the same channel rules as decodeFloat. */
LILA_FOUNDATION_API std::vector<uint8_t> decodeRgba8(Format f, int width, int height, const uint8_t* texels);
} // namespace lila::dds
