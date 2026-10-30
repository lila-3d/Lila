#include "gn/Volume.h"

#include "core/Profile.h"
#include "geo/Bvh.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace lila::gn
{
namespace volume_detail
{
constexpr size_t kMaxVoxels = size_t(64) << 20;

/** Index-space crossings and quads of surface nets, before they are moved to world space. */
struct Nets
{
    std::vector<glm::dvec3> points;
    std::vector<geo::Index> quads;
};

Nets surfaceNets(const Volume& v, double threshold)
{
    const int nx = v.dims.x, ny = v.dims.y, nz = v.dims.z;
    const bool sdf = v.isSdf();
    // Positive where solid.
    auto s = [&](int i, int j, int k) {
        double val = v.at(i, j, k);
        return sdf ? threshold - val : val - threshold;
    };
    // Cells run from -1 to n - 1 on each axis, so the grid's border closes against the background.
    const size_t cx = size_t(nx) + 1, cy = size_t(ny) + 1, cz = size_t(nz) + 1;
    auto cellIndex = [&](int i, int j, int k) { return size_t(i + 1) + cx * (size_t(j + 1) + cy * size_t(k + 1)); };

    std::vector<std::vector<std::pair<size_t, glm::dvec3>>> slices(cz);
    tbb::parallel_for(size_t(0), cz, [&](size_t sk) {
        int k = int(sk) - 1;
        auto& out = slices[sk];
        for (int j = -1; j < ny; ++j)
            for (int i = -1; i < nx; ++i)
            {
                double c[8];
                int solid = 0;
                for (int n = 0; n < 8; ++n)
                {
                    c[n] = s(i + (n & 1), j + ((n >> 1) & 1), k + ((n >> 2) & 1));
                    solid += c[n] > 0.0 ? 1 : 0;
                }
                if (solid == 0 || solid == 8)
                    continue;
                glm::dvec3 sum(0.0);
                int count = 0;
                for (int a = 0; a < 8; ++a)
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        int b = a | (1 << axis);
                        if (b == a || (c[a] > 0.0) == (c[b] > 0.0))
                            continue;
                        double t = c[a] / (c[a] - c[b]);
                        glm::dvec3 p(i + (a & 1), j + ((a >> 1) & 1), k + ((a >> 2) & 1));
                        p[axis] += t;
                        sum += p;
                        ++count;
                    }
                out.emplace_back(cellIndex(i, j, k), sum / double(count));
            }
    });

    Nets nets;
    std::vector<geo::Index> cellVert(cx * cy * cz, geo::kInvalid);
    for (const auto& slice : slices)
        for (const auto& [cell, p] : slice)
        {
            cellVert[cell] = geo::Index(nets.points.size());
            nets.points.push_back(p);
        }

    auto quad = [&](bool lowSolid, size_t a, size_t b, size_t c, size_t d) {
        geo::Index q[4] = {cellVert[a], cellVert[b], cellVert[c], cellVert[d]};
        if (!lowSolid)
            std::swap(q[1], q[3]);
        nets.quads.insert(nets.quads.end(), q, q + 4);
    };
    // Each edge crossing the surface gets the quad joining the four cells around it, facing from solid to empty.
    for (int k = -1; k < nz; ++k)
        for (int j = -1; j < ny; ++j)
            for (int i = -1; i < nx; ++i)
            {
                bool here = s(i, j, k) > 0.0;
                if (j >= 0 && k >= 0 && j < ny && k < nz && here != (s(i + 1, j, k) > 0.0))
                    quad(here, cellIndex(i, j - 1, k - 1), cellIndex(i, j, k - 1), cellIndex(i, j, k), cellIndex(i, j - 1, k));
                if (i >= 0 && k >= 0 && i < nx && k < nz && here != (s(i, j + 1, k) > 0.0))
                    quad(here, cellIndex(i - 1, j, k - 1), cellIndex(i - 1, j, k), cellIndex(i, j, k), cellIndex(i, j, k - 1));
                if (i >= 0 && j >= 0 && i < nx && j < ny && here != (s(i, j, k + 1) > 0.0))
                    quad(here, cellIndex(i - 1, j - 1, k), cellIndex(i, j - 1, k), cellIndex(i, j, k), cellIndex(i - 1, j, k));
            }
    return nets;
}
} // namespace volume_detail

Volume Volume::grid(const geo::Bounds& box, double voxelSize, Kind kind, float background)
{
    if (!(voxelSize > 0.0))
        throw std::runtime_error("voxel size must be above zero");
    Volume v;
    v.kind = kind;
    v.name = kind == Kind::Sdf ? "sdf" : "density";
    v.background = background;
    glm::dvec3 size = box.valid() ? box.size() : glm::dvec3(0.0);
    glm::dvec3 origin = box.valid() ? box.min : glm::dvec3(0.0);
    for (int a = 0; a < 3; ++a)
        v.dims[a] = int(std::ceil(size[a] / voxelSize - 1e-9)) + 1;
    if (double(v.dims.x) * v.dims.y * v.dims.z > double(volume_detail::kMaxVoxels))
        throw std::runtime_error("the volume would have over 64 million voxels; raise the voxel size");
    v.indexToWorld = glm::dmat4(voxelSize);
    v.indexToWorld[3] = glm::dvec4(origin, 1.0);
    v.values.assign(size_t(v.dims.x) * v.dims.y * v.dims.z, background);
    return v;
}

glm::dvec3 Volume::voxelOf(const glm::dvec3& world) const { return glm::dvec3(glm::inverse(indexToWorld) * glm::dvec4(world, 1.0)); }

double Volume::sample(const glm::dvec3& world) const
{
    glm::dvec3 p = voxelOf(world);
    glm::dvec3 f = glm::floor(p);
    glm::dvec3 t = p - f;
    int i = int(f.x), j = int(f.y), k = int(f.z);
    double c00 = glm::mix(double(at(i, j, k)), double(at(i + 1, j, k)), t.x);
    double c10 = glm::mix(double(at(i, j + 1, k)), double(at(i + 1, j + 1, k)), t.x);
    double c01 = glm::mix(double(at(i, j, k + 1)), double(at(i + 1, j, k + 1)), t.x);
    double c11 = glm::mix(double(at(i, j + 1, k + 1)), double(at(i + 1, j + 1, k + 1)), t.x);
    return glm::mix(glm::mix(c00, c10, t.y), glm::mix(c01, c11, t.y), t.z);
}

double Volume::sampleNearest(const glm::dvec3& world) const
{
    glm::dvec3 p = glm::round(voxelOf(world));
    return at(int(p.x), int(p.y), int(p.z));
}

double Volume::voxelSize() const { return std::cbrt(std::abs(glm::determinant(glm::dmat3(indexToWorld)))); }

geo::Bounds Volume::bounds() const
{
    geo::Bounds b;
    if (empty())
        return b;
    for (int n = 0; n < 8; ++n)
        b.extend(worldOf({(n & 1) ? dims.x - 1 : 0, (n & 2) ? dims.y - 1 : 0, (n & 4) ? dims.z - 1 : 0}));
    return b;
}

std::vector<float> Volume::resampled(const Volume& target) const
{
    std::vector<float> out(target.size(), background);
    tbb::parallel_for(0, target.dims.z, [&](int k) {
        for (int j = 0; j < target.dims.y; ++j)
            for (int i = 0; i < target.dims.x; ++i)
                out[target.index(i, j, k)] = float(sample(target.worldOf({i, j, k})));
    });
    return out;
}

GeometryPtr volumeGeometry(Volume v)
{
    Geometry g;
    g.volume = std::make_shared<const Volume>(std::move(v));
    return makeGeometry(std::move(g));
}

Volume meshToSdf(const geo::Mesh& m, double voxelSize, int band)
{
    LILA_ZONE();

    band = std::max(band, 1);
    const double bandDist = band * voxelSize;
    geo::Bounds b = m.bounds();
    if (!b.valid())
        return Volume::grid({}, voxelSize, Volume::Kind::Sdf, float(bandDist));
    glm::dvec3 pad(bandDist + voxelSize);
    b.min -= pad;
    b.max += pad;
    Volume v = Volume::grid(b, voxelSize, Volume::Kind::Sdf, float(bandDist));
    geo::MeshBvh bvh(m);
    if (bvh.empty())
        return v;

    const int nx = v.dims.x;
    tbb::parallel_for(0, v.dims.y * v.dims.z, [&](int row) {
        int j = row % v.dims.y, k = row / v.dims.y;
        // One ray along the row; a voxel is inside when an odd number of crossings lie before it. The row is nudged off the
        // grid lines so it doesn't run exactly along the shared edges of axis-aligned faces.
        glm::dvec3 origin = v.worldOf({-1.0, j + 1.37e-5, k + 2.71e-5});
        glm::dvec3 step = v.worldOf({0.0, j + 1.37e-5, k + 2.71e-5}) - origin;
        double stepLen = glm::length(step);
        glm::dvec3 dir = step / stepLen;
        double length = (nx + 1) * stepLen, eps = 1e-6 * stepLen, travelled = 0.0;
        std::vector<double> crossings;
        for (int guard = 0; guard < 4096 && travelled < length; ++guard)
        {
            geo::BvhHit hit;
            if (!bvh.raycast(origin + dir * travelled, dir, hit, length - travelled, true))
                break;
            crossings.push_back((travelled + hit.t) / stepLen - 1.0);
            travelled += hit.t + eps;
        }
        size_t passed = 0;
        for (int i = 0; i < nx; ++i)
        {
            while (passed < crossings.size() && crossings[passed] < double(i))
                ++passed;
            geo::BvhClosest c;
            double d = bvh.closestPoint(v.worldOf({i, j, k}), c, bandDist) ? std::min(c.distance, bandDist) : bandDist;
            v.values[v.index(i, j, k)] = float((passed & 1) ? -d : d);
        }
    });
    return v;
}

Volume pointsToSdf(std::span<const glm::dvec3> points, std::span<const double> radii, double voxelSize, int band)
{
    LILA_ZONE();

    band = std::max(band, 1);
    const double bandDist = band * voxelSize;
    auto radius = [&](size_t p) { return radii.empty() ? 0.0 : std::max(0.0, radii[radii.size() == 1 ? 0 : p]); };
    geo::Bounds b;
    double maxR = 0.0;
    for (size_t p = 0; p < points.size(); ++p)
    {
        b.extend(points[p]);
        maxR = std::max(maxR, radius(p));
    }
    if (!b.valid())
        return Volume::grid({}, voxelSize, Volume::Kind::Sdf, float(bandDist));
    glm::dvec3 pad(maxR + bandDist + voxelSize);
    b.min -= pad;
    b.max += pad;
    Volume v = Volume::grid(b, voxelSize, Volume::Kind::Sdf, float(bandDist));
    tbb::parallel_for(0, v.dims.z, [&](int k) {
        for (size_t p = 0; p < points.size(); ++p)
        {
            double r = radius(p), reach = r + bandDist;
            glm::dvec3 lo = v.voxelOf(points[p] - glm::dvec3(reach)), hi = v.voxelOf(points[p] + glm::dvec3(reach));
            if (k < int(std::floor(lo.z)) || k > int(std::ceil(hi.z)))
                continue;
            int i0 = std::max(0, int(std::floor(lo.x))), i1 = std::min(v.dims.x - 1, int(std::ceil(hi.x)));
            int j0 = std::max(0, int(std::floor(lo.y))), j1 = std::min(v.dims.y - 1, int(std::ceil(hi.y)));
            for (int j = j0; j <= j1; ++j)
                for (int i = i0; i <= i1; ++i)
                {
                    double d = std::clamp(glm::length(v.worldOf({i, j, k}) - points[p]) - r, -bandDist, bandDist);
                    float& cell = v.values[v.index(i, j, k)];
                    cell = std::min(cell, float(d));
                }
        }
    });
    return v;
}

geo::Mesh volumeToMesh(const Volume& v, double threshold)
{
    LILA_ZONE();

    if (v.empty())
        return {};
    volume_detail::Nets nets = volume_detail::surfaceNets(v, threshold);
    for (glm::dvec3& p : nets.points)
        p = v.worldOf(p);
    if (glm::determinant(glm::dmat3(v.indexToWorld)) < 0.0)
        for (size_t q = 0; q + 3 < nets.quads.size(); q += 4)
            std::swap(nets.quads[q + 1], nets.quads[q + 3]);
    std::vector<geo::Index> sizes(nets.quads.size() / 4, 4);
    return geo::Mesh::fromPolygons(nets.points, sizes, nets.quads);
}
} // namespace lila::gn
