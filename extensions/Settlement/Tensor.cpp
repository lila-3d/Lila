// Settlement: streets traced along a tensor field (Chen et al. 2008). Basis fields (grids, rings round a point, curves
// such as a coast or river, the ground's contours) blend into one direction field; major streets are evenly spaced
// streamlines of it (Jobard and Lefer 1997), minor streets fill between them along both directions.

#include "Generators.h"

#include <lila/Profile.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <unordered_map>

namespace settlement
{
namespace
{
double falloff(double d, double decay) { return decay > 0.0 ? std::exp(-(d / decay) * (d / decay)) : 1.0; }

/** The tensor of one direction, as (cos 2a, sin 2a) times weight. */
Vec2 tensorOf(Vec2 dir, double w)
{
    const double a = std::atan2(dir.y, dir.x);
    return Vec2(std::cos(2.0 * a), std::sin(2.0 * a)) * w;
}

/** The default field: the profile's grid, rings round the centre, the outline and water followed near them, contours on a slope. */
std::vector<TensorBasis> defaultFields(const Layout& l, const Ground& g)
{
    std::vector<TensorBasis> out;
    TensorBasis grid;
    grid.kind = "grid";
    grid.angle = l.profile.gridAngle;
    out.push_back(grid);
    TensorBasis radial;
    radial.kind = "radial";
    radial.weight = 0.7;
    radial.decay = 0.5 * l.radius;
    out.push_back(radial);
    TensorBasis edge;
    edge.kind = "boundary";
    edge.follow = "outline";
    edge.decay = 0.12 * l.radius;
    out.push_back(edge);
    if (!l.water.empty())
    {
        TensorBasis water;
        water.kind = "boundary";
        water.follow = "water";
        water.weight = 2.0;
        water.decay = 0.15 * l.radius;
        out.push_back(water);
    }
    if (!g.isPlane())
    {
        TensorBasis height;
        height.kind = "height";
        height.weight = 1.5;
        out.push_back(height);
    }
    return out;
}

/** Sample points of traced streets, per direction family, hashed by cell for spacing tests. */
struct Samples
{
    double cell;
    std::unordered_map<int64_t, std::vector<Vec2>> cells;

    explicit Samples(double c) : cell(std::max(c, 1.0)) {}
    int64_t key(int x, int y) const { return (int64_t(x) << 32) ^ int64_t(uint32_t(y)); }
    void add(Vec2 p) { cells[key(int(std::floor(p.x / cell)), int(std::floor(p.y / cell)))].push_back(p); }
    bool near(Vec2 p, double r) const
    {
        const int x0 = int(std::floor((p.x - r) / cell)), x1 = int(std::floor((p.x + r) / cell));
        const int y0 = int(std::floor((p.y - r) / cell)), y1 = int(std::floor((p.y + r) / cell));
        for (int x = x0; x <= x1; ++x)
            for (int y = y0; y <= y1; ++y)
                if (auto it = cells.find(key(x, y)); it != cells.end())
                    for (const Vec2& q : it->second)
                        if (glm::dot(q - p, q - p) < r * r)
                            return true;
        return false;
    }
};

struct Tracer
{
    const Layout& l;
    const Ground& g;
    const TensorSpec& spec;
    double step = 500.0;
    double outer = 0.0;

    Vec2 dirAt(Vec2 p, int family, double* strength) const
    {
        double s = 0.0;
        Vec2 d = tensorDirection(l, g, spec, p, &s);
        if (strength)
            *strength = s;
        return family == 0 ? d : leftNormal(d);
    }

    bool inside(Vec2 p) const
    {
        if (!contains(l.outline.pts, p) || excluded(l, p) || !g.solid(p))
            return false;
        if (outer > 0.0 && glm::length(p - l.center) > outer)
            return false;
        return l.water.empty() || bankDistance(p, l.water) > 0.0;
    }

    /** A streamline through seed both ways, stopped near other streets of its family, at the outline, or where the field fades. */
    std::vector<Vec2> trace(Vec2 seed, int family, const Samples& same, double dtest, double maxLength) const
    {
        if (!inside(seed) || same.near(seed, dtest))
            return {};
        std::vector<Vec2> halves[2];
        double s0 = 0.0;
        const Vec2 d0 = dirAt(seed, family, &s0);
        if (s0 < 1e-6)
            return {};
        const int maxSteps = int(std::clamp(maxLength / step, 4.0, 20000.0));
        bool closed = false;
        for (int side = 0; side < 2 && !closed; ++side)
        {
            Vec2 pos = seed, prev = side == 0 ? d0 : -d0;
            std::vector<Vec2>& out = halves[side];
            for (int i = 0; i < maxSteps; ++i)
            {
                double sa = 0.0, sb = 0.0;
                Vec2 a = dirAt(pos, family, &sa);
                if (glm::dot(a, prev) < 0.0)
                    a = -a;
                Vec2 b = dirAt(pos + a * (0.5 * step), family, &sb);
                if (glm::dot(b, prev) < 0.0)
                    b = -b;
                if (sa < 1e-6 || sb < 1e-6 || glm::dot(b, prev) < 0.5)
                    break;
                const Vec2 next = pos + b * step;
                if (!inside(next) || same.near(next, dtest))
                {
                    // Run on to the edge or the street ahead, so the street ends on it instead of short of it.
                    out.push_back(pos + b * std::min(step, dtest));
                    break;
                }
                if (i > 8 && side == 0 && glm::length(next - seed) < step * 1.2)
                {
                    out.push_back(seed);
                    closed = true;
                    break;
                }
                out.push_back(next);
                prev = b;
                pos = next;
            }
        }
        std::vector<Vec2> line(halves[1].rbegin(), halves[1].rend());
        line.push_back(seed);
        line.insert(line.end(), halves[0].begin(), halves[0].end());
        return line;
    }
};

/** Corner points of a polyline kept where it bends or every so often, so streets are not made of tiny pieces. */
std::vector<Vec2> thinned(const std::vector<Vec2>& line, double every)
{
    if (line.size() < 3)
        return line;
    std::vector<Vec2> out = {line.front()};
    double run = 0.0;
    for (size_t k = 1; k + 1 < line.size(); ++k)
    {
        run += glm::length(line[k] - line[k - 1]);
        const Vec2 a = glm::normalize(line[k] - out.back()), b = line[k + 1] - line[k];
        const double bend = glm::length(b) > 1e-9 ? 1.0 - glm::dot(a, glm::normalize(b)) : 0.0;
        if (run >= every || bend > 0.02)
        {
            out.push_back(line[k]);
            run = 0.0;
        }
    }
    out.push_back(line.back());
    return out;
}
} // namespace

Vec2 tensorDirection(const Layout& l, const Ground& g, const TensorSpec& spec, Vec2 p, double* strength)
{
    const std::vector<TensorBasis> defaults = spec.fields.empty() ? defaultFields(l, g) : std::vector<TensorBasis>();
    const std::vector<TensorBasis>& fields = spec.fields.empty() ? defaults : spec.fields;
    Vec2 t(0.0);
    for (const TensorBasis& b : fields)
    {
        const Vec2 c = b.hasCenter ? b.center : l.center;
        if (b.kind == "grid")
        {
            const double a = b.angle * kPi / 180.0;
            const double w = b.weight * (b.hasCenter ? falloff(glm::length(p - c), b.decay) : 1.0);
            t += tensorOf(Vec2(std::cos(a), std::sin(a)), w);
        }
        else if (b.kind == "radial")
        {
            const Vec2 d = p - c;
            const double len = glm::length(d);
            if (len < 1e-6)
                continue;
            // Fades to nothing at the very centre, where every direction is radial.
            const double w = b.weight * falloff(len, b.decay) * std::min(1.0, len / std::max(l.profile.segment, 100.0));
            t += tensorOf(d / len, w);
        }
        else if (b.kind == "boundary")
        {
            const double decay = b.decay > 0.0 ? b.decay : 0.25 * l.radius;
            auto follow = [&](std::span<const Vec2> line, bool closed) {
                Vec2 tan(1.0, 0.0);
                const double d = gen::lineDistance(line, p, &tan, closed);
                if (d < std::numeric_limits<double>::max())
                    t += tensorOf(tan, b.weight * falloff(d, decay));
            };
            if (b.follow == "outline")
                follow(l.outline.pts, true);
            else if (b.follow == "water")
                for (const WaterLine& w : l.water)
                    follow(w.pts, false);
            for (const std::vector<Vec2>& line : b.lines)
                follow(line, false);
        }
        else if (b.kind == "height")
        {
            if (g.isPlane())
                continue;
            const double e = std::max(g.cellSize(), 100.0);
            const Vec2 grad((g.height(p + Vec2(e, 0.0)) - g.height(p - Vec2(e, 0.0))) / (2.0 * e),
                            (g.height(p + Vec2(0.0, e)) - g.height(p - Vec2(0.0, e))) / (2.0 * e));
            const double len = glm::length(grad);
            if (len < 1e-6)
                continue;
            // Along the contour: across the gradient.
            t += tensorOf(leftNormal(grad / len), b.weight * std::min(1.0, len / 0.05) * (b.hasCenter ? falloff(glm::length(p - c), b.decay) : 1.0));
        }
    }
    for (const TensorPaint& s : spec.paint)
    {
        Vec2 tan(1.0, 0.0);
        double d = gen::lineDistance(s.pts, p, &tan, s.closed);
        if (s.closed && s.pts.size() >= 3 && contains(s.pts, p))
            d = 0.0;
        if (d >= s.radius)
            continue;
        const double f = 1.0 - d / s.radius;
        const double w = s.weight * f * f * (spec.mask ? spec.mask->at(p) : 1.0);
        if (s.angle)
        {
            const double a = *s.angle * kPi / 180.0;
            tan = Vec2(std::cos(a), std::sin(a));
        }
        t += tensorOf(tan, w);
    }
    const double mag = glm::length(t);
    if (strength)
        *strength = mag;
    if (mag < 1e-12)
        return {1.0, 0.0};
    const double a = 0.5 * std::atan2(t.y, t.x);
    return {std::cos(a), std::sin(a)};
}

void layTensor(Layout& l, const Ground& g, const Options& o, const TensorSpec& spec, double limit)
{
    LILA_ZONE();

    const Profile& p = o.profile;
    const std::vector<TensorBasis> fields = spec.fields.empty() ? defaultFields(l, g) : spec.fields;
    TensorSpec resolved = spec;
    resolved.fields = fields;
    const double minor = spec.minorSpacing > 0.0 ? spec.minorSpacing : 0.5 * (p.gridWidth + p.gridDepth) + p.widthLocal;
    const double major = spec.majorSpacing > 0.0 ? spec.majorSpacing : minor * std::max(2, p.arterialEvery);
    Tracer tr{l, g, resolved};
    tr.step = spec.step > 0.0 ? spec.step : std::clamp(std::min(minor * 0.25, p.segment * 0.5), 100.0, 5000.0);
    tr.outer = limit;
    const double maxLength = 4.0 * std::max(l.radius, 1000.0);
    gen::Rng rng(o.seed, 11);

    struct Traced
    {
        std::vector<Vec2> pts;
        int family = 0;
        StreetClass cls = StreetClass::Local;
    };
    std::vector<Traced> lines;
    // Growth thins the network: fewer minor streets, and the major ones stop sooner from the centre.
    const double growth = std::clamp(o.growth, 0.0, 1.0);
    const double reach = growth >= 1.0 ? 0.0 : std::max(major, l.radius * (0.25 + 0.75 * growth));

    auto run = [&](double dsep, bool minorPass, std::vector<Samples>& samples) {
        std::deque<std::pair<Vec2, int>> seeds;
        if (!minorPass)
        {
            const Vec2 c = l.center + Vec2(rng.range(-0.1, 0.1), rng.range(-0.1, 0.1)) * dsep;
            seeds.push_back({c, 0});
            seeds.push_back({c, 1});
        }
        else
            for (const Traced& t : lines)
            {
                double carry = dsep * 0.5;
                for (size_t k = 1; k < t.pts.size(); ++k)
                {
                    carry += glm::length(t.pts[k] - t.pts[k - 1]);
                    if (carry < dsep)
                        continue;
                    carry = 0.0;
                    for (int f : {0, 1})
                        seeds.push_back({t.pts[k], f});
                }
            }
        // Ground the streets already traced never reach (across a river, past a lake) is seeded on a lattice.
        std::deque<std::pair<Vec2, int>> lattice;
        {
            Vec2 lo(1e300), hi(-1e300);
            for (const Vec2& q : l.outline.pts)
                lo = glm::min(lo, q), hi = glm::max(hi, q);
            for (double y = lo.y + 0.5 * dsep; y < hi.y; y += dsep)
                for (double x = lo.x + 0.5 * dsep; x < hi.x; x += dsep)
                    for (int f : {0, 1})
                        lattice.push_back({Vec2(x, y), f});
        }
        const double dtest = 0.5 * dsep;
        int perFamily[2] = {0, 0};
        size_t guard = 0;
        while ((!seeds.empty() || !lattice.empty()) && guard++ < 400000)
        {
            if (seeds.empty())
            {
                seeds.push_back(lattice.front());
                lattice.pop_front();
            }
            const auto [seed, family] = seeds.front();
            seeds.pop_front();
            if (reach > 0.0 && glm::length(seed - l.center) > reach)
                continue;
            if (samples[size_t(family)].near(seed, dsep * 0.9))
                continue;
            std::vector<Vec2> pts = tr.trace(seed, family, samples[size_t(family)], dtest, maxLength);
            if (reach > 0.0)
            {
                std::vector<Vec2> kept;
                for (const Vec2& q : pts)
                    if (glm::length(q - l.center) <= reach)
                        kept.push_back(q);
                    else if (!kept.empty())
                        break;
                pts = std::move(kept);
            }
            if (pts.size() < 2 || gen::length(pts) < dsep * 0.5)
                continue;
            for (const Vec2& q : pts)
                samples[size_t(family)].add(q);
            Traced t{pts, family, StreetClass::Local};
            if (!minorPass)
                t.cls = perFamily[family] < 1 ? StreetClass::Arterial : StreetClass::Collector;
            ++perFamily[family];
            // New seeds a spacing to each side, every spacing along: the next parallel streets.
            double carry = 0.0;
            for (size_t k = 1; k < pts.size(); ++k)
            {
                carry += glm::length(pts[k] - pts[k - 1]);
                if (carry < dsep && k + 1 < pts.size())
                    continue;
                carry = 0.0;
                const Vec2 d = glm::normalize(pts[k] - pts[k - 1]);
                for (double side : {1.0, -1.0})
                    seeds.push_back({pts[k] + leftNormal(d) * (side * dsep), family});
                seeds.push_back({pts[k], 1 - family});
            }
            lines.push_back(std::move(t));
        }
    };

    std::vector<Samples> majorSamples = {Samples(major), Samples(major)};
    run(major, false, majorSamples);
    if (spec.minor && growth > 0.0)
    {
        std::vector<Samples> minorSamples = {Samples(minor), Samples(minor)};
        for (const Traced& t : lines)
            for (const Vec2& q : t.pts)
                minorSamples[size_t(t.family)].add(q);
        run(minor / std::max(std::sqrt(growth), 0.3), true, minorSamples);
    }

    std::vector<int> made;
    for (const Traced& t : lines)
        for (int e : gen::lay(l, g, p, thinned(t.pts, std::max(p.segment, tr.step * 2.0)), t.cls, 0, 0.0, limit))
            made.push_back(e);

    // Streamlines stop just short of each other: dead ends run on to the street ahead.
    gen::closeEnds(l, g, p, made, 0.75 * minor);
}
} // namespace settlement
