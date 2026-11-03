// Blockout slicing into wall footprints, stone contacts for destruction links, and contact sheets of wedged variants.

#include "MasonryTools.h"

#include <lila/ImageIO.h>
#include <lila/Profile.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>

namespace masonry
{
namespace
{
using Loop = std::vector<glm::dvec3>;

double area(const Loop& l)
{
    double a = 0.0;
    for (size_t i = 0; i < l.size(); ++i)
    {
        const glm::dvec3& p = l[i];
        const glm::dvec3& q = l[(i + 1) % l.size()];
        a += p.x * q.z - q.x * p.z;
    }
    return a * 0.5;
}

bool contains(const Loop& poly, const glm::dvec3& q)
{
    bool in = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
        if ((poly[i].z > q.z) != (poly[j].z > q.z) && q.x < (poly[j].x - poly[i].x) * (q.z - poly[i].z) / (poly[j].z - poly[i].z) + poly[i].x)
            in = !in;
    return in;
}

double distanceToLoop(const Loop& l, const glm::dvec3& q)
{
    double best = 1e300;
    for (size_t i = 0; i < l.size(); ++i)
    {
        const glm::dvec2 a(l[i].x, l[i].z), b(l[(i + 1) % l.size()].x, l[(i + 1) % l.size()].z), p(q.x, q.z);
        const glm::dvec2 ab = b - a;
        const double t = glm::dot(ab, ab) > 1e-12 ? std::clamp(glm::dot(p - a, ab) / glm::dot(ab, ab), 0.0, 1.0) : 0.0;
        best = std::min(best, glm::length(p - (a + ab * t)));
    }
    return best;
}

/** The outlines where the plane y cuts the mesh, as closed loops. */
std::vector<Loop> slice(const geo::Mesh& m, double y)
{
    std::vector<std::pair<glm::dvec3, glm::dvec3>> segs;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        const std::vector<geo::Index> pts = m.facePoints(f);
        for (size_t k = 1; k + 1 < pts.size(); ++k)
        {
            const glm::dvec3 t[3] = {m.position(pts[0]), m.position(pts[k]), m.position(pts[k + 1])};
            std::vector<glm::dvec3> hits;
            for (int e = 0; e < 3; ++e)
            {
                const glm::dvec3 &a = t[e], &b = t[(e + 1) % 3];
                const double da = a.y - y, db = b.y - y;
                if ((da < 0.0) != (db < 0.0))
                    hits.push_back(a + (b - a) * (da / (da - db)));
            }
            if (hits.size() == 2)
                segs.push_back({hits[0], hits[1]});
        }
    }
    auto key = [](const glm::dvec3& p) { return std::make_pair(int64_t(std::llround(p.x * 100.0)), int64_t(std::llround(p.z * 100.0))); };
    std::map<std::pair<int64_t, int64_t>, std::vector<size_t>> at;
    for (size_t k = 0; k < segs.size(); ++k)
    {
        at[key(segs[k].first)].push_back(k);
        at[key(segs[k].second)].push_back(k);
    }
    std::vector<char> used(segs.size(), 0);
    std::vector<Loop> loops;
    for (size_t s = 0; s < segs.size(); ++s)
    {
        if (used[s])
            continue;
        Loop l;
        size_t cur = s;
        glm::dvec3 end = segs[s].second;
        l.push_back(segs[s].first);
        used[s] = 1;
        for (size_t guard = 0; guard < segs.size(); ++guard)
        {
            l.push_back(end);
            size_t next = SIZE_MAX;
            for (size_t o : at[key(end)])
                if (!used[o])
                    next = o;
            if (next == SIZE_MAX)
                break;
            used[next] = 1;
            end = key(segs[next].first) == key(end) ? segs[next].second : segs[next].first;
            cur = next;
        }
        (void)cur;
        if (l.size() >= 4 && key(l.front()) == key(l.back()))
            l.pop_back();
        // Drop points on a straight run and points too close to the last.
        Loop clean;
        for (size_t i = 0; i < l.size(); ++i)
        {
            const glm::dvec3 prev = clean.empty() ? l[(i + l.size() - 1) % l.size()] : clean.back();
            const glm::dvec3 next = l[(i + 1) % l.size()];
            const glm::dvec3 d0 = l[i] - prev, d1 = next - l[i];
            if (glm::length(d0) < 1.0)
                continue;
            const double turn = glm::length(d0) > 1e-9 && glm::length(d1) > 1e-9 ? glm::dot(glm::normalize(d0), glm::normalize(d1)) : 1.0;
            if (turn > std::cos(glm::radians(2.0)))
                continue;
            clean.push_back(l[i]);
        }
        if (clean.size() >= 3)
            loops.push_back(std::move(clean));
    }
    return loops;
}

/** A loop moved inward (toward its inside) by dist, mitred at each corner. */
Loop inset(const Loop& l, double dist)
{
    const double sign = area(l) > 0.0 ? 1.0 : -1.0;
    const size_t n = l.size();
    Loop out;
    auto dir = [&](size_t i) {
        glm::dvec3 d = l[(i + 1) % n] - l[i % n];
        d.y = 0.0;
        return glm::length(d) > 1e-12 ? glm::normalize(d) : glm::dvec3(1, 0, 0);
    };
    for (size_t i = 0; i < n; ++i)
        out.push_back(l[i] + miterOffset(dir((i + n - 1) % n), dir(i), sign * dist));
    return out;
}

bool sameLoop(const Loop& a, const Loop& b)
{
    if (a.size() != b.size())
        return false;
    for (const glm::dvec3& p : a)
        if (distanceToLoop(b, p) > 1.0)
            return false;
    return true;
}
} // namespace

std::vector<Footprint> footprintsFromBlockout(const geo::Mesh& mesh, double thickness, double band, double* thicknessUsed)
{
    LILA_ZONE();
    std::vector<Footprint> out;
    const geo::Bounds b = mesh.bounds();
    if (!b.valid() || b.max.y - b.min.y < 1.0)
        return out;
    const double height = b.max.y - b.min.y;
    const int bands = band > 0.0 ? std::max(1, int(std::lround(height / band))) : 8;
    const double step = height / bands;
    double usedT = 0.0;
    int usedCount = 0;
    struct Section
    {
        std::vector<Loop> lines;
        double y0, y1;
    };
    std::vector<Section> sections;
    for (int k = 0; k < bands; ++k)
    {
        const double y0 = b.min.y + k * step;
        std::vector<Loop> loops = slice(mesh, y0 + step * 0.5 + 1e-4);
        Section s{{}, y0, y0 + step};
        for (size_t i = 0; i < loops.size(); ++i)
        {
            // Outer outlines only: those no other loop contains.
            bool inner = false;
            for (size_t o = 0; o < loops.size() && !inner; ++o)
                inner = o != i && contains(loops[o], loops[i][0]);
            if (inner)
                continue;
            double t = thickness;
            for (size_t h = 0; h < loops.size(); ++h)
                if (h != i && contains(loops[i], loops[h][0]))
                {
                    double sum = 0.0;
                    for (const glm::dvec3& q : loops[h])
                        sum += distanceToLoop(loops[i], q);
                    t = sum / double(loops[h].size());
                }
            if (t <= 0.0)
            {
                // A solid blockout: walls a tenth of its narrower side thick, at least 20 cm.
                glm::dvec3 lo(1e300), hi(-1e300);
                for (const glm::dvec3& q : loops[i])
                {
                    lo = glm::min(lo, q);
                    hi = glm::max(hi, q);
                }
                t = std::max(20.0, std::min(hi.x - lo.x, hi.z - lo.z) * 0.1);
            }
            usedT += t;
            ++usedCount;
            s.lines.push_back(inset(loops[i], t * 0.5));
        }
        sections.push_back(std::move(s));
    }
    // Bands whose outlines match stack into one wall.
    for (size_t k = 0; k < sections.size();)
    {
        size_t e = k + 1;
        while (e < sections.size() && sections[e].lines.size() == sections[k].lines.size() &&
               std::equal(sections[k].lines.begin(), sections[k].lines.end(), sections[e].lines.begin(), sameLoop))
            ++e;
        for (const Loop& l : sections[k].lines)
        {
            Footprint f;
            f.closed = true;
            for (const glm::dvec3& q : l)
            {
                f.points.push_back({q.x, sections[k].y0, q.z});
                f.heights.push_back(sections[e - 1].y1 - sections[k].y0);
            }
            out.push_back(std::move(f));
        }
        k = e;
    }
    if (thicknessUsed)
        *thicknessUsed = usedCount > 0 ? usedT / usedCount : thickness;
    return out;
}

std::vector<StoneLink> stoneLinks(std::span<const Stone> stones, double pad)
{
    LILA_ZONE();
    std::vector<StoneLink> out;
    StoneGrid grid;
    double big = 10.0;
    for (const Stone& s : stones)
        big = std::max(big, std::max({s.size.x, s.size.y, s.size.z}));
    grid.cell = big;
    grid.build(stones, 0.0);
    for (size_t a = 0; a < stones.size(); ++a)
    {
        const Stone& s = stones[a];
        const glm::dvec3 ea = halfExtentAabb(s) + pad;
        if (s.grounded)
            out.push_back({int(a), -1, s.center - glm::dvec3(0, ea.y - pad, 0), s.size.x * s.size.z});
        for (int o : grid.around(s, pad))
        {
            if (size_t(o) <= a || !overlaps(s, stones[size_t(o)], -2.0 * pad))
                continue;
            const Stone& t = stones[size_t(o)];
            const glm::dvec3 eb = halfExtentAabb(t) + pad;
            const glm::dvec3 lo = glm::max(s.center - ea, t.center - eb), hi = glm::min(s.center + ea, t.center + eb);
            glm::dvec3 ov = glm::max(hi - lo, glm::dvec3(0.0));
            // The thinnest overlap is across the joint; the other two span the contact.
            int across = 0;
            for (int k = 1; k < 3; ++k)
                if (ov[k] < ov[across])
                    across = k;
            ov[across] = 1.0;
            out.push_back({int(a), o, (lo + hi) * 0.5, ov.x * ov.y * ov.z});
        }
    }
    return out;
}

bool writeContactSheet(std::span<const std::vector<Stone>> variants, const glm::dvec3& viewIn, int tile, const std::filesystem::path& path,
                       std::string* err)
{
    LILA_ZONE();
    if (variants.empty())
        return false;
    const int n = int(variants.size());
    const int cols = int(std::ceil(std::sqrt(double(n)))), rows = (n + cols - 1) / cols;
    const int size = std::clamp(tile, 64, 2048);
    const int w = cols * size, h = rows * size;
    std::vector<float> rgba(size_t(w) * size_t(h) * 4, 0.0f);
    for (size_t i = 0; i < rgba.size(); i += 4)
    {
        rgba[i] = 0.11f;
        rgba[i + 1] = 0.12f;
        rgba[i + 2] = 0.14f;
        rgba[i + 3] = 1.0f;
    }
    glm::dvec3 view = viewIn;
    view.y = 0.0;
    view = glm::length(view) > 1e-9 ? glm::normalize(view) : glm::dvec3(0, 0, -1);
    const glm::dvec3 right = glm::normalize(glm::cross(view, kUp)), up = kUp;
    // One scale for every tile, so variants compare.
    double ulo = 1e300, uhi = -1e300, vlo = 1e300, vhi = -1e300;
    for (const auto& stones : variants)
        for (const Stone& s : stones)
            for (const glm::dvec3& c : stoneCorners(s))
            {
                ulo = std::min(ulo, glm::dot(c, right));
                uhi = std::max(uhi, glm::dot(c, right));
                vlo = std::min(vlo, glm::dot(c, up));
                vhi = std::max(vhi, glm::dot(c, up));
            }
    const double span = std::max({uhi - ulo, vhi - vlo, 1.0}) * 1.06;
    const double scale = size / span;
    for (int v = 0; v < n; ++v)
    {
        const int ox = (v % cols) * size, oy = (v / cols) * size;
        std::vector<size_t> order(variants[size_t(v)].size());
        for (size_t k = 0; k < order.size(); ++k)
            order[k] = k;
        const auto& stones = variants[size_t(v)];
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return glm::dot(stones[a].center, view) > glm::dot(stones[b].center, view); });
        for (size_t k : order)
        {
            const Stone& s = stones[k];
            std::vector<glm::dvec2> p;
            for (const glm::dvec3& c : stoneCorners(s))
                p.push_back({(glm::dot(c, right) - (ulo + uhi) * 0.5) * scale + size * 0.5, size * 0.5 - (glm::dot(c, up) - (vlo + vhi) * 0.5) * scale});
            // Convex hull, then a scanline fill.
            std::sort(p.begin(), p.end(), [](const glm::dvec2& a, const glm::dvec2& b) { return a.x != b.x ? a.x < b.x : a.y < b.y; });
            std::vector<glm::dvec2> hull;
            auto cross = [](const glm::dvec2& o, const glm::dvec2& a, const glm::dvec2& b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); };
            for (int pass = 0; pass < 2; ++pass)
            {
                const size_t start = hull.size();
                for (size_t i = 0; i < p.size(); ++i)
                {
                    const glm::dvec2& q = pass == 0 ? p[i] : p[p.size() - 1 - i];
                    while (hull.size() >= start + 2 && cross(hull[hull.size() - 2], hull.back(), q) <= 0.0)
                        hull.pop_back();
                    hull.push_back(q);
                }
                hull.pop_back();
            }
            if (hull.size() < 3)
                continue;
            const float shade = 0.55f + 0.35f * s.random;
            glm::vec3 colour = s.role == Role::Rubble ? glm::vec3(0.45f, 0.42f, 0.38f) : glm::vec3(0.78f, 0.72f, 0.62f) * shade;
            colour = glm::mix(colour, glm::vec3(0.25f, 0.4f, 0.15f), s.moss * 0.8f);
            double ymin = 1e300, ymax = -1e300;
            for (const glm::dvec2& q : hull)
            {
                ymin = std::min(ymin, q.y);
                ymax = std::max(ymax, q.y);
            }
            for (int y = std::max(0, int(std::floor(ymin))); y <= std::min(size - 1, int(std::ceil(ymax))); ++y)
            {
                const double yc = y + 0.5;
                double xl = 1e300, xr = -1e300;
                for (size_t i = 0; i < hull.size(); ++i)
                {
                    const glm::dvec2 a = hull[i], b = hull[(i + 1) % hull.size()];
                    if ((a.y <= yc) == (b.y <= yc))
                        continue;
                    const double x = a.x + (yc - a.y) / (b.y - a.y) * (b.x - a.x);
                    xl = std::min(xl, x);
                    xr = std::max(xr, x);
                }
                for (int x = std::max(0, int(std::ceil(xl - 0.5))); x <= std::min(size - 1, int(std::floor(xr - 0.5))); ++x)
                {
                    float* px = &rgba[(size_t(oy + y) * size_t(w) + size_t(ox + x)) * 4];
                    px[0] = colour.r;
                    px[1] = colour.g;
                    px[2] = colour.b;
                }
            }
        }
    }
    try
    {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        lila::image::writePng(path, w, h, rgba, 8);
    }
    catch (const std::exception& e)
    {
        if (err)
            *err = e.what();
        return false;
    }
    return true;
}
} // namespace masonry
