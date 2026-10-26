// Settlement: polygons in the XZ plane and the ground everything stands on.

#include "SettlementCore.h"

#include <lila/Profile.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace settlement
{
namespace
{
double cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }

Vec2 leftNormal(Vec2 d) { return {-d.y, d.x}; }

bool segmentsCross(Vec2 a, Vec2 b, Vec2 c, Vec2 d)
{
    const double d1 = cross(b - a, c - a), d2 = cross(b - a, d - a);
    const double d3 = cross(d - c, a - c), d4 = cross(d - c, b - c);
    return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0)) && d1 != 0 && d2 != 0 && d3 != 0 && d4 != 0;
}

/** Drops points closer than eps to the next; the side leaving the dropped point goes with it. */
void dropDuplicates(Polygon& p, double eps = 1e-6)
{
    for (size_t k = 0; k < p.pts.size() && p.pts.size() > 2;)
    {
        const size_t n = (k + 1) % p.pts.size();
        if (glm::length(p.pts[n] - p.pts[k]) < eps)
        {
            p.pts.erase(p.pts.begin() + long(k));
            p.tag.erase(p.tag.begin() + long(k));
        }
        else
            ++k;
    }
}

Polygon reversed(const Polygon& p)
{
    Polygon r;
    const size_t n = p.pts.size();
    for (size_t i = 0; i < n; ++i)
    {
        r.pts.push_back(p.pts[n - 1 - i]);
        r.tag.push_back(p.tag[(2 * n - 2 - i) % n]);
    }
    return r;
}

Polygon clipTagged(const Polygon& p, Vec2 origin, Vec2 normal, int cutTag)
{
    Polygon out;
    const size_t n = p.pts.size();
    for (size_t k = 0; k < n; ++k)
    {
        const Vec2 a = p.pts[k], b = p.pts[(k + 1) % n];
        const double da = glm::dot(a - origin, normal), db = glm::dot(b - origin, normal);
        const bool ina = da >= 0.0, inb = db >= 0.0;
        if (ina)
        {
            out.pts.push_back(a);
            out.tag.push_back(p.tag[k]);
        }
        if (ina != inb)
        {
            out.pts.push_back(a + (b - a) * (da / (da - db)));
            out.tag.push_back(ina ? cutTag : p.tag[k]);
        }
    }
    dropDuplicates(out);
    if (out.pts.size() < 3 || signedArea(out.pts) <= 1e-6)
        return {};
    return out;
}
} // namespace

double signedArea(std::span<const Vec2> pts)
{
    double a = 0.0;
    for (size_t k = 0; k < pts.size(); ++k)
        a += cross(pts[k], pts[(k + 1) % pts.size()]);
    return 0.5 * a;
}

double area(const Polygon& p) { return std::abs(signedArea(p.pts)); }

Vec2 centroid(const Polygon& p)
{
    double a = 0.0;
    Vec2 c{0.0};
    for (size_t k = 0; k < p.pts.size(); ++k)
    {
        const Vec2 u = p.pts[k], v = p.pts[(k + 1) % p.pts.size()];
        const double w = cross(u, v);
        a += w;
        c += (u + v) * w;
    }
    if (std::abs(a) < 1e-12)
    {
        Vec2 m{0.0};
        for (const Vec2& q : p.pts)
            m += q;
        return p.pts.empty() ? m : m / double(p.pts.size());
    }
    return c / (3.0 * a);
}

bool contains(std::span<const Vec2> pts, Vec2 q)
{
    bool in = false;
    for (size_t i = 0, j = pts.size() - 1; i < pts.size(); j = i++)
        if ((pts[i].y > q.y) != (pts[j].y > q.y) &&
            q.x < (pts[j].x - pts[i].x) * (q.y - pts[i].y) / (pts[j].y - pts[i].y) + pts[i].x)
            in = !in;
    return in;
}

Polygon clipHalfPlane(const Polygon& p, Vec2 origin, Vec2 normal, int cutTag) { return clipTagged(p, origin, normal, cutTag); }

bool selfIntersects(std::span<const Vec2> pts)
{
    const size_t n = pts.size();
    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 2; j < n; ++j)
        {
            if (i == 0 && j == n - 1)
                continue;
            if (segmentsCross(pts[i], pts[(i + 1) % n], pts[j], pts[(j + 1) % n]))
                return true;
        }
    return false;
}

Polygon inset(const Polygon& p, std::span<const double> d)
{
    const size_t n = p.pts.size();
    if (n < 3)
        return {};
    std::vector<Vec2> dir(n), nrm(n);
    double maxD = 0.0;
    for (size_t k = 0; k < n; ++k)
    {
        const Vec2 e = p.pts[(k + 1) % n] - p.pts[k];
        const double len = glm::length(e);
        dir[k] = len > 1e-12 ? e / len : Vec2(1.0, 0.0);
        nrm[k] = leftNormal(dir[k]);
        maxD = std::max(maxD, d[k]);
    }
    Polygon out;
    out.tag = p.tag;
    bool ok = true;
    for (size_t k = 0; k < n; ++k)
    {
        const size_t j = (k + n - 1) % n;
        const Vec2 pj = p.pts[j] + nrm[j] * d[j], pk = p.pts[k] + nrm[k] * d[k];
        const double den = cross(dir[j], dir[k]);
        Vec2 v = pk;
        if (std::abs(den) > 1e-9)
            v = pj + dir[j] * (cross(pk - pj, dir[k]) / den);
        if (glm::length(v - p.pts[k]) > 4.0 * maxD + 1e-6)
            ok = false;
        out.pts.push_back(v);
    }
    if (ok)
        for (size_t k = 0; k < n && ok; ++k)
            ok = glm::dot(out.pts[(k + 1) % n] - out.pts[k], dir[k]) > 0.0;
    if (ok && signedArea(out.pts) > 1e-6 && !selfIntersects(out.pts))
        return out;

    Polygon cut = p;
    for (size_t k = 0; k < n && !cut.pts.empty(); ++k)
        cut = clipTagged(cut, p.pts[k] + nrm[k] * d[k], nrm[k], p.tag[k]);
    return cut;
}

Polygon makePolygon(std::vector<Vec2> pts, int tag)
{
    Polygon p;
    p.pts = std::move(pts);
    p.tag.assign(p.pts.size(), tag);
    dropDuplicates(p, 1e-3);
    for (size_t k = 0; k < p.pts.size() && p.pts.size() > 3;)
    {
        const size_t n = p.pts.size();
        const Vec2 a = p.pts[(k + n - 1) % n], b = p.pts[k], c = p.pts[(k + 1) % n];
        const double len = glm::length(c - a);
        if (len > 1e-9 && std::abs(cross(b - a, c - a)) / len < 1e-3)
        {
            p.pts.erase(p.pts.begin() + long(k));
            p.tag.erase(p.tag.begin() + long(k));
        }
        else
            ++k;
    }
    if (p.pts.size() < 3)
        return {};
    if (signedArea(p.pts) < 0.0)
        p = reversed(p);
    return p;
}

// ---------------------------------------------------------------------------------------------------------------------

Ground Ground::plane(double height)
{
    Ground g;
    g.mPlane = height;
    return g;
}

Ground Ground::fromMesh(const geo::Mesh& mesh, double cell)
{
    LILA_ZONE();

    Ground g;
    const geo::Bounds b = mesh.bounds();
    if (!b.valid() || mesh.numFaces() == 0)
        return g;
    const double ex = b.max.x - b.min.x, ez = b.max.z - b.min.z;
    g.mCell = std::max({cell, ex / 1023.0, ez / 1023.0, 1e-3});
    g.mOrigin = {b.min.x, b.min.z};
    g.mW = int(std::ceil(ex / g.mCell)) + 1;
    g.mH = int(std::ceil(ez / g.mCell)) + 1;
    g.mHeights.assign(size_t(g.mW) * size_t(g.mH), std::numeric_limits<float>::quiet_NaN());
    geo::MeshBvh bvh(mesh);
    const double top = b.max.y + 100.0, depth = (b.max.y - b.min.y) + 200.0;
    for (int z = 0; z < g.mH; ++z)
        for (int x = 0; x < g.mW; ++x)
        {
            geo::BvhHit hit;
            const glm::dvec3 o(g.mOrigin.x + x * g.mCell, top, g.mOrigin.y + z * g.mCell);
            if (bvh.raycast(o, {0.0, -1.0, 0.0}, hit, depth))
                g.mHeights[size_t(z) * size_t(g.mW) + size_t(x)] = float(hit.point.y);
        }
    g.mPlane = b.min.y;
    return g;
}

double Ground::height(Vec2 p) const
{
    if (mHeights.empty())
        return mPlane;
    const double fx = std::clamp((p.x - mOrigin.x) / mCell, 0.0, double(mW - 1));
    const double fz = std::clamp((p.y - mOrigin.y) / mCell, 0.0, double(mH - 1));
    const int x0 = std::min(int(fx), mW - 2 < 0 ? 0 : mW - 2), z0 = std::min(int(fz), mH - 2 < 0 ? 0 : mH - 2);
    const int x1 = std::min(x0 + 1, mW - 1), z1 = std::min(z0 + 1, mH - 1);
    const double tx = fx - x0, tz = fz - z0;
    const float h[4] = {mHeights[size_t(z0) * mW + x0], mHeights[size_t(z0) * mW + x1], mHeights[size_t(z1) * mW + x0],
                        mHeights[size_t(z1) * mW + x1]};
    const double w[4] = {(1 - tx) * (1 - tz), tx * (1 - tz), (1 - tx) * tz, tx * tz};
    double sum = 0.0, wsum = 0.0;
    for (int k = 0; k < 4; ++k)
        if (!std::isnan(h[k]))
        {
            sum += h[k] * w[k];
            wsum += w[k];
        }
    if (wsum <= 1e-12)
    {
        for (float v : h)
            if (!std::isnan(v))
                return v;
        return mPlane;
    }
    return sum / wsum;
}

double Ground::grade(Vec2 p) const
{
    if (mHeights.empty())
        return 0.0;
    const double c = mCell;
    const double gx = (height(p + Vec2(c, 0)) - height(p - Vec2(c, 0))) / (2.0 * c);
    const double gz = (height(p + Vec2(0, c)) - height(p - Vec2(0, c))) / (2.0 * c);
    return std::sqrt(gx * gx + gz * gz);
}

double Ground::gradeAlong(Vec2 a, Vec2 b) const
{
    if (mHeights.empty())
        return 0.0;
    const double len = glm::length(b - a);
    if (len < 1e-6)
        return 0.0;
    const int steps = std::clamp(int(len / mCell), 1, 16);
    double worst = 0.0, prev = height(a);
    for (int k = 1; k <= steps; ++k)
    {
        const double h = height(a + (b - a) * (double(k) / steps));
        worst = std::max(worst, std::abs(h - prev) / (len / steps));
        prev = h;
    }
    return worst;
}

bool Ground::solid(Vec2 p) const
{
    if (mHeights.empty())
        return true;
    const double fx = (p.x - mOrigin.x) / mCell, fz = (p.y - mOrigin.y) / mCell;
    if (fx < 0.0 || fz < 0.0 || fx > mW - 1 || fz > mH - 1)
        return false;
    const int x = int(std::lround(fx)), z = int(std::lround(fz));
    return !std::isnan(mHeights[size_t(z) * mW + x]);
}

uint64_t hash64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

double unitRandom(uint64_t k) { return double(hash64(k) >> 11) * (1.0 / 9007199254740992.0); }
} // namespace settlement
