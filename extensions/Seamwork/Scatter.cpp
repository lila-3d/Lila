#include "Scatter.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>

namespace seamwork
{
using lila::geo::Index;

namespace
{

glm::dvec3 unit(const glm::dvec3& v, const glm::dvec3& fallback)
{
    const double l = glm::length(v);
    return l > 1e-12 ? v / l : fallback;
}

/** v with its part along n removed. */
glm::dvec3 flatten(const glm::dvec3& v, const glm::dvec3& n) { return v - n * glm::dot(v, n); }

uint32_t hashPoint(uint32_t seed, size_t curve, double along)
{
    uint64_t h = (uint64_t(seed) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(curve) * 0xC2B2AE3D27D4EB4Full) ^ uint64_t(int64_t(std::llround(along * 100.0)));
    h ^= h >> 31;
    h *= 0x7FB5D329728EA185ull;
    h ^= h >> 27;
    return uint32_t(h);
}

/** A frame whose Y is up, Z as close to forward as up allows. */
glm::dmat3 frameOf(const glm::dvec3& up, const glm::dvec3& forward)
{
    const glm::dvec3 y = unit(up, {0, 1, 0});
    glm::dvec3 z = flatten(forward, y);
    if (glm::length(z) < 1e-9)
        z = flatten(std::abs(y.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0), y);
    z = glm::normalize(z);
    const glm::dvec3 x = glm::cross(y, z);
    return glm::dmat3(x, y, z);
}

double closestDistance(const Object& o, const glm::dvec3& p, glm::dvec3* closest)
{
    const glm::dmat4 inv = glm::inverse(o.world);
    lila::geo::BvhClosest c;
    if (!o.bvh->closestPoint(glm::dvec3(inv * glm::dvec4(p, 1.0)), c))
        return 1e300;
    const glm::dvec3 w(o.world * glm::dvec4(c.point, 1.0));
    if (closest)
        *closest = w;
    return glm::length(w - p);
}

// ------------------------------------------------------------------------------------------------ built-in assets

/** An octahedron subdivided n times onto the unit sphere. */
Mesh sphereMesh(int subdivisions, const std::function<glm::dvec3(const glm::dvec3&)>& shape)
{
    std::vector<glm::dvec3> pts = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    std::vector<glm::uvec3> tris = {{0, 2, 4}, {4, 2, 1}, {1, 2, 5}, {5, 2, 0}, {4, 3, 0}, {1, 3, 4}, {5, 3, 1}, {0, 3, 5}};
    for (int s = 0; s < subdivisions; ++s)
    {
        std::map<std::pair<unsigned, unsigned>, unsigned> mids;
        auto mid = [&](unsigned a, unsigned b) {
            const auto key = std::minmax(a, b);
            auto [it, fresh] = mids.try_emplace({key.first, key.second}, unsigned(pts.size()));
            if (fresh)
                pts.push_back(glm::normalize(pts[a] + pts[b]));
            return it->second;
        };
        std::vector<glm::uvec3> next;
        for (const glm::uvec3& t : tris)
        {
            const unsigned ab = mid(t.x, t.y), bc = mid(t.y, t.z), ca = mid(t.z, t.x);
            next.push_back({t.x, ab, ca});
            next.push_back({ab, t.y, bc});
            next.push_back({ca, bc, t.z});
            next.push_back({ab, bc, ca});
        }
        tris = std::move(next);
    }
    for (glm::dvec3& p : pts)
        p = shape(p);
    std::vector<Index> sizes(tris.size(), 3), idx;
    for (const glm::uvec3& t : tris)
    {
        idx.push_back(t.x);
        idx.push_back(t.y);
        idx.push_back(t.z);
    }
    return Mesh::fromPolygons(pts, sizes, idx);
}

/** A blob from the unit sphere: radii, flattened below so it sits on its base at y = 0, noisy by rough. */
Mesh blob(glm::dvec3 radii, double rough, double flatBottom, uint32_t seed, int subdivisions = 2)
{
    return sphereMesh(subdivisions, [=](const glm::dvec3& d) {
        const double n = 1.0 + rough * noise3(d * 1.7 + glm::dvec3(seed % 97), seed);
        glm::dvec3 p = d * radii * n;
        p.y = std::max(p.y, -radii.y * flatBottom);
        p.y += radii.y * flatBottom;
        return p;
    });
}

/** Blades fanned from the base: each a tapered quad leaning out. */
Mesh tuft(int blades, double height, double spreadAngle, uint32_t seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    std::vector<glm::dvec3> pts;
    std::vector<Index> sizes, idx;
    for (int b = 0; b < blades; ++b)
    {
        const double yaw = 2.0 * kPi * (b + u(rng)) / blades;
        const double lean = glm::radians(spreadAngle) * (0.4 + 0.6 * u(rng));
        const double h = height * (0.6 + 0.4 * u(rng));
        const glm::dvec3 dir(std::cos(yaw) * std::sin(lean), std::cos(lean), std::sin(yaw) * std::sin(lean));
        const glm::dvec3 side(-std::sin(yaw), 0.0, std::cos(yaw));
        const double w = height * 0.05;
        const Index base = Index(pts.size());
        pts.push_back(-side * w);
        pts.push_back(side * w);
        pts.push_back(dir * h * 0.6 + side * w * 0.6);
        pts.push_back(dir * h * 0.6 - side * w * 0.6);
        pts.push_back(dir * h);
        sizes.insert(sizes.end(), {4, 3});
        idx.insert(idx.end(), {base, base + 1, base + 2, base + 3, base + 3, base + 2, base + 4});
    }
    return Mesh::fromPolygons(pts, sizes, idx);
}

/** A tube along a bent path from the base, tapering. */
Mesh root(double length, double radius, uint32_t seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    const int rings = 8, sides = 6;
    std::vector<glm::dvec3> path;
    glm::dvec3 p(0.0), dir = glm::normalize(glm::dvec3(1.0, 0.15, 0.2 * u(rng)));
    for (int k = 0; k < rings; ++k)
    {
        // Kept above the base, so the root lies on the ground rather than through it.
        path.push_back(glm::dvec3(p.x, std::max(p.y, 0.0) + radius, p.z));
        dir = glm::normalize(dir + glm::dvec3(0.0, -0.08 + 0.1 * u(rng), 0.25 * u(rng)));
        p += dir * (length / (rings - 1));
    }
    std::vector<glm::dvec3> pts;
    std::vector<Index> sizes, idx;
    for (int k = 0; k < rings; ++k)
    {
        const glm::dvec3 t = glm::normalize(path[size_t(std::min(k + 1, rings - 1))] - path[size_t(std::max(k - 1, 0))]);
        const glm::dmat3 f = frameOf(t, {0, 1, 0});
        const double r = radius * (1.0 - 0.8 * k / (rings - 1));
        for (int s = 0; s < sides; ++s)
        {
            const double a = 2.0 * kPi * s / sides;
            pts.push_back(path[size_t(k)] + (f[0] * std::cos(a) + f[2] * std::sin(a)) * r);
        }
    }
    for (int k = 0; k + 1 < rings; ++k)
        for (int s = 0; s < sides; ++s)
        {
            const Index a = Index(k * sides + s), b = Index(k * sides + (s + 1) % sides);
            sizes.push_back(4);
            idx.insert(idx.end(), {a, a + Index(sides), b + Index(sides), b});
        }
    return Mesh::fromPolygons(pts, sizes, idx);
}

/** A few flat leaves lying near the base, each tipped a little. */
Mesh leaves(int count, double size, uint32_t seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    std::vector<glm::dvec3> pts;
    std::vector<Index> sizes, idx;
    for (int k = 0; k < count; ++k)
    {
        const glm::dvec3 c(u(rng) * size, 0.3 + 0.3 * k, u(rng) * size);
        const double yaw = kPi * u(rng);
        const glm::dvec3 ax(std::cos(yaw), 0.08 * u(rng), std::sin(yaw)), az(-std::sin(yaw), 0.08 * u(rng), std::cos(yaw));
        const Index base = Index(pts.size());
        pts.push_back(c - ax * size * 0.5);
        pts.push_back(c - az * size * 0.25);
        pts.push_back(c + ax * size * 0.5);
        pts.push_back(c + az * size * 0.25);
        sizes.push_back(4);
        idx.insert(idx.end(), {base, base + 3, base + 2, base + 1});
    }
    return Mesh::fromPolygons(pts, sizes, idx);
}

ScatterOptions opts(double density, int clump, double clumpRadius, double spread, bool wedge, double sink, double sMin, double sMax, double tilt,
                    int kinds = 7, double stretch = 1.0, Side side = Side::Ground)
{
    ScatterOptions o;
    o.density = density;
    o.clump = clump;
    o.clumpRadius = clumpRadius;
    o.spread = spread;
    o.wedge = wedge;
    o.sink = sink;
    o.scaleMin = sMin;
    o.scaleMax = sMax;
    o.tilt = tilt;
    o.kinds = kinds;
    o.stretch = stretch;
    o.side = side;
    return o;
}
} // namespace

const std::vector<std::string>& sideNames()
{
    static const std::vector<std::string> n = {"ground", "wall", "both"};
    return n;
}

const std::vector<std::string>& ruleNames()
{
    static const std::vector<std::string> n = {"repel", "shrink", "tilt", "cullInside", "cullOutside"};
    return n;
}

bool accepts(const SeamPoint& p, const ScatterOptions& o)
{
    if (!(o.kinds & (1 << int(p.kind))))
        return false;
    if (p.angle < o.angleMin || p.angle > o.angleMax)
        return false;
    if (p.exposure < o.exposureMin || p.exposure > o.exposureMax)
        return false;
    return o.depthMax <= 0.0 || p.depth <= o.depthMax;
}

std::vector<Placement> distributePoints(const std::vector<SeamPoint>& pts, const std::vector<AssetShape>& assets, const ScatterOptions& o, const DropFn& drop)
{
    std::vector<Placement> out;
    if (assets.empty())
        return out;
    const glm::dvec3 up = unit(o.up, {0, 1, 0});
    for (const SeamPoint& p : pts)
    {
        if (!accepts(p, o))
            continue;
        std::mt19937 rng(hashPoint(o.seed, p.curve, p.along));
        std::uniform_real_distribution<double> u(0.0, 1.0);
        const glm::dvec3 bis = bisector(p, up);
        const glm::dvec3 t = unit(p.tangent, {1, 0, 0});
        for (int c = 0; c < std::max(1, o.clump); ++c)
        {
            Placement pl;
            pl.asset = int(rng() % assets.size());
            pl.curve = p.curve;
            pl.seamPoint = p.position;
            const AssetShape& a = assets[size_t(pl.asset)];
            const double scale = o.scaleMin + (o.scaleMax - o.scaleMin) * u(rng);
            const bool wall = o.side == Side::Wall || (o.side == Side::Both && u(rng) < 0.5);
            // The surface it spreads over and the direction away from the join on it.
            const glm::dvec3 surface = wall || p.b == kNone ? p.normalA : p.normalB;
            const glm::dvec3 other = wall ? p.normalB : p.normalA;
            const glm::dvec3 away = unit(flatten(other, surface), unit(flatten(bis, surface), flatten(up, surface)));
            const glm::dvec3 along = unit(flatten(t, surface), t);
            const double s = o.spread > 0.0 ? o.spread * (1.0 - std::sqrt(1.0 - u(rng))) : 0.0;
            const double cx = (u(rng) * 2.0 - 1.0) * o.clumpRadius, cy = u(rng) * o.clumpRadius;
            glm::dvec3 base = p.position + along * cx + away * (cy + s);
            glm::dvec3 normal = surface;
            if (drop && (s > 0.0 || o.clumpRadius > 0.0))
            {
                glm::dvec3 hit, n;
                if (drop(base + surface * 50.0, -surface, hit, n))
                {
                    base = hit;
                    normal = n;
                }
            }
            const bool inCrease = s + cy < std::max(a.radius * scale, 1.0) * 1.5;
            const glm::dvec3 lean = o.wedge && inCrease && p.b != kNone ? bis : normal;
            glm::dmat3 f = frameOf(lean, along);
            const double yaw = o.stretch > 1.0 ? 0.0 : 2.0 * kPi * u(rng);
            const double tiltAngle = glm::radians(o.tilt) * u(rng), tiltDir = 2.0 * kPi * u(rng);
            glm::dmat4 m(1.0);
            m = glm::rotate(m, yaw, glm::dvec3(0, 1, 0));
            m = glm::rotate(glm::dmat4(1.0), tiltAngle, glm::dvec3(std::cos(tiltDir), 0.0, std::sin(tiltDir))) * m;
            const glm::dmat4 frame(glm::dvec4(f[0], 0.0), glm::dvec4(f[1], 0.0), glm::dvec4(f[2], 0.0), glm::dvec4(0, 0, 0, 1));
            const glm::dvec3 sunk = base - f[1] * (o.sink * a.height * scale);
            pl.base = base;
            pl.world = glm::translate(glm::dmat4(1.0), sunk) * frame * m * glm::scale(glm::dmat4(1.0), glm::dvec3(scale, scale, scale * std::max(1.0, o.stretch)));
            out.push_back(pl);
        }
    }
    return out;
}

std::vector<Placement> distribute(const Result& r, const std::vector<AssetShape>& assets, const ScatterOptions& o, const DropFn& drop)
{
    return distributePoints(samplePoints(r, 100.0 / std::max(1e-3, o.density)), assets, o, drop);
}

const std::vector<Preset>& presets()
{
    static const std::vector<Preset> list = [] {
        std::vector<Preset> p;
        p.push_back({"rubble", "Broken stone chunks wedged along the base, a few spilling out.", opts(3.0, 3, 15.0, 40.0, true, 0.15, 0.6, 1.4, 25.0, 3),
                     [](int k, uint32_t s) { return blob({10.0, 7.0, 8.0}, 0.45, 0.3, s * 31 + uint32_t(k), 1); }});
        p.push_back({"gravel", "Small stones in a band along the join.", opts(8.0, 5, 20.0, 30.0, false, 0.2, 0.5, 1.5, 30.0, 3),
                     [](int k, uint32_t s) { return blob({2.5, 1.6, 2.0}, 0.35, 0.3, s * 17 + uint32_t(k), 1); }});
        ScatterOptions grass = opts(4.0, 3, 12.0, 25.0, false, 0.05, 0.7, 1.3, 12.0, 3);
        grass.exposureMin = 0.2;
        p.push_back({"grassTufts", "Grass tufts where light reaches the base.", grass, [](int k, uint32_t s) { return tuft(9, 25.0, 35.0, s * 13 + uint32_t(k)); }});
        ScatterOptions moss = opts(5.0, 2, 8.0, 5.0, true, 0.3, 0.6, 1.2, 5.0, 7, 1.0, Side::Both);
        moss.exposureMax = 0.8;
        p.push_back({"mossClumps", "Moss hugging the crease on both surfaces, out of full sun.", moss,
                     [](int k, uint32_t s) { return blob({8.0, 2.5, 6.0}, 0.3, 0.5, s * 7 + uint32_t(k)); }});
        p.push_back({"roots", "Roots running from the join out over the ground.", opts(1.0, 2, 10.0, 10.0, false, 0.3, 0.7, 1.3, 8.0, 3),
                     [](int k, uint32_t s) { return root(60.0, 3.0, s * 5 + uint32_t(k)); }});
        ScatterOptions snow = opts(2.0, 1, 0.0, 0.0, true, 0.35, 0.9, 1.2, 2.0, 3, 2.5);
        snow.angleMax = 135.0;
        p.push_back({"snowDrift", "Soft mounds banked against the base, stretched along it.", snow,
                     [](int k, uint32_t s) { return blob({30.0, 15.0, 25.0}, 0.15, 0.6, s * 3 + uint32_t(k)); }});
        p.push_back({"leafLitter", "Leaves gathered in the crease and scattered out from it.", opts(6.0, 4, 15.0, 50.0, false, 0.0, 0.7, 1.3, 15.0, 3),
                     [](int k, uint32_t s) { return leaves(4, 6.0, s * 11 + uint32_t(k)); }});
        return p;
    }();
    return list;
}

const Preset* preset(const std::string& name)
{
    for (const Preset& p : presets())
        if (p.name == name)
            return &p;
    return nullptr;
}

bool insideClosed(const Object& o, const glm::dvec3& p)
{
    const glm::dmat4 inv = glm::inverse(o.world);
    const glm::dvec3 lp(inv * glm::dvec4(p, 1.0));
    int votes = 0;
    for (const glm::dvec3& d : {glm::dvec3(0.0123, 1, 0.0071), glm::dvec3(1, 0.0093, 0.0131), glm::dvec3(0.0087, 0.0113, 1)})
    {
        int crossings = 0;
        glm::dvec3 from = lp;
        for (int k = 0; k < 256; ++k)
        {
            lila::geo::BvhHit h;
            if (!o.bvh->raycast(from, d, h))
                break;
            ++crossings;
            from = h.point + glm::normalize(d) * 1e-6;
        }
        votes += crossings % 2;
    }
    return votes >= 2;
}

RuleOutcome applyRules(const glm::dmat4& world, const std::vector<ProximityRule>& rules, const glm::dvec3& upIn)
{
    RuleOutcome r;
    r.world = world;
    const glm::dvec3 up = unit(upIn, {0, 1, 0});
    for (const ProximityRule& rule : rules)
    {
        if (!rule.target.bvh)
            continue;
        const glm::dvec3 base(r.world[3]);
        if (rule.kind == RuleKind::CullInside || rule.kind == RuleKind::CullOutside)
        {
            const bool in = insideClosed(rule.target, base);
            if (in == (rule.kind == RuleKind::CullInside))
            {
                r.culled = true;
                return r;
            }
            continue;
        }
        glm::dvec3 closest;
        const double d = closestDistance(rule.target, base, &closest);
        if (d >= rule.radius)
            continue;
        const double t = std::clamp(d / std::max(1e-9, rule.radius), 0.0, 1.0);
        const glm::dvec3 away = unit(flatten(base - closest, up), unit(flatten(base - glm::dvec3(rule.target.world[3]), up), {1, 0, 0}));
        switch (rule.kind)
        {
        case RuleKind::Repel:
            r.world[3] += glm::dvec4(away * (rule.radius - d) * std::clamp(rule.strength, 0.0, 1.0), 0.0);
            break;
        case RuleKind::Shrink:
        {
            const double s = std::clamp(rule.strength, 0.0, 1.0) + (1.0 - std::clamp(rule.strength, 0.0, 1.0)) * t;
            const glm::dvec4 pos = r.world[3];
            r.world = r.world * glm::scale(glm::dmat4(1.0), glm::dvec3(s));
            r.world[3] = pos;
            break;
        }
        case RuleKind::Tilt:
        {
            const glm::dvec3 axis = unit(glm::cross(up, away), {1, 0, 0});
            const glm::dvec4 pos = r.world[3];
            r.world = glm::rotate(glm::dmat4(1.0), glm::radians(rule.strength) * (1.0 - t), axis) * r.world;
            r.world[3] = pos;
            break;
        }
        default:
            break;
        }
    }
    return r;
}
} // namespace seamwork
