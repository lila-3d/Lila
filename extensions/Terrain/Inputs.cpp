// How terrains start from outside data: elevation files (GeoTIFF, HGT, ASC, XYZ, Terrarium) and painted images.

#include "Inputs.h"

#include <lila/ImageIO.h>
#include <lila/Profile.h>

#include <miniz.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>

using namespace lila;

namespace terrain
{
namespace
{
namespace fs = std::filesystem;
constexpr float kVoid = std::numeric_limits<float>::quiet_NaN();
constexpr size_t kMaxSamples = size_t(1) << 28;

std::runtime_error fail(const fs::path& path, const std::string& why) { return std::runtime_error(path.filename().string() + ": " + why); }

std::vector<uint8_t> readBytes(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        throw std::runtime_error("can't open " + path.string());
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

std::string lowerExt(const fs::path& path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return ext;
}

void checkSize(const fs::path& path, int64_t w, int64_t h)
{
    if (w < 2 || h < 2 || uint64_t(w) * uint64_t(h) > kMaxSamples)
        throw fail(path, "grid is " + std::to_string(w) + " x " + std::to_string(h));
}

// TIFF ------------------------------------------------------------------------------------------------------------

/** TIFF LZW: MSB-first codes, 256 clears, 257 ends, widths grow one code early. */
bool lzwDecode(const uint8_t* src, size_t n, std::vector<uint8_t>& out)
{
    uint16_t prefix[4096];
    uint8_t suffix[4096], first[4096];
    uint16_t len[4096];
    for (int i = 0; i < 256; ++i)
    {
        suffix[i] = first[i] = uint8_t(i);
        len[i] = 1;
    }
    size_t bit = 0;
    int width = 9, next = 258, prev = -1;
    auto emit = [&](int code) {
        const size_t at = out.size();
        out.resize(at + len[code]);
        for (size_t k = len[code]; k-- > 0; code = prefix[code])
            out[at + k] = suffix[code];
    };
    while (bit + size_t(width) <= n * 8)
    {
        int code = 0;
        for (int b = 0; b < width; ++b, ++bit)
            code = (code << 1) | ((src[bit >> 3] >> (7 - (bit & 7))) & 1);
        if (code == 257)
            break;
        if (code == 256)
        {
            width = 9;
            next = 258;
            prev = -1;
            continue;
        }
        if (prev < 0)
        {
            if (code > 255)
                return false;
            emit(code);
            prev = code;
            continue;
        }
        if (code > next || next >= 4096)
            return false;
        prefix[next] = uint16_t(prev);
        first[next] = first[prev];
        suffix[next] = code < next ? first[code] : first[prev];
        len[next] = uint16_t(len[prev] + 1);
        emit(code);
        ++next;
        if (next >= (1 << width) - 1 && width < 12)
            ++width;
        prev = code;
    }
    return true;
}

struct Tiff
{
    std::map<uint16_t, std::vector<double>> nums;
    std::map<uint16_t, std::string> text;

    double one(uint16_t tag, double fallback) const
    {
        auto it = nums.find(tag);
        return it != nums.end() && !it->second.empty() ? it->second[0] : fallback;
    }
    const std::vector<double>& all(uint16_t tag) const
    {
        static const std::vector<double> none;
        auto it = nums.find(tag);
        return it != nums.end() ? it->second : none;
    }
};

Dem readGeoTiff(const fs::path& path)
{
    LILA_ZONE();

    const std::vector<uint8_t> d = readBytes(path);
    if (d.size() < 16 || !((d[0] == 'I' && d[1] == 'I') || (d[0] == 'M' && d[1] == 'M')))
        throw fail(path, "not a TIFF");
    const bool le = d[0] == 'I';
    auto need = [&](uint64_t at, uint64_t n) {
        if (at + n > d.size() || at + n < at)
            throw fail(path, "truncated");
    };
    auto uint = [&](uint64_t at, int bytes) {
        need(at, uint64_t(bytes));
        uint64_t v = 0;
        for (int b = 0; b < bytes; ++b)
            v |= uint64_t(d[at + uint64_t(le ? b : bytes - 1 - b)]) << (8 * b);
        return v;
    };
    const uint64_t version = uint(2, 2);
    if (version != 42 && version != 43)
        throw fail(path, "not a TIFF");
    const bool big = version == 43;
    const uint64_t ifd = big ? uint(8, 8) : uint(4, 4);
    const uint64_t count = uint(ifd, big ? 8 : 2);
    const uint64_t entrySize = big ? 20 : 12, inlineSize = big ? 8 : 4;
    Tiff t;
    for (uint64_t e = 0; e < count; ++e)
    {
        const uint64_t p = ifd + (big ? 8 : 2) + e * entrySize;
        const uint16_t tag = uint16_t(uint(p, 2)), type = uint16_t(uint(p + 2, 2));
        const uint64_t n = big ? uint(p + 4, 8) : uint(p + 4, 4);
        static constexpr int kSizes[] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8, 0, 0, 0, 8, 8, 8};
        const int size = type < std::size(kSizes) ? kSizes[type] : 0;
        if (!size || n > (uint64_t(1) << 28))
            continue;
        const uint64_t at = uint64_t(size) * n <= inlineSize ? p + (big ? 12 : 8) : uint(p + (big ? 12 : 8), int(inlineSize));
        need(at, uint64_t(size) * n);
        if (type == 2)
        {
            t.text[tag] = std::string(reinterpret_cast<const char*>(&d[at]), size_t(n));
            continue;
        }
        std::vector<double>& v = t.nums[tag];
        v.resize(size_t(n));
        for (uint64_t i = 0; i < n; ++i)
        {
            const uint64_t q = at + i * uint64_t(size);
            switch (type)
            {
            case 5: v[i] = double(uint(q, 4)) / std::max<double>(1.0, double(uint(q + 4, 4))); break;
            case 6: v[i] = double(int8_t(d[q])); break;
            case 8: v[i] = double(int16_t(uint(q, 2))); break;
            case 9: v[i] = double(int32_t(uint(q, 4))); break;
            case 10: v[i] = double(int32_t(uint(q, 4))) / std::max(1.0, double(int32_t(uint(q + 4, 4)))); break;
            case 11: v[i] = double(std::bit_cast<float>(uint32_t(uint(q, 4)))); break;
            case 12: v[i] = std::bit_cast<double>(uint(q, 8)); break;
            case 17: v[i] = double(int64_t(uint(q, 8))); break;
            default: v[i] = double(uint(q, size)); break;
            }
        }
    }

    const int w = int(t.one(256, 0)), h = int(t.one(257, 0));
    checkSize(path, w, h);
    const int spp = int(t.one(277, 1)), bits = int(t.one(258, 8)), format = int(t.one(339, 1));
    const int compression = int(t.one(259, 1)), predictor = int(t.one(317, 1)), planar = int(t.one(284, 1));
    if (bits != 8 && bits != 16 && bits != 32 && bits != 64)
        throw fail(path, std::to_string(bits) + "-bit samples");
    if (format == 3 && bits < 32)
        throw fail(path, "half floats");
    if (compression != 1 && compression != 5 && compression != 8 && compression != 32946)
        throw fail(path, "compression " + std::to_string(compression) + " (uncompressed, LZW and deflate are read)");
    const int bps = bits / 8;
    // Separate planes store band 1 first, so it reads as a one-sample image.
    const int chunkSpp = planar == 2 ? 1 : spp;
    const bool tiled = t.nums.contains(322);
    const int cw = tiled ? int(t.one(322, 0)) : w;
    const int ch = tiled ? int(t.one(323, 0)) : std::clamp(int(t.one(278, h)), 1, h);
    const std::vector<double>& offsets = t.all(tiled ? 324 : 273);
    const std::vector<double>& sizes = t.all(tiled ? 325 : 279);
    if (cw <= 0 || ch <= 0)
        throw fail(path, "bad tile size");
    const int across = tiled ? (w + cw - 1) / cw : 1, down = (h + ch - 1) / ch;
    if (offsets.size() < size_t(across) * size_t(down) || sizes.size() < offsets.size())
        throw fail(path, "missing strips or tiles");

    double nodata = std::numeric_limits<double>::quiet_NaN();
    if (auto it = t.text.find(42113); it != t.text.end())
        nodata = std::strtod(it->second.c_str(), nullptr);

    Dem dem;
    dem.width = w;
    dem.height = h;
    dem.metres.assign(size_t(w) * size_t(h), kVoid);
    std::string error;
    std::mutex errorLock;
    auto failed = [&](const char* why) {
        std::lock_guard lock(errorLock);
        error = why;
    };
    tbb::parallel_for(0, across * down, [&](int c) {
        const int cx = (c % across) * cw, cy = (c / across) * ch;
        const int rows = tiled ? ch : std::min(ch, h - cy);
        const size_t rowBytes = size_t(cw) * size_t(chunkSpp) * size_t(bps);
        const size_t want = rowBytes * size_t(rows);
        const uint64_t at = uint64_t(offsets[size_t(c)]), n = uint64_t(sizes[size_t(c)]);
        if (at + n > d.size())
        {
            failed("truncated data");
            return;
        }
        std::vector<uint8_t> raw;
        if (compression == 1)
            raw.assign(d.begin() + ptrdiff_t(at), d.begin() + ptrdiff_t(at + std::min<uint64_t>(n, want)));
        else if (compression == 5)
        {
            raw.reserve(want);
            if (!lzwDecode(&d[at], size_t(n), raw))
                failed("bad LZW data");
        }
        else
        {
            raw.resize(want);
            mz_ulong got = mz_ulong(want);
            if (mz_uncompress(raw.data(), &got, &d[at], mz_ulong(n)) != MZ_OK)
                failed("bad deflate data");
            raw.resize(got);
        }
        raw.resize(want, 0);
        std::vector<uint8_t> shuffled(predictor == 3 ? rowBytes : 0);
        std::vector<uint64_t> s(size_t(cw) * size_t(chunkSpp));
        const uint64_t mask = bits == 64 ? ~uint64_t(0) : (uint64_t(1) << bits) - 1;
        for (int r = 0; r < rows; ++r)
        {
            uint8_t* row = raw.data() + size_t(r) * rowBytes;
            bool sampleLe = le;
            if (predictor == 3)
            {
                for (size_t k = size_t(chunkSpp); k < rowBytes; ++k)
                    row[k] = uint8_t(row[k] + row[k - size_t(chunkSpp)]);
                // Floating-point prediction stores each byte plane most significant first.
                const size_t wc = s.size();
                for (size_t i = 0; i < wc; ++i)
                    for (int b = 0; b < bps; ++b)
                        shuffled[i * size_t(bps) + size_t(b)] = row[size_t(b) * wc + i];
                row = shuffled.data();
                sampleLe = false;
            }
            for (size_t i = 0; i < s.size(); ++i)
            {
                uint64_t v = 0;
                for (int b = 0; b < bps; ++b)
                    v |= uint64_t(row[i * size_t(bps) + size_t(sampleLe ? b : bps - 1 - b)]) << (8 * b);
                s[i] = v;
            }
            if (predictor == 2)
                for (size_t i = size_t(chunkSpp); i < s.size(); ++i)
                    s[i] = (s[i] + s[i - size_t(chunkSpp)]) & mask;
            const int y = cy + r;
            if (y >= h)
                break;
            for (int x = 0; x < cw && cx + x < w; ++x)
            {
                const uint64_t q = s[size_t(x) * size_t(chunkSpp)];
                double v;
                if (format == 3)
                    v = bits == 32 ? double(std::bit_cast<float>(uint32_t(q))) : std::bit_cast<double>(q);
                else if (format == 2)
                    v = bits == 64 ? double(int64_t(q)) : double(int64_t(q << (64 - bits)) >> (64 - bits));
                else
                    v = double(q);
                if (std::isfinite(v) && v != nodata && !(format == 3 && bits == 32 && float(v) == float(nodata)))
                    dem.metres[size_t(y) * size_t(w) + size_t(cx + x)] = float(v);
            }
        }
    });
    if (!error.empty())
        throw fail(path, error);

    std::map<int, int> keys;
    if (const std::vector<double>& dir = t.all(34735); dir.size() >= 4)
        for (size_t k = 4; k + 3 < dir.size(); k += 4)
            if (dir[k + 1] == 0)
                keys[int(dir[k])] = int(dir[k + 3]);
    dem.geographic = keys.contains(1024) ? keys[1024] == 2 : false;
    dem.epsg = keys.contains(3072) ? keys[3072] : keys.contains(2048) ? keys[2048] : 0;
    const bool pointSamples = keys.contains(1025) && keys[1025] == 2;
    double unit = 1.0;
    if (keys.contains(3076))
        unit = keys[3076] == 9002 ? 0.3048 : keys[3076] == 9003 ? 1200.0 / 3937.0 : 1.0;
    const std::vector<double>& scale = t.all(33550);
    const std::vector<double>& tie = t.all(33922);
    const std::vector<double>& m = t.all(34264);
    const double half = pointSamples ? 0.0 : 0.5;
    if (scale.size() >= 2 && tie.size() >= 6)
    {
        dem.dx = scale[0];
        dem.dy = scale[1];
        dem.x0 = tie[3] + (half - tie[0]) * dem.dx;
        dem.y0 = tie[4] - (half - tie[1]) * dem.dy;
    }
    else if (m.size() >= 16 && m[1] == 0.0 && m[4] == 0.0)
    {
        dem.dx = m[0];
        dem.dy = -m[5];
        dem.x0 = m[3] + half * dem.dx;
        dem.y0 = m[7] - half * dem.dy;
    }
    if (!dem.geographic && unit != 1.0)
    {
        dem.dx *= unit;
        dem.dy *= unit;
        dem.x0 *= unit;
        dem.y0 *= unit;
    }
    return dem;
}

// Other formats ---------------------------------------------------------------------------------------------------

/** SRTM: big-endian int16 squares, named after their south-west corner (N45W122.hgt). */
Dem readHgt(const fs::path& path)
{
    LILA_ZONE();

    const std::vector<uint8_t> d = readBytes(path);
    const int n = int(std::lround(std::sqrt(double(d.size() / 2))));
    if (size_t(n) * size_t(n) * 2 != d.size())
        throw fail(path, "not a square of 16-bit samples");
    checkSize(path, n, n);
    Dem dem;
    dem.width = dem.height = n;
    dem.metres.resize(size_t(n) * size_t(n));
    for (size_t i = 0; i < dem.metres.size(); ++i)
    {
        const int16_t v = int16_t(uint16_t(d[2 * i]) << 8 | d[2 * i + 1]);
        dem.metres[i] = v == -32768 ? kVoid : float(v);
    }
    dem.geographic = true;
    dem.dx = dem.dy = 1.0 / double(n - 1);
    const std::string name = path.stem().string();
    double lat = 0.0, lon = 0.0;
    if (name.size() >= 7 && std::strchr("NnSs", name[0]) && std::strchr("EeWw", name[3]))
    {
        lat = std::atof(name.substr(1, 2).c_str()) * (std::toupper(name[0]) == 'S' ? -1.0 : 1.0);
        lon = std::atof(name.substr(4, 3).c_str()) * (std::toupper(name[3]) == 'W' ? -1.0 : 1.0);
    }
    dem.x0 = lon;
    dem.y0 = lat + 1.0;
    return dem;
}

/** Whether coordinates with this spacing look like degrees, for files that don't say. */
bool looksGeographic(const fs::path& path, double x0, double y0, double x1, double y1, double spacing)
{
    fs::path prj = path;
    prj.replace_extension(".prj");
    if (std::ifstream f(prj); f)
    {
        const std::string wkt((std::istreambuf_iterator<char>(f)), {});
        if (wkt.find("PROJCS") != std::string::npos || wkt.find("PROJCRS") != std::string::npos)
            return false;
        if (wkt.find("GEOGCS") != std::string::npos || wkt.find("GEOGCRS") != std::string::npos)
            return true;
    }
    return spacing < 0.1 && std::abs(x0) <= 180.0 && std::abs(x1) <= 180.0 && std::abs(y0) <= 90.0 && std::abs(y1) <= 90.0;
}

/** ESRI ASCII grid: a header of keys and values, then rows from north to south. */
Dem readAsc(const fs::path& path)
{
    LILA_ZONE();

    std::ifstream f(path);
    if (!f)
        throw std::runtime_error("can't open " + path.string());
    std::map<std::string, double> header;
    std::string key;
    while (f >> std::ws && std::isalpha(f.peek()))
    {
        double value = 0.0;
        f >> key >> value;
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        header[key] = value;
    }
    const int w = int(header["ncols"]), h = int(header["nrows"]);
    checkSize(path, w, h);
    const double dx = header.contains("cellsize") ? header["cellsize"] : header["dx"];
    const double dy = header.contains("cellsize") ? header["cellsize"] : header["dy"];
    if (!(dx > 0.0 && dy > 0.0))
        throw fail(path, "no cellsize");
    const bool corner = header.contains("xllcorner");
    const double xll = corner ? header["xllcorner"] : header["xllcenter"];
    const double yll = header.contains("yllcorner") ? header["yllcorner"] : header["yllcenter"];
    const double nodata = header.contains("nodata_value") ? header["nodata_value"] : std::numeric_limits<double>::quiet_NaN();
    Dem dem;
    dem.width = w;
    dem.height = h;
    dem.metres.resize(size_t(w) * size_t(h));
    for (float& v : dem.metres)
    {
        double x = 0.0;
        if (!(f >> x))
            throw fail(path, "fewer values than ncols x nrows");
        v = x == nodata ? kVoid : float(x);
    }
    dem.dx = dx;
    dem.dy = dy;
    dem.x0 = xll + (corner ? dx * 0.5 : 0.0);
    dem.y0 = yll + (corner ? dy * 0.5 : 0.0) + dy * double(h - 1);
    dem.geographic = looksGeographic(path, dem.x0, dem.y0, dem.x0 + dx * (w - 1), yll, dx);
    return dem;
}

/** The smallest gap between distinct sorted values, ignoring floating-point noise. */
double gridStep(std::vector<double>& v)
{
    std::sort(v.begin(), v.end());
    const double tol = (v.back() - v.front()) * 1e-9;
    double step = std::numeric_limits<double>::infinity();
    for (size_t i = 1; i < v.size(); ++i)
        if (v[i] - v[i - 1] > tol)
            step = std::min(step, v[i] - v[i - 1]);
    return step;
}

/** One "x y z" point per line (spaces, commas, tabs or semicolons) on a regular grid; lines that aren't numbers are skipped. */
Dem readXyz(const fs::path& path)
{
    LILA_ZONE();

    std::ifstream f(path);
    if (!f)
        throw std::runtime_error("can't open " + path.string());
    std::vector<double> xs, ys, zs;
    std::string line;
    while (std::getline(f, line))
    {
        std::replace_if(line.begin(), line.end(), [](char c) { return c == ',' || c == ';' || c == '\t'; }, ' ');
        std::istringstream s(line);
        double x, y, z;
        if (s >> x >> y >> z)
        {
            xs.push_back(x);
            ys.push_back(y);
            zs.push_back(z);
        }
    }
    if (xs.size() < 4)
        throw fail(path, "fewer than 4 points");
    std::vector<double> sx = xs, sy = ys;
    const double dx = gridStep(sx), dy = gridStep(sy);
    if (!std::isfinite(dx) || !std::isfinite(dy))
        throw fail(path, "points aren't on a grid");
    const double xmin = sx.front(), ymax = sy.back();
    const int w = int(std::lround((sx.back() - xmin) / dx)) + 1, h = int(std::lround((ymax - sy.front()) / dy)) + 1;
    checkSize(path, w, h);
    Dem dem;
    dem.width = w;
    dem.height = h;
    dem.metres.assign(size_t(w) * size_t(h), kVoid);
    for (size_t i = 0; i < xs.size(); ++i)
    {
        const int c = int(std::lround((xs[i] - xmin) / dx)), r = int(std::lround((ymax - ys[i]) / dy));
        dem.metres[size_t(r) * size_t(w) + size_t(c)] = float(zs[i]);
    }
    dem.dx = dx;
    dem.dy = dy;
    dem.x0 = xmin;
    dem.y0 = ymax;
    dem.geographic = looksGeographic(path, xmin, ymax, sx.back(), sy.front(), dx);
    return dem;
}

/** Elevation encoded in the colour of 8-bit PNG map tiles. */
Dem readRgbTile(const fs::path& path, bool terrarium)
{
    LILA_ZONE();

    std::string err;
    const image::Image img = image::readPng(path, &err);
    if (img.width < 2 || img.height < 2)
        throw fail(path, err.empty() ? "no image" : err);
    Dem dem;
    dem.width = img.width;
    dem.height = img.height;
    dem.metres.resize(size_t(img.width) * size_t(img.height));
    tbb::parallel_for(size_t(0), dem.metres.size(), [&](size_t i) {
        const double r = std::round(img.rgba[i * 4] * 255.0), g = std::round(img.rgba[i * 4 + 1] * 255.0), b = std::round(img.rgba[i * 4 + 2] * 255.0);
        dem.metres[i] = float(terrarium ? r * 256.0 + g + b / 256.0 - 32768.0 : -10000.0 + (r * 65536.0 + g * 256.0 + b) * 0.1);
    });
    return dem;
}

/** Push-pull: voids take the average of the data around them at the coarsest scale that has any, refined level by level. */
void pushPull(std::vector<float>& v, std::vector<uint8_t>& valid, int w, int h)
{
    if (std::all_of(valid.begin(), valid.end(), [](uint8_t b) { return b != 0; }))
        return;
    const int cw = (w + 1) / 2, chh = (h + 1) / 2;
    std::vector<float> coarse(size_t(cw) * size_t(chh), 0.0f);
    std::vector<uint8_t> cvalid(coarse.size(), 0);
    for (int y = 0; y < chh; ++y)
        for (int x = 0; x < cw; ++x)
        {
            double sum = 0.0;
            int n = 0;
            for (int j = 2 * y; j < std::min(h, 2 * y + 2); ++j)
                for (int i = 2 * x; i < std::min(w, 2 * x + 2); ++i)
                    if (valid[size_t(j) * size_t(w) + size_t(i)])
                    {
                        sum += v[size_t(j) * size_t(w) + size_t(i)];
                        ++n;
                    }
            if (n)
            {
                coarse[size_t(y) * size_t(cw) + size_t(x)] = float(sum / n);
                cvalid[size_t(y) * size_t(cw) + size_t(x)] = 1;
            }
        }
    if (cw == w && chh == h)
        return;
    pushPull(coarse, cvalid, cw, chh);
    tbb::parallel_for(0, h, [&](int y) {
        for (int x = 0; x < w; ++x)
        {
            const size_t k = size_t(y) * size_t(w) + size_t(x);
            if (valid[k])
                continue;
            const double fx = std::clamp((x + 0.5) * 0.5 - 0.5, 0.0, double(cw - 1)), fy = std::clamp((y + 0.5) * 0.5 - 0.5, 0.0, double(chh - 1));
            const int x0 = int(fx), y0 = int(fy), x1 = std::min(x0 + 1, cw - 1), y1 = std::min(y0 + 1, chh - 1);
            const double tx = fx - x0, ty = fy - y0;
            auto at = [&](int i, int j) { return double(coarse[size_t(j) * size_t(cw) + size_t(i)]); };
            v[k] = float((at(x0, y0) * (1 - tx) + at(x1, y0) * tx) * (1 - ty) + (at(x0, y1) * (1 - tx) + at(x1, y1) * tx) * ty);
        }
    });
    std::fill(valid.begin(), valid.end(), uint8_t(1));
}

double metresPerDegreeLat(double lat)
{
    const double p = lat * 0.017453292519943295;
    return 111132.92 - 559.82 * std::cos(2 * p) + 1.175 * std::cos(4 * p);
}

double metresPerDegreeLon(double lat)
{
    const double p = lat * 0.017453292519943295;
    return 111412.84 * std::cos(p) - 93.5 * std::cos(3 * p);
}

/** Separable Gaussian blur of the heights, radius in samples. */
void blur(Heightfield& h, double sigma)
{
    if (sigma <= 0.05)
        return;
    const int r = std::max(1, int(std::ceil(sigma * 3.0)));
    std::vector<float> k(size_t(2 * r + 1));
    float sum = 0.0f;
    for (int i = -r; i <= r; ++i)
        sum += k[size_t(i + r)] = float(std::exp(-0.5 * i * i / (sigma * sigma)));
    for (float& v : k)
        v /= sum;
    std::vector<float> tmp(h.size());
    tbb::parallel_for(0, h.nz, [&](int z) {
        for (int x = 0; x < h.nx; ++x)
        {
            float s = 0.0f;
            for (int i = -r; i <= r; ++i)
                s += k[size_t(i + r)] * h.heights[h.index(std::clamp(x + i, 0, h.nx - 1), z)];
            tmp[h.index(x, z)] = s;
        }
    });
    tbb::parallel_for(0, h.nz, [&](int z) {
        for (int x = 0; x < h.nx; ++x)
        {
            float s = 0.0f;
            for (int i = -r; i <= r; ++i)
                s += k[size_t(i + r)] * tmp[h.index(x, std::clamp(z + i, 0, h.nz - 1))];
            h.heights[h.index(x, z)] = s;
        }
    });
}

void addDetail(Heightfield& h, double amplitude, uint32_t seed)
{
    if (amplitude <= 0.0)
        return;
    NoiseParams n;
    n.featureSize = std::max(h.extent().x, h.extent().y) / 12.0;
    n.amplitude = amplitude;
    n.octaves = 5;
    n.seed = seed;
    tbb::parallel_for(0, h.nz, [&](int z) {
        for (int x = 0; x < h.nx; ++x)
        {
            const glm::dvec2 p = h.samplePos(x, z);
            h.heights[h.index(x, z)] += float(noiseHeight(n, p.x, p.y) - amplitude * 0.5);
        }
    });
}

void rescale(Heightfield& h, double low, double high)
{
    const glm::dvec2 r = h.range();
    const double s = r.y > r.x ? (high - low) / (r.y - r.x) : 0.0;
    for (float& v : h.heights)
        v = float(low + (double(v) - r.x) * s);
}
} // namespace

const std::vector<std::string>& demFormatNames()
{
    static const std::vector<std::string> n = {"auto", "geotiff", "hgt", "asc", "xyz", "terrarium", "terrainrgb"};
    return n;
}

bool parseDemFormat(std::string_view name, DemFormat* out)
{
    const auto& n = demFormatNames();
    auto it = std::find(n.begin(), n.end(), name);
    if (it == n.end())
        return false;
    *out = DemFormat(it - n.begin());
    return true;
}

Dem readDem(const fs::path& path, DemFormat format)
{
    if (format == DemFormat::Auto)
    {
        const std::string ext = lowerExt(path);
        if (ext == ".tif" || ext == ".tiff")
            format = DemFormat::GeoTiff;
        else if (ext == ".hgt")
            format = DemFormat::Hgt;
        else if (ext == ".asc")
            format = DemFormat::Asc;
        else if (ext == ".xyz" || ext == ".txt" || ext == ".csv")
            format = DemFormat::Xyz;
        else if (ext == ".png")
            format = DemFormat::Terrarium;
        else
            throw fail(path, "unknown elevation file type (tif, hgt, asc, xyz, png)");
    }
    switch (format)
    {
    case DemFormat::GeoTiff: return readGeoTiff(path);
    case DemFormat::Hgt: return readHgt(path);
    case DemFormat::Asc: return readAsc(path);
    case DemFormat::Xyz: return readXyz(path);
    case DemFormat::Terrarium: return readRgbTile(path, true);
    default: return readRgbTile(path, false);
    }
}

bool looksLikeElevation(const fs::path& path)
{
    const std::string ext = lowerExt(path);
    if (ext == ".hgt" || ext == ".asc" || ext == ".xyz")
        return true;
    if (ext != ".tif" && ext != ".tiff")
        return false;
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> d(1 << 16);
    f.read(reinterpret_cast<char*>(d.data()), std::streamsize(d.size()));
    d.resize(size_t(f.gcount()));
    if (d.size() < 16 || !((d[0] == 'I' && d[1] == 'I') || (d[0] == 'M' && d[1] == 'M')))
        return false;
    const bool le = d[0] == 'I';
    auto uint = [&](size_t at, int bytes) -> uint64_t {
        if (at + size_t(bytes) > d.size())
            return 0;
        uint64_t v = 0;
        for (int b = 0; b < bytes; ++b)
            v |= uint64_t(d[at + size_t(le ? b : bytes - 1 - b)]) << (8 * b);
        return v;
    };
    const bool big = uint(2, 2) == 43;
    const uint64_t ifd = big ? uint(8, 8) : uint(4, 4);
    const uint64_t n = uint(size_t(ifd), big ? 8 : 2);
    for (uint64_t e = 0; e < n && e < 4096; ++e)
    {
        const uint64_t tag = uint(size_t(ifd + (big ? 8 : 2) + e * (big ? 20 : 12)), 2);
        if (tag == 34735 || tag == 33922 || tag == 34264)
            return true;
    }
    return false;
}

size_t fillVoids(Dem& d)
{
    LILA_ZONE();

    std::vector<uint8_t> valid(d.metres.size());
    size_t voids = 0;
    for (size_t i = 0; i < valid.size(); ++i)
    {
        valid[i] = std::isfinite(d.metres[i]) ? 1 : 0;
        voids += valid[i] ? 0 : 1;
    }
    if (!voids)
        return 0;
    if (voids == valid.size())
        throw std::runtime_error("the elevation data is all voids");
    d.filled.resize(valid.size());
    for (size_t i = 0; i < valid.size(); ++i)
        d.filled[i] = valid[i] ? 0 : 1;
    pushPull(d.metres, valid, d.width, d.height);
    return voids;
}

Heightfield demToHeightfield(const Dem& d, const DemOptions& o)
{
    LILA_ZONE();

    double mx = d.dx > 0.0 ? d.dx : o.spacing, my = d.dy > 0.0 ? d.dy : o.spacing;
    if (d.geographic && d.dx > 0.0)
    {
        const double lat = d.y0 - d.dy * double(d.height - 1) * 0.5;
        mx *= metresPerDegreeLon(lat);
        my *= metresPerDegreeLat(lat);
    }
    if (!(mx > 0.0 && my > 0.0))
        throw std::runtime_error("the elevation data has no sample spacing; give one");
    const double cell = o.cell > 0.0 ? o.cell : std::min(mx, my) * 100.0;
    const double ex = double(d.width - 1) * mx * 100.0, ez = double(d.height - 1) * my * 100.0;
    const int64_t nx = std::max<int64_t>(2, std::llround(ex / cell) + 1), nz = std::max<int64_t>(2, std::llround(ez / cell) + 1);
    if (uint64_t(nx) * uint64_t(nz) > kMaxSamples)
        throw std::runtime_error("that is " + std::to_string(nx) + " x " + std::to_string(nz) + " samples; give a larger cell size");
    Heightfield h = geo::makeHeightfield(int(nx), int(nz), cell, o.center);
    h.origin = o.center - h.extent() * 0.5;

    double base = 0.0;
    if (o.fromLowest)
    {
        base = std::numeric_limits<double>::infinity();
        for (float v : d.metres)
            if (std::isfinite(v))
                base = std::min(base, double(v));
        if (!std::isfinite(base))
            base = 0.0;
    }
    const bool voids = !d.filled.empty();
    std::vector<float>* voidMask = voids ? &h.mask("voids") : nullptr;
    const double scale = 100.0 * o.exaggeration;
    tbb::parallel_for(0, h.nz, [&](int z) {
        for (int x = 0; x < h.nx; ++x)
        {
            const double fx = std::clamp(x * cell / (mx * 100.0), 0.0, double(d.width - 1));
            const double fy = std::clamp(z * cell / (my * 100.0), 0.0, double(d.height - 1));
            const int x0 = std::min(int(fx), d.width - 2), y0 = std::min(int(fy), d.height - 2);
            const double tx = fx - x0, ty = fy - y0;
            auto bilerp = [&](auto&& at) {
                return (at(x0, y0) * (1 - tx) + at(x0 + 1, y0) * tx) * (1 - ty) + (at(x0, y0 + 1) * (1 - tx) + at(x0 + 1, y0 + 1) * tx) * ty;
            };
            const double m = bilerp([&](int i, int j) { return double(d.metres[size_t(j) * size_t(d.width) + size_t(i)]); });
            const size_t k = h.index(x, z);
            h.heights[k] = std::isfinite(m) ? float((m - base) * scale) : 0.0f;
            if (voidMask)
                (*voidMask)[k] = float(bilerp([&](int i, int j) { return double(d.filled[size_t(j) * size_t(d.width) + size_t(i)]); }));
        }
    });
    return h;
}

const std::vector<std::string>& imageModeNames()
{
    static const std::vector<std::string> n = {"height", "guide", "uplift", "mask", "sketch"};
    return n;
}

bool parseImageMode(std::string_view name, ImageMode* out)
{
    const auto& n = imageModeNames();
    auto it = std::find(n.begin(), n.end(), name);
    if (it == n.end())
        return false;
    *out = ImageMode(it - n.begin());
    return true;
}

void interpretImage(Heightfield& h, const ImageOptions& o)
{
    LILA_ZONE();

    if (h.empty())
        return;
    dequantize(h);
    blur(h, o.smooth);
    const double shape = std::max(0.05, o.shape);
    for (float& v : h.heights)
        v = float(std::pow(std::clamp(double(v), 0.0, 1.0), shape));
    const double range = o.high - o.low;
    const int iterations = int(std::lround(std::clamp(o.erosion, 0.0, 1.0) * 150.0));
    switch (o.mode)
    {
    case ImageMode::Mask:
        h.mask(o.mask.empty() ? "painted" : o.mask) = h.heights;
        std::fill(h.heights.begin(), h.heights.end(), float(o.low));
        return;
    case ImageMode::Uplift:
    {
        h.mask("uplift") = h.heights;
        std::fill(h.heights.begin(), h.heights.end(), 0.0f);
        addDetail(h, std::abs(range) * o.detail, o.seed);
        ErosionParams p;
        p.iterations = std::max(30, iterations);
        p.uplift = std::abs(range) * 2.0 / double(p.iterations);
        p.upliftMask = "uplift";
        erode(h, p);
        rescale(h, o.low, o.high);
        return;
    }
    default:
        for (float& v : h.heights)
            v = float(o.low + range * double(v));
        if (o.mode == ImageMode::Height || iterations == 0)
            return;
        addDetail(h, std::abs(range) * o.detail, o.seed);
        ErosionParams p;
        p.iterations = iterations;
        erode(h, p);
        return;
    }
}
} // namespace terrain
