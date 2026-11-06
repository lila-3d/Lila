#include "geo/Heightfield.h"

#include "core/Profile.h"
#include "geo/Triangulate.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>

namespace lila::geo
{
namespace
{
/** Grid coordinates of a world position, clamped to the grid, as the cell's corner and the fraction across it. */
struct Bilinear
{
    int x0, z0, x1, z1;
    double fx, fz;
};

Bilinear locate(const Heightfield& h, double wx, double wz)
{
    double gx = std::clamp((wx - h.origin.x) / h.cell, 0.0, double(h.nx - 1));
    double gz = std::clamp((wz - h.origin.y) / h.cell, 0.0, double(h.nz - 1));
    int x0 = std::min(int(gx), h.nx - 2), z0 = std::min(int(gz), h.nz - 2);
    return {x0, z0, x0 + 1, z0 + 1, gx - x0, gz - z0};
}

double lerp2(const std::vector<float>& v, const Heightfield& h, const Bilinear& b)
{
    double a = v[h.index(b.x0, b.z0)] * (1.0 - b.fx) + v[h.index(b.x1, b.z0)] * b.fx;
    double c = v[h.index(b.x0, b.z1)] * (1.0 - b.fx) + v[h.index(b.x1, b.z1)] * b.fx;
    return a * (1.0 - b.fz) + c * b.fz;
}

/** Sample indices kept at a step: 0, step, 2 step ... and always the last. */
std::vector<int> stepped(int n, int step)
{
    std::vector<int> out;
    for (int k = 0; k < n - 1; k += step)
        out.push_back(k);
    out.push_back(n - 1);
    return out;
}
} // namespace

glm::dvec3 Heightfield::position(int x, int z) const
{
    glm::dvec2 p = samplePos(x, z);
    return {p.x, double(at(x, z)), p.y};
}

bool Heightfield::contains(double wx, double wz) const
{
    glm::dvec2 e = extent();
    return wx >= origin.x && wz >= origin.y && wx <= origin.x + e.x && wz <= origin.y + e.y;
}

double Heightfield::heightAt(double wx, double wz) const
{
    if (empty())
        return heights.empty() ? 0.0 : heights[0];
    return lerp2(heights, *this, locate(*this, wx, wz));
}

glm::dvec3 Heightfield::normalAt(double wx, double wz) const
{
    if (empty())
        return {0.0, 1.0, 0.0};
    const double d = cell * 0.5;
    double dx = heightAt(wx + d, wz) - heightAt(wx - d, wz);
    double dz = heightAt(wx, wz + d) - heightAt(wx, wz - d);
    return glm::normalize(glm::dvec3(-dx, 2.0 * d, -dz));
}

double Heightfield::maskAt(std::string_view name, double wx, double wz) const
{
    const std::vector<float>* m = findMask(name);
    if (!m || empty())
        return 0.0;
    return lerp2(*m, *this, locate(*this, wx, wz));
}

glm::dvec2 Heightfield::range() const
{
    if (heights.empty())
        return glm::dvec2(0.0);
    auto [lo, hi] = std::minmax_element(heights.begin(), heights.end());
    return {*lo, *hi};
}

Bounds Heightfield::bounds() const
{
    Bounds b;
    if (heights.empty())
        return b;
    glm::dvec2 r = range();
    glm::dvec2 e = extent();
    b.extend({origin.x, r.x, origin.y});
    b.extend({origin.x + e.x, r.y, origin.y + e.y});
    return b;
}

std::vector<float>& Heightfield::mask(std::string_view name)
{
    auto it = masks.find(name);
    if (it == masks.end())
        it = masks.emplace(std::string(name), std::vector<float>(size(), 0.0f)).first;
    it->second.resize(size(), 0.0f);
    return it->second;
}

const std::vector<float>* Heightfield::findMask(std::string_view name) const
{
    auto it = masks.find(name);
    return it == masks.end() ? nullptr : &it->second;
}

Heightfield makeHeightfield(int nx, int nz, double cell, glm::dvec2 origin, float h)
{
    Heightfield out;
    out.nx = std::max(2, nx);
    out.nz = std::max(2, nz);
    out.cell = cell > 0.0 ? cell : 1.0;
    out.origin = origin;
    out.heights.assign(out.size(), h);
    return out;
}

Heightfield makeHeightfieldCovering(glm::dvec2 center, glm::dvec2 size, int resolution, float h)
{
    size = glm::max(size, glm::dvec2(1e-6));
    resolution = std::max(2, resolution);
    double cell = std::max(size.x, size.y) / double(resolution - 1);
    int nx = std::max(2, int(std::lround(size.x / cell)) + 1);
    int nz = std::max(2, int(std::lround(size.y / cell)) + 1);
    glm::dvec2 extent = glm::dvec2(nx - 1, nz - 1) * cell;
    return makeHeightfield(nx, nz, cell, center - extent * 0.5, h);
}

Mesh heightfieldToMesh(const Heightfield& h, const HeightfieldMeshOptions& o)
{
    LILA_ZONE();

    if (h.empty())
        return {};
    const std::vector<int> xs = stepped(h.nx, std::max(1, o.step)), zs = stepped(h.nz, std::max(1, o.step));
    const size_t cx = xs.size(), cz = zs.size();
    std::vector<glm::dvec3> points(cx * cz);
    tbb::parallel_for(size_t(0), cz, [&](size_t j) {
        for (size_t i = 0; i < cx; ++i)
            points[j * cx + i] = h.position(xs[i], zs[j]);
    });
    std::vector<Index> sizes((cx - 1) * (cz - 1), 4);
    std::vector<Index> idx;
    idx.reserve(sizes.size() * 4);
    auto id = [&](size_t i, size_t j) { return Index(j * cx + i); };
    const std::vector<float>* holes = o.holes.empty() ? nullptr : h.findMask(o.holes);
    if (holes && holes->size() != h.size())
        holes = nullptr;
    auto hole = [&](size_t i, size_t j) {
        for (int z = zs[j]; z <= zs[j + 1]; ++z)
            for (int x = xs[i]; x <= xs[i + 1]; ++x)
                if ((*holes)[h.index(x, z)] > 0.5f)
                    return true;
        return false;
    };
    for (size_t j = 0; j + 1 < cz; ++j)
        for (size_t i = 0; i + 1 < cx; ++i)
        {
            if (holes && hole(i, j))
            {
                sizes.pop_back();
                continue;
            }
            // Rows advance toward +Z, so this order faces +Y.
            idx.push_back(id(i, j));
            idx.push_back(id(i, j + 1));
            idx.push_back(id(i + 1, j + 1));
            idx.push_back(id(i + 1, j));
        }
    Mesh m = Mesh::fromPolygons(points, sizes, idx);
    if (o.masks)
        for (const auto& [name, values] : h.masks)
        {
            if (values.size() != h.size())
                continue;
            std::span<float> dst = m.pointAttrs().getOrAdd<float>(name);
            for (size_t j = 0; j < cz; ++j)
                for (size_t i = 0; i < cx; ++i)
                    dst[j * cx + i] = values[h.index(xs[i], zs[j])];
        }
    if (o.uvs)
    {
        std::span<glm::vec2> uv = m.cornerAttrs().getOrAdd<glm::vec2>(attr::uvName(0));
        const double ux = 1.0 / std::max(1, h.nx - 1), uz = 1.0 / std::max(1, h.nz - 1);
        for (Index c = 0; c < Index(uv.size()); ++c)
        {
            Index p = m.cornerPoint(c);
            size_t i = size_t(p) % cx, j = size_t(p) / cx;
            uv[c] = {float(xs[i] * ux), float(zs[j] * uz)};
        }
    }
    return m;
}

Heightfield meshToHeightfield(const Mesh& m, int nx, int nz, double cell, glm::dvec2 origin, float fill, size_t* covered)
{
    LILA_ZONE();

    Heightfield h = makeHeightfield(nx, nz, cell, origin, -std::numeric_limits<float>::infinity());
    const Triangulation tris = triangulate(m);
    const size_t count = tris.faces.size();
    // Rows are locked in bands so triangles rasterize in parallel without two writers on one sample.
    constexpr int kBand = 64;
    std::vector<std::mutex> locks(size_t(h.nz / kBand + 1));
    tbb::parallel_for(size_t(0), count, [&](size_t t) {
        glm::dvec3 p[3];
        for (int k = 0; k < 3; ++k)
            p[k] = m.position(m.cornerPoint(tris.corners[t * 3 + k]));
        glm::dvec2 g[3];
        for (int k = 0; k < 3; ++k)
            g[k] = (glm::dvec2(p[k].x, p[k].z) - h.origin) / h.cell;
        double area = (g[1].x - g[0].x) * (g[2].y - g[0].y) - (g[2].x - g[0].x) * (g[1].y - g[0].y);
        if (std::abs(area) < 1e-14)
            return;
        int x0 = std::max(0, int(std::ceil(std::min({g[0].x, g[1].x, g[2].x}) - 1e-9)));
        int x1 = std::min(h.nx - 1, int(std::floor(std::max({g[0].x, g[1].x, g[2].x}) + 1e-9)));
        int z0 = std::max(0, int(std::ceil(std::min({g[0].y, g[1].y, g[2].y}) - 1e-9)));
        int z1 = std::min(h.nz - 1, int(std::floor(std::max({g[0].y, g[1].y, g[2].y}) + 1e-9)));
        for (int z = z0; z <= z1; ++z)
        {
            std::lock_guard lock(locks[size_t(z / kBand)]);
            for (int x = x0; x <= x1; ++x)
            {
                glm::dvec2 q(x, z);
                double w0 = ((g[1].x - q.x) * (g[2].y - q.y) - (g[2].x - q.x) * (g[1].y - q.y)) / area;
                double w1 = ((g[2].x - q.x) * (g[0].y - q.y) - (g[0].x - q.x) * (g[2].y - q.y)) / area;
                double w2 = 1.0 - w0 - w1;
                const double eps = -1e-9;
                if (w0 < eps || w1 < eps || w2 < eps)
                    continue;
                float y = float(w0 * p[0].y + w1 * p[1].y + w2 * p[2].y);
                float& dst = h.heights[h.index(x, z)];
                dst = std::max(dst, y);
            }
        }
    });
    size_t hit = 0;
    for (float& v : h.heights)
    {
        if (std::isinf(v))
            v = fill;
        else
            ++hit;
    }
    if (covered)
        *covered = hit;
    return h;
}

Heightfield resampled(const Heightfield& h, int nx, int nz, double cell, glm::dvec2 origin)
{
    LILA_ZONE();

    Heightfield out = makeHeightfield(nx, nz, cell, origin);
    if (h.empty())
        return out;
    std::vector<Bilinear> at(out.size());
    tbb::parallel_for(0, out.nz, [&](int z) {
        for (int x = 0; x < out.nx; ++x)
        {
            glm::dvec2 p = out.samplePos(x, z);
            at[out.index(x, z)] = locate(h, p.x, p.y);
        }
    });
    auto fillFrom = [&](const std::vector<float>& src, std::vector<float>& dst) {
        tbb::parallel_for(size_t(0), dst.size(), [&](size_t k) { dst[k] = float(lerp2(src, h, at[k])); });
    };
    fillFrom(h.heights, out.heights);
    for (const auto& [name, values] : h.masks)
        if (values.size() == h.size())
            fillFrom(values, out.mask(name));
    return out;
}
} // namespace lila::geo
