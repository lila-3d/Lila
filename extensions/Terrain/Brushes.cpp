// Terrain brushes: dabs from a stroke (spaced along a path or given one by one, each with its pen pressure), mirrored
// across the terrain's centre, weighted by a brush falloff and an optional alpha tip, then one operation over the
// samples they reach. The procedural stamp library lives here too.

#include "Brushes.h"

#include "Erosion.h"
#include "Shaping.h"

#include <lila/Brush.h>
#include <lila/Profile.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <map>
#include <memory>
#include <mutex>

namespace terrain
{
namespace brushes_
{
constexpr double kDegB = 0.017453292519943295;

struct Area
{
    int x0 = 0, z0 = 0, x1 = -1, z1 = -1;
    int w() const { return x1 - x0 + 1; }
    int d() const { return z1 - z0 + 1; }
    bool empty() const { return x1 < x0 || z1 < z0; }
    size_t size() const { return empty() ? 0 : size_t(w()) * size_t(d()); }
    size_t at(int x, int z) const { return size_t(z - z0) * size_t(w()) + size_t(x - x0); }
};

Area areaAround(const Heightfield& h, glm::dvec2 lo, glm::dvec2 hi, double margin)
{
    Area r;
    r.x0 = std::max(0, int(std::floor((lo.x - margin - h.origin.x) / h.cell)));
    r.z0 = std::max(0, int(std::floor((lo.y - margin - h.origin.y) / h.cell)));
    r.x1 = std::min(h.nx - 1, int(std::ceil((hi.x + margin - h.origin.x) / h.cell)));
    r.z1 = std::min(h.nz - 1, int(std::ceil((hi.y + margin - h.origin.y) / h.cell)));
    return r;
}

struct DabAt
{
    glm::dvec2 p;
    double pressure = 1.0;
};

double smoothB(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

/** The classic terrain falloff: full over the inner 30%, then (1 - u^2)^2. */
double terrainFalloff(double t)
{
    if (t >= 1.0)
        return 0.0;
    const double u = std::max(0.0, (t - 0.3) / 0.7);
    return (1.0 - u * u) * (1.0 - u * u);
}

std::vector<glm::dvec2> mirrorsOf(glm::dvec2 p, const std::string& mirror, glm::dvec2 c)
{
    std::vector<glm::dvec2> out = {p};
    const bool mx = mirror.find('x') != std::string::npos, mz = mirror.find('z') != std::string::npos;
    if (mx)
        out.push_back({2.0 * c.x - p.x, p.y});
    if (mz)
        out.push_back({p.x, 2.0 * c.y - p.y});
    if (mx && mz)
        out.push_back({2.0 * c.x - p.x, 2.0 * c.y - p.y});
    return out;
}

/** The stroke's dabs: path points every spacing (pressure interpolated) or the points themselves, then mirrored. */
std::vector<DabAt> dabsOf(const Heightfield& h, const SculptStroke& s)
{
    auto pressureAt = [&](size_t k) { return k < s.pressure.size() ? std::clamp(double(s.pressure[k]), 0.0, 1.0) : 1.0; };
    std::vector<DabAt> dabs;
    if (s.path && s.points.size() >= 2)
    {
        const double step = s.spacing > 0.0 ? s.spacing : std::max(s.radius * 0.25, h.cell * 0.5);
        dabs.push_back({s.points[0], pressureAt(0)});
        double carry = 0.0;
        for (size_t k = 1; k < s.points.size(); ++k)
        {
            const glm::dvec2 a = s.points[k - 1], b = s.points[k];
            const double len = glm::length(b - a);
            double t = step - carry;
            for (; t <= len; t += step)
            {
                const double f = len > 0.0 ? t / len : 0.0;
                dabs.push_back({a + (b - a) * f, pressureAt(k - 1) + (pressureAt(k) - pressureAt(k - 1)) * f});
            }
            carry = len - (t - step);
        }
        if (dabs.back().p != s.points.back())
            dabs.push_back({s.points.back(), pressureAt(s.points.size() - 1)});
    }
    else
        for (size_t k = 0; k < s.points.size(); ++k)
            dabs.push_back({s.points[k], pressureAt(k)});
    if (s.mirror.empty() || s.mirror == "off")
        return dabs;
    const glm::dvec2 c = h.origin + h.extent() * 0.5;
    std::vector<DabAt> out;
    for (const DabAt& d : dabs)
        for (const glm::dvec2& m : mirrorsOf(d.p, s.mirror, c))
            out.push_back({m, d.pressure});
    return out;
}

std::shared_ptr<const lila::brush::AlphaImage> loadAlpha(const std::string& ref)
{
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<const lila::brush::AlphaImage>> cache;
    std::lock_guard lock(mutex);
    auto it = cache.find(ref);
    if (it != cache.end())
        return it->second;
    auto img = lila::brush::AlphaImage::load(lila::brush::alphas::resolve(ref));
    cache[ref] = img;
    return img;
}

/** A tip or stamp shape: a library stamp or an image, sampled at (u, v) in the unit square around the dab. */
class Shape
{
public:
    explicit Shape(const std::string& ref, double angleDeg, uint32_t seed) : mSeed(seed)
    {
        if (ref.empty())
            return;
        if (isStampName(ref))
            mStamp = ref;
        else
        {
            mImage = loadAlpha(ref);
            if (!mImage)
                throw std::runtime_error("no stamp or image '" + ref + "' (stamps: peak, crater, dune, rocks, mesa, ridge, volcano)");
        }
        mCos = std::cos(-angleDeg * kDegB);
        mSin = std::sin(-angleDeg * kDegB);
    }

    bool any() const { return !mStamp.empty() || mImage; }

    /** rel is the sample minus the dab centre, over the radius. */
    double operator()(glm::dvec2 rel) const
    {
        const double u = rel.x * mCos - rel.y * mSin, v = rel.x * mSin + rel.y * mCos;
        if (!mStamp.empty())
            return stampShape(mStamp, u, v, mSeed);
        if (std::abs(u) > 1.0 || std::abs(v) > 1.0)
            return 0.0;
        return mImage->sample(glm::dvec2(u, v) * 0.5 + 0.5);
    }

private:
    std::string mStamp;
    std::shared_ptr<const lila::brush::AlphaImage> mImage;
    uint32_t mSeed = 0;
    double mCos = 1.0, mSin = 0.0;
};

double settingOr(const SculptStroke& s, const char* key, double fallback)
{
    if (s.settings.is_object())
        if (auto it = s.settings.find(key); it != s.settings.end() && it->is_number())
            return it->get<double>();
    return fallback;
}

std::string settingOr(const SculptStroke& s, const char* key, const std::string& fallback)
{
    if (s.settings.is_object())
        if (auto it = s.settings.find(key); it != s.settings.end() && it->is_string())
            return it->get<std::string>();
    return fallback;
}

bool settingOr(const SculptStroke& s, const char* key, bool fallback)
{
    if (s.settings.is_object())
        if (auto it = s.settings.find(key); it != s.settings.end() && it->is_boolean())
            return it->get<bool>();
    return fallback;
}

double fbmB(double x, double z, uint32_t seed, int octaves)
{
    double n = 0.0, amp = 1.0, freq = 1.0, norm = 0.0;
    for (int o = 0; o < octaves; ++o)
    {
        n += amp * gradientNoise(x * freq, z * freq, seed + uint32_t(o) * 977u);
        norm += amp;
        amp *= 0.5;
        freq *= 2.0;
    }
    return n / norm;
}

uint32_t hashB(uint32_t a, uint32_t b)
{
    uint32_t h = a * 0x9e3779b9u ^ (b + 0x7f4a7c15u) * 0x85ebca6bu;
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    return h;
}

double unitB(uint32_t h) { return double(h & 0xffffffu) / double(0x1000000u); }
} // namespace brushes_

std::vector<float> unpackFloats(const std::string& text);

namespace brushes_
{
/** A Sculpt Region bake: heights added over a block of samples every step, bilinear between, faded at the rim. */
void applyPatch(Heightfield& h, const SculptStroke& s)
{
    const nlohmann::json p = s.settings.value("patch", nlohmann::json::object());
    const int x0 = p.value("x0", 0), z0 = p.value("z0", 0), step = std::max(1, p.value("step", 1));
    const int nx = p.value("nx", 0), nz = p.value("nz", 0);
    const std::vector<float> delta = unpackFloats(p.value("delta", std::string()));
    if (nx < 2 || nz < 2 || delta.size() != size_t(nx) * size_t(nz))
        throw std::runtime_error("patch stroke: its delta doesn't match its size");
    const double fade = std::max(1.0, 0.06 * double(std::min(nx, nz)));
    for (int z = std::max(0, z0); z <= std::min(h.nz - 1, z0 + (nz - 1) * step); ++z)
        for (int x = std::max(0, x0); x <= std::min(h.nx - 1, x0 + (nx - 1) * step); ++x)
        {
            const double u = double(x - x0) / step, v = double(z - z0) / step;
            const int iu = std::min(int(u), nx - 2), iv = std::min(int(v), nz - 2);
            const double fu = u - iu, fv = v - iv;
            auto at = [&](int a, int b) { return double(delta[size_t(b) * size_t(nx) + size_t(a)]); };
            const double d = at(iu, iv) * (1 - fu) * (1 - fv) + at(iu + 1, iv) * fu * (1 - fv) + at(iu, iv + 1) * (1 - fu) * fv + at(iu + 1, iv + 1) * fu * fv;
            const double rim = std::min({u, v, double(nx - 1) - u, double(nz - 1) - v});
            h.heights[h.index(x, z)] = float(h.at(x, z) + d * smoothB(rim / fade));
        }
}
} // namespace brushes_

// ------------------------------------------------------------------------------------------------- stamps

const std::vector<std::string>& stampNames()
{
    static const std::vector<std::string> n = {"peak", "crater", "dune", "rocks", "mesa", "ridge", "volcano"};
    return n;
}

bool isStampName(const std::string& name) { return std::ranges::find(stampNames(), name) != stampNames().end(); }

double stampShape(const std::string& name, double u, double v, uint32_t seed)
{
    using namespace brushes_;
    const double r = std::sqrt(u * u + v * v);
    if (r >= 1.0)
        return 0.0;
    // Every shape fades out over the outer 15%, so stamps never leave a step at their rim.
    const double rim = 1.0 - smoothB((r - 0.85) / 0.15);
    double hgt = 0.0;
    if (name == "peak")
    {
        const double ridges = 1.0 - std::abs(fbmB(u * 2.5, v * 2.5, seed + 11u, 3));
        hgt = std::pow(1.0 - r, 1.6) * (0.8 + 0.35 * ridges);
    }
    else if (name == "crater")
    {
        const double R = 0.62;
        const double bowl = r < R ? -0.7 * (1.0 - (r / R) * (r / R)) : 0.0;
        const double lip = 0.45 * std::exp(-((r - R) / 0.11) * ((r - R) / 0.11));
        const double ejecta = r > R ? 0.12 * (1.0 - smoothB((r - R) / (1.0 - R))) * (0.6 + 0.4 * fbmB(u * 6.0, v * 6.0, seed + 3u, 2)) : 0.0;
        hgt = bowl + lip + ejecta;
    }
    else if (name == "dune")
    {
        // A crescent dune with the wind along +u: long windward slope, steep lee face, horns trailing downwind.
        const double crest = 0.15 + 0.55 * v * v;
        double t;
        if (u < crest)
            t = smoothB((u - (crest - 0.95)) / 0.95);
        else
            t = 1.0 - smoothB((u - crest) / 0.2);
        hgt = t * std::pow(std::max(0.0, 1.0 - v * v), 1.5) * 0.8;
    }
    else if (name == "rocks")
    {
        for (uint32_t k = 0; k < 7; ++k)
        {
            const uint32_t hs = hashB(seed + 17u, k);
            const double a = unitB(hs) * 6.283185307179586, d = 0.55 * std::sqrt(unitB(hashB(hs, 1u)));
            const double cu = std::cos(a) * d, cv = std::sin(a) * d;
            const double br = 0.14 + 0.2 * unitB(hashB(hs, 2u)), top = 0.5 + 0.5 * unitB(hashB(hs, 3u));
            const double q = std::sqrt((u - cu) * (u - cu) + (v - cv) * (v - cv)) / br;
            if (q < 1.0)
                hgt = std::max(hgt, top * std::pow(1.0 - q * q, 0.6) * (0.9 + 0.1 * fbmB(u * 12.0, v * 12.0, seed + k, 2)));
        }
    }
    else if (name == "mesa")
    {
        const double edge = r + 0.07 * fbmB(u * 4.0, v * 4.0, seed + 5u, 3);
        hgt = smoothB((0.72 - edge) / 0.12) * (0.95 + 0.05 * fbmB(u * 8.0, v * 8.0, seed + 6u, 2));
    }
    else if (name == "ridge")
    {
        const double across = std::abs(v + 0.08 * fbmB(u * 3.0, 0.0, seed + 9u, 2));
        hgt = std::pow(std::max(0.0, 1.0 - across * 1.6), 1.5) * (1.0 - u * u) * (0.85 + 0.25 * (1.0 - std::abs(fbmB(u * 5.0, v * 5.0, seed + 2u, 3))));
    }
    else if (name == "volcano")
    {
        const double cone = std::pow(1.0 - r, 1.3);
        const double caldera = r < 0.16 ? 0.35 * (1.0 - (r / 0.16) * (r / 0.16)) : 0.0;
        hgt = cone - caldera;
    }
    return hgt * rim;
}

std::vector<float> stampImage(const std::string& name, int size, uint32_t seed)
{
    size = std::clamp(size, 2, 4096);
    std::vector<float> img(size_t(size) * size_t(size));
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
        {
            const double u = (x + 0.5) / size * 2.0 - 1.0, v = (y + 0.5) / size * 2.0 - 1.0;
            img[size_t(y) * size_t(size) + size_t(x)] = float(std::clamp(0.5 + 0.5 * stampShape(name, u, v, seed), 0.0, 1.0));
        }
    return img;
}

std::string packFloats(const std::vector<float>& v)
{
    static const char* kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto* b = reinterpret_cast<const unsigned char*>(v.data());
    const size_t n = v.size() * sizeof(float);
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3)
    {
        const uint32_t x = uint32_t(b[i]) << 16 | (i + 1 < n ? uint32_t(b[i + 1]) << 8 : 0u) | (i + 2 < n ? uint32_t(b[i + 2]) : 0u);
        out += kAlphabet[(x >> 18) & 63];
        out += kAlphabet[(x >> 12) & 63];
        out += i + 1 < n ? kAlphabet[(x >> 6) & 63] : '=';
        out += i + 2 < n ? kAlphabet[x & 63] : '=';
    }
    return out;
}

std::vector<float> unpackFloats(const std::string& text)
{
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z')
            return c - 'A';
        if (c >= 'a' && c <= 'z')
            return c - 'a' + 26;
        if (c >= '0' && c <= '9')
            return c - '0' + 52;
        return c == '+' ? 62 : c == '/' ? 63 : -1;
    };
    if (text.size() % 4)
        return {};
    std::vector<unsigned char> bytes;
    bytes.reserve(text.size() / 4 * 3);
    for (size_t i = 0; i < text.size(); i += 4)
    {
        uint32_t x = 0;
        int pad = 0;
        for (int k = 0; k < 4; ++k)
        {
            const char c = text[i + size_t(k)];
            int v = 0;
            if (c == '=')
                ++pad;
            else if ((v = val(c)) < 0)
                return {};
            x = x << 6 | uint32_t(v);
        }
        bytes.push_back(uint8_t(x >> 16));
        if (pad < 2)
            bytes.push_back(uint8_t(x >> 8));
        if (pad < 1)
            bytes.push_back(uint8_t(x));
    }
    std::vector<float> out(bytes.size() / sizeof(float));
    std::memcpy(out.data(), bytes.data(), out.size() * sizeof(float));
    return out;
}

// ------------------------------------------------------------------------------------------------- strokes

bool usesBrushPath(const SculptStroke& s)
{
    return int(s.op) > int(SculptOp::Noise) || !s.pressure.empty() || !s.falloff.empty() || (!s.mirror.empty() && s.mirror != "off") ||
           (s.settings.is_object() && !s.settings.empty());
}

void applyBrushStroke(Heightfield& h, const SculptStroke& s)
{
    LILA_ZONE();
    using namespace brushes_;

    if (h.empty() || s.points.empty())
        return;
    if (s.op == SculptOp::Patch)
        return applyPatch(h, s);
    const std::vector<DabAt> dabs = dabsOf(h, s);
    const double radius = std::max(s.radius, 1e-3);
    glm::dvec2 lo(std::numeric_limits<double>::infinity()), hi(-std::numeric_limits<double>::infinity());
    for (const DabAt& d : dabs)
    {
        lo = glm::min(lo, d.p);
        hi = glm::max(hi, d.p);
    }
    std::vector<std::pair<glm::dvec2, glm::dvec2>> segments;
    if (s.op == SculptOp::Ramp)
    {
        const glm::dvec2 c = h.origin + h.extent() * 0.5;
        const std::vector<glm::dvec2> a = mirrorsOf(s.points.front(), s.mirror, c), b = mirrorsOf(s.points.back(), s.mirror, c);
        for (size_t k = 0; k < a.size(); ++k)
        {
            segments.push_back({a[k], b[k]});
            lo = glm::min(lo, glm::min(a[k], b[k]));
            hi = glm::max(hi, glm::max(a[k], b[k]));
        }
    }
    const int blurR = std::max(1, int(std::lround(radius * 0.15 / h.cell)));
    const Area r = areaAround(h, lo, hi, radius + h.cell * double(2 * blurR + 2));
    if (r.empty())
        return;

    const std::optional<lila::brush::FalloffTable> table =
        s.falloff.empty() ? std::nullopt : std::optional<lila::brush::FalloffTable>(lila::brush::FalloffTable(lila::brush::Falloff::fromName(s.falloff)));
    auto falloff = [&](double t) { return table ? double((*table)(t)) : terrainFalloff(t); };
    const double angle = settingOr(s, "angle", 0.0);
    const Shape tip(settingOr(s, "alpha", std::string()), angle, s.seed);

    // Weight per sample: the strongest dab there, so spacing never changes how much a stroke does.
    std::vector<float> w(r.size(), 0.0f);
    std::vector<float> shape;
    std::vector<float> dabHeight;
    const bool stamping = s.op == SculptOp::Stamp;
    const Shape stamp = stamping ? Shape(settingOr(s, "stamp", std::string("peak")), angle, s.seed) : Shape(std::string(), 0.0, 0);
    if (stamping)
    {
        shape.assign(r.size(), 0.0f);
        dabHeight.assign(r.size(), 0.0f);
    }
    if (s.op == SculptOp::Ramp)
        for (int z = r.z0; z <= r.z1; ++z)
            for (int x = r.x0; x <= r.x1; ++x)
            {
                const glm::dvec2 q = h.samplePos(x, z);
                float best = 0.0f;
                for (const auto& [a, b] : segments)
                {
                    const glm::dvec2 ab = b - a;
                    const double t = std::clamp(glm::dot(q - a, ab) / std::max(1e-12, glm::dot(ab, ab)), 0.0, 1.0);
                    best = std::max(best, float(falloff(glm::length(q - (a + ab * t)) / radius)));
                }
                w[r.at(x, z)] = best;
            }
    else
        for (const DabAt& d : dabs)
        {
            const Area dr = areaAround(h, d.p, d.p, radius);
            const double centre = stamping ? h.heightAt(d.p.x, d.p.y) : 0.0;
            for (int z = std::max(dr.z0, r.z0); z <= std::min(dr.z1, r.z1); ++z)
                for (int x = std::max(dr.x0, r.x0); x <= std::min(dr.x1, r.x1); ++x)
                {
                    const glm::dvec2 rel = (h.samplePos(x, z) - d.p) / radius;
                    const double t = glm::length(rel);
                    if (t >= 1.0)
                        continue;
                    const size_t k = r.at(x, z);
                    if (stamping)
                    {
                        const double v = stamp(rel) * d.pressure;
                        if (std::abs(v) > std::abs(double(shape[k])))
                        {
                            shape[k] = float(v);
                            dabHeight[k] = float(centre);
                        }
                        w[k] = 1.0f;
                        continue;
                    }
                    double f = falloff(t) * d.pressure;
                    if (tip.any())
                        f *= std::clamp(tip(rel), 0.0, 1.0);
                    w[k] = std::max(w[k], float(f));
                }
        }

    const double mix01 = std::clamp(s.strength, 0.0, 1.0);
    auto each = [&](auto&& fn) {
        for (int z = r.z0; z <= r.z1; ++z)
            for (int x = r.x0; x <= r.x1; ++x)
                if (const double v = w[r.at(x, z)]; v > 0.0)
                    fn(x, z, r.at(x, z), v);
    };
    auto regionHeights = [&] {
        std::vector<float> a(r.size());
        for (int z = r.z0; z <= r.z1; ++z)
            for (int x = r.x0; x <= r.x1; ++x)
                a[r.at(x, z)] = h.at(x, z);
        return a;
    };

    switch (s.op)
    {
    case SculptOp::Raise:
    case SculptOp::Lower:
    {
        const double sign = s.op == SculptOp::Raise ? 1.0 : -1.0;
        each([&](int x, int z, size_t, double v) { h.heights[h.index(x, z)] = float(h.at(x, z) + sign * s.strength * v); });
        break;
    }
    case SculptOp::Noise:
        each([&](int x, int z, size_t, double v) {
            const glm::dvec2 p = h.samplePos(x, z) / std::max(1e-3, s.noiseSize);
            h.heights[h.index(x, z)] = float(h.at(x, z) + s.strength * v * fbmB(p.x, p.y, s.seed, 3));
        });
        break;
    case SculptOp::Smooth:
    {
        const std::vector<float> blurred = boxBlurred(regionHeights(), r.w(), r.d(), blurR, 2);
        each([&](int x, int z, size_t k, double v) { h.heights[h.index(x, z)] = float(h.at(x, z) + (blurred[k] - h.at(x, z)) * v * mix01); });
        break;
    }
    case SculptOp::Flatten:
    {
        double a = s.height, bx = 0.0, bz = 0.0;
        if (std::isnan(a))
        {
            double sw = 0.0, sx = 0.0, sz = 0.0, sh = 0.0, sxx = 0.0, szz = 0.0, sxz = 0.0, sxh = 0.0, szh = 0.0;
            const glm::dvec2 c = (lo + hi) * 0.5;
            each([&](int x, int z, size_t, double v) {
                const glm::dvec2 q = h.samplePos(x, z) - c;
                const double hv = h.at(x, z);
                sw += v;
                sx += v * q.x;
                sz += v * q.y;
                sh += v * hv;
                sxx += v * q.x * q.x;
                szz += v * q.y * q.y;
                sxz += v * q.x * q.y;
                sxh += v * q.x * hv;
                szh += v * q.y * hv;
            });
            if (sw <= 0.0)
                break;
            a = sh / sw;
            if (settingOr(s, "plane", false))
            {
                // Weighted least squares for h = a + bx x + bz z about the stroke's centre.
                const double mx = sx / sw, mz = sz / sw;
                const double cxx = sxx / sw - mx * mx, czz = szz / sw - mz * mz, cxz = sxz / sw - mx * mz;
                const double cxh = sxh / sw - mx * a, czh = szh / sw - mz * a;
                const double det = cxx * czz - cxz * cxz;
                if (std::abs(det) > 1e-12)
                {
                    bx = (cxh * czz - czh * cxz) / det;
                    bz = (czh * cxx - cxh * cxz) / det;
                    a -= bx * mx + bz * mz;
                }
            }
            const glm::dvec2 cc = c;
            each([&](int x, int z, size_t, double v) {
                const glm::dvec2 q = h.samplePos(x, z) - cc;
                const double target = a + bx * q.x + bz * q.y;
                h.heights[h.index(x, z)] = float(h.at(x, z) + (target - h.at(x, z)) * v * mix01);
            });
            break;
        }
        each([&](int x, int z, size_t, double v) { h.heights[h.index(x, z)] = float(h.at(x, z) + (a - h.at(x, z)) * v * mix01); });
        break;
    }
    case SculptOp::Ramp:
    {
        const glm::dvec2 a0 = s.points.front(), b0 = s.points.back();
        const double ha = std::isnan(s.height) ? h.heightAt(a0.x, a0.y) : s.height;
        const double hb = settingOr(s, "endHeight", h.heightAt(b0.x, b0.y));
        each([&](int x, int z, size_t, double v) {
            const glm::dvec2 q = h.samplePos(x, z);
            double best = std::numeric_limits<double>::infinity(), target = h.at(x, z);
            for (const auto& [a, b] : segments)
            {
                const glm::dvec2 ab = b - a;
                const double t = std::clamp(glm::dot(q - a, ab) / std::max(1e-12, glm::dot(ab, ab)), 0.0, 1.0);
                const double d = glm::length(q - (a + ab * t));
                if (d < best)
                {
                    best = d;
                    target = ha + (hb - ha) * t;
                }
            }
            h.heights[h.index(x, z)] = float(h.at(x, z) + (target - h.at(x, z)) * v * mix01);
        });
        break;
    }
    case SculptOp::Terrace:
    {
        const double step = std::max(1.0, settingOr(s, "step", 1000.0));
        const double sharp = settingOr(s, "sharpness", 0.8), offset = settingOr(s, "offset", 0.0);
        each([&](int x, int z, size_t, double v) {
            const double hv = h.at(x, z);
            h.heights[h.index(x, z)] = float(hv + (terraceHeight(hv, step, sharp, offset) - hv) * v * mix01);
        });
        break;
    }
    case SculptOp::Stamp:
    {
        const std::string blendMode = settingOr(s, "blend", std::string("add"));
        each([&](int x, int z, size_t k, double) {
            const double add = s.strength * shape[k];
            float& hv = h.heights[h.index(x, z)];
            if (blendMode == "raise")
                hv = std::max(hv, float(dabHeight[k] + add));
            else if (blendMode == "lower")
                hv = std::min(hv, float(dabHeight[k] + add));
            else
                hv = float(hv + add);
        });
        break;
    }
    case SculptOp::Erode:
    {
        // Erode a copy of the patch, its border held as base level, and blend it in by the brush weight.
        Heightfield patch = lila::geo::makeHeightfield(r.w(), r.d(), h.cell, h.samplePos(r.x0, r.z0));
        patch.heights = regionHeights();
        ErosionParams ep;
        ep.iterations = int(std::clamp(settingOr(s, "iterations", 10.0), 1.0, 200.0));
        ep.thermalIterations = 2;
        erode(patch, ep);
        each([&](int x, int z, size_t k, double v) { h.heights[h.index(x, z)] = float(h.at(x, z) + (patch.heights[k] - h.at(x, z)) * v * mix01); });
        for (const char* m : {"wear", "deposit", "flow"})
        {
            const std::vector<float>& src = patch.mask(m);
            std::vector<float>& dst = h.mask(m);
            each([&](int x, int z, size_t k, double v) {
                float& d = dst[h.index(x, z)];
                d = std::max(d, float(src[k] * v));
            });
        }
        break;
    }
    case SculptOp::Relax:
    {
        const int iterations = int(std::clamp(settingOr(s, "iterations", 4.0), 1.0, 100.0));
        std::vector<float> a = regionHeights(), b = a;
        for (int it = 0; it < iterations; ++it)
        {
            for (int z = 0; z < r.d(); ++z)
                for (int x = 0; x < r.w(); ++x)
                {
                    const size_t k = size_t(z) * size_t(r.w()) + size_t(x);
                    double sum = 0.0;
                    int c = 0;
                    for (int dz = -1; dz <= 1; ++dz)
                        for (int dx = -1; dx <= 1; ++dx)
                        {
                            const int xx = x + dx, zz = z + dz;
                            if ((dx || dz) && xx >= 0 && zz >= 0 && xx < r.w() && zz < r.d())
                            {
                                sum += a[size_t(zz) * size_t(r.w()) + size_t(xx)];
                                ++c;
                            }
                        }
                    b[k] = c ? float(a[k] + (sum / c - a[k]) * 0.5 * w[k] * mix01) : a[k];
                }
            std::swap(a, b);
        }
        each([&](int x, int z, size_t k, double) { h.heights[h.index(x, z)] = a[k]; });
        break;
    }
    case SculptOp::Patch:
        break;
    case SculptOp::Uplift:
    case SculptOp::Mask:
    {
        const std::string name = settingOr(s, "mask", std::string(s.op == SculptOp::Uplift ? "uplift" : "paint"));
        const double value = std::clamp(settingOr(s, "value", 1.0), 0.0, 1.0);
        std::vector<float>& m = h.mask(name);
        each([&](int x, int z, size_t, double v) {
            float& mv = m[h.index(x, z)];
            mv = float(mv + (value - mv) * v * mix01);
        });
        break;
    }
    }
}
} // namespace terrain
