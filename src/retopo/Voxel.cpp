#include "retopo/Retopo.h"

#include "core/Profile.h"
#include "geo/Bvh.h"
#include "geo/Ops.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <deque>

namespace lila::retopo
{
geo::Mesh voxelRemesh(const geo::Mesh& mesh, const VoxelOptions& opts)
{
    LILA_ZONE();

    geo::Bounds b = mesh.bounds();
    if (!b.valid() || mesh.numFaces() == 0)
        return {};
    const int res = std::clamp(opts.resolution, 8, 1024);
    glm::dvec3 size = b.size();
    const double voxel = std::max({size.x, size.y, size.z, 1e-9}) / double(res);
    const glm::dvec3 origin = b.min - glm::dvec3(2.0 * voxel);
    const int nx = int(std::ceil(size.x / voxel)) + 5, ny = int(std::ceil(size.y / voxel)) + 5, nz = int(std::ceil(size.z / voxel)) + 5;
    auto at = [&](int i, int j, int k) { return origin + glm::dvec3(i, j, k) * voxel; };
    auto id = [&](int i, int j, int k) { return (size_t(k) * ny + j) * nx + i; };
    const size_t count = size_t(nx) * ny * nz;

    geo::MeshBvh bvh(mesh);
    // Wide enough that holes up to about two voxels close up.
    const double iso = 0.9 * voxel;
    const double far = 3.0 * voxel;
    std::vector<float> dist(count);
    tbb::parallel_for(0, nz, [&](int k) {
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i)
            {
                geo::BvhClosest c;
                dist[id(i, j, k)] = float(bvh.closestPoint(at(i, j, k), c, far) ? c.distance : far);
            }
    });

    // Outside is whatever the grid border reaches without crossing the band around the surface.
    std::vector<uint8_t> outside(count, 0);
    std::deque<std::array<int, 3>> queue;
    auto push = [&](int i, int j, int k) {
        size_t n = id(i, j, k);
        if (outside[n] || dist[n] <= iso)
            return;
        outside[n] = 1;
        queue.push_back({i, j, k});
    };
    for (int k = 0; k < nz; ++k)
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i)
                if (i == 0 || j == 0 || k == 0 || i == nx - 1 || j == ny - 1 || k == nz - 1)
                    push(i, j, k);
    while (!queue.empty())
    {
        auto [i, j, k] = queue.front();
        queue.pop_front();
        if (i > 0) push(i - 1, j, k);
        if (i + 1 < nx) push(i + 1, j, k);
        if (j > 0) push(i, j - 1, k);
        if (j + 1 < ny) push(i, j + 1, k);
        if (k > 0) push(i, j, k - 1);
        if (k + 1 < nz) push(i, j, k + 1);
    }
    std::vector<float> f(count);
    for (size_t n = 0; n < count; ++n)
        f[n] = outside[n] ? dist[n] - float(iso) : -std::max(std::abs(dist[n] - float(iso)), float(voxel * 1e-3));

    // Surface nets: a point per cell the surface crosses, at the mean of its edge crossings.
    std::vector<uint32_t> cellPoint(count, geo::kInvalid);
    std::vector<glm::dvec3> pts;
    static const int kCorner[8][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}, {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1}};
    static const int kEdge[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (int k = 0; k + 1 < nz; ++k)
        for (int j = 0; j + 1 < ny; ++j)
            for (int i = 0; i + 1 < nx; ++i)
            {
                float v[8];
                bool pos = false, neg = false;
                for (int c = 0; c < 8; ++c)
                {
                    v[c] = f[id(i + kCorner[c][0], j + kCorner[c][1], k + kCorner[c][2])];
                    (v[c] > 0.0f ? pos : neg) = true;
                }
                if (!pos || !neg)
                    continue;
                glm::dvec3 sum(0.0);
                int crossings = 0;
                for (const auto& e : kEdge)
                {
                    float a = v[e[0]], c = v[e[1]];
                    if ((a > 0.0f) == (c > 0.0f))
                        continue;
                    double t = double(a) / double(a - c);
                    glm::dvec3 pa(kCorner[e[0]][0], kCorner[e[0]][1], kCorner[e[0]][2]);
                    glm::dvec3 pc(kCorner[e[1]][0], kCorner[e[1]][1], kCorner[e[1]][2]);
                    sum += glm::mix(pa, pc, t);
                    ++crossings;
                }
                cellPoint[id(i, j, k)] = uint32_t(pts.size());
                pts.push_back(at(i, j, k) + sum / double(crossings) * voxel);
            }

    std::vector<geo::Index> sizes, idx;
    auto quad = [&](size_t c0, size_t c1, size_t c2, size_t c3, bool flip) {
        uint32_t q[4] = {cellPoint[c0], cellPoint[c1], cellPoint[c2], cellPoint[c3]};
        for (uint32_t x : q)
            if (x == geo::kInvalid)
                return;
        sizes.push_back(4);
        if (flip)
            idx.insert(idx.end(), {q[3], q[2], q[1], q[0]});
        else
            idx.insert(idx.end(), {q[0], q[1], q[2], q[3]});
    };
    for (int k = 1; k + 1 < nz; ++k)
        for (int j = 1; j + 1 < ny; ++j)
            for (int i = 1; i + 1 < nx; ++i)
            {
                bool inside = f[id(i, j, k)] <= 0.0f;
                // Grid edges along +x, +y, +z from this grid point; faces point from inside to outside.
                if (i + 1 < nx && inside != (f[id(i + 1, j, k)] <= 0.0f))
                    quad(id(i, j - 1, k - 1), id(i, j, k - 1), id(i, j, k), id(i, j - 1, k), !inside);
                if (j + 1 < ny && inside != (f[id(i, j + 1, k)] <= 0.0f))
                    quad(id(i - 1, j, k - 1), id(i - 1, j, k), id(i, j, k), id(i, j, k - 1), !inside);
                if (k + 1 < nz && inside != (f[id(i, j, k + 1)] <= 0.0f))
                    quad(id(i - 1, j - 1, k), id(i, j - 1, k), id(i, j, k), id(i - 1, j, k), !inside);
            }
    geo::Mesh out = geo::Mesh::fromPolygons(pts, sizes, idx);
    if (opts.project)
        tbb::parallel_for(geo::Index(0), out.pointCapacity(), [&](geo::Index p) {
            geo::BvhClosest c;
            if (out.pointValid(p) && bvh.closestPoint(out.position(p), c, 1.5 * voxel))
                out.setPosition(p, c.point);
        });
    out.removeAllLoose();
    out.compact();
    return out;
}
} // namespace lila::retopo
