#pragma once

#include "core/Export.h"

#include <cstdint>
#include <span>
#include <vector>

namespace lila::render
{
/** Texel layouts a viewport texture can hold. 8-bit formats are unorm, 16F are half floats, 32F floats. */
enum class TextureFormat : uint8_t
{
    R8,
    RG8,
    RGBA8,
    R16F,
    RGBA16F,
    R32F,
};

constexpr int channelCount(TextureFormat f)
{
    switch (f)
    {
    case TextureFormat::R8:
    case TextureFormat::R16F:
    case TextureFormat::R32F: return 1;
    case TextureFormat::RG8: return 2;
    case TextureFormat::RGBA8:
    case TextureFormat::RGBA16F: return 4;
    }
    return 4;
}

constexpr int texelBytes(TextureFormat f)
{
    switch (f)
    {
    case TextureFormat::R8: return 1;
    case TextureFormat::RG8: return 2;
    case TextureFormat::RGBA8: return 4;
    case TextureFormat::R16F: return 2;
    case TextureFormat::RGBA16F: return 8;
    case TextureFormat::R32F: return 4;
    }
    return 4;
}

constexpr bool isFloat(TextureFormat f)
{
    return f == TextureFormat::R16F || f == TextureFormat::RGBA16F || f == TextureFormat::R32F;
}

/** Only RGBA8 colour data is stored sRGB-encoded; every other format holds linear values. */
constexpr bool usesSrgb(TextureFormat f, bool srgb)
{
    return srgb && f == TextureFormat::RGBA8;
}

/** "r8", "rg8", "rgba8", "r16f", "rgba16f", "r32f". */
LILA_UI_API const char* formatName(TextureFormat f);
/** False when name is none of formatName's. */
LILA_UI_API bool parseFormat(const char* name, TextureFormat& out);

/**
 * Encodes RGBA float texels (4 per texel) into f's bytes: the first channelCount channels, clamped to 0..1 and rounded
 * for 8-bit formats (sRGB-encoded first when srgb and f is RGBA8), stored as they are for float formats.
 */
LILA_UI_API void encodeTexels(TextureFormat f, bool srgb, std::span<const float> rgba, std::vector<uint8_t>& out);
/** The inverse of encodeTexels: RGBA floats, missing channels 0 and alpha 1, sRGB decoded for an sRGB RGBA8. */
LILA_UI_API void decodeTexels(TextureFormat f, bool srgb, std::span<const uint8_t> texels, std::vector<float>& rgba);
} // namespace lila::render
