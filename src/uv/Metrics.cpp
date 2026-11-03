#include "uv/Metrics.h"

#include "core/Profile.h"
#include "geo/Triangulate.h"
#include "uv/UvCommon.h"

#include <tbb/enumerable_thread_specific.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>

namespace lila::uv
{
namespace
{
using detail::Tri2;

bool separated(const Tri2& a, const Tri2& b, double eps)
{
    for (const Tri2* t : {&a, &b})
        for (int i = 0; i < 3; ++i)
        {
            glm::dvec2 e = (*t)[(i + 1) % 3] - (*t)[i];
            double len = glm::length(e);
            if (len <= 0.0)
                continue;
            glm::dvec2 n(-e.y / len, e.x / len);
            double a0 = std::numeric_limits<double>::max(), a1 = -a0, b0 = a0, b1 = -a0;
            for (int k = 0; k < 3; ++k)
            {
                double pa = glm::dot(a[k], n), pb = glm::dot(b[k], n);
                a0 = std::min(a0, pa);
                a1 = std::max(a1, pa);
                b0 = std::min(b0, pb);
                b1 = std::max(b1, pb);
            }
            if (a1 <= b0 + eps || b1 <= a0 + eps)
                return true;
        }
    return false;
}

double signedArea(const Tri2& t)
{
    glm::dvec2 e1 = t[1] - t[0], e2 = t[2] - t[0];
    return 0.5 * (e1.x * e2.y - e1.y * e2.x);
}

double area3(const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c)
{
    return 0.5 * glm::length(glm::cross(b - a, c - a));
}

double polyUvArea(const geo::Mesh& mesh, std::span<const glm::vec2> uv, Index f)
{
    double a = 0.0;
    mesh.forEachFaceCorner(f, [&](Index c) {
        glm::dvec2 p = uv[c], q = uv[mesh.cornerNext(c)];
        a += p.x * q.y - p.y * q.x;
    });
    return 0.5 * a;
}

double faceArea3(const geo::Mesh& mesh, Index f, const glm::dmat3& linear)
{
    glm::dvec3 n(0.0);
    mesh.forEachFaceCorner(f, [&](Index c) {
        glm::dvec3 p = linear * mesh.position(mesh.cornerPoint(c));
        glm::dvec3 q = linear * mesh.position(mesh.cornerNextPoint(c));
        n += glm::cross(p, q);
    });
    return 0.5 * glm::length(n);
}
} // namespace

size_t triangleOverlaps(std::span<const Tri2> tris, std::vector<uint8_t>& flags, std::span<const Index> group, bool sameGroupOk)
{
    LILA_ZONE();

    flags.assign(tris.size(), 0);
    if (tris.size() < 2)
        return 0;
    std::vector<glm::dvec2> lo(tris.size()), hi(tris.size());
    glm::dvec2 all0(std::numeric_limits<double>::max()), all1(-std::numeric_limits<double>::max());
    double sumExt = 0.0;
    for (size_t i = 0; i < tris.size(); ++i)
    {
        lo[i] = glm::min(tris[i][0], glm::min(tris[i][1], tris[i][2]));
        hi[i] = glm::max(tris[i][0], glm::max(tris[i][1], tris[i][2]));
        all0 = glm::min(all0, lo[i]);
        all1 = glm::max(all1, hi[i]);
        sumExt += std::max(hi[i].x - lo[i].x, hi[i].y - lo[i].y);
    }
    glm::dvec2 ext = all1 - all0;
    double eps = 1e-7 * std::max(ext.x, ext.y);
    double cell = std::max(sumExt / double(tris.size()) * 1.5, std::max(ext.x, ext.y) / 2048.0);
    if (cell <= 0.0)
        return 0;
    const int gx = std::max(1, int(std::ceil(ext.x / cell))), gy = std::max(1, int(std::ceil(ext.y / cell)));
    auto cellOf = [&](const glm::dvec2& p) {
        return glm::ivec2(std::clamp(int((p.x - all0.x) / cell), 0, gx - 1), std::clamp(int((p.y - all0.y) / cell), 0, gy - 1));
    };
    std::vector<uint32_t> start(size_t(gx) * gy + 1, 0);
    std::vector<uint8_t> skip(tris.size(), 0);
    for (size_t i = 0; i < tris.size(); ++i)
    {
        if (std::abs(signedArea(tris[i])) <= eps * eps)
        {
            skip[i] = 1;
            continue;
        }
        glm::ivec2 a = cellOf(lo[i]), b = cellOf(hi[i]);
        for (int y = a.y; y <= b.y; ++y)
            for (int x = a.x; x <= b.x; ++x)
                ++start[size_t(y) * gx + x + 1];
    }
    for (size_t i = 1; i < start.size(); ++i)
        start[i] += start[i - 1];
    std::vector<uint32_t> items(start.back());
    std::vector<uint32_t> fill(start.begin(), start.end() - 1);
    for (size_t i = 0; i < tris.size(); ++i)
    {
        if (skip[i])
            continue;
        glm::ivec2 a = cellOf(lo[i]), b = cellOf(hi[i]);
        for (int y = a.y; y <= b.y; ++y)
            for (int x = a.x; x <= b.x; ++x)
                items[fill[size_t(y) * gx + x]++] = uint32_t(i);
    }

    tbb::enumerable_thread_specific<std::vector<std::pair<uint32_t, uint32_t>>> found;
    tbb::parallel_for(size_t(0), size_t(gx) * gy, [&](size_t c) {
        const int cx = int(c % gx), cy = int(c / gx);
        auto& out = found.local();
        for (uint32_t s = start[c]; s < start[c + 1]; ++s)
            for (uint32_t t = s + 1; t < start[c + 1]; ++t)
            {
                uint32_t i = items[s], j = items[t];
                if (!group.empty() && sameGroupOk && group[i] == group[j])
                    continue;
                glm::dvec2 l = glm::max(lo[i], lo[j]), h = glm::min(hi[i], hi[j]);
                if (l.x >= h.x - eps || l.y >= h.y - eps)
                    continue;
                // Only the cell holding the overlap's lower corner reports the pair.
                if (cellOf(l) != glm::ivec2(cx, cy))
                    continue;
                if (!separated(tris[i], tris[j], eps))
                    out.emplace_back(i, j);
            }
    });
    size_t pairs = 0;
    for (auto& v : found)
        for (auto [i, j] : v)
        {
            flags[i] = flags[j] = 1;
            ++pairs;
        }
    return pairs;
}

std::vector<FaceDistortion> faceDistortion(const geo::Mesh& mesh, std::string_view uvSet, std::span<const Index> faces)
{
    LILA_ZONE();

    std::vector<FaceDistortion> out(mesh.faceCapacity());
    if (mesh.cornerAttrs().get<glm::vec2>(uvSet).empty())
        return out;
    ChartOptions co;
    co.source = IslandSource::Uvs;
    co.uvSet = uvSet;
    std::vector<Chart> charts = buildCharts(mesh, faces, co);
    tbb::parallel_for(size_t(0), charts.size(), [&](size_t ci) {
        const Chart& ch = charts[ci];
        double a3 = ch.area3d(), au = ch.areaUv();
        if (a3 <= 0.0 || std::abs(au) <= 0.0)
            return;
        double k = std::abs(au) / a3;
        double sign = au < 0.0 ? -1.0 : 1.0;
        std::vector<double> fu(ch.faces.size(), 0.0), f3(ch.faces.size(), 0.0), fa(ch.faces.size(), 0.0);
        std::vector<uint8_t> fold(ch.faces.size(), 0);
        size_t fi = 0;
        for (size_t t = 0; t < ch.tris.size(); ++t)
        {
            while (ch.faces[fi] != ch.triFace[t])
                ++fi;
            const auto& tri = ch.tris[t];
            glm::dvec3 p0 = ch.pos[tri[0]], p1 = ch.pos[tri[1]], p2 = ch.pos[tri[2]];
            double a = area3(p0, p1, p2);
            Tri2 u{ch.uv[tri[0]], ch.uv[tri[1]], ch.uv[tri[2]]};
            double su = signedArea(u) * sign;
            f3[fi] += a;
            fu[fi] += su;
            if (a <= 0.0)
                continue;
            if (su <= 0.0 && std::abs(su) > 1e-12 * k * a)
                fold[fi] = 1;
            // Singular values of the 3D-to-uv map from its first fundamental form.
            glm::dvec3 e1 = p1 - p0, e2 = p2 - p0;
            double l1 = glm::length(e1);
            if (l1 <= 0.0)
                continue;
            glm::dvec3 x = e1 / l1;
            double qx = glm::dot(e2, x), qy = glm::length(e2 - qx * x);
            if (qy <= 0.0)
                continue;
            glm::dvec2 d1 = u[1] - u[0], d2 = u[2] - u[0];
            // J maps (l1,0)->d1 and (qx,qy)->d2.
            glm::dvec2 c0 = d1 / l1;
            glm::dvec2 c1 = (d2 - qx * c0) / qy;
            double E = glm::dot(c0, c0), F = glm::dot(c0, c1), G = glm::dot(c1, c1);
            double disc = std::sqrt(std::max(0.0, (E - G) * (E - G) + 4.0 * F * F));
            double s1 = std::sqrt(std::max(0.0, 0.5 * (E + G + disc))), s2 = std::sqrt(std::max(0.0, 0.5 * (E + G - disc)));
            double aniso = s2 > 1e-12 ? s1 / s2 - 1.0 : 100.0;
            fa[fi] += a * std::min(aniso, 100.0);
        }
        for (size_t i = 0; i < ch.faces.size(); ++i)
        {
            FaceDistortion& d = out[ch.faces[i]];
            if (f3[i] > 0.0)
            {
                double ratio = std::max(fu[i], 1e-12 * k * f3[i]) / (k * f3[i]);
                d.area = float(std::log2(ratio));
                d.angle = float(fa[i] / f3[i]);
            }
            d.folded = fold[i] != 0;
        }
    });
    return out;
}

DistortionSummary summarize(const geo::Mesh& mesh, std::span<const FaceDistortion> d, std::span<const Index> faces)
{
    DistortionSummary s;
    double w = 0.0;
    for (Index f : liveFaces(mesh, faces))
    {
        double a = mesh.faceArea(f);
        s.meanArea += a * std::abs(d[f].area);
        s.meanAngle += a * d[f].angle;
        s.maxArea = std::max(s.maxArea, double(std::abs(d[f].area)));
        s.maxAngle = std::max(s.maxAngle, double(d[f].angle));
        s.folded += d[f].folded ? 1 : 0;
        w += a;
    }
    if (w > 0.0)
    {
        s.meanArea /= w;
        s.meanAngle /= w;
    }
    return s;
}

size_t overlappingFaces(const geo::Mesh& mesh, std::string_view uvSet, std::vector<uint8_t>& flags, std::span<const Index> faces)
{
    LILA_ZONE();

    flags.assign(mesh.faceCapacity(), 0);
    std::span<const glm::vec2> uv = mesh.cornerAttrs().get<glm::vec2>(uvSet);
    if (uv.empty())
        return 0;
    std::vector<Index> live = liveFaces(mesh, faces);
    std::vector<Tri2> tris;
    std::vector<Index> group;
    std::vector<Index> corners;
    for (Index f : live)
    {
        corners.clear();
        geo::triangulateFace(mesh, f, corners);
        for (size_t i = 0; i + 2 < corners.size(); i += 3)
        {
            tris.push_back({glm::dvec2(uv[corners[i]]), glm::dvec2(uv[corners[i + 1]]), glm::dvec2(uv[corners[i + 2]])});
            group.push_back(f);
        }
    }
    std::vector<uint8_t> triFlags;
    triangleOverlaps(tris, triFlags, group, true);
    for (size_t i = 0; i < tris.size(); ++i)
        if (triFlags[i])
            flags[group[i]] = 1;
    std::vector<FaceDistortion> d = faceDistortion(mesh, uvSet, live);
    for (Index f : live)
        if (d[f].folded)
            flags[f] = 1;
    size_t n = 0;
    for (Index f : live)
        n += flags[f] ? 1 : 0;
    return n;
}

bool chartOverlaps(const Chart& ch)
{
    std::vector<Tri2> tris(ch.tris.size());
    for (size_t i = 0; i < ch.tris.size(); ++i)
        tris[i] = {ch.uv[ch.tris[i][0]], ch.uv[ch.tris[i][1]], ch.uv[ch.tris[i][2]]};
    std::vector<uint8_t> flags;
    return triangleOverlaps(tris, flags, ch.triFace, true) > 0;
}

double texelDensity(const geo::Mesh& mesh, std::string_view uvSet, std::span<const Index> faces, double resolution, const glm::dmat3& linear)
{
    std::span<const glm::vec2> uv = mesh.cornerAttrs().get<glm::vec2>(uvSet);
    if (uv.empty())
        return 0.0;
    double au = 0.0, a3 = 0.0;
    for (Index f : liveFaces(mesh, faces))
    {
        au += std::abs(polyUvArea(mesh, uv, f));
        a3 += faceArea3(mesh, f, linear);
    }
    return a3 > 0.0 ? std::sqrt(au / a3) * resolution : 0.0;
}

void setTexelDensity(geo::Mesh& mesh, std::string_view uvSet, std::span<const Index> faces, double resolution, double density,
                     const glm::dmat3& linear)
{
    LILA_ZONE();

    if (mesh.cornerAttrs().get<glm::vec2>(uvSet).empty() || density <= 0.0)
        return;
    ChartOptions co;
    co.source = IslandSource::Uvs;
    co.uvSet = uvSet;
    std::vector<std::vector<Index>> islands = islandFaces(mesh, faces, co);
    std::span<glm::vec2> uv = uvLayer(mesh, uvSet);
    for (const std::vector<Index>& isl : islands)
    {
        double cur = texelDensity(mesh, uvSet, isl, resolution, linear);
        if (cur <= 0.0)
            continue;
        float s = float(density / cur);
        glm::vec2 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
        for (Index f : isl)
            mesh.forEachFaceCorner(f, [&](Index c) {
                lo = glm::min(lo, uv[c]);
                hi = glm::max(hi, uv[c]);
            });
        glm::vec2 mid = (lo + hi) * 0.5f;
        for (Index f : isl)
            mesh.forEachFaceCorner(f, [&](Index c) { uv[c] = mid + (uv[c] - mid) * s; });
    }
}
} // namespace lila::uv
