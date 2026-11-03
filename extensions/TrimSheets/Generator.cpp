// The high poly a trim sheet bakes from: every trim's profile swept across its rectangle (strips along U, past both
// sheet edges so the bake tiles), framed panels for rectangles, domes for round trims, and details instanced along them.

#include "Generator.h"

#include <lila/Profile.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <numbers>
#include <optional>

namespace trims
{
namespace
{
using glm::dvec2;
using glm::dvec3;

/** Polygons with a colour per face, made into one mesh at the end. */
struct Builder
{
    std::vector<dvec3> points;
    std::vector<geo::Index> sizes;
    std::vector<geo::Index> indices;
    std::vector<glm::vec4> colors;
    glm::vec4 color{1.0f};
    double top = 0.0;
    double bottom = 0.0;

    geo::Index point(const dvec3& p)
    {
        top = std::max(top, p.y);
        bottom = std::min(bottom, p.y);
        points.push_back(p);
        return geo::Index(points.size() - 1);
    }

    void face(std::initializer_list<geo::Index> ids)
    {
        sizes.push_back(geo::Index(ids.size()));
        indices.insert(indices.end(), ids.begin(), ids.end());
        colors.push_back(color);
    }

    void face(const std::vector<geo::Index>& ids)
    {
        sizes.push_back(geo::Index(ids.size()));
        indices.insert(indices.end(), ids.begin(), ids.end());
        colors.push_back(color);
    }

    geo::Mesh build() const
    {
        geo::Mesh m = geo::Mesh::fromPolygons(points, sizes, indices);
        // Bad faces are skipped by fromPolygons; colours only line up when none were.
        if (m.numFaces() == geo::Index(sizes.size()))
        {
            std::span<glm::vec4> c = m.cornerAttrs().getOrAdd<glm::vec4>(geo::attr::kColor);
            for (geo::Index f = 0; f < m.faceCapacity(); ++f)
                if (m.faceValid(f))
                    m.forEachFaceCorner(f, [&](geo::Index k) { c[k] = colors[f]; });
        }
        return m;
    }
};

/** A cross-section from a trim's border inward: (distance in, height), cm, starting at (0, 0). */
using Edge = std::vector<dvec2>;

Edge edgeProfile(const Profile& p, double cmPerPx, double room)
{
    const double b = std::min(p.bevelPx * cmPerPx, room);
    const double h = b * std::tan(glm::radians(p.angle));
    const int n = std::max(1, p.segments);
    auto rounded = [&](double sign) {
        Edge e;
        for (int k = 0; k <= n; ++k)
        {
            const double t = n == 1 ? double(k) : double(k) / n * std::numbers::pi * 0.5;
            e.push_back(n == 1 ? dvec2(b * t, sign * h * t) : dvec2(b * (1.0 - std::cos(t)), sign * h * std::sin(t)));
        }
        return e;
    };
    switch (p.style)
    {
    case Style::Flat: return {dvec2(0.0)};
    case Style::Recessed: return rounded(-1.0);
    case Style::DoubleBevel:
    {
        const double g = std::min(2.0 * b, std::max(0.0, room - 2.0 * b));
        return {dvec2(0.0), dvec2(b, h), dvec2(b + g, h), dvec2(2.0 * b + g, 2.0 * h)};
    }
    case Style::Step: return {dvec2(0.0), dvec2(b * 0.25, b * 1.25)};
    default: return rounded(1.0);
    }
}

double edgeDepth(const Edge& e) { return e.back().x; }
double edgeHeight(const Edge& e) { return e.back().y; }

/** Repeats a period of length period so a tiling strip covers [-period, 2 period). */
std::vector<double> periodic(double period, double spacing, double phase = 0.5)
{
    const int n = std::max(1, int(std::lround(period / std::max(spacing, 1e-6))));
    const double sp = period / n;
    std::vector<double> out;
    for (int k = -n; k < 2 * n; ++k)
        out.push_back((k + phase) * sp);
    return out;
}

/** A box with every top edge chamfered: the frustum used for plates, planks, slats and tread bars. */
void frustum(Builder& b, dvec2 lo, dvec2 hi, double base, double height, double bevel, double angleDeg = 45.0)
{
    const double inset = std::min({bevel, (hi.x - lo.x) * 0.45, (hi.y - lo.y) * 0.45});
    const double rise = std::min(height, inset * std::tan(glm::radians(angleDeg)));
    const double y0 = base, y2 = base + height;
    // Outer ring at the base, vertical wall up to where the chamfer starts, chamfer ring, top.
    const geo::Index o0 = b.point({lo.x, y0, lo.y}), o1 = b.point({hi.x, y0, lo.y}), o2 = b.point({hi.x, y0, hi.y}), o3 = b.point({lo.x, y0, hi.y});
    const double wallTop = y2 - rise;
    const geo::Index w0 = b.point({lo.x, wallTop, lo.y}), w1 = b.point({hi.x, wallTop, lo.y}), w2 = b.point({hi.x, wallTop, hi.y}),
                     w3 = b.point({lo.x, wallTop, hi.y});
    const dvec2 ilo = lo + dvec2(inset), ihi = hi - dvec2(inset);
    const geo::Index t0 = b.point({ilo.x, y2, ilo.y}), t1 = b.point({ihi.x, y2, ilo.y}), t2 = b.point({ihi.x, y2, ihi.y}), t3 = b.point({ilo.x, y2, ihi.y});
    // Walls face outward, the top faces +Y.
    b.face({o0, w0, w1, o1});
    b.face({o1, w1, w2, o2});
    b.face({o2, w2, w3, o3});
    b.face({o3, w3, w0, o0});
    b.face({w0, t0, t1, w1});
    b.face({w1, t1, t2, w2});
    b.face({w2, t2, t3, w3});
    b.face({w3, t3, t0, w0});
    b.face({t0, t3, t2, t1});
}

/** A low dome (rivet, cap) of radius r and height h on a plate at y. */
void dome(Builder& b, dvec2 c, double r, double h, double y, int around = 16, int rings = 4)
{
    std::vector<std::vector<geo::Index>> ring(static_cast<size_t>(rings));
    for (int k = 0; k < rings; ++k)
    {
        const double t = double(k) / rings * std::numbers::pi * 0.5;
        const double rr = r * std::cos(t), yy = y + h * std::sin(t);
        for (int a = 0; a < around; ++a)
        {
            const double phi = 2.0 * std::numbers::pi * a / around;
            ring[size_t(k)].push_back(b.point({c.x + rr * std::cos(phi), yy, c.y + rr * std::sin(phi)}));
        }
    }
    const geo::Index apex = b.point({c.x, y + h, c.y});
    for (int k = 0; k + 1 < rings; ++k)
        for (int a = 0; a < around; ++a)
        {
            const int n = (a + 1) % around;
            b.face({ring[size_t(k)][size_t(a)], ring[size_t(k + 1)][size_t(a)], ring[size_t(k + 1)][size_t(n)], ring[size_t(k)][size_t(n)]});
        }
    for (int a = 0; a < around; ++a)
        b.face({ring.back()[size_t(a)], apex, ring.back()[size_t((a + 1) % around)]});
}

/** A hex bolt head with a chamfered top. */
void bolt(Builder& b, dvec2 c, double r, double h, double y)
{
    std::vector<geo::Index> base, wall, top;
    for (int a = 0; a < 6; ++a)
    {
        const double phi = std::numbers::pi / 3.0 * a + std::numbers::pi / 6.0;
        const dvec2 d(std::cos(phi), std::sin(phi));
        base.push_back(b.point({c.x + r * d.x, y, c.y + r * d.y}));
        wall.push_back(b.point({c.x + r * d.x, y + h * 0.7, c.y + r * d.y}));
        top.push_back(b.point({c.x + r * 0.7 * d.x, y + h, c.y + r * 0.7 * d.y}));
    }
    for (int a = 0; a < 6; ++a)
    {
        const int n = (a + 1) % 6;
        b.face({base[size_t(a)], wall[size_t(a)], wall[size_t(n)], base[size_t(n)]});
        b.face({wall[size_t(a)], top[size_t(a)], top[size_t(n)], wall[size_t(n)]});
    }
    b.face(std::vector<geo::Index>(top.rbegin(), top.rend()));
}

/** Sweeps a cross-section (z across the strip, height) along X from x0 to x1. */
void sweepX(Builder& b, const std::vector<dvec2>& section, double x0, double x1)
{
    std::vector<geo::Index> a, c;
    for (const dvec2& s : section)
    {
        a.push_back(b.point({x0, s.y, s.x}));
        c.push_back(b.point({x1, s.y, s.x}));
    }
    for (size_t i = 0; i + 1 < section.size(); ++i)
        b.face({a[i], a[i + 1], c[i + 1], c[i]});
}

/** Cross-section of a strip from z0 to z1: the edge profile in from both borders, the plateau between, with its features. */
std::vector<dvec2> stripSection(const Trim& t, double cmPerPx, double z0, double z1)
{
    const double hs = z1 - z0;
    const Profile& p = t.profile;
    if (p.style == Style::HalfPipe)
    {
        std::vector<dvec2> s;
        const int n = 16;
        for (int k = 0; k <= n; ++k)
        {
            const double a = std::numbers::pi * (1.0 - double(k) / n);
            s.emplace_back(z0 + hs * 0.5 * (1.0 + std::cos(a)), hs * 0.3 * std::sin(a));
        }
        return s;
    }
    const Edge e = edgeProfile(p, cmPerPx, hs * 0.4);
    std::vector<dvec2> s;
    for (const dvec2& q : e)
        s.emplace_back(z0 + q.x, q.y);
    const double h = edgeHeight(e);
    const double pz0 = z0 + edgeDepth(e), pz1 = z1 - edgeDepth(e);
    const double plateau = pz1 - pz0;
    if (p.style == Style::Groove && plateau > 0.0)
    {
        const double gw = std::min(plateau * 0.3, 2.0 * p.bevelPx * cmPerPx), zc = (pz0 + pz1) * 0.5;
        s.emplace_back(zc - gw * 0.5, h);
        s.emplace_back(zc, h - gw * 0.5);
        s.emplace_back(zc + gw * 0.5, h);
    }
    else if (p.style == Style::Ribs && plateau > 0.0)
    {
        const double amp = p.detailPx * cmPerPx * 0.5;
        const int count = std::max(1, int(std::lround(plateau / std::max(p.spacingPx * cmPerPx, 1e-6))));
        const int samples = count * 8;
        for (int k = 1; k < samples; ++k)
        {
            const double u = double(k) / samples;
            s.emplace_back(pz0 + plateau * u, h + amp * 0.5 * (1.0 - std::cos(2.0 * std::numbers::pi * u * count)));
        }
    }
    for (auto it = e.rbegin(); it != e.rend(); ++it)
        s.emplace_back(z1 - it->x, it->y);
    return s;
}

/** Details that sit on a strip's plateau, repeated along X so they tile. */
void stripDetails(Builder& b, const Trim& t, double cmPerPx, double z0, double z1, double period)
{
    const Profile& p = t.profile;
    const Edge e = edgeProfile(p, cmPerPx, (z1 - z0) * 0.4);
    const double h = edgeHeight(e), pz0 = z0 + edgeDepth(e), pz1 = z1 - edgeDepth(e), zc = (z0 + z1) * 0.5;
    const double plateau = pz1 - pz0;
    const double detail = std::min(p.detailPx * cmPerPx, plateau * 0.8);
    const double spacing = p.spacingPx * cmPerPx;
    if (plateau <= 0.0 || detail <= 0.0)
        return;
    switch (p.style)
    {
    case Style::Rivets:
        for (double x : periodic(period, spacing))
            dome(b, {x, zc}, detail * 0.5, detail * 0.3, h);
        break;
    case Style::Bolts:
        for (double x : periodic(period, spacing))
            bolt(b, {x, zc}, detail * 0.6, detail * 0.45, h);
        break;
    case Style::Vents:
        for (double x : periodic(period, spacing))
            frustum(b, {x - detail * 0.5, pz0 + plateau * 0.12}, {x + detail * 0.5, pz1 - plateau * 0.12}, h, detail * 0.35, detail * 0.3);
        break;
    case Style::Tread:
    {
        // Diamond plate: short bars leaning alternately, approximated by offset frusta.
        const double len = detail * 2.5, wid = detail * 0.6;
        int k = 0;
        for (double x : periodic(period, spacing))
        {
            const double dz = (k++ % 2 ? 1.0 : -1.0) * len * 0.15;
            frustum(b, {x - len * 0.5, zc - wid * 0.5 + dz}, {x + len * 0.5, zc + wid * 0.5 + dz}, h, wid * 0.4, wid * 0.3);
        }
        break;
    }
    default: break;
    }
}

/** Planks of varied length end to end along a strip, each its own bevelled board; the pattern repeats every period so it tiles. */
void planks(Builder& b, const Trim& t, double cmPerPx, double z0, double z1, double period)
{
    const Profile& p = t.profile;
    const double bevel = p.bevelPx * cmPerPx;
    const double target = std::max(p.spacingPx * cmPerPx, bevel * 4.0);
    std::vector<double> lengths;
    double sum = 0.0;
    for (uint64_t k = 0; sum < period - target * 0.5; ++k)
    {
        const double l = target * (0.6 + 0.8 * uv::hotspotRandom(p.seed, uint64_t(t.min.y), k));
        lengths.push_back(l);
        sum += l;
    }
    // Stretched so the run closes exactly on the period.
    for (double& l : lengths)
        l *= period / sum;
    const double gap = bevel * 0.25;
    for (int rep = -1; rep <= 1; ++rep)
    {
        double x = rep * period;
        for (size_t k = 0; k < lengths.size(); ++k)
        {
            const double jitter = bevel * 0.4 * uv::hotspotRandom(p.seed, uint64_t(t.min.y) + 7, k);
            frustum(b, {x + gap, z0 + gap}, {x + lengths[k] - gap, z1 - gap}, 0.0, bevel + jitter, bevel, p.angle);
            x += lengths[k];
        }
    }
}

/** Height over a trim at u along it and v across it from its top border (sheet z), cm; both from the trim's corner. */
using HeightFn = std::function<double(double u, double v)>;

/** A relief sampled on a grid, for styles whose shape isn't a swept section: carvings, borders, holes. */
struct Relief
{
    HeightFn height;
    /** False when the height only changes across the trim, so a few columns along it are enough. */
    bool alongVaries = true;
};

double posMod(double x, double m) { return x - std::floor(x / m) * m; }
double ramp(double d, double width) { return std::clamp(d / std::max(width, 1e-9), 0.0, 1.0); }
/** A repeat of about `want` that fits a whole number of times in `length`. */
double fitRepeat(double length, double want)
{
    const double n = std::max(1.0, std::round(length / std::max(want, 1e-9)));
    return length / n;
}

double segmentDistance(dvec2 p, dvec2 a, dvec2 b)
{
    const dvec2 ab = b - a;
    const double t = std::clamp(glm::dot(p - a, ab) / std::max(glm::dot(ab, ab), 1e-18), 0.0, 1.0);
    return glm::length(p - (a + ab * t));
}

/** Smooth 1D noise in [0, 1] repeating every `period`, `cells` lattice points per period. */
double periodicNoise(double u, double period, int cells, uint32_t seed, uint64_t channel)
{
    const double x = posMod(u, period) / period * cells;
    const int i = int(std::floor(x));
    const double f = x - i, s = f * f * (3.0 - 2.0 * f);
    const double a = uv::hotspotRandom(seed, channel, uint64_t(i % cells)), c = uv::hotspotRandom(seed, channel, uint64_t((i + 1) % cells));
    return a + (c - a) * s;
}

/** A Greek key repeat, 6 x 6 units, row 0 at the bottom: one line along the bottom feeding a spiral. */
bool meanderCell(int col, int row)
{
    static constexpr const char* kKey[6] = {
        "XXXXX.", // row 5, top
        "X...X.", //
        "X.X.X.", //
        "X.XXX.", //
        "X.....", //
        "XXXXXX", // row 0, bottom: joins the next repeat
    };
    return kKey[5 - row][col] == 'X';
}

/** Up to five strokes between nodes of a 3 x 4 grid, seeded per glyph: reads as lettering without spelling anything. */
std::vector<std::pair<dvec2, dvec2>> glyphStrokes(uint32_t seed, uint64_t glyph)
{
    std::vector<std::pair<dvec2, dvec2>> out;
    const int count = 2 + int(uv::hotspotRandom(seed, glyph, 99) * 3.0);
    for (int k = 0; k < count; ++k)
    {
        const int x = int(uv::hotspotRandom(seed, glyph, uint64_t(k * 4)) * 3.0), y = int(uv::hotspotRandom(seed, glyph, uint64_t(k * 4 + 1)) * 4.0);
        // Horizontal, vertical or diagonal, one or two grid steps long, kept on the grid.
        const int dir = int(uv::hotspotRandom(seed, glyph, uint64_t(k * 4 + 2)) * 4.0), len = 1 + int(uv::hotspotRandom(seed, glyph, uint64_t(k * 4 + 3)) * 2.0);
        const glm::ivec2 step = std::array{glm::ivec2(1, 0), glm::ivec2(0, 1), glm::ivec2(1, 1), glm::ivec2(1, -1)}[size_t(std::min(dir, 3))];
        const glm::ivec2 a(x, y), c = glm::clamp(a + step * len, glm::ivec2(0), glm::ivec2(2, 3));
        if (c != a)
            out.push_back({dvec2(a) / dvec2(2.0, 3.0), dvec2(c) / dvec2(2.0, 3.0)});
    }
    return out;
}

/**
 * The relief of the styles built on a grid, over a trim `along` cm long (one tiling period for strips) and `across` cm
 * across. tiles: only the borders across the trim bevel (a strip), else all four (a rectangle). Null for swept styles.
 */
std::optional<Relief> styleRelief(const Trim& t, double cmPerPx, double along, double across, bool tiles)
{
    const Profile& p = t.profile;
    const double bevel = std::min(p.bevelPx * cmPerPx, across * 0.3);
    const double rise = bevel * std::tan(glm::radians(p.angle));
    const double detail = std::max(p.detailPx * cmPerPx, cmPerPx);
    const double spacing = std::max(p.spacingPx * cmPerPx, cmPerPx * 2.0);
    const double inner = across - 2.0 * bevel;
    auto border = [=](double u, double v) {
        double e = std::min(v, across - v);
        if (!tiles)
            e = std::min({e, u, along - u});
        return e;
    };
    auto plate = [=](double u, double v) { return rise * ramp(border(u, v), bevel); };
    // Up the trim: 0 at its bottom border, 1 at its top (the texture's V runs opposite to the sheet's z).
    auto up = [=](double v) { return std::clamp((across - v - bevel) / std::max(inner, 1e-9), 0.0, 1.0); };
    const double relief = std::max(inner * 0.3, rise);

    switch (p.style)
    {
    case Style::Channel:
        return Relief{[=](double u, double v) {
                          const double w = inner * 0.5, dv = v - across * 0.5;
                          const double cut = std::abs(dv) < w * 0.5 ? std::sqrt(1.0 - std::pow(dv / (w * 0.5), 2.0)) * w * 0.35 : 0.0;
                          return plate(u, v) - cut * ramp(border(u, v) - bevel, bevel);
                      },
                      !tiles};
    case Style::Torus:
        return Relief{[=](double u, double v) {
                          const double r = std::max(inner * 0.5 - bevel, cmPerPx), dv = v - across * 0.5;
                          const double round = std::abs(dv) < r ? std::sqrt(1.0 - std::pow(dv / r, 2.0)) * r * 0.6 : 0.0;
                          return plate(u, v) + round * (tiles ? 1.0 : ramp(border(u, v), bevel * 2.0));
                      },
                      !tiles};
    case Style::Cavetto:
        return Relief{[=](double u, double v) {
                          const double s = up(v);
                          return plate(u, v) + relief * (1.0 - std::sqrt(std::max(0.0, 1.0 - s * s))) * ramp(border(u, v), bevel);
                      },
                      !tiles};
    case Style::Ogee:
        return Relief{[=](double u, double v) {
                          const double s = up(v);
                          const double h = s < 0.5 ? 0.5 * (1.0 - std::sqrt(std::max(0.0, 1.0 - std::pow(s * 2.0, 2.0))))
                                                   : 0.5 + 0.5 * std::sqrt(std::max(0.0, 1.0 - std::pow(2.0 - s * 2.0, 2.0)));
                          return plate(u, v) + relief * h * ramp(border(u, v), bevel);
                      },
                      !tiles};
    case Style::Steps:
        return Relief{[=](double u, double v) {
                          const int n = std::max(3, p.segments);
                          const double s = up(v) * n;
                          // Each riser bevels over a fifth of a step so the treads read as separate.
                          const double k = std::floor(s), f = s - k;
                          const double h = (k + std::clamp((f - 0.8) / 0.2, 0.0, 1.0)) / n;
                          return plate(u, v) + relief * std::min(h, 1.0) * ramp(border(u, v), bevel);
                      },
                      !tiles};
    case Style::Rail:
        return Relief{[=](double u, double v) {
                          const double r = std::min(across * 0.3, inner * 0.5), dv = v - across * 0.5;
                          const double tube = std::abs(dv) < r ? r + std::sqrt(1.0 - std::pow(dv / r, 2.0)) * r : 0.0;
                          return std::max(plate(u, v) * 0.25, tube * (tiles ? 1.0 : ramp(border(u, v), bevel)));
                      },
                      !tiles};
    case Style::Fluting:
    {
        const double rep = fitRepeat(along, spacing);
        return Relief{[=](double u, double v) {
            const double w = rep * 0.7, du = posMod(u, rep) - rep * 0.5;
            const double depth = std::min(detail, w * 0.5);
            double cut = std::abs(du) < w * 0.5 ? std::sqrt(1.0 - std::pow(du / (w * 0.5), 2.0)) : 0.0;
            // Rounded flute ends inside the plate's border.
            cut *= std::sqrt(ramp(border(u, v) - bevel * 1.5, w * 0.5));
            return plate(u, v) - depth * cut;
        }};
    }
    case Style::Dentils:
    {
        const double rep = fitRepeat(along, spacing);
        return Relief{[=](double u, double v) {
            const double gap = rep * 0.35, du = posMod(u, rep);
            const double inBlock = std::min({du - gap * 0.5, rep - gap * 0.5 - du, v - bevel * 2.0, across - bevel * 2.0 - v});
            return plate(u, v) + (inBlock > 0.0 ? detail * ramp(inBlock, detail * 0.4) : 0.0);
        }};
    }
    case Style::Meander:
    {
        const double unitV = inner * 0.8 / 6.0;
        const double rep = fitRepeat(along, unitV * 6.0);
        const double v0 = (across - unitV * 6.0) * 0.5;
        return Relief{[=](double u, double v) {
            const double lu = posMod(u, rep) / rep * 6.0, lv = (across - v - v0) / unitV;
            if (lv < 0.0 || lv >= 6.0 || (!tiles && (u < bevel * 2.0 || u > along - bevel * 2.0)))
                return plate(u, v);
            return plate(u, v) + (meanderCell(std::clamp(int(lu), 0, 5), std::clamp(int(lv), 0, 5)) ? std::min(detail, unitV) * 0.6 : 0.0);
        }};
    }
    case Style::Glyphs:
    {
        const double cell = fitRepeat(along, inner * 0.75);
        const int count = std::max(1, int(std::lround(along / cell)));
        std::vector<std::vector<std::pair<dvec2, dvec2>>> glyphs;
        for (int k = 0; k < count; ++k)
            glyphs.push_back(glyphStrokes(p.seed, uint64_t(k)));
        const double stroke = std::max(inner * 0.09, cmPerPx * 2.0);
        return Relief{[=](double u, double v) {
            const double h = plate(u, v);
            const int k = int(std::floor(posMod(u, along) / cell)) % count;
            // The glyph's box: 70% of the cell across, 70% of the plate up, lettering origin at its bottom left.
            const dvec2 box(cell * 0.7, inner * 0.7);
            const dvec2 at((posMod(u, cell) - cell * 0.15) / box.x, (across - v - bevel - inner * 0.15) / box.y);
            double d = 1e300;
            for (const auto& [a, c] : glyphs[size_t(k)])
                d = std::min(d, segmentDistance(at * box, a * box, c * box));
            // A chisel cut: V-shaped, as deep as it is wide.
            return d < stroke * 0.5 ? h - (stroke * 0.5 - d) : h;
        }};
    }
    case Style::Chipped:
    {
        const int cells = std::max(2, int(std::lround(along / std::max(spacing, bevel * 2.0))));
        return Relief{[=](double u, double v) {
            const double wear0 = bevel * (0.5 + 2.5 * periodicNoise(u, along, cells, p.seed, 11));
            const double wear1 = bevel * (0.5 + 2.5 * periodicNoise(u, along, cells, p.seed, 12));
            const double side = tiles ? 1e300 : std::min(u, along - u);
            const double e = std::min({v / wear0, (across - v) / wear1, side / bevel});
            double h = rise * 1.5 * std::pow(std::clamp(e, 0.0, 1.0), 0.6);
            // Pits: one bowl in some cells of a coarse grid, placed and sized by the seed.
            const double pit = fitRepeat(along, spacing * 1.5);
            const int pi = int(std::floor(posMod(u, along) / pit)), pj = int(std::floor(v / pit));
            const uint64_t key = uint64_t(pi) * 977 + uint64_t(std::max(pj, 0));
            if (uv::hotspotRandom(p.seed, key, 21) < 0.35)
            {
                const dvec2 c((pi + 0.2 + 0.6 * uv::hotspotRandom(p.seed, key, 22)) * pit, (pj + 0.2 + 0.6 * uv::hotspotRandom(p.seed, key, 23)) * pit);
                const double r = pit * (0.1 + 0.2 * uv::hotspotRandom(p.seed, key, 24));
                const double d = glm::length(dvec2(posMod(u, along), v) - c);
                if (d < r)
                    h -= r * 0.4 * (1.0 - (d * d) / (r * r));
            }
            return h;
        }};
    }
    case Style::Grating:
    {
        const double rep = fitRepeat(along, spacing);
        return Relief{[=](double u, double v) {
            const double h = plate(u, v);
            if (border(u, v) < bevel * 1.5)
                return h;
            const double du = std::abs(posMod(u, rep) - rep * 0.5), dv = std::abs(v - across * 0.5);
            const double bar = std::max(detail * 0.5, cmPerPx);
            if (du > rep * 0.5 - bar * 0.5 || dv < bar * 0.5)
                return h;
            return h - rise - detail * 2.0;
        }};
    }
    case Style::Conduit:
    {
        const double rep = fitRepeat(along, spacing);
        return Relief{[=](double u, double v) {
            const double r = across / 8.0;
            double h = 0.0;
            for (double c : {across * 0.25, across * 0.5, across * 0.75})
                if (const double dv = v - c; std::abs(dv) < r)
                    h = std::max(h, r + std::sqrt(1.0 - std::pow(dv / r, 2.0)) * r);
            // Clips: flat straps over all three, one per repeat.
            const double du = std::abs(posMod(u, rep) - rep * 0.5), clip = std::max(detail, cmPerPx * 2.0);
            if (du < clip * 0.5 && v > across * 0.1 && v < across * 0.9)
                h = std::max(h, r * 2.0 + detail * 0.3);
            return h;
        }};
    }
    case Style::Hazard:
    {
        const double rep = fitRepeat(along, std::max(inner, cmPerPx * 4.0) * 2.0);
        return Relief{[=](double u, double v) {
            const double stripe = posMod(u + (across - v), rep) < rep * 0.5 ? 1.0 : 0.0;
            return plate(u, v) + stripe * rise * 0.15 * ramp(border(u, v) - bevel, bevel * 0.5);
        }};
    }
    default: return std::nullopt;
    }
}

/** Samples a relief on a grid over x0..x1 (u from xu0) and z0..z1, facing +Y. */
void reliefGrid(Builder& b, const Relief& r, double x0, double x1, double xu0, double z0, double z1, double cmPerPx, double cellPx)
{
    double cell = std::max(cmPerPx, cellPx * cmPerPx);
    // Keeps a big sheet's relief to a few hundred thousand quads.
    while (((x1 - x0) / cell) * ((z1 - z0) / cell) > 600000.0 && r.alongVaries)
        cell *= 1.25;
    const int nz = std::max(2, int(std::ceil((z1 - z0) / cell)));
    const int nx = r.alongVaries ? std::max(2, int(std::ceil((x1 - x0) / cell))) : 3;
    std::vector<geo::Index> ids(size_t(nx + 1) * size_t(nz + 1));
    for (int i = 0; i <= nx; ++i)
        for (int j = 0; j <= nz; ++j)
        {
            const double x = x0 + (x1 - x0) * i / nx, z = z0 + (z1 - z0) * j / nz;
            ids[size_t(i) * size_t(nz + 1) + size_t(j)] = b.point({x, r.height(x - xu0, z - z0), z});
        }
    auto at = [&](int i, int j) { return ids[size_t(i) * size_t(nz + 1) + size_t(j)]; };
    for (int i = 0; i < nx; ++i)
        for (int j = 0; j < nz; ++j)
            b.face({at(i, j), at(i, j + 1), at(i + 1, j + 1), at(i + 1, j)});
}

/** The grid cell a style's relief needs, px: fine enough for its smallest feature. */
double reliefCellPx(const Profile& p) { return std::clamp(std::min(p.bevelPx, p.detailPx) * 0.34, 1.0, 6.0); }

/** A rectangle trim: rings of its edge profile from the border in, the plateau on top, and its details. */
void rectTrim(Builder& b, const Trim& t, const Sheet& sheet, double cmPerPx)
{
    const dvec3 lo3 = sheetPoint(sheet, dvec2(t.min)), hi3 = sheetPoint(sheet, dvec2(t.max));
    const dvec2 lo(lo3.x, lo3.z), hi(hi3.x, hi3.z);
    const dvec2 size = hi - lo;
    const Profile& p = t.profile;
    if (const std::optional<Relief> r = styleRelief(t, cmPerPx, size.x, size.y, false))
    {
        reliefGrid(b, *r, lo.x, hi.x, lo.x, lo.y, hi.y, cmPerPx, reliefCellPx(p));
        return;
    }
    if (t.kind == uv::TrimKind::Radial || p.style == Style::HalfPipe)
    {
        // Base plate under the dome so the corners bake flat.
        const geo::Index a = b.point({lo.x, 0.0, lo.y}), c = b.point({hi.x, 0.0, lo.y}), d = b.point({hi.x, 0.0, hi.y}), e = b.point({lo.x, 0.0, hi.y});
        b.face({a, e, d, c});
        const double r = std::min(size.x, size.y) * 0.45;
        dome(b, (lo + hi) * 0.5, r, r * 0.35, 0.0, 32, 8);
        return;
    }
    const Edge e = edgeProfile(p, cmPerPx, std::min(size.x, size.y) * 0.4);
    std::vector<std::array<geo::Index, 4>> rings;
    for (const dvec2& q : e)
    {
        const dvec2 a = lo + dvec2(q.x), c = hi - dvec2(q.x);
        rings.push_back({b.point({a.x, q.y, a.y}), b.point({c.x, q.y, a.y}), b.point({c.x, q.y, c.y}), b.point({a.x, q.y, c.y})});
    }
    for (size_t k = 0; k + 1 < rings.size(); ++k)
        for (int s = 0; s < 4; ++s)
        {
            const int n = (s + 1) % 4;
            b.face({rings[k][size_t(s)], rings[k + 1][size_t(s)], rings[k + 1][size_t(n)], rings[k][size_t(n)]});
        }
    const auto& in = rings.back();
    b.face({in[0], in[3], in[2], in[1]});

    const double h = edgeHeight(e), depth = edgeDepth(e);
    const dvec2 plo = lo + dvec2(depth), phi = hi - dvec2(depth);
    const double detail = p.detailPx * cmPerPx;
    switch (p.style)
    {
    case Style::Bolts:
    {
        const double inset = detail * 1.2;
        for (dvec2 c : {dvec2(plo.x + inset, plo.y + inset), dvec2(phi.x - inset, plo.y + inset), dvec2(phi.x - inset, phi.y - inset),
                        dvec2(plo.x + inset, phi.y - inset)})
            bolt(b, c, detail * 0.6, detail * 0.45, h);
        break;
    }
    case Style::Rivets:
    {
        const double inset = detail * 1.2, spacing = std::max(p.spacingPx * cmPerPx, detail * 2.0);
        for (double x = plo.x + inset; x <= phi.x - inset + 1e-9; x += spacing)
        {
            dome(b, {x, plo.y + inset}, detail * 0.5, detail * 0.3, h);
            dome(b, {x, phi.y - inset}, detail * 0.5, detail * 0.3, h);
        }
        break;
    }
    case Style::Vents:
    {
        const double spacing = std::max(p.spacingPx * cmPerPx, detail * 1.5);
        const bool alongX = phi.x - plo.x >= phi.y - plo.y;
        const double len = alongX ? phi.x - plo.x : phi.y - plo.y;
        const int n = std::max(1, int((len - detail) / spacing));
        for (int k = 0; k < n; ++k)
        {
            const double at = (alongX ? plo.x : plo.y) + (len - (n - 1) * spacing) * 0.5 + k * spacing;
            const dvec2 a = alongX ? dvec2(at - detail * 0.5, plo.y + detail) : dvec2(plo.x + detail, at - detail * 0.5);
            const dvec2 c = alongX ? dvec2(at + detail * 0.5, phi.y - detail) : dvec2(phi.x - detail, at + detail * 0.5);
            frustum(b, a, c, h, detail * 0.35, detail * 0.3);
        }
        break;
    }
    default: break;
    }
}
} // namespace

glm::dvec3 sheetPoint(const Sheet& sheet, glm::dvec2 px, double height)
{
    const double cmPerPx = 100.0 / sheet.pxPerM;
    return {px.x * cmPerPx, height, px.y * cmPerPx};
}

geo::Mesh lowPlane(const Sheet& sheet)
{
    const dvec3 ext = sheetPoint(sheet, dvec2(sheet.width, sheet.height));
    const dvec3 pts[] = {{0.0, 0.0, 0.0}, {0.0, 0.0, ext.z}, {ext.x, 0.0, ext.z}, {ext.x, 0.0, 0.0}};
    const geo::Index sizes[] = {4};
    const geo::Index idx[] = {0, 1, 2, 3};
    geo::Mesh m = geo::Mesh::fromPolygons(pts, sizes, idx);
    std::span<glm::vec2> uv = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
    m.forEachFaceCorner(0, [&](geo::Index c) {
        const dvec3& p = m.position(m.cornerPoint(c));
        uv[c] = glm::vec2(float(p.x / ext.x), float(1.0 - p.z / ext.z));
    });
    return m;
}

Generated generate(const Sheet& sheet, const GenerateOptions& opts)
{
    LILA_ZONE();

    const double cmPerPx = 100.0 / sheet.pxPerM;
    const double period = sheet.width * cmPerPx;
    Builder b;
    for (size_t i = 0; i < sheet.trims.size(); ++i)
    {
        if (!opts.only.empty() && std::ranges::find(opts.only, i) == opts.only.end())
            continue;
        const Trim& t = sheet.trims[i];
        b.color = glm::vec4(trimColor(i), 1.0f);
        const bool flat = t.kind == uv::TrimKind::Flat || t.profile.style == Style::Flat;
        if (t.kind == uv::TrimKind::StripU && !flat)
        {
            const double z0 = t.min.y * cmPerPx, z1 = t.max.y * cmPerPx;
            const double x0 = opts.tile ? -period : 0.0, x1 = opts.tile ? 2.0 * period : period;
            if (const std::optional<Relief> r = styleRelief(t, cmPerPx, period, z1 - z0, true))
                reliefGrid(b, *r, x0, x1, 0.0, z0, z1, cmPerPx, reliefCellPx(t.profile));
            else if (t.profile.style == Style::Planks)
            {
                // The plank boards stand on a base so their seams read as gaps, not holes.
                sweepX(b, {{z0, 0.0}, {z1, 0.0}}, x0, x1);
                planks(b, t, cmPerPx, z0, z1, period);
            }
            else
            {
                sweepX(b, stripSection(t, cmPerPx, z0, z1), x0, x1);
                stripDetails(b, t, cmPerPx, z0, z1, period);
            }
        }
        else if (flat)
        {
            const dvec3 lo = sheetPoint(sheet, dvec2(t.min)), hi = sheetPoint(sheet, dvec2(t.max));
            const geo::Index a = b.point({lo.x, 0.0, lo.z}), c = b.point({hi.x, 0.0, lo.z}), d = b.point({hi.x, 0.0, hi.z}), e = b.point({lo.x, 0.0, hi.z});
            b.face({a, e, d, c});
        }
        else
            rectTrim(b, t, sheet, cmPerPx);
    }
    Generated g;
    g.low = lowPlane(sheet);
    g.high = b.build();
    g.above = b.top;
    g.below = -b.bottom;
    return g;
}
} // namespace trims
