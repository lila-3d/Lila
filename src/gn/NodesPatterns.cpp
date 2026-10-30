#include "gn/NodesCommon.h"
#include "gn/Texture.h"

#include <bit>

namespace lila::gn::nodes::patterns
{
using D2 = glm::dvec2;
using D3 = glm::dvec3;
using D4 = glm::dvec4;

constexpr double kTwoPi = 2.0 * kPi;

SocketDecl vectorIn() { return fin("vector", T::Vector, nullptr, Implicit::Position, "Where to sample; defaults to the position."); }
D4 grey(double f) { return {f, f, f, 1.0}; }

/** Gradient noise fBm in 0..1, as the distortion of Blender's Wave Texture reads it. */
double fbm(const D3& p, double detail, double roughness)
{
    detail = std::clamp(detail, 0.0, 15.0);
    double amp = 1.0, sum = 0.0, norm = 0.0, freq = 1.0;
    int octaves = int(detail);
    for (int o = 0; o <= octaves; ++o)
    {
        sum += tex::gradientNoise4(D4(p * freq, 0.0)) * amp;
        norm += amp;
        amp *= std::clamp(roughness, 0.0, 1.0);
        freq *= 2.0;
    }
    double rest = detail - double(octaves);
    double v = sum / norm;
    if (rest > 0.0)
    {
        double with = (sum + tex::gradientNoise4(D4(p * freq, 0.0)) * amp) / (norm + amp);
        v += (with - v) * rest;
    }
    return 0.5 + 0.5 * v;
}

double gradient(const std::string& type, const D3& p)
{
    if (type == "quadratic")
    {
        double r = std::max(p.x, 0.0);
        return r * r;
    }
    if (type == "easing")
    {
        double r = std::clamp(p.x, 0.0, 1.0), t = r * r;
        return 3.0 * t - 2.0 * t * r;
    }
    if (type == "diagonal")
        return (p.x + p.y) * 0.5;
    if (type == "radial")
        return std::atan2(p.y, p.x) / kTwoPi + 0.5;
    if (type == "quadraticSphere" || type == "spherical")
    {
        // Biased so a unit-length vector gives exactly zero.
        double r = std::max(0.999999 - glm::length(p), 0.0);
        return type == "spherical" ? r : r * r;
    }
    return p.x;
}

void gradientNode()
{
    registerDecl({
        .type = "GradientTexture",
        .label = "Gradient Texture",
        .category = "Texture",
        .doc = "Blender's Gradient Texture: a ramp from the vector's X (linear, quadratic, easing), X and Y (diagonal, radial) or its length "
               "(spherical, quadratic sphere), clamped to 0..1. Positions are in cm, so scale the vector to spread the ramp.",
        .props = {prop("gradientType", {"linear", "quadratic", "easing", "diagonal", "spherical", "quadraticSphere", "radial"}, "linear")},
        .sockets = {.inputs = {vectorIn()}, .outputs = {fout("color", T::Color), fout("fac", T::Float)}},
        .exec =
            [](ExecContext& c) {
                std::string type = c.prop("gradientType");
                auto fac = [type](D3 p) { return std::clamp(gradient(type, p), 0.0, 1.0); };
                c.out("fac", map<double, D3>(T::Float, fac, {c.field("vector")}));
                c.out("color", map<D4, D3>(T::Color, [fac](D3 p) { return grey(fac(p)); }, {c.field("vector")}));
            },
    });
}

double wave(bool rings, const std::string& dir, const std::string& profile, D3 p, double distortion, double detail, double detailScale,
            double detailRoughness, double phase)
{
    p = (p + 0.000001) * 0.999999;
    double n;
    if (!rings)
        n = dir == "x" ? p.x * 20.0 : dir == "y" ? p.y * 20.0 : dir == "z" ? p.z * 20.0 : (p.x + p.y + p.z) * 10.0;
    else
    {
        D3 rp = p;
        if (dir == "x")
            rp.x = 0.0;
        else if (dir == "y")
            rp.y = 0.0;
        else if (dir == "z")
            rp.z = 0.0;
        n = glm::length(rp) * 20.0;
    }
    n += phase;
    if (distortion != 0.0)
        n += distortion * (fbm(p * detailScale, detail, detailRoughness) * 2.0 - 1.0);
    if (profile == "saw")
    {
        n /= kTwoPi;
        return n - std::floor(n);
    }
    if (profile == "triangle")
    {
        n /= kTwoPi;
        return std::abs(n - std::floor(n + 0.5)) * 2.0;
    }
    return 0.5 + 0.5 * std::sin(n - kPi / 2.0);
}

void waveNode()
{
    registerDecl({
        .type = "WaveTexture",
        .label = "Wave Texture",
        .category = "Texture",
        .doc = "Blender's Wave Texture: bands or rings with a sine, saw or triangle profile, optionally distorted by noise. Positions are in "
               "cm, so the default scale of 0.05 gives one wave about every 6 cm.",
        .props = {prop("waveType", {"bands", "rings"}, "bands"), prop("bandsDirection", {"x", "y", "z", "diagonal"}, "x"),
                  prop("ringsDirection", {"x", "y", "z", "spherical"}, "x"), prop("profile", {"sine", "saw", "triangle"}, "sine")},
        .sockets = {.inputs = {vectorIn(), fin("scale", T::Float, 0.05), fin("distortion", T::Float, 0.0), bounded(fin("detail", T::Float, 2.0), 0.0, 15.0),
                               fin("detailScale", T::Float, 1.0), bounded(fin("detailRoughness", T::Float, 0.5), 0.0, 1.0), fin("phaseOffset", T::Float, 0.0)},
                    .outputs = {fout("color", T::Color), fout("fac", T::Float)}},
        .exec =
            [](ExecContext& c) {
                bool rings = c.prop("waveType") == "rings";
                std::string dir = rings ? c.prop("ringsDirection") : c.prop("bandsDirection");
                std::string profile = c.prop("profile");
                auto fn = [rings, dir, profile](D3 p, double scale, double dist, double det, double ds, double dr, double phase) {
                    return wave(rings, dir, profile, p * scale, dist, det, ds, dr, phase);
                };
                std::array<FieldPtr, 7> ins{c.field("vector"),      c.field("scale"),           c.field("distortion"), c.field("detail"),
                                            c.field("detailScale"), c.field("detailRoughness"), c.field("phaseOffset")};
                c.out("fac", map<double, D3, double, double, double, double, double, double>(T::Float, fn, ins));
                c.out("color", map<D4, D3, double, double, double, double, double, double>(
                                   T::Color, [fn](D3 p, double s, double d, double de, double ds, double dr, double ph) { return grey(fn(p, s, d, de, ds, dr, ph)); },
                                   ins));
            },
    });
}

void checkerNode()
{
    registerDecl({
        .type = "CheckerTexture",
        .label = "Checker Texture",
        .category = "Texture",
        .doc = "Blender's Checker Texture: alternating 3D cells of two colours; fac is 1 on color1. Positions are in cm, so the default scale "
               "of 0.05 gives 20 cm squares.",
        .sockets = {.inputs = {vectorIn(), fin("color1", T::Color, json::array({0.8, 0.8, 0.8, 1.0})), fin("color2", T::Color, json::array({0.2, 0.2, 0.2, 1.0})),
                               fin("scale", T::Float, 0.05)},
                    .outputs = {fout("color", T::Color), fout("fac", T::Float)}},
        .exec =
            [](ExecContext& c) {
                auto fac = [](D3 p, double scale) {
                    p = (p * scale + 0.000001) * 0.999999;
                    int64_t xi = std::abs(int64_t(std::floor(p.x))), yi = std::abs(int64_t(std::floor(p.y))), zi = std::abs(int64_t(std::floor(p.z)));
                    return ((xi % 2 == yi % 2) == (zi % 2 == 1)) ? 1.0 : 0.0;
                };
                c.out("fac", map<double, D3, double>(T::Float, fac, {c.field("vector"), c.field("scale")}));
                c.out("color", map<D4, D3, double, D4, D4>(
                                   T::Color, [fac](D3 p, double s, D4 a, D4 b) { return fac(p, s) == 1.0 ? a : b; },
                                   {c.field("vector"), c.field("scale"), c.field("color1"), c.field("color2")}));
            },
    });
}

double brickNoise(uint32_t n)
{
    n = (n + 1013u) & 0x7fffffffu;
    n = (n >> 13) ^ n;
    uint32_t nn = (n * (n * n * 60493u + 19990303u) + 1376312589u) & 0x7fffffffu;
    return 0.5 * (double(nn) / 1073741824.0);
}

struct Brick
{
    double tint = 0.0;
    double mortar = 0.0;
};

struct BrickParams
{
    double offset = 0.5;
    int offsetFrequency = 2;
    double squash = 1.0;
    int squashFrequency = 2;
};

Brick brick(const D3& p, const BrickParams& bp, double mortarSize, double mortarSmooth, double bias, double brickWidth, double rowHeight)
{
    if (brickWidth <= 0.0 || rowHeight <= 0.0)
        return {};
    int64_t row = int64_t(std::floor(p.y / rowHeight));
    double offset = 0.0;
    if (bp.offsetFrequency != 0 && bp.squashFrequency != 0)
    {
        brickWidth *= (row % bp.squashFrequency) != 0 ? 1.0 : bp.squash;
        offset = (row % bp.offsetFrequency) != 0 ? 0.0 : brickWidth * bp.offset;
    }
    int64_t col = int64_t(std::floor((p.x + offset) / brickWidth));
    double x = (p.x + offset) - brickWidth * double(col);
    double y = p.y - rowHeight * double(row);
    Brick b;
    b.tint = std::clamp(brickNoise(uint32_t((int32_t(row) << 16) + (int32_t(col) & 0xFFFF))) + bias, 0.0, 1.0);
    double minDist = std::min(std::min(x, y), std::min(brickWidth - x, rowHeight - y));
    if (minDist >= mortarSize)
        b.mortar = 0.0;
    else if (mortarSmooth == 0.0)
        b.mortar = 1.0;
    else
    {
        minDist = 1.0 - minDist / mortarSize;
        double f = minDist / mortarSmooth;
        b.mortar = minDist < mortarSmooth ? f * f * (3.0 - 2.0 * f) : 1.0;
    }
    return b;
}

void brickNode()
{
    registerDecl({
        .type = "BrickTexture",
        .label = "Brick Texture",
        .category = "Texture",
        .doc = "Blender's Brick Texture: rows of bricks tinted between color1 and color2, separated by mortar; fac is the mortar mask. Brick "
               "sizes are in scaled units, so the default scale of 0.05 gives 10 cm by 5 cm bricks.",
        .props = {prop("offset", {}, "0.5", "Shift of every offsetFrequency-th row, as a fraction of the brick width."),
                  prop("offsetFrequency", {}, "2"), prop("squash", {}, "1.0", "Brick width factor on every squashFrequency-th row."),
                  prop("squashFrequency", {}, "2")},
        .sockets = {.inputs = {vectorIn(), fin("color1", T::Color, json::array({0.8, 0.8, 0.8, 1.0})), fin("color2", T::Color, json::array({0.2, 0.2, 0.2, 1.0})),
                               fin("mortar", T::Color, json::array({0.0, 0.0, 0.0, 1.0})), fin("scale", T::Float, 0.05),
                               bounded(fin("mortarSize", T::Float, 0.02), 0.0, 0.125), bounded(fin("mortarSmooth", T::Float, 0.1), 0.0, 1.0),
                               bounded(fin("bias", T::Float, 0.0), -1.0, 1.0), bounded(fin("brickWidth", T::Float, 0.5), 0.01),
                               bounded(fin("rowHeight", T::Float, 0.25), 0.01)},
                    .outputs = {fout("color", T::Color), fout("fac", T::Float)}},
        .exec =
            [](ExecContext& c) {
                auto num = [&](const char* id, double def) {
                    try
                    {
                        return std::stod(c.prop(id));
                    }
                    catch (const std::exception&)
                    {
                        return def;
                    }
                };
                BrickParams bp{num("offset", 0.5), int(num("offsetFrequency", 2)), num("squash", 1.0), int(num("squashFrequency", 2))};
                std::array<FieldPtr, 7> ins{c.field("vector"),       c.field("scale"), c.field("mortarSize"), c.field("mortarSmooth"),
                                            c.field("bias"),         c.field("brickWidth"), c.field("rowHeight")};
                c.out("fac", map<double, D3, double, double, double, double, double, double>(
                                 T::Float, [bp](D3 p, double s, double ms, double sm, double bias, double bw, double rh) { return brick(p * s, bp, ms, sm, bias, bw, rh).mortar; },
                                 ins));
                std::array<FieldPtr, 10> cins{c.field("vector"), c.field("scale"),     c.field("mortarSize"), c.field("mortarSmooth"), c.field("bias"),
                                              c.field("brickWidth"), c.field("rowHeight"), c.field("color1"),     c.field("color2"),       c.field("mortar")};
                c.out("color", map<D4, D3, double, double, double, double, double, double, D4, D4, D4>(
                                   T::Color,
                                   [bp](D3 p, double s, double ms, double sm, double bias, double bw, double rh, D4 c1, D4 c2, D4 mortar) {
                                       Brick b = brick(p * s, bp, ms, sm, bias, bw, rh);
                                       D4 col = b.mortar != 1.0 ? (1.0 - b.tint) * c1 + b.tint * c2 : c1;
                                       return glm::mix(col, mortar, b.mortar);
                                   },
                                   cins));
            },
    });
}

D3 magic(const D3& p, int depth, double distortion)
{
    double x = std::sin((p.x + p.y + p.z) * 5.0);
    double y = std::cos((-p.x + p.y - p.z) * 5.0);
    double z = -std::cos((-p.x - p.y + p.z) * 5.0);
    if (depth > 0)
    {
        x *= distortion;
        y *= distortion;
        z *= distortion;
        y = -std::cos(x - y + z) * distortion;
        if (depth > 1)
        {
            x = std::cos(x - y - z) * distortion;
            if (depth > 2)
            {
                z = std::sin(-x - y - z) * distortion;
                if (depth > 3)
                {
                    x = -std::cos(-x + y - z) * distortion;
                    if (depth > 4)
                    {
                        y = -std::sin(-x + y + z) * distortion;
                        if (depth > 5)
                        {
                            y = -std::cos(-x + y + z) * distortion;
                            if (depth > 6)
                            {
                                x = std::cos(x + y + z) * distortion;
                                if (depth > 7)
                                {
                                    z = std::sin(x + y - z) * distortion;
                                    if (depth > 8)
                                    {
                                        x = -std::cos(-x - y + z) * distortion;
                                        if (depth > 9)
                                            y = -std::sin(x - y + z) * distortion;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    if (distortion != 0.0)
    {
        x /= 2.0 * distortion;
        y /= 2.0 * distortion;
        z /= 2.0 * distortion;
    }
    return {0.5 - x, 0.5 - y, 0.5 - z};
}

void magicNode()
{
    registerDecl({
        .type = "MagicTexture",
        .label = "Magic Texture",
        .category = "Texture",
        .doc = "Blender's Magic Texture: psychedelic interference of nested sines, deeper with depth. fac is the colour's average. Positions "
               "are in cm; the default scale is 0.05.",
        .props = {prop("depth", {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10"}, "2")},
        .sockets = {.inputs = {vectorIn(), fin("scale", T::Float, 0.05), fin("distortion", T::Float, 1.0)}, .outputs = {fout("color", T::Color), fout("fac", T::Float)}},
        .exec =
            [](ExecContext& c) {
                int depth = std::clamp(std::atoi(c.prop("depth").c_str()), 0, 10);
                std::array<FieldPtr, 3> ins{c.field("vector"), c.field("scale"), c.field("distortion")};
                c.out("color", map<D4, D3, double, double>(T::Color, [depth](D3 p, double s, double d) { return D4(magic(p * s, depth, d), 1.0); }, ins));
                c.out("fac", map<double, D3, double, double>(
                                 T::Float,
                                 [depth](D3 p, double s, double d) {
                                     D3 m = magic(p * s, depth, d);
                                     return (m.x + m.y + m.z) / 3.0;
                                 },
                                 ins));
            },
    });
}

int dims(const std::string& s) { return s == "1D" ? 1 : s == "2D" ? 2 : s == "4D" ? 4 : 3; }

double whiteValue(int d, const D3& v, double w)
{
    switch (d)
    {
    case 1: return tex::hashToFloat(w);
    case 2: return tex::hashToFloat(v.x, v.y);
    case 4: return tex::hashToFloat(v.x, v.y, v.z, w);
    default: return tex::hashToFloat(v.x, v.y, v.z);
    }
}

D4 whiteColor(int d, const D3& v, double w)
{
    switch (d)
    {
    case 1: return {tex::hashToFloat(w), tex::hashToFloat(w, 1.0), tex::hashToFloat(w, 2.0), 1.0};
    case 2: return {tex::hashToFloat(v.x, v.y), tex::hashToFloat(v.x, v.y, 1.0), tex::hashToFloat(v.x, v.y, 2.0), 1.0};
    case 4: return {tex::hashToFloat(v.x, v.y, v.z, w), tex::hashToFloat(v.z, v.x, w, v.y), tex::hashToFloat(w, v.z, v.y, v.x), 1.0};
    default: return {tex::hashToFloat(v.x, v.y, v.z), tex::hashToFloat(v.x, v.y, v.z, 1.0), tex::hashToFloat(v.x, v.y, v.z, 2.0), 1.0};
    }
}

void whiteNoiseNode()
{
    registerDecl({
        .type = "WhiteNoiseTexture",
        .label = "White Noise Texture",
        .category = "Texture",
        .doc = "Blender's White Noise Texture: a random value and colour hashed from the coordinates, the same for equal inputs. Feed it "
               "snapped or per-element values (an index, a cell position) for stable per-element randomness.",
        .props = {prop("dimensions", {"1D", "2D", "3D", "4D"}, "3D")},
        .dynamicSockets =
            [](const Group&, const GNode& n) {
                int d = dims(findDecl("WhiteNoiseTexture")->propValue(n, "dimensions"));
                Sockets s;
                if (d != 1)
                    s.inputs.push_back(vectorIn());
                if (d == 1 || d == 4)
                    s.inputs.push_back(fin("w", T::Float, 0.0));
                s.outputs = {fout("value", T::Float), fout("color", T::Color)};
                return s;
            },
        .exec =
            [](ExecContext& c) {
                int d = dims(c.prop("dimensions"));
                FieldPtr vector = c.sockets.inputIndex("vector") >= 0 ? c.field("vector") : constantVector(D3(0.0));
                FieldPtr w = c.sockets.inputIndex("w") >= 0 ? c.field("w") : constantFloat(0.0);
                c.out("value", map<double, D3, double>(T::Float, [d](D3 v, double ww) { return whiteValue(d, v, ww); }, {vector, w}));
                c.out("color", map<D4, D3, double>(T::Color, [d](D3 v, double ww) { return whiteColor(d, v, ww); }, {vector, w}));
            },
    });
}

constexpr int kGaborImpulses = 8;

double cellRandom(uint32_t a, uint32_t b, uint32_t c, uint32_t k) { return double(tex::hashU(a, b, c, k)) / 4294967295.0; }

D2 gaborKernel(const D3& direction, double frequency, const D3& position)
{
    double d2 = glm::dot(position, position);
    if (d2 >= 1.0)
        return D2(0.0);
    double window = std::exp(-kPi * d2) * (0.5 + 0.5 * std::cos(kPi * d2));
    double angle = kTwoPi * glm::dot(position, frequency * direction);
    return window * D2(std::cos(angle), std::sin(angle));
}

/** Sparse convolution of Gabor kernels scattered in unit cells; the phasor's real and imaginary parts. */
D2 gabor(const D3& p, bool is3d, double frequency, double anisotropy, double orientation2d, const D3& orientation3d)
{
    D3 cell = glm::floor(p);
    D3 local = p - cell;
    D2 sum(0.0);
    double isotropy = 1.0 - std::clamp(anisotropy, 0.0, 1.0);
    D3 baseDir = glm::length(orientation3d) > 1e-12 ? glm::normalize(orientation3d) : D3(0, 0, 1);
    int zr = is3d ? 1 : 0;
    for (int dz = -zr; dz <= zr; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
            {
                D3 offset(dx, dy, dz);
                D3 c = cell + offset;
                uint32_t cx = uint32_t(int32_t(c.x)), cy = uint32_t(int32_t(c.y)), cz = uint32_t(int32_t(c.z));
                for (int i = 0; i < kGaborImpulses; ++i)
                {
                    uint32_t k = uint32_t(i) * 8u;
                    D3 at(cellRandom(cx, cy, cz, k), cellRandom(cx, cy, cz, k + 1), is3d ? cellRandom(cx, cy, cz, k + 2) : 0.0);
                    double weight = cellRandom(cx, cy, cz, k + 3) < 0.5 ? -1.0 : 1.0;
                    D3 dir;
                    if (!is3d)
                    {
                        double a = orientation2d + (cellRandom(cx, cy, cz, k + 4) - 0.5) * kPi * isotropy;
                        dir = D3(std::cos(a), std::sin(a), 0.0);
                    }
                    else
                    {
                        double u = cellRandom(cx, cy, cz, k + 4) * 2.0 - 1.0, phi = cellRandom(cx, cy, cz, k + 5) * kTwoPi;
                        double r = std::sqrt(std::max(0.0, 1.0 - u * u));
                        D3 random(r * std::cos(phi), r * std::sin(phi), u);
                        dir = glm::mix(baseDir, random, isotropy);
                        dir = glm::length(dir) > 1e-12 ? glm::normalize(dir) : baseDir;
                    }
                    D3 rel = local - (offset + at);
                    if (!is3d)
                        rel.z = 0.0;
                    sum += weight * gaborKernel(dir, frequency, rel);
                }
            }
    return sum;
}

double gaborNormalization(bool is3d)
{
    double integral = is3d ? 1.0 / (4.0 * std::sqrt(2.0)) : 0.25;
    return 6.0 * std::sqrt(kGaborImpulses * 0.5 * integral);
}

void gaborNode()
{
    registerDecl({
        .type = "GaborTexture",
        .label = "Gabor Texture",
        .category = "Texture",
        .doc = "Gabor noise in the manner of Blender's Gabor Texture: directional, band-limited waves scattered over cells. Anisotropy 1 "
               "aligns every wave with the orientation; 0 points them anywhere. Positions are in cm; the default scale is 0.05.",
        .props = {prop("gaborType", {"2D", "3D"}, "2D")},
        .dynamicSockets =
            [](const Group&, const GNode& n) {
                bool is3d = findDecl("GaborTexture")->propValue(n, "gaborType") == "3D";
                Sockets s;
                s.inputs = {vectorIn(), fin("scale", T::Float, 0.05), bounded(fin("frequency", T::Float, 2.0), 0.0),
                            bounded(fin("anisotropy", T::Float, 1.0), 0.0, 1.0)};
                if (is3d)
                    s.inputs.push_back(fin("orientation", T::Vector, vec(0.70710678, 0.70710678, 0.0), Implicit::None, "Direction of the waves."));
                else
                    s.inputs.push_back(fin("orientation", T::Float, kPi / 4.0, Implicit::None, "Angle of the waves in the XY plane, radians."));
                s.outputs = {fout("value", T::Float), fout("phase", T::Float), fout("intensity", T::Float)};
                return s;
            },
        .exec =
            [](ExecContext& c) {
                bool is3d = c.prop("gaborType") == "3D";
                double norm = gaborNormalization(is3d);
                FieldPtr orient = c.field("orientation");
                FieldPtr orient2 = is3d ? constantFloat(0.0) : orient;
                FieldPtr orient3 = is3d ? orient : constantVector(D3(0, 0, 1));
                std::array<FieldPtr, 6> ins{c.field("vector"), c.field("scale"), c.field("frequency"), c.field("anisotropy"), orient2, orient3};
                // 0 value, 1 phase, 2 intensity.
                auto make = [&](int what) {
                    return map<double, D3, double, double, double, double, D3>(
                        T::Float,
                        [is3d, norm, what](D3 p, double s, double f, double an, double o2, D3 o3) {
                            D2 g = gabor(p * s, is3d, std::max(f, 0.001), an, o2, o3);
                            if (what == 1)
                                return (std::atan2(g.y, g.x) + kPi) / kTwoPi;
                            if (what == 2)
                                return glm::length(g) / norm;
                            return (g.y / norm) * 0.5 + 0.5;
                        },
                        ins);
                };
                c.out("value", make(0));
                c.out("phase", make(1));
                c.out("intensity", make(2));
            },
    });
}
} // namespace lila::gn::nodes::patterns

namespace lila::gn::nodes
{
void registerPatternNodes()
{
    patterns::gradientNode();
    patterns::waveNode();
    patterns::checkerNode();
    patterns::brickNode();
    patterns::magicNode();
    patterns::whiteNoiseNode();
    patterns::gaborNode();
}
} // namespace lila::gn::nodes
