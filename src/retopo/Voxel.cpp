#include "retopo/Retopo.h"

#include "core/Profile.h"
#include "geo/Bvh.h"
#include "geo/Ops.h"
#include "geo/SurfaceNets.h"

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

    // Zero counts as inside here; surface nets takes only negative values as solid.
    for (float& v : f)
        v = v <= 0.0f ? std::min(v, -1e-30f) : v;
    geo::Mesh out = geo::surfaceNetsMesh({{nx, ny, nz}, f}, origin, voxel);
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
