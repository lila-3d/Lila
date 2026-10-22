#include "render/HdrImage.h"

#include "core/Profile.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string_view>

namespace lila::render
{
namespace
{
using Rgbe = std::array<uint8_t, 4>;

struct Reader
{
    const uint8_t* p;
    const uint8_t* end;

    bool line(std::string_view& out)
    {
        const uint8_t* start = p;
        while (p < end && *p != '\n')
            ++p;
        if (p >= end)
            return false;
        out = std::string_view(reinterpret_cast<const char*>(start), size_t(p - start));
        if (!out.empty() && out.back() == '\r')
            out.remove_suffix(1);
        ++p;
        return true;
    }

    size_t left() const { return size_t(end - p); }
};

glm::vec3 toFloat(const Rgbe& c)
{
    if (c[3] == 0)
        return glm::vec3(0.0f);
    float f = std::ldexp(1.0f, int(c[3]) - (128 + 8));
    return glm::vec3(float(c[0]), float(c[1]), float(c[2])) * f;
}

/** Flat pixels, possibly with the original Radiance run-length scheme (1,1,1,count repeats the previous pixel). */
bool readFlat(Reader& r, std::vector<Rgbe>& line, int width)
{
    int x = 0, shift = 0;
    while (x < width)
    {
        if (r.left() < 4)
            return false;
        Rgbe c{r.p[0], r.p[1], r.p[2], r.p[3]};
        r.p += 4;
        if (c[0] == 1 && c[1] == 1 && c[2] == 1)
        {
            if (x == 0)
                return false;
            int count = int(c[3]) << shift;
            if (x + count > width)
                return false;
            for (int i = 0; i < count; ++i, ++x)
                line[size_t(x)] = line[size_t(x - 1)];
            shift += 8;
        }
        else
        {
            line[size_t(x++)] = c;
            shift = 0;
        }
    }
    return true;
}

bool readScanline(Reader& r, std::vector<Rgbe>& line, int width)
{
    if (width < 8 || width > 0x7fff || r.left() < 4 || r.p[0] != 2 || r.p[1] != 2 || (r.p[2] & 0x80))
        return readFlat(r, line, width);
    if (((int(r.p[2]) << 8) | int(r.p[3])) != width)
        return false;
    r.p += 4;
    // Each channel is run-length coded separately: >128 is a run of the next byte, otherwise that many literals.
    for (int ch = 0; ch < 4; ++ch)
    {
        int x = 0;
        while (x < width)
        {
            if (r.left() < 1)
                return false;
            int count = *r.p++;
            if (count > 128)
            {
                count -= 128;
                if (r.left() < 1 || x + count > width)
                    return false;
                uint8_t v = *r.p++;
                for (int i = 0; i < count; ++i)
                    line[size_t(x++)][size_t(ch)] = v;
            }
            else
            {
                if (count == 0 || r.left() < size_t(count) || x + count > width)
                    return false;
                for (int i = 0; i < count; ++i)
                    line[size_t(x++)][size_t(ch)] = *r.p++;
            }
        }
    }
    return true;
}
} // namespace

bool loadHdr(const std::filesystem::path& path, HdrImage& out, std::string& error)
{
    LILA_ZONE();

    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
    {
        error = "cannot open file";
        return false;
    }
    std::vector<uint8_t> data(size_t(in.tellg()));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size()));
    Reader r{data.data(), data.data() + data.size()};

    std::string_view ln;
    if (!r.line(ln) || !ln.starts_with("#?"))
    {
        error = "not a Radiance HDR file";
        return false;
    }
    while (r.line(ln) && !ln.empty())
        if (ln.starts_with("FORMAT=") && ln != "FORMAT=32-bit_rle_rgbe")
        {
            error = "unsupported format " + std::string(ln.substr(7));
            return false;
        }

    char ySign = 0, xSign = 0;
    int width = 0, height = 0;
    std::string res;
    if (r.line(ln))
        res = std::string(ln);
    if (std::sscanf(res.c_str(), "%cY %d %cX %d", &ySign, &height, &xSign, &width) != 4 || xSign != '+' || width <= 0 ||
        height <= 0 || (ySign != '-' && ySign != '+'))
    {
        error = "unsupported resolution line '" + res + "'";
        return false;
    }

    out.width = width;
    out.height = height;
    out.pixels.resize(size_t(width) * size_t(height));
    std::vector<Rgbe> line(static_cast<size_t>(width));
    for (int y = 0; y < height; ++y)
    {
        if (!readScanline(r, line, width))
        {
            error = "truncated or corrupt pixel data";
            return false;
        }
        int row = ySign == '-' ? y : height - 1 - y;
        glm::vec3* dst = &out.pixels[size_t(row) * size_t(width)];
        for (int x = 0; x < width; ++x)
            dst[x] = toFloat(line[size_t(x)]);
    }
    return true;
}
} // namespace lila::render
