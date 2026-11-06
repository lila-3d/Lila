#include "geo/SurfaceNets.h"

#include "core/Profile.h"

#include <tbb/parallel_for.h>

#include <algorithm>

namespace lila::geo
{
SurfaceNets surfaceNets(const SurfaceNetsGrid& g, const SurfaceNetsOptions& o)
{
    LILA_ZONE();

    SurfaceNets out;
    const glm::ivec3 n = g.dims;
    if (n.x < 2 || n.y < 2 || n.z < 2 || g.values.size() < size_t(n.x) * size_t(n.y) * size_t(n.z))
        return out;
    // Points whose edges may make quads, then the cells those quads join: one cell below the range on each axis.
    const glm::ivec3 fMin = glm::max(o.faceMin, glm::ivec3(0)), fMax = glm::min(o.faceMax, n);
    if (fMin.x >= fMax.x || fMin.y >= fMax.y || fMin.z >= fMax.z)
        return out;
    const glm::ivec3 cMin = glm::max(fMin - 1, glm::ivec3(0)), cMax = glm::min(fMax, n - 1);
    const glm::ivec3 cn = glm::max(cMax - cMin, glm::ivec3(0));
    const size_t cellCount = size_t(cn.x) * size_t(cn.y) * size_t(cn.z);
    if (cellCount == 0)
        return out;
    auto cellIndex = [&](int i, int j, int k) { return size_t(i - cMin.x) + size_t(cn.x) * (size_t(j - cMin.y) + size_t(cn.y) * size_t(k - cMin.z)); };
    auto solid = [&](int i, int j, int k) { return g.at(i, j, k) < 0.0f; };

    struct Found
    {
        size_t cell;
        glm::ivec3 at;
        glm::dvec3 offset;
    };
    std::vector<std::vector<Found>> slices(size_t(cn.z));
    tbb::parallel_for(0, cn.z, [&](int sk) {
        const int k = cMin.z + sk;
        std::vector<Found>& found = slices[size_t(sk)];
        for (int j = cMin.y; j < cMax.y; ++j)
            for (int i = cMin.x; i < cMax.x; ++i)
            {
                float v[8];
                int inside = 0;
                for (int c = 0; c < 8; ++c)
                {
                    v[c] = g.at(i + (c & 1), j + ((c >> 1) & 1), k + ((c >> 2) & 1));
                    inside += v[c] < 0.0f ? 1 : 0;
                }
                if (inside == 0 || inside == 8)
                    continue;
                glm::dvec3 sum(0.0);
                int count = 0;
                for (int a = 0; a < 8; ++a)
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        const int b = a | (1 << axis);
                        if (b == a || (v[a] < 0.0f) == (v[b] < 0.0f))
                            continue;
                        glm::dvec3 p(a & 1, (a >> 1) & 1, (a >> 2) & 1);
                        p[axis] += double(v[a]) / (double(v[a]) - double(v[b]));
                        sum += p;
                        ++count;
                    }
                found.push_back({cellIndex(i, j, k), {i, j, k}, sum / double(count)});
            }
    });

    std::vector<Index> cellPoint(cellCount, kInvalid);
    std::vector<glm::ivec3> cells;
    std::vector<glm::dvec3> offsets;
    for (const auto& slice : slices)
        for (const Found& f : slice)
        {
            cellPoint[f.cell] = Index(cells.size());
            cells.push_back(f.at);
            offsets.push_back(f.offset);
        }

    // Each edge crossing the surface joins the four cells around it, facing from solid to empty.
    struct Quad
    {
        Index p[4];
        glm::ivec4 edge;
    };
    std::vector<std::vector<Quad>> quadSlices(size_t(fMax.z - fMin.z));
    tbb::parallel_for(fMin.z, fMax.z, [&](int k) {
        std::vector<Quad>& qs = quadSlices[size_t(k - fMin.z)];
        auto quad = [&](bool lowSolid, glm::ivec4 edge, glm::ivec3 c0, glm::ivec3 c1, glm::ivec3 c2, glm::ivec3 c3) {
            for (const glm::ivec3& c : {c0, c1, c2, c3})
                if (glm::any(glm::lessThan(c, cMin)) || glm::any(glm::greaterThanEqual(c, cMax)))
                    return;
            Quad q{{cellPoint[cellIndex(c0.x, c0.y, c0.z)], cellPoint[cellIndex(c1.x, c1.y, c1.z)], cellPoint[cellIndex(c2.x, c2.y, c2.z)],
                    cellPoint[cellIndex(c3.x, c3.y, c3.z)]},
                   edge};
            for (Index p : q.p)
                if (p == kInvalid)
                    return;
            if (!lowSolid)
                std::swap(q.p[1], q.p[3]);
            qs.push_back(q);
        };
        for (int j = fMin.y; j < fMax.y; ++j)
            for (int i = fMin.x; i < fMax.x; ++i)
            {
                const bool here = solid(i, j, k);
                if (i + 1 < n.x && here != solid(i + 1, j, k))
                    quad(here, {i, j, k, 0}, {i, j - 1, k - 1}, {i, j, k - 1}, {i, j, k}, {i, j - 1, k});
                if (j + 1 < n.y && here != solid(i, j + 1, k))
                    quad(here, {i, j, k, 1}, {i - 1, j, k - 1}, {i - 1, j, k}, {i, j, k}, {i, j, k - 1});
                if (k + 1 < n.z && here != solid(i, j, k + 1))
                    quad(here, {i, j, k, 2}, {i - 1, j - 1, k}, {i, j - 1, k}, {i, j, k}, {i - 1, j, k});
            }
    });

    // Keep only the points quads use, in cell order.
    std::vector<Index> remap(cells.size(), kInvalid);
    for (const auto& qs : quadSlices)
        for (const Quad& q : qs)
            for (Index p : q.p)
                remap[p] = 0;
    for (size_t p = 0; p < cells.size(); ++p)
        if (remap[p] != kInvalid)
        {
            remap[p] = Index(out.cells.size());
            out.cells.push_back(cells[p]);
            out.offsets.push_back(offsets[p]);
        }
    for (const auto& qs : quadSlices)
        for (const Quad& q : qs)
        {
            for (Index p : q.p)
                out.quads.push_back(remap[p]);
            out.edges.push_back(q.edge);
        }
    return out;
}

Mesh surfaceNetsMesh(const SurfaceNetsGrid& grid, const glm::dvec3& origin, double voxel, const SurfaceNetsOptions& opts)
{
    LILA_ZONE();

    const SurfaceNets nets = surfaceNets(grid, opts);
    std::vector<glm::dvec3> pts(nets.cells.size());
    for (size_t p = 0; p < pts.size(); ++p)
        pts[p] = origin + nets.point(p) * voxel;
    const std::vector<Index> sizes(nets.numQuads(), 4);
    return Mesh::fromPolygons(pts, sizes, nets.quads);
}
} // namespace lila::geo
