// The high poly a trim sheet bakes from: every trim's profile swept across its rectangle (strips along U, past both
// sheet edges so the bake tiles), framed panels for rectangles, domes for round trims, and details instanced along them.

#include "Generator.h"

#include <lila/Profile.h>

#include <algorithm>
#include <cmath>
#include <numbers>

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

/** A rectangle trim: rings of its edge profile from the border in, the plateau on top, and its details. */
void rectTrim(Builder& b, const Trim& t, const Sheet& sheet, double cmPerPx)
{
    const dvec3 lo3 = sheetPoint(sheet, dvec2(t.min)), hi3 = sheetPoint(sheet, dvec2(t.max));
    const dvec2 lo(lo3.x, lo3.z), hi(hi3.x, hi3.z);
    const dvec2 size = hi - lo;
    const Profile& p = t.profile;
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
            if (t.profile.style == Style::Planks)
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
