#include "render/TextureFormat.h"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>

namespace lila::render
{
namespace
{
constexpr TextureFormat kAll[] = {TextureFormat::R8,      TextureFormat::RG8,     TextureFormat::RGBA8,
                                  TextureFormat::R16F,    TextureFormat::RGBA16F, TextureFormat::R32F};

float toSrgb(float v)
{
    v = std::clamp(v, 0.0f, 1.0f);
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

float fromSrgb(float v)
{
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

uint8_t unorm8(float v)
{
    return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}
} // namespace

const char* formatName(TextureFormat f)
{
    switch (f)
    {
    case TextureFormat::R8: return "r8";
    case TextureFormat::RG8: return "rg8";
    case TextureFormat::RGBA8: return "rgba8";
    case TextureFormat::R16F: return "r16f";
    case TextureFormat::RGBA16F: return "rgba16f";
    case TextureFormat::R32F: return "r32f";
    }
    return "rgba8";
}

bool parseFormat(const char* name, TextureFormat& out)
{
    for (TextureFormat f : kAll)
        if (std::string_view(formatName(f)) == name)
        {
            out = f;
            return true;
        }
    return false;
}

void encodeTexels(TextureFormat f, bool srgb, std::span<const float> rgba, std::vector<uint8_t>& out)
{
    const size_t texels = rgba.size() / 4;
    const int channels = channelCount(f);
    const bool encode = usesSrgb(f, srgb);
    out.resize(texels * size_t(texelBytes(f)));
    uint8_t* o = out.data();
    for (size_t i = 0; i < texels; ++i)
    {
        const float* t = &rgba[i * 4];
        for (int c = 0; c < channels; ++c)
        {
            const float v = t[c];
            switch (f)
            {
            case TextureFormat::R8:
            case TextureFormat::RG8:
            case TextureFormat::RGBA8: *o++ = unorm8(encode && c < 3 ? toSrgb(v) : v); break;
            case TextureFormat::R16F:
            case TextureFormat::RGBA16F:
            {
                const uint16_t h = glm::packHalf1x16(v);
                std::memcpy(o, &h, 2);
                o += 2;
                break;
            }
            case TextureFormat::R32F:
                std::memcpy(o, &v, 4);
                o += 4;
                break;
            }
        }
    }
}

void decodeTexels(TextureFormat f, bool srgb, std::span<const uint8_t> texels, std::vector<float>& rgba)
{
    const size_t bytes = size_t(texelBytes(f));
    const size_t n = texels.size() / bytes;
    const int channels = channelCount(f);
    const bool decode = usesSrgb(f, srgb);
    rgba.assign(n * 4, 0.0f);
    const uint8_t* p = texels.data();
    for (size_t i = 0; i < n; ++i)
    {
        float* t = &rgba[i * 4];
        t[3] = 1.0f;
        for (int c = 0; c < channels; ++c)
            switch (f)
            {
            case TextureFormat::R8:
            case TextureFormat::RG8:
            case TextureFormat::RGBA8:
            {
                const float v = float(*p++) / 255.0f;
                t[c] = decode && c < 3 ? fromSrgb(v) : v;
                break;
            }
            case TextureFormat::R16F:
            case TextureFormat::RGBA16F:
            {
                uint16_t h;
                std::memcpy(&h, p, 2);
                p += 2;
                t[c] = glm::unpackHalf1x16(h);
                break;
            }
            case TextureFormat::R32F:
                std::memcpy(&t[c], p, 4);
                p += 4;
                break;
            }
    }
}
} // namespace lila::render
