#include "curve/RegionSplit.h"

#include "core/Profile.h"

#ifdef _MSC_VER
// The optimizer reports Clipper2's unreachable code after any warning scope around the include ends.
#pragma warning(disable : 4702)
#endif
#include <clipper2/clipper.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace lila::curve
{
namespace
{
namespace c2 = Clipper2Lib;

constexpr int kPrecision = 6;

double segDistance(glm::dvec2 p, glm::dvec2 a, glm::dvec2 b)
{
    glm::dvec2 d = b - a;
    double len2 = glm::dot(d, d);
    double t = len2 > 0.0 ? std::clamp(glm::dot(p - a, d) / len2, 0.0, 1.0) : 0.0;
    return glm::length(a + d * t - p);
}

/** Distance along the ray from o in direction d to the first loop edge it crosses. */
std::optional<double> rayHit(const std::vector<RegionLoop>& region, glm::dvec2 o, glm::dvec2 d)
{
    std::optional<double> best;
    for (const RegionLoop& loop : region)
        for (size_t i = 0; i < loop.size(); ++i)
        {
            glm::dvec2 a = loop[i], e = loop[(i + 1) % loop.size()] - a;
            double den = d.x * e.y - d.y * e.x;
            if (std::abs(den) < 1e-300)
                continue;
            glm::dvec2 w = a - o;
            double t = (w.x * e.y - w.y * e.x) / den;
            double u = (w.x * d.y - w.y * d.x) / den;
            if (t > 0.0 && u >= 0.0 && u <= 1.0 && (!best || t < *best))
                best = t;
        }
    return best;
}

/** How far each run-on turns out from straight across the limb, so a limb that widens past the joint stays whole. */
constexpr double kFan = 0.5236; // 30 degrees

/** Across the joint span (the cut's first segment), toward the side away from keep. Zero for a span of no length. */
glm::dvec2 limbNormal(const RegionLoop& cut, glm::dvec2 keep)
{
    glm::dvec2 span = cut[1] - cut[0];
    if (glm::length(span) <= 0.0)
        return glm::dvec2(0.0);
    span = glm::normalize(span);
    glm::dvec2 n(-span.y, span.x);
    return glm::dot(keep - (cut[0] + cut[1]) * 0.5, n) > 0.0 ? -n : n;
}

/**
 * Opens every hole to the outside with a slit gap wide, from the hole's first point to the nearest outer edge, so each
 * piece is one simple loop the viewport can triangulate.
 */
c2::PathsD openHoles(c2::PathsD paths, double gap)
{
    for (int guard = 0; guard < 64; ++guard)
    {
        auto hole = std::ranges::find_if(paths, [](const c2::PathD& p) { return c2::Area(p) < 0.0; });
        if (hole == paths.end() || hole->empty())
            return paths;
        const glm::dvec2 h((*hole)[0].x, (*hole)[0].y);
        glm::dvec2 best(0.0);
        double bestDist = 1e300;
        for (const c2::PathD& p : paths)
            if (c2::Area(p) > 0.0)
                for (size_t i = 0; i < p.size(); ++i)
                {
                    const glm::dvec2 a(p[i].x, p[i].y), b(p[(i + 1) % p.size()].x, p[(i + 1) % p.size()].y);
                    const glm::dvec2 d = b - a;
                    const double len2 = glm::dot(d, d);
                    const glm::dvec2 q = a + d * (len2 > 0.0 ? std::clamp(glm::dot(h - a, d) / len2, 0.0, 1.0) : 0.0);
                    if (double dist = glm::length(q - h); dist < bestDist)
                        bestDist = dist, best = q;
                }
        if (bestDist >= 1e300 || bestDist <= 0.0)
            return paths;
        const glm::dvec2 dir = (best - h) / bestDist;
        const glm::dvec2 from = h - dir * gap * 2.0, to = best + dir * gap * 2.0;
        const c2::PathsD slit = c2::InflatePaths({{c2::PointD(from.x, from.y), c2::PointD(to.x, to.y)}}, gap * 0.5, c2::JoinType::Miter,
                                                 c2::EndType::Butt, 2.0, kPrecision);
        paths = c2::Difference(paths, slit, c2::FillRule::EvenOdd, kPrecision);
    }
    return paths;
}

c2::PathD toPath(const RegionLoop& loop)
{
    c2::PathD p;
    p.reserve(loop.size());
    for (const glm::dvec2& v : loop)
        p.emplace_back(v.x, v.y);
    return p;
}
} // namespace

bool regionContains(const std::vector<RegionLoop>& region, glm::dvec2 p)
{
    bool inside = false;
    for (const RegionLoop& loop : region)
        for (size_t i = 0, j = loop.size() - 1; i < loop.size(); j = i++)
        {
            const glm::dvec2 &a = loop[i], &b = loop[j];
            if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
                inside = !inside;
        }
    return inside;
}

double regionDistance(const std::vector<RegionLoop>& region, glm::dvec2 p)
{
    if (regionContains(region, p))
        return 0.0;
    double best = std::numeric_limits<double>::infinity();
    for (const RegionLoop& loop : region)
        for (size_t i = 0; i < loop.size(); ++i)
            best = std::min(best, segDistance(p, loop[i], loop[(i + 1) % loop.size()]));
    return best;
}

RegionLoop runOnCut(const std::vector<RegionLoop>& region, const RegionLoop& cut, glm::dvec2 keep, double overshoot)
{
    RegionLoop path = cut;
    const glm::dvec2 n = cut.size() == 2 ? limbNormal(cut, keep) : glm::dvec2(0.0);
    if (n == glm::dvec2(0.0))
        return path;
    const glm::dvec2 span = glm::normalize(cut[1] - cut[0]);
    auto run = [&](glm::dvec2 from, glm::dvec2 d) -> std::optional<glm::dvec2> {
        if (!regionContains(region, from))
            return std::nullopt;
        std::optional<double> t = rayHit(region, from, d);
        return t ? std::optional(from + d * (*t + overshoot)) : std::nullopt;
    };
    const double c = std::cos(kFan), s = std::sin(kFan);
    if (std::optional<glm::dvec2> a = run(cut.front(), glm::normalize(n * c - span * s)))
        path.insert(path.begin(), *a);
    if (std::optional<glm::dvec2> b = run(cut.back(), glm::normalize(n * c + span * s)))
        path.push_back(*b);
    return path;
}

RegionCut cutRegion(const std::vector<RegionLoop>& region, const RegionLoop& cut, glm::dvec2 keep, double gap)
{
    LILA_ZONE();

    RegionCut result;
    if (cut.size() < 2 || region.empty() || gap <= 0.0)
        return result;

    c2::PathsD subject;
    double total = 0.0;
    for (const RegionLoop& loop : region)
        if (loop.size() >= 3)
        {
            subject.push_back(toPath(loop));
            total += std::abs(c2::Area(subject.back()));
        }
    auto loops = [&](const c2::PathsD& holed) {
        std::vector<RegionLoop> out;
        for (const c2::PathD& p : openHoles(holed, gap))
            if (c2::Area(p) > total * 1e-9)
            {
                RegionLoop& loop = out.emplace_back();
                for (const c2::PointD& v : p)
                    loop.emplace_back(v.x, v.y);
            }
        return out;
    };
    if (cut.size() >= 3)
    {
        const c2::PathsD lasso = {toPath(cut)};
        result.limb = loops(c2::Intersect(subject, lasso, c2::FillRule::EvenOdd, kPrecision));
        result.rest = loops(c2::Difference(subject, lasso, c2::FillRule::EvenOdd, kPrecision));
        if (result.limb.empty() || result.rest.empty())
            result = {};
        return result;
    }
    const RegionLoop path = runOnCut(region, cut, keep, gap * 4.0);
    c2::PathsD band = c2::InflatePaths({toPath(path)}, gap * 0.5, c2::JoinType::Miter, c2::EndType::Butt, 2.0, kPrecision);
    c2::PathsD parts = c2::Difference(subject, band, c2::FillRule::EvenOdd, kPrecision);

    // Holes can't come from a cut, and slivers are the band's own corners.
    std::vector<RegionLoop> out = loops(parts);
    if (out.size() < 2)
        return result;

    // The limb is the part just past the joint span's middle; whatever else the cut parted stays with the rest.
    const glm::dvec2 probe = (cut[0] + cut[1]) * 0.5 + limbNormal(cut, keep) * (gap * 4.0);
    size_t limb = 0;
    double nearest = 1e300;
    for (size_t i = 0; i < out.size(); ++i)
        if (double d = regionDistance({out[i]}, probe); d < nearest)
            nearest = d, limb = i;
    for (size_t i = 0; i < out.size(); ++i)
        (i == limb ? result.limb : result.rest).push_back(std::move(out[i]));
    return result;
}
} // namespace lila::curve
