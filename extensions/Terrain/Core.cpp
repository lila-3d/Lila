#include "Terrain.h"

#include <lila/ImageIO.h>
#include <lila/Profile.h>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>

using namespace lila;

namespace terrain
{
namespace
{
uint32_t hash(int32_t x, int32_t z, uint32_t seed)
{
    uint32_t h = uint32_t(x) * 0x8da6b343u ^ uint32_t(z) * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

double quintic(double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }

double corner(int32_t ix, int32_t iz, double dx, double dz, uint32_t seed)
{
    const double a = double(hash(ix, iz, seed)) * (6.283185307179586 / 4294967296.0);
    return std::cos(a) * dx + std::sin(a) * dz;
}

constexpr char kMagic[8] = {'L', 'I', 'L', 'A', 'H', 'F', '0', '1'};
} // namespace

const std::vector<std::string>& blendNames()
{
    static const std::vector<std::string> names = {"replace", "add", "subtract", "multiply", "max", "min"};
    return names;
}

bool parseBlend(std::string_view name, Blend* out)
{
    const auto& names = blendNames();
    for (size_t k = 0; k < names.size(); ++k)
        if (names[k] == name)
        {
            *out = Blend(k);
            return true;
        }
    return false;
}

float blend(Blend mode, float old, float value, float factor)
{
    float r = value;
    switch (mode)
    {
    case Blend::Replace: break;
    case Blend::Add: r = old + value; break;
    case Blend::Subtract: r = old - value; break;
    case Blend::Multiply: r = old * value; break;
    case Blend::Max: r = std::max(old, value); break;
    case Blend::Min: r = std::min(old, value); break;
    }
    return old + (r - old) * std::clamp(factor, 0.0f, 1.0f);
}

double gradientNoise(double x, double z, uint32_t seed)
{
    const double fx = std::floor(x), fz = std::floor(z);
    const int32_t ix = int32_t(fx), iz = int32_t(fz);
    const double dx = x - fx, dz = z - fz;
    const double u = quintic(dx), v = quintic(dz);
    double a = corner(ix, iz, dx, dz, seed), b = corner(ix + 1, iz, dx - 1.0, dz, seed);
    double c = corner(ix, iz + 1, dx, dz - 1.0, seed), d = corner(ix + 1, iz + 1, dx - 1.0, dz - 1.0, seed);
    return 1.41421356 * ((a + (b - a) * u) * (1.0 - v) + (c + (d - c) * u) * v);
}

double noiseHeight(const NoiseParams& p, double wx, double wz)
{
    const double inv = 1.0 / std::max(1e-6, p.featureSize);
    double x = wx * inv, z = wz * inv;
    if (p.warp != 0.0)
    {
        double qx = gradientNoise(x + 5.2, z + 1.3, p.seed ^ 0x51ed27u), qz = gradientNoise(x + 9.7, z + 2.8, p.seed ^ 0x2c1b3cu);
        x += p.warp * qx;
        z += p.warp * qz;
    }
    const double ridged = std::clamp(p.ridged, 0.0, 1.0);
    double sum = 0.0, norm = 0.0, amp = 1.0, freq = 1.0, weight = 1.0;
    for (int o = 0; o < std::max(1, p.octaves); ++o)
    {
        const uint32_t s = p.seed + uint32_t(o) * 0x9e3779b9u;
        const double n = gradientNoise(x * freq, z * freq, s);
        const double smooth = n * 0.5 + 0.5;
        // Ridges: 1 - |n| squared, with each octave weighted by the last so detail gathers on the crests.
        double r = 1.0 - std::abs(n);
        r *= r;
        const double rw = r * weight;
        weight = std::clamp(r * 2.0, 0.0, 1.0);
        sum += amp * (smooth * (1.0 - ridged) + rw * ridged);
        norm += amp;
        amp *= std::clamp(p.roughness, 0.0, 1.0);
        freq *= std::max(1.0, p.lacunarity);
    }
    return p.amplitude * std::clamp(sum / std::max(1e-12, norm), 0.0, 1.0);
}

const Heightfield* heightfieldOf(const gn::Geometry& g) { return g.heightfield && !g.heightfield->empty() ? g.heightfield.get() : nullptr; }

std::vector<double> evaluateOnGrid(const Heightfield& h, const gn::FieldPtr& field)
{
    LILA_ZONE();

    std::vector<double> out(h.size(), 0.0);
    if (!field)
        return out;
    if (field->constant())
    {
        std::fill(out.begin(), out.end(), gn::evaluateSingle<double>(field));
        return out;
    }
    constexpr size_t kChunk = size_t(1) << 16;
    const size_t chunks = (h.size() + kChunk - 1) / kChunk;
    tbb::parallel_for(size_t(0), chunks, [&](size_t c) {
        const size_t first = c * kChunk, last = std::min(h.size(), first + kChunk);
        geo::Mesh pts;
        pts.reserve(last - first, 0, 0, 0);
        for (size_t k = first; k < last; ++k)
            pts.addPoint(h.position(int(k % size_t(h.nx)), int(k / size_t(h.nx))));
        std::span<float> height = pts.pointAttrs().getOrAdd<float>("height");
        std::copy(h.heights.begin() + std::ptrdiff_t(first), h.heights.begin() + std::ptrdiff_t(last), height.begin());
        for (const auto& [name, values] : h.masks)
            if (values.size() == h.size() && name != "height")
            {
                std::span<float> dst = pts.pointAttrs().getOrAdd<float>(name);
                std::copy(values.begin() + std::ptrdiff_t(first), values.begin() + std::ptrdiff_t(last), dst.begin());
            }
        const std::vector<double> v = gn::evaluateAll<double>(field, gn::FieldContext::forPoints(pts));
        std::copy(v.begin(), v.end(), out.begin() + std::ptrdiff_t(first));
    });
    return out;
}

void writeHeightfield(const std::filesystem::path& path, const Heightfield& h)
{
    LILA_ZONE();

    nlohmann::json header = {{"nx", h.nx}, {"nz", h.nz}, {"cell", h.cell}, {"origin", {h.origin.x, h.origin.y}}, {"masks", nlohmann::json::array()}};
    for (const auto& [name, values] : h.masks)
        if (values.size() == h.size())
            header["masks"].push_back(name);
    const std::string text = header.dump();
    std::ofstream f(path, std::ios::binary);
    if (!f)
        throw std::runtime_error("can't write " + path.string());
    const uint32_t len = uint32_t(text.size());
    f.write(kMagic, sizeof(kMagic));
    f.write(reinterpret_cast<const char*>(&len), sizeof(len));
    f.write(text.data(), std::streamsize(text.size()));
    f.write(reinterpret_cast<const char*>(h.heights.data()), std::streamsize(h.heights.size() * sizeof(float)));
    for (const auto& [name, values] : h.masks)
        if (values.size() == h.size())
            f.write(reinterpret_cast<const char*>(values.data()), std::streamsize(values.size() * sizeof(float)));
    if (!f)
        throw std::runtime_error("can't write " + path.string());
}

Heightfield readHeightfield(const std::filesystem::path& path)
{
    LILA_ZONE();

    std::ifstream f(path, std::ios::binary);
    if (!f)
        throw std::runtime_error("can't open " + path.string());
    char magic[sizeof(kMagic)] = {};
    uint32_t len = 0;
    f.read(magic, sizeof(magic));
    f.read(reinterpret_cast<char*>(&len), sizeof(len));
    if (!f || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0 || len > (1u << 24))
        throw std::runtime_error(path.string() + " isn't a Lila heightfield");
    std::string text(len, '\0');
    f.read(text.data(), len);
    const nlohmann::json header = nlohmann::json::parse(text);
    Heightfield h;
    h.nx = header.at("nx").get<int>();
    h.nz = header.at("nz").get<int>();
    h.cell = header.at("cell").get<double>();
    h.origin = {header.at("origin")[0].get<double>(), header.at("origin")[1].get<double>()};
    if (h.nx < 2 || h.nz < 2 || h.size() > (size_t(1) << 32))
        throw std::runtime_error(path.string() + ": bad grid size");
    h.heights.resize(h.size());
    f.read(reinterpret_cast<char*>(h.heights.data()), std::streamsize(h.size() * sizeof(float)));
    for (const auto& name : header.value("masks", nlohmann::json::array()))
    {
        std::vector<float>& m = h.mask(name.get<std::string>());
        f.read(reinterpret_cast<char*>(m.data()), std::streamsize(m.size() * sizeof(float)));
    }
    if (!f)
        throw std::runtime_error(path.string() + " is cut short");
    return h;
}

Heightfield loadHeightfield(const std::filesystem::path& path, glm::dvec2 center, double width, double low, double high, bool raw)
{
    LILA_ZONE();

    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    if (ext == ".lilahf")
        return readHeightfield(path);
    std::string err;
    image::Image img;
    bool floats = false;
    if (ext == ".png")
        img = image::readPng(path, &err);
    else if (ext == ".exr")
    {
        img = image::readExr(path, &err);
        floats = true;
    }
    else if (ext == ".tif" || ext == ".tiff")
    {
        img = image::readTiff(path, &err);
        floats = true;
    }
    else if (ext == ".psd")
        img = image::readPsd(path, &err);
    else if (ext == ".tga")
        img = image::readTga(path, &err);
    else
        throw std::runtime_error(path.string() + ": unknown height file type (lilahf, png, exr, tif, psd, tga)");
    if (img.width < 2 || img.height < 2)
        throw std::runtime_error(path.string() + ": " + (err.empty() ? std::string("no image") : err));
    Heightfield h = geo::makeHeightfield(img.width, img.height, std::max(1e-6, width) / double(img.width - 1), center);
    const bool values = raw && floats;
    tbb::parallel_for(0, img.height, [&](int z) {
        for (int x = 0; x < img.width; ++x)
        {
            const float r = img.rgba[(size_t(z) * size_t(img.width) + size_t(x)) * 4];
            h.heights[h.index(x, z)] = values ? r : float(low + (high - low) * double(r));
        }
    });
    h.origin = center - h.extent() * 0.5;
    return h;
}
} // namespace terrain
