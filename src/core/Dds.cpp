#include "core/Dds.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

#define BCDEC_IMPLEMENTATION
#define BCDEC_STATIC
#include <bcdec.h>

namespace lila::dds
{
namespace
{
constexpr uint32_t fourCC(char a, char b, char c, char d)
{
    return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 | uint32_t(uint8_t(d)) << 24;
}

uint32_t u32(const uint8_t* p)
{
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

bool fail(std::string* err, const std::string& why)
{
    if (err)
        *err = why;
    return false;
}

/** DXGI_FORMAT values a DX10 header may carry, and what they read as. */
bool fromDxgi(uint32_t dxgi, Format& f, bool& srgb)
{
    srgb = false;
    switch (dxgi)
    {
    case 28: f = Format::RGBA8; return true;
    case 29: f = Format::RGBA8; srgb = true; return true;
    case 87: f = Format::BGRA8; return true;
    case 91: f = Format::BGRA8; srgb = true; return true;
    case 70:
    case 71: f = Format::BC1; return true;
    case 72: f = Format::BC1; srgb = true; return true;
    case 73:
    case 74: f = Format::BC2; return true;
    case 75: f = Format::BC2; srgb = true; return true;
    case 76:
    case 77: f = Format::BC3; return true;
    case 78: f = Format::BC3; srgb = true; return true;
    case 79:
    case 80: f = Format::BC4; return true;
    case 82:
    case 83: f = Format::BC5; return true;
    case 94:
    case 95: f = Format::BC6H; return true;
    case 97:
    case 98: f = Format::BC7; return true;
    case 99: f = Format::BC7; srgb = true; return true;
    default: return false;
    }
}

uint8_t unorm8(float v)
{
    return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

/** Blue from a two-channel normal: the Z that makes (X, Y, Z) unit length. */
uint8_t rebuildZ(uint8_t x, uint8_t y)
{
    const float nx = x / 127.5f - 1.0f, ny = y / 127.5f - 1.0f;
    const float nz = std::sqrt(std::max(0.0f, 1.0f - nx * nx - ny * ny));
    return unorm8(nz * 0.5f + 0.5f);
}
} // namespace

bool isBlockCompressed(Format f)
{
    return f != Format::RGBA8 && f != Format::BGRA8;
}

int blockBytes(Format f)
{
    switch (f)
    {
    case Format::BC1:
    case Format::BC4: return 8;
    case Format::BC2:
    case Format::BC3:
    case Format::BC5:
    case Format::BC6H:
    case Format::BC7: return 16;
    default: return 0;
    }
}

size_t levelBytes(Format f, int w, int h)
{
    if (!isBlockCompressed(f))
        return size_t(w) * size_t(h) * 4;
    return size_t(std::max(1, (w + 3) / 4)) * size_t(std::max(1, (h + 3) / 4)) * size_t(blockBytes(f));
}

bool read(const fs::path& path, File& out, std::string* err)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return fail(err, "cannot open file");
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() < 128 || u32(bytes.data()) != fourCC('D', 'D', 'S', ' '))
        return fail(err, "not a DDS file");
    const uint8_t* h = bytes.data() + 4;
    const uint32_t flags = u32(h + 4), height = u32(h + 8), width = u32(h + 12), mipCount = u32(h + 24);
    const uint32_t pfFlags = u32(h + 76), pfFourCC = u32(h + 80), bitCount = u32(h + 84), rMask = u32(h + 88);
    const uint32_t caps2 = u32(h + 108);
    if (width == 0 || height == 0 || width > 32768 || height > 32768)
        return fail(err, "bad DDS size");
    // Cube maps and volumes.
    if (caps2 & (0x200 | 0x200000))
        return fail(err, "DDS cube maps and volume textures aren't supported");

    size_t dataStart = 128;
    File f;
    f.width = int(width);
    f.height = int(height);
    if (pfFlags & 0x4)
    {
        switch (pfFourCC)
        {
        case fourCC('D', 'X', 'T', '1'): f.format = Format::BC1; break;
        case fourCC('D', 'X', 'T', '2'):
        case fourCC('D', 'X', 'T', '3'): f.format = Format::BC2; break;
        case fourCC('D', 'X', 'T', '4'):
        case fourCC('D', 'X', 'T', '5'): f.format = Format::BC3; break;
        case fourCC('A', 'T', 'I', '1'):
        case fourCC('B', 'C', '4', 'U'): f.format = Format::BC4; break;
        case fourCC('A', 'T', 'I', '2'):
        case fourCC('B', 'C', '5', 'U'): f.format = Format::BC5; break;
        case fourCC('D', 'X', '1', '0'):
        {
            if (bytes.size() < 148)
                return fail(err, "truncated DDS header");
            const uint8_t* dx = bytes.data() + 128;
            const uint32_t dimension = u32(dx + 4), miscFlag = u32(dx + 8), arraySize = u32(dx + 12);
            if (dimension != 3 || (miscFlag & 0x4) || arraySize > 1)
                return fail(err, "only 2D DDS textures are supported");
            if (!fromDxgi(u32(dx), f.format, f.srgb))
                return fail(err, "unsupported DDS format (DXGI " + std::to_string(u32(dx)) + ")");
            dataStart = 148;
            break;
        }
        default: return fail(err, "unsupported DDS compression");
        }
    }
    else if ((pfFlags & 0x40) && bitCount == 32)
        f.format = rMask == 0x00ff0000 ? Format::BGRA8 : Format::RGBA8;
    else
        return fail(err, "unsupported DDS pixel format");

    const int levels = (flags & 0x20000) ? std::clamp(int(mipCount), 1, 16) : 1;
    size_t offset = 0;
    const size_t available = bytes.size() - dataStart;
    for (int i = 0, w = f.width, hh = f.height; i < levels; ++i, w = std::max(1, w / 2), hh = std::max(1, hh / 2))
    {
        const size_t size = levelBytes(f.format, w, hh);
        if (offset + size > available)
            break;
        f.levels.push_back({w, hh, offset, size});
        offset += size;
    }
    if (f.levels.empty())
        return fail(err, "truncated DDS file");
    f.data.assign(bytes.begin() + std::ptrdiff_t(dataStart), bytes.begin() + std::ptrdiff_t(dataStart + offset));
    out = std::move(f);
    return true;
}

std::vector<uint8_t> decodeRgba8(Format format, int width, int height, const uint8_t* texels)
{
    std::vector<uint8_t> rgba(size_t(width) * size_t(height) * 4);
    if (format == Format::RGBA8 || format == Format::BGRA8)
    {
        std::memcpy(rgba.data(), texels, rgba.size());
        if (format == Format::BGRA8)
            for (size_t i = 0; i < rgba.size(); i += 4)
                std::swap(rgba[i], rgba[i + 2]);
        return rgba;
    }
    const int bw = std::max(1, (width + 3) / 4), bh = std::max(1, (height + 3) / 4), bytes = blockBytes(format);
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx)
        {
            const uint8_t* block = texels + (size_t(by) * size_t(bw) + size_t(bx)) * size_t(bytes);
            uint8_t px[16 * 4];
            switch (format)
            {
            case Format::BC1: bcdec_bc1(block, px, 16); break;
            case Format::BC2: bcdec_bc2(block, px, 16); break;
            case Format::BC3: bcdec_bc3(block, px, 16); break;
            case Format::BC7: bcdec_bc7(block, px, 16); break;
            case Format::BC4:
            {
                uint8_t r[16];
                bcdec_bc4(block, r, 4);
                for (int i = 0; i < 16; ++i)
                    px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = r[i], px[i * 4 + 3] = 255;
                break;
            }
            case Format::BC5:
            {
                uint8_t rg[16 * 2];
                bcdec_bc5(block, rg, 8);
                for (int i = 0; i < 16; ++i)
                {
                    px[i * 4] = rg[i * 2];
                    px[i * 4 + 1] = rg[i * 2 + 1];
                    px[i * 4 + 2] = rebuildZ(rg[i * 2], rg[i * 2 + 1]);
                    px[i * 4 + 3] = 255;
                }
                break;
            }
            default: std::memset(px, 0, sizeof(px)); break;
            }
            for (int y = 0; y < 4 && by * 4 + y < height; ++y)
            {
                const int cols = std::min(4, width - bx * 4);
                std::memcpy(&rgba[((size_t(by) * 4 + size_t(y)) * size_t(width) + size_t(bx) * 4) * 4], &px[y * 16], size_t(cols) * 4);
            }
        }
    return rgba;
}

std::vector<float> decodeFloat(const File& f, size_t level)
{
    if (level >= f.levels.size())
        return {};
    const Level& l = f.levels[level];
    const uint8_t* texels = f.data.data() + l.offset;
    std::vector<float> out(size_t(l.width) * size_t(l.height) * 4);
    if (f.format == Format::BC6H)
    {
        const int bw = std::max(1, (l.width + 3) / 4), bh = std::max(1, (l.height + 3) / 4);
        for (int by = 0; by < bh; ++by)
            for (int bx = 0; bx < bw; ++bx)
            {
                float px[16 * 3];
                bcdec_bc6h_float(texels + (size_t(by) * size_t(bw) + size_t(bx)) * 16, px, 4 * 3, 0);
                for (int y = 0; y < 4 && by * 4 + y < l.height; ++y)
                    for (int x = 0; x < 4 && bx * 4 + x < l.width; ++x)
                    {
                        float* o = &out[((size_t(by) * 4 + size_t(y)) * size_t(l.width) + size_t(bx) * 4 + size_t(x)) * 4];
                        o[0] = px[(y * 4 + x) * 3];
                        o[1] = px[(y * 4 + x) * 3 + 1];
                        o[2] = px[(y * 4 + x) * 3 + 2];
                        o[3] = 1.0f;
                    }
            }
        return out;
    }
    const std::vector<uint8_t> rgba = decodeRgba8(f.format, l.width, l.height, texels);
    for (size_t i = 0; i < rgba.size(); ++i)
        out[i] = rgba[i] / 255.0f;
    return out;
}
} // namespace lila::dds
