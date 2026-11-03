// Dressing on the stones: a peeling render coat and climbing ivy, as meshes beside the stones and core.

#include "MasonryRuin.h"

#include <lila/Profile.h>

#include <algorithm>
#include <cmath>

namespace masonry
{
namespace
{
double smoothStep(double t) { return t * t * (3.0 - 2.0 * t); }

/** Value noise in [0, 1], seeded. */
double patchNoise(const glm::dvec3& p, uint64_t seed)
{
    const glm::dvec3 f = glm::floor(p), t = p - f;
    auto at = [&](int dx, int dy, int dz) {
        return unitRandom(mixKey(seed, uint64_t(int64_t(f.x) + dx), uint64_t(int64_t(f.y) + dy), uint64_t(int64_t(f.z) + dz), 991));
    };
    const double sx = smoothStep(t.x), sy = smoothStep(t.y), sz = smoothStep(t.z);
    auto mix = [](double a, double b, double s) { return a + (b - a) * s; };
    return mix(mix(mix(at(0, 0, 0), at(1, 0, 0), sx), mix(at(0, 1, 0), at(1, 1, 0), sx), sy),
               mix(mix(at(0, 0, 1), at(1, 0, 1), sx), mix(at(0, 1, 1), at(1, 1, 1), sx), sy), sz);
}

bool onFace(const Stone& s)
{
    return s.t1 >= s.t0 && s.face != Face::Inner && s.role != Role::Rubble && s.role != Role::Slab && s.role != Role::Paving;
}

double typical(std::span<const Stone> stones)
{
    std::vector<double> h;
    for (const Stone& s : stones)
        h.push_back(s.size.y);
    if (h.empty())
        return 10.0;
    std::nth_element(h.begin(), h.begin() + std::ptrdiff_t(h.size() / 2), h.end());
    return std::max(1.0, h[h.size() / 2]);
}

/** Quads (four points each, any count) as one mesh with uv0 set per quad from the given corners. */
geo::Mesh quads(const std::vector<glm::dvec3>& pts, const std::vector<glm::vec2>& uvs)
{
    std::vector<geo::Index> sizes, idx;
    for (size_t k = 0; k + 3 < pts.size(); k += 4)
    {
        sizes.push_back(4);
        for (size_t i = 0; i < 4; ++i)
            idx.push_back(geo::Index(k + i));
    }
    geo::Mesh m = geo::Mesh::fromPolygons(pts, sizes, idx);
    std::span<glm::vec2> uv = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
            for (geo::Index c : m.faceCorners(f))
                uv[c] = uvs[size_t(m.cornerPoint(c))];
    return m;
}
} // namespace

geo::Mesh plasterMesh(std::span<const Stone> stones, std::span<const float> height, const AgeParams& p, double joint)
{
    LILA_ZONE_N("Masonry plaster");
    geo::Mesh out;
    const double unit = typical(stones);
    const double thick = p.plasterThickness > 0.0 ? p.plasterThickness : unit / 3.0;
    const double cover = std::clamp(p.plaster, 0.0, 1.0);
    const double j = std::max(0.0, joint);
    for (size_t k = 0; k < stones.size(); ++k)
    {
        const Stone& s = stones[k];
        if (!onFace(s))
            continue;
        // Peels where it is high up, damaged, exposed to the weather or just weak, in patches.
        const double patches = patchNoise(s.center / (unit * 5.0), p.seed + 31);
        const double peel = 0.55 * patches + 0.25 * (k < height.size() ? double(height[k]) : 0.5) + 0.35 * s.damage +
                            0.1 * unitRandom(mixKey(p.seed, 32, k));
        if (peel > cover * 1.25)
            continue;
        const glm::dvec3 out3 = -s.frame[2];
        const glm::dvec3 centre = s.center + out3 * (s.size.z * 0.5 + thick * 0.5 + 0.05);
        const glm::dvec3 h((s.size.x + j) * 0.5, (s.size.y + j) * 0.5, thick * 0.5);
        std::array<glm::dvec3, 8> c;
        for (int i = 0; i < 8; ++i)
            c[size_t(i)] = centre + s.frame * glm::dvec3((i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z);
        out.append(hexMesh(c));
    }
    if (out.numFaces() > 0)
    {
        std::span<glm::vec2> uv = out.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
        const double tile = unit * 15.0;
        for (geo::Index f = 0; f < out.faceCapacity(); ++f)
            if (out.faceValid(f))
            {
                const glm::dvec3 n = glm::abs(out.faceNormal(f));
                const int axis = n.x >= n.y && n.x >= n.z ? 0 : (n.y >= n.z ? 1 : 2);
                for (geo::Index c : out.faceCorners(f))
                {
                    const glm::dvec3 q = out.position(out.cornerPoint(c));
                    uv[c] = axis == 0 ? glm::vec2(float(q.z / tile), float(q.y / tile))
                                      : (axis == 1 ? glm::vec2(float(q.x / tile), float(q.z / tile)) : glm::vec2(float(q.x / tile), float(q.y / tile)));
                }
            }
        lila::scene::materials::assign(out, kPlasterSlot, {});
    }
    return out;
}

geo::Mesh ivyMesh(std::span<const Stone> stones, const AgeParams& p)
{
    LILA_ZONE_N("Masonry ivy");
    std::vector<glm::dvec3> pts;
    std::vector<glm::vec2> uvs;
    const double unit = typical(stones);
    const double leaf = p.leaf > 0.0 ? p.leaf : unit * 0.66;
    StoneGrid grid;
    double big = unit;
    double lo = 1e300, hi = -1e300;
    for (const Stone& s : stones)
    {
        big = std::max(big, std::max(s.size.x, s.size.y));
        lo = std::min(lo, s.center.y - s.size.y * 0.5);
        hi = std::max(hi, s.center.y + s.size.y * 0.5);
    }
    grid.cell = std::max(unit * 4.0, big);
    grid.build(stones, 0.0);
    const double reachTop = lo + (hi - lo) * std::clamp(p.ivyHeight, 0.0, 1.0);

    // The face stone under a point, and the point moved onto its outer face.
    auto faceAt = [&](const glm::dvec3& q, glm::dvec3* snapped, glm::dvec3* normal) {
        Stone probe;
        probe.center = q;
        probe.size = glm::dvec3(unit * 0.5);
        double best = 1e300;
        for (int o : grid.around(probe, unit))
        {
            const Stone& s = stones[size_t(o)];
            if (!onFace(s))
                continue;
            const glm::dvec3 local = glm::transpose(s.frame) * (q - s.center);
            const glm::dvec3 h = s.size * 0.5;
            if (std::abs(local.x) > h.x + unit * 0.3 || std::abs(local.y) > h.y + unit * 0.3 || local.z > h.z || local.z < -h.z - unit)
                continue;
            const double off = std::abs(local.z + h.z);
            if (off < best)
            {
                best = off;
                *snapped = s.center + s.frame * glm::dvec3(std::clamp(local.x, -h.x, h.x), std::clamp(local.y, -h.y, h.y), -h.z - 0.3);
                *normal = -s.frame[2];
            }
        }
        return best < 1e300;
    };
    auto addQuad = [&](const glm::dvec3& c, const glm::dvec3& right, const glm::dvec3& up, glm::vec2 uvLo, glm::vec2 uvHi) {
        pts.insert(pts.end(), {c - right - up, c + right - up, c + right + up, c - right + up});
        uvs.insert(uvs.end(), {uvLo, {uvHi.x, uvLo.y}, uvHi, {uvLo.x, uvHi.y}});
    };

    for (size_t k = 0; k < stones.size(); ++k)
    {
        const Stone& s = stones[k];
        if (!onFace(s) || !s.grounded || unitRandom(mixKey(p.seed, 41, k)) > std::clamp(p.ivy, 0.0, 1.0) * 0.35)
            continue;
        glm::dvec3 at = s.center - s.frame[1] * (s.size.y * 0.5), normal = -s.frame[2];
        if (!faceAt(at, &at, &normal))
            continue;
        const double maxUp = lo + (reachTop - lo) * (0.4 + 0.6 * unitRandom(mixKey(p.seed, 42, k)));
        glm::dvec3 prev = at;
        for (int step = 0; step < 400 && at.y < maxUp; ++step)
        {
            auto rnd = [&](int n) { return unitRandom(mixKey(p.seed, 43, k, uint64_t(step), uint64_t(n))); };
            const glm::dvec3 side = glm::normalize(glm::cross(normal, glm::dvec3(0, 1, 0)) + glm::dvec3(1e-9));
            glm::dvec3 next = at + glm::dvec3(0, unit * 0.9, 0) + side * ((rnd(0) - 0.5) * unit * 1.2);
            glm::dvec3 n2 = normal;
            if (!faceAt(next, &next, &n2))
                break;
            // Stem: a thin ribbon from the last point.
            const glm::dvec3 mid = (prev + next) * 0.5, axis = next - prev;
            const double len = glm::length(axis);
            if (len > 1e-6)
                addQuad(mid, side * (leaf * 0.04), axis * 0.5, {0.0f, 0.0f}, {0.05f, 1.0f});
            for (int l = 0; l < 3; ++l)
            {
                const double a = (rnd(1 + l) - 0.5) * 2.0;
                const glm::dvec3 tilt = glm::normalize(n2 + side * (a * 0.5) + glm::dvec3(0, 0.4, 0));
                const glm::dvec3 right = glm::normalize(glm::cross(glm::dvec3(0, 1, 0), tilt) + glm::dvec3(1e-9)) * (leaf * 0.5);
                const glm::dvec3 up = glm::normalize(glm::cross(tilt, right)) * (leaf * 0.5);
                const double size = 0.7 + 0.6 * rnd(5 + l);
                const glm::dvec3 c = next + side * ((rnd(8 + l) - 0.5) * leaf) + n2 * (leaf * 0.3) + glm::dvec3(0, (rnd(11 + l) - 0.5) * leaf, 0);
                addQuad(c, right * size, up * size, {0.1f, 0.0f}, {1.0f, 1.0f});
            }
            prev = next;
            at = next;
            normal = n2;
        }
    }
    if (pts.empty())
        return {};
    geo::Mesh m = quads(pts, uvs);
    lila::scene::materials::assign(m, kIvySlot, {});
    return m;
}
} // namespace masonry
