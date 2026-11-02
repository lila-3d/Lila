#include "TailorCore.h"

#include <glm/glm.hpp>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace tailor
{
namespace
{
using glm::vec3;

vec3 v3(const Vec3& a) { return {a[0], a[1], a[2]}; }

/** Closest point on a triangle (Ericson), as barycentric weights. */
vec3 closestWeights(const vec3& p, const vec3& a, const vec3& b, const vec3& c)
{
    const vec3 ab = b - a, ac = c - a, ap = p - a;
    const float d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
    if (d1 <= 0.f && d2 <= 0.f)
        return {1, 0, 0};
    const vec3 bp = p - b;
    const float d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
    if (d3 >= 0.f && d4 <= d3)
        return {0, 1, 0};
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.f && d1 >= 0.f && d3 <= 0.f)
    {
        const float v = d1 / (d1 - d3);
        return {1 - v, v, 0};
    }
    const vec3 cp = p - c;
    const float d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
    if (d6 >= 0.f && d5 <= d6)
        return {0, 0, 1};
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.f && d2 >= 0.f && d6 <= 0.f)
    {
        const float w = d2 / (d2 - d6);
        return {1 - w, 0, w};
    }
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.f && (d4 - d3) >= 0.f && (d5 - d6) >= 0.f)
    {
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return {0, 1 - w, w};
    }
    const float denom = 1.f / (va + vb + vc);
    const float v = vb * denom, w = vc * denom;
    return {1 - v - w, v, w};
}

std::vector<vec3> smoothNormals(const std::vector<Vec3>& x, const std::vector<std::array<int, 3>>& tris)
{
    std::vector<vec3> n(x.size(), vec3(0.f));
    for (const auto& t : tris)
    {
        const vec3 a = v3(x[size_t(t[0])]), b = v3(x[size_t(t[1])]), c = v3(x[size_t(t[2])]);
        const vec3 an = glm::cross(b - a, c - a);
        for (int v : t)
            n[size_t(v)] += an;
    }
    for (vec3& v : n)
    {
        const float l = glm::length(v);
        v = l > 1e-20f ? v / l : vec3(0.f, 1.f, 0.f);
    }
    return n;
}

/** The foot point and its frame: smoothed normal, the first edge made orthogonal to it, and their cross. */
void footFrame(const std::vector<Vec3>& x, const std::vector<vec3>& normals, const std::array<int, 3>& t, const vec3& w,
               vec3& foot, vec3& tangent, vec3& bitangent, vec3& normal)
{
    const vec3 a = v3(x[size_t(t[0])]), b = v3(x[size_t(t[1])]), c = v3(x[size_t(t[2])]);
    foot = a * w.x + b * w.y + c * w.z;
    normal = normals[size_t(t[0])] * w.x + normals[size_t(t[1])] * w.y + normals[size_t(t[2])] * w.z;
    const float nl = glm::length(normal);
    normal = nl > 1e-20f ? normal / nl : glm::normalize(glm::cross(b - a, c - a));
    vec3 e = b - a;
    e -= normal * glm::dot(e, normal);
    const float el = glm::length(e);
    tangent = el > 1e-20f ? e / el : vec3(1.f, 0.f, 0.f);
    bitangent = glm::cross(normal, tangent);
}
} // namespace

RenderBinding bindRenderMesh(const std::vector<Vec3>& simPositions, const std::vector<std::array<int, 3>>& simTris,
                             const std::vector<Vec3>& renderPoints)
{
    RenderBinding b;
    const size_t n = renderPoints.size();
    b.tri.assign(n, -1);
    b.bary.assign(n, {1.f, 0.f, 0.f});
    b.local = renderPoints;
    if (simTris.empty())
        return b;

    // Triangles bucketed on a grid by their bounds, searched ring by ring around each point.
    float edge = 0.f;
    for (const auto& t : simTris)
        edge += glm::length(v3(simPositions[size_t(t[1])]) - v3(simPositions[size_t(t[0])]));
    const float cell = std::max(2.f * edge / float(simTris.size()), 1e-4f);
    auto key = [](int x, int y, int z) {
        return (uint64_t(uint32_t(x + (1 << 20))) << 42) ^ (uint64_t(uint32_t(y + (1 << 20))) << 21) ^ uint64_t(uint32_t(z + (1 << 20)));
    };
    auto cellOf = [&](float v) { return int(std::floor(v / cell)); };
    std::unordered_map<uint64_t, std::vector<int>> grid;
    for (size_t t = 0; t < simTris.size(); ++t)
    {
        vec3 lo(1e30f), hi(-1e30f);
        for (int v : simTris[t])
        {
            lo = glm::min(lo, v3(simPositions[size_t(v)]));
            hi = glm::max(hi, v3(simPositions[size_t(v)]));
        }
        for (int z = cellOf(lo.z); z <= cellOf(hi.z); ++z)
            for (int y = cellOf(lo.y); y <= cellOf(hi.y); ++y)
                for (int x = cellOf(lo.x); x <= cellOf(hi.x); ++x)
                    grid[key(x, y, z)].push_back(int(t));
    }
    const std::vector<vec3> normals = smoothNormals(simPositions, simTris);

    tbb::parallel_for(tbb::blocked_range<size_t>(0, n, 256), [&](const tbb::blocked_range<size_t>& r) {
        for (size_t i = r.begin(); i != r.end(); ++i)
        {
            const vec3 p = v3(renderPoints[i]);
            const int cx = cellOf(p.x), cy = cellOf(p.y), cz = cellOf(p.z);
            float best = 1e30f;
            int bestTri = -1;
            vec3 bestW(1.f, 0.f, 0.f);
            auto test = [&](int t) {
                const auto& tri = simTris[size_t(t)];
                const vec3 a = v3(simPositions[size_t(tri[0])]), bb = v3(simPositions[size_t(tri[1])]), c = v3(simPositions[size_t(tri[2])]);
                const vec3 w = closestWeights(p, a, bb, c);
                const float d = glm::length(p - (a * w.x + bb * w.y + c * w.z));
                if (d < best)
                    best = d, bestTri = t, bestW = w;
            };
            // Rings outwards until the nearest found is closer than anything a further ring could hold.
            for (int ring = 0; ring < 64; ++ring)
            {
                for (int z = cz - ring; z <= cz + ring; ++z)
                    for (int y = cy - ring; y <= cy + ring; ++y)
                        for (int x = cx - ring; x <= cx + ring; ++x)
                        {
                            if (std::max({std::abs(x - cx), std::abs(y - cy), std::abs(z - cz)}) != ring)
                                continue;
                            if (const auto it = grid.find(key(x, y, z)); it != grid.end())
                                for (int t : it->second)
                                    test(t);
                        }
                if (bestTri >= 0 && best < float(ring) * cell)
                    break;
            }
            if (bestTri < 0)
                for (size_t t = 0; t < simTris.size(); ++t)
                    test(int(t));
            vec3 foot, tangent, bitangent, normal;
            footFrame(simPositions, normals, simTris[size_t(bestTri)], bestW, foot, tangent, bitangent, normal);
            const vec3 d = p - foot;
            b.tri[i] = bestTri;
            b.bary[i] = {bestW.x, bestW.y, bestW.z};
            b.local[i] = {glm::dot(d, tangent), glm::dot(d, bitangent), glm::dot(d, normal)};
        }
    });
    return b;
}

RenderBinding bindRigidPieces(const std::vector<Vec3>& simPositions, const std::vector<std::array<int, 3>>& simTris,
                              const std::vector<Vec3>& renderPoints, const std::vector<std::array<int, 3>>& renderTris)
{
    const size_t n = renderPoints.size();
    std::vector<int> parent(n);
    for (size_t i = 0; i < n; ++i)
        parent[i] = int(i);
    auto root = [&](int i) {
        while (parent[size_t(i)] != i)
            i = parent[size_t(i)] = parent[size_t(parent[size_t(i)])];
        return i;
    };
    for (const auto& t : renderTris)
        for (int k = 1; k < 3; ++k)
            parent[size_t(root(t[size_t(k)]))] = root(t[0]);
    std::vector<int> piece(n);
    std::vector<vec3> sum;
    std::vector<int> count, index(n, -1);
    for (size_t i = 0; i < n; ++i)
    {
        const int r = root(int(i));
        if (index[size_t(r)] < 0)
        {
            index[size_t(r)] = int(sum.size());
            sum.push_back(vec3(0.f));
            count.push_back(0);
        }
        piece[i] = index[size_t(r)];
        sum[size_t(piece[i])] += v3(renderPoints[i]);
        ++count[size_t(piece[i])];
    }
    std::vector<Vec3> middles(sum.size());
    for (size_t k = 0; k < sum.size(); ++k)
    {
        const vec3 c = sum[k] / float(std::max(count[k], 1));
        middles[k] = {c.x, c.y, c.z};
    }
    // Each piece's middle binds as a point would; its points then share that foot and frame.
    const RenderBinding at = bindRenderMesh(simPositions, simTris, middles);
    RenderBinding b;
    b.tri.assign(n, -1);
    b.bary.assign(n, {1.f, 0.f, 0.f});
    b.local = renderPoints;
    if (simTris.empty())
        return b;
    const std::vector<vec3> normals = smoothNormals(simPositions, simTris);
    for (size_t i = 0; i < n; ++i)
    {
        const size_t k = size_t(piece[i]);
        if (at.tri[k] < 0)
            continue;
        vec3 foot, tangent, bitangent, normal;
        footFrame(simPositions, normals, simTris[size_t(at.tri[k])], {at.bary[k][0], at.bary[k][1], at.bary[k][2]}, foot, tangent, bitangent, normal);
        const vec3 d = v3(renderPoints[i]) - foot;
        b.tri[i] = at.tri[k];
        b.bary[i] = at.bary[k];
        b.local[i] = {glm::dot(d, tangent), glm::dot(d, bitangent), glm::dot(d, normal)};
    }
    return b;
}

std::vector<Vec3> deformRenderMesh(const RenderBinding& b, const std::vector<Vec3>& simPositions,
                                   const std::vector<std::array<int, 3>>& simTris)
{
    const std::vector<vec3> normals = smoothNormals(simPositions, simTris);
    std::vector<Vec3> out(b.tri.size());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, out.size(), 1024), [&](const tbb::blocked_range<size_t>& r) {
        for (size_t i = r.begin(); i != r.end(); ++i)
        {
            // Unbound points (no sim triangles at bind time) keep their own position.
            if (b.tri[i] < 0 || size_t(b.tri[i]) >= simTris.size())
            {
                out[i] = b.local[i];
                continue;
            }
            vec3 foot, tangent, bitangent, normal;
            const auto& w = b.bary[i];
            footFrame(simPositions, normals, simTris[size_t(b.tri[i])], {w[0], w[1], w[2]}, foot, tangent, bitangent, normal);
            const vec3 p = foot + tangent * b.local[i][0] + bitangent * b.local[i][1] + normal * b.local[i][2];
            out[i] = {p.x, p.y, p.z};
        }
    });
    return out;
}
} // namespace tailor
