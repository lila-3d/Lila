#include "gltf/GltfInternal.h"

#include "core/ImageIO.h"
#include "core/Profile.h"

#include <miniz.h>
#include <stb_image.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>

namespace lila::gltf
{
fs::path utf8Path(std::string_view s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string utf8(const fs::path& p)
{
    std::u8string u8 = p.generic_u8string();
    return std::string(u8.begin(), u8.end());
}

bool readFile(const fs::path& path, std::string& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size < 0)
        return false;
    out.resize(size_t(size));
    f.seekg(0);
    f.read(out.data(), size);
    return bool(f) || f.eof();
}

bool writeFile(const fs::path& path, std::string_view bytes)
{
    std::error_code ec;
    if (path.has_parent_path())
        fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f)
        return false;
    f.write(bytes.data(), std::streamsize(bytes.size()));
    return bool(f);
}

std::string sniffMime(std::string_view b)
{
    auto starts = [&](std::string_view sig) { return b.size() >= sig.size() && b.substr(0, sig.size()) == sig; };
    if (starts("\x89PNG"))
        return "image/png";
    if (starts("\xFF\xD8\xFF"))
        return "image/jpeg";
    if (b.size() >= 12 && b.substr(0, 4) == "RIFF" && b.substr(8, 4) == "WEBP")
        return "image/webp";
    if (starts("\xABKTX 20"))
        return "image/ktx2";
    if (starts("GIF8"))
        return "image/gif";
    if (starts("BM"))
        return "image/bmp";
    return {};
}

std::string extensionForMime(std::string_view mime)
{
    if (mime == "image/png")
        return ".png";
    if (mime == "image/jpeg" || mime == "image/jpg")
        return ".jpg";
    if (mime == "image/webp")
        return ".webp";
    if (mime == "image/ktx2")
        return ".ktx2";
    if (mime == "image/gif")
        return ".gif";
    if (mime == "image/bmp")
        return ".bmp";
    return ".bin";
}

std::string gltfMimeForPath(const fs::path& path)
{
    std::string ext = utf8(path.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (ext == ".png")
        return "image/png";
    if (ext == ".jpg" || ext == ".jpeg")
        return "image/jpeg";
    return {};
}

glm::vec4 Pixels::at(int x, int y) const
{
    x = std::clamp(x, 0, width - 1);
    y = std::clamp(y, 0, height - 1);
    const float* p = &rgba[(size_t(y) * size_t(width) + size_t(x)) * 4];
    return {p[0], p[1], p[2], p[3]};
}

glm::vec4 Pixels::sample(float u, float v) const
{
    const float x = u * float(width) - 0.5f, y = v * float(height) - 0.5f;
    const float fx = std::floor(x), fy = std::floor(y);
    const float tx = x - fx, ty = y - fy;
    auto wrap = [](int i, int n) { return ((i % n) + n) % n; };
    const int x0 = wrap(int(fx), width), x1 = wrap(int(fx) + 1, width);
    const int y0 = wrap(int(fy), height), y1 = wrap(int(fy) + 1, height);
    const glm::vec4 a = glm::mix(at(x0, y0), at(x1, y0), tx);
    const glm::vec4 b = glm::mix(at(x0, y1), at(x1, y1), tx);
    return glm::mix(a, b, ty);
}

bool decodeImage(std::string_view bytes, Pixels& out, std::string* error)
{
    LILA_ZONE();

    const auto* data = reinterpret_cast<const stbi_uc*>(bytes.data());
    const int len = int(bytes.size());
    int w = 0, h = 0, n = 0;
    out = {};
    if (stbi_is_16_bit_from_memory(data, len))
    {
        stbi_us* px = stbi_load_16_from_memory(data, len, &w, &h, &n, 4);
        if (px)
        {
            out.width = w;
            out.height = h;
            out.bits = 16;
            out.rgba.resize(size_t(w) * size_t(h) * 4);
            for (size_t i = 0; i < out.rgba.size(); ++i)
                out.rgba[i] = float(px[i]) / 65535.0f;
            stbi_image_free(px);
            return true;
        }
    }
    else if (stbi_uc* px = stbi_load_from_memory(data, len, &w, &h, &n, 4))
    {
        out.width = w;
        out.height = h;
        out.rgba.resize(size_t(w) * size_t(h) * 4);
        for (size_t i = 0; i < out.rgba.size(); ++i)
            out.rgba[i] = float(px[i]) / 255.0f;
        stbi_image_free(px);
        return true;
    }
    if (error)
        *error = stbi_failure_reason() ? stbi_failure_reason() : "unreadable image";
    return false;
}

bool loadImage(const fs::path& path, Pixels& out, std::string* error)
{
    std::string ext = utf8(path.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (ext == ".exr" || ext == ".tif" || ext == ".tiff")
    {
        image::Image img = image::read(path, error);
        if (img.width <= 0)
            return false;
        out = {img.width, img.height, 16, std::move(img.rgba)};
        return true;
    }
    std::string bytes;
    if (!readFile(path, bytes))
    {
        if (error)
            *error = "can't read " + utf8(path);
        return false;
    }
    return decodeImage(bytes, out, error);
}

std::string encodePng(const Pixels& p)
{
    LILA_ZONE();

    std::vector<uint8_t> rgba(p.rgba.size());
    for (size_t i = 0; i < rgba.size(); ++i)
        rgba[i] = uint8_t(std::lround(std::clamp(p.rgba[i], 0.0f, 1.0f) * 255.0f));
    size_t len = 0;
    void* png = tdefl_write_image_to_png_file_in_memory_ex(rgba.data(), p.width, p.height, 4, &len, 6, MZ_FALSE);
    if (!png)
        return {};
    std::string out(static_cast<const char*>(png), len);
    mz_free(png);
    return out;
}

std::string dataUri(std::string_view mime, std::string_view bytes)
{
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out = "data:" + std::string(mime) + ";base64,";
    out.reserve(out.size() + (bytes.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3)
    {
        const uint32_t v = (uint32_t(uint8_t(bytes[i])) << 16) | (uint32_t(uint8_t(bytes[i + 1])) << 8) | uint8_t(bytes[i + 2]);
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += kAlphabet[v & 63];
    }
    if (i < bytes.size())
    {
        uint32_t v = uint32_t(uint8_t(bytes[i])) << 16;
        if (i + 1 < bytes.size())
            v |= uint32_t(uint8_t(bytes[i + 1])) << 8;
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += i + 1 < bytes.size() ? kAlphabet[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}
} // namespace lila::gltf
