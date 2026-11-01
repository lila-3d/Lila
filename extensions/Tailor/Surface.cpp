#include "TailorCore.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <map>
#include <numeric>

namespace tailor
{
namespace
{
glm::vec3 toVec(const Vec3& v) { return {v[0], v[1], v[2]}; }

int root(std::vector<int>& parent, int i)
{
    while (parent[i] != i)
    {
        parent[i] = parent[parent[i]];
        i = parent[i];
    }
    return i;
}
} // namespace

GarmentSurface garmentSurface(const ClothMesh& mesh, const std::vector<Vec3>& positions, const std::vector<Vec2>& packedUv,
                              const SurfaceOptions& options)
{
    const int n = int(std::min(positions.size(), mesh.rest3d.size()));
    std::vector<int> parent(n);
    std::iota(parent.begin(), parent.end(), 0);
    if (options.weldSeams)
        for (const auto& pr : mesh.seamPairs)
            if (pr[0] >= 0 && pr[1] >= 0 && pr[0] < n && pr[1] < n)
            {
                const int a = root(parent, pr[0]), b = root(parent, pr[1]);
                parent[std::max(a, b)] = std::min(a, b);
            }

    GarmentSurface out;
    std::vector<int> index(n, -1);
    std::vector<glm::vec3> sum;
    std::vector<int> count;
    for (int i = 0; i < n; ++i)
    {
        const int r = root(parent, i);
        if (index[r] < 0)
        {
            index[r] = int(sum.size());
            sum.emplace_back(0.f);
            count.push_back(0);
        }
        index[i] = index[r];
        sum[index[i]] += toVec(positions[i]);
        ++count[index[i]];
    }
    out.points.resize(sum.size());
    for (size_t k = 0; k < sum.size(); ++k)
    {
        const glm::vec3 p = sum[k] / float(count[k]);
        out.points[k] = {p.x, p.y, p.z};
    }

    auto patternUv = [&](int v) { return size_t(v) < mesh.uv.size() ? mesh.uv[v] : Vec2{0.f, 0.f}; };
    for (size_t t = 0; t < mesh.tris.size(); ++t)
    {
        const auto& tri = mesh.tris[t];
        if (tri[0] >= n || tri[1] >= n || tri[2] >= n)
            continue;
        const std::array<int, 3> w{index[tri[0]], index[tri[1]], index[tri[2]]};
        if (w[0] == w[1] || w[1] == w[2] || w[0] == w[2])
            continue;
        std::array<Vec2, 3> fabric, packed;
        for (int k = 0; k < 3; ++k)
        {
            fabric[k] = patternUv(tri[k]);
            packed[k] = 3 * t + k < packedUv.size() ? packedUv[3 * t + k] : fabric[k];
        }
        out.tris.push_back(w);
        out.uv.push_back(packed);
        out.fabricUv.push_back(fabric);
        out.triPanel.push_back(t < mesh.triPanel.size() ? mesh.triPanel[t] : 0);
        out.triSide.push_back(GarmentSurface::Outer);
    }
    if (options.thickness <= 0.f)
        return out;

    const int np = int(out.points.size());
    const size_t nt = out.tris.size();
    std::vector<glm::vec3> normal(np, glm::vec3(0.f));
    for (const auto& t : out.tris)
    {
        const glm::vec3 a = toVec(out.points[t[0]]), b = toVec(out.points[t[1]]), c = toVec(out.points[t[2]]);
        const glm::vec3 an = glm::cross(b - a, c - a);
        for (int v : t)
            normal[v] += an;
    }
    for (int i = 0; i < np; ++i)
    {
        const float l = glm::length(normal[i]);
        const glm::vec3 p = toVec(out.points[i]) - (l > 1e-20f ? normal[i] / l : glm::vec3(0.f)) * options.thickness;
        out.points.push_back({p.x, p.y, p.z});
    }

    // Each undirected edge with its directed uses; an edge used once is a border to close with a rim.
    std::map<std::pair<int, int>, std::vector<std::pair<size_t, int>>> edges;
    for (size_t t = 0; t < nt; ++t)
        for (int k = 0; k < 3; ++k)
        {
            const int a = out.tris[t][k], b = out.tris[t][(k + 1) % 3];
            edges[{std::min(a, b), std::max(a, b)}].push_back({t, k});
        }

    for (size_t t = 0; t < nt; ++t)
    {
        const auto tri = out.tris[t];
        const auto uv = out.uv[t], fabric = out.fabricUv[t];
        out.tris.push_back({tri[0] + np, tri[2] + np, tri[1] + np});
        out.uv.push_back({uv[0], uv[2], uv[1]});
        out.fabricUv.push_back({fabric[0], fabric[2], fabric[1]});
        out.triPanel.push_back(out.triPanel[t]);
        out.triSide.push_back(GarmentSurface::Inner);
    }
    for (const auto& [key, uses] : edges)
    {
        if (uses.size() != 1)
            continue;
        const auto [t, k] = uses.front();
        const int a = out.tris[t][k], b = out.tris[t][(k + 1) % 3];
        const Vec2 ua = out.uv[t][k], ub = out.uv[t][(k + 1) % 3];
        const Vec2 fa = out.fabricUv[t][k], fb = out.fabricUv[t][(k + 1) % 3];
        out.tris.push_back({b, a, a + np});
        out.tris.push_back({b, a + np, b + np});
        out.uv.push_back({ub, ua, ua});
        out.uv.push_back({ub, ua, ub});
        out.fabricUv.push_back({fb, fa, fa});
        out.fabricUv.push_back({fb, fa, fb});
        for (int r = 0; r < 2; ++r)
        {
            out.triPanel.push_back(out.triPanel[t]);
            out.triSide.push_back(GarmentSurface::Rim);
        }
    }
    return out;
}

} // namespace tailor

namespace tailor
{
namespace
{
/** Folded seams are sewn as pressed: this much stiffer than the fabric's own bending. */
constexpr float kSeamStiffening = 10.f;
} // namespace

std::vector<SeamHinge> seamHinges(const ClothMesh& mesh, const Garment& g)
{
    std::vector<SeamHinge> out;
    if (mesh.seamOfPair.size() != mesh.seamPairs.size())
        return out;
    // Each edge with its triangle and the triangle's third vertex; seam edges lie on the outline, so they have one.
    std::map<std::pair<int, int>, std::pair<int, int>> wing;
    for (size_t t = 0; t < mesh.tris.size(); ++t)
        for (int k = 0; k < 3; ++k)
        {
            const int a = mesh.tris[t][k], b = mesh.tris[t][(k + 1) % 3];
            wing[{std::min(a, b), std::max(a, b)}] = {int(t), mesh.tris[t][(k + 2) % 3]};
        }
    auto find = [&](int a, int b) -> const std::pair<int, int>* {
        const auto it = wing.find({std::min(a, b), std::max(a, b)});
        return it == wing.end() ? nullptr : &it->second;
    };
    auto bendOf = [&](int tri) {
        const int panel = size_t(tri) < mesh.triPanel.size() ? mesh.triPanel[size_t(tri)] : 0;
        const int f = panel >= 0 && size_t(panel) < g.panels.size() ? g.panels[size_t(panel)].fabric : 0;
        const Fabric fab = f >= 0 && size_t(f) < g.fabrics.size() ? g.fabrics[size_t(f)] : Fabric{};
        return std::max(fab.bendWarp, fab.bendWeft);
    };
    for (size_t k = 0; k + 1 < mesh.seamPairs.size(); ++k)
    {
        const int s = mesh.seamOfPair[k];
        if (s != mesh.seamOfPair[k + 1] || s < 0 || size_t(s) >= g.seams.size() || g.seams[size_t(s)].foldAngle == 0.f)
            continue;
        const auto [a0, b0] = mesh.seamPairs[k];
        const auto [a1, b1] = mesh.seamPairs[k + 1];
        const auto* wa = find(a0, a1);
        const auto* wb = find(b0, b1);
        if (!wa || !wb)
            continue;
        SeamHinge h;
        h.v = {a0, a1, wa->second, wb->second};
        h.tris = {wa->first, wb->first};
        h.restAngle = g.seams[size_t(s)].foldAngle;
        h.bend = std::max(bendOf(wa->first), bendOf(wb->first)) * kSeamStiffening;
        out.push_back(h);
    }
    return out;
}
} // namespace tailor

namespace tailor
{
std::vector<SeamShrink> seamShrinks(const ClothMesh& mesh, const Garment& g)
{
    std::vector<SeamShrink> out;
    if (mesh.seamOfPair.size() != mesh.seamPairs.size())
        return out;
    auto restLen = [&](int a, int b) {
        const Vec3 &p = mesh.rest3d[size_t(a)], &q = mesh.rest3d[size_t(b)];
        return std::hypot(p[0] - q[0], p[1] - q[1], p[2] - q[2]);
    };
    for (size_t k = 0; k + 1 < mesh.seamPairs.size(); ++k)
    {
        const int s = mesh.seamOfPair[k];
        if (s != mesh.seamOfPair[k + 1] || s < 0 || size_t(s) >= g.seams.size() || g.seams[size_t(s)].shrink >= 1.f)
            continue;
        const float shrink = std::max(g.seams[size_t(s)].shrink, 0.05f);
        for (int side = 0; side < 2; ++side)
        {
            const int a = mesh.seamPairs[k][size_t(side)], b = mesh.seamPairs[k + 1][size_t(side)];
            if (a != b)
                out.push_back({a, b, shrink * restLen(a, b)});
        }
    }
    return out;
}
} // namespace tailor
