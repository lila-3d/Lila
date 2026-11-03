#include "deform/Deform.h"

#include "core/Profile.h"
#include "deform/DeformUtil.h"
#include "geo/Bvh.h"
#include "geo/Triangulate.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace lila::deform
{
namespace
{
double smoothstep01(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

/** Position of y within [low, high] as 0..1, clamped. */
double along(double y, double low, double high)
{
    double span = high - low;
    return std::abs(span) < 1e-12 ? 0.0 : std::clamp((y - low) / span, 0.0, 1.0);
}
} // namespace

// ------------------------------------------------------------------------------------------------ adjacency

Adjacency pointAdjacency(const geo::Mesh& mesh)
{
    LILA_ZONE();

    Adjacency a;
    const Index n = mesh.pointCapacity();
    a.offsets.reserve(size_t(n) + 1);
    a.boundary.assign(n, 0);
    for (Index p = 0; p < n; ++p)
    {
        if (mesh.pointValid(p))
            mesh.forEachPointEdge(p, [&](Index e) {
                a.neighbors.push_back(mesh.edgeOtherPoint(e, p));
                if (mesh.edgeFaceCount(e) < 2)
                    a.boundary[p] = 1;
            });
        a.offsets.push_back(uint32_t(a.neighbors.size()));
    }
    return a;
}

void smooth(std::vector<glm::dvec3>& points, const Adjacency& adj, int iterations, double step, bool pinBoundary)
{
    LILA_ZONE();

    std::vector<glm::dvec3> next(points.size());
    const size_t n = std::min(points.size(), adj.offsets.size() - 1);
    for (int it = 0; it < iterations; ++it)
    {
        parallelPoints(points.size(), [&](size_t p) {
            next[p] = points[p];
            if (p >= n || adj.offsets[p] == adj.offsets[p + 1] || (pinBoundary && adj.boundary[p]))
                return;
            glm::dvec3 sum(0.0);
            for (uint32_t k = adj.offsets[p]; k < adj.offsets[p + 1]; ++k)
                sum += points[adj.neighbors[k]];
            glm::dvec3 avg = sum / double(adj.offsets[p + 1] - adj.offsets[p]);
            next[p] = points[p] + (avg - points[p]) * step;
        });
        points.swap(next);
    }
}

// ------------------------------------------------------------------------------------------------ falloff

double sphericalFalloff(double distance, double radius, double softness)
{
    if (radius <= 0.0)
        return 1.0;
    double inner = radius * (1.0 - std::clamp(softness, 0.0, 1.0));
    if (distance <= inner)
        return 1.0;
    if (distance >= radius)
        return 0.0;
    return 1.0 - smoothstep01((distance - inner) / (radius - inner));
}

// ------------------------------------------------------------------------------------------------ nonlinear

glm::dvec3 bend(const glm::dvec3& p, const BendParams& o)
{
    double low = std::min(o.low, o.high), high = std::max(o.low, o.high);
    double span = high - low;
    double k = span > 1e-12 ? glm::radians(o.angle) / span : 0.0;
    if (std::abs(k) < 1e-12)
        return p;
    // Barr's bend in the XY plane: the Y axis follows an arc of radius 1/k, rigid past the bounds.
    double yc = std::clamp(p.y, low, high);
    double phi = k * yc;
    double r = 1.0 / k;
    double s = std::sin(phi), c = std::cos(phi);
    double x = r - (r - p.x) * c;
    double y = (r - p.x) * s;
    double beyond = p.y - yc;
    return {x + beyond * s, y + beyond * c, p.z};
}

glm::dvec3 twist(const glm::dvec3& p, const TwistParams& o)
{
    double t = along(p.y, o.low, o.high);
    double a = glm::radians(o.startAngle + (o.endAngle - o.startAngle) * t);
    double s = std::sin(a), c = std::cos(a);
    return {p.x * c + p.z * s, p.y, -p.x * s + p.z * c};
}

glm::dvec3 taper(const glm::dvec3& p, const TaperParams& o)
{
    double t = along(p.y, o.low, o.high);
    double s = o.startScale + (o.endScale - o.startScale) * t + o.curve * std::sin(std::numbers::pi * t);
    return {p.x * s, p.y, p.z * s};
}

glm::dvec3 squash(const glm::dvec3& p, const SquashParams& o)
{
    double low = std::min(o.low, o.high), high = std::max(o.low, o.high);
    double sy = std::max(1.0 + o.factor, 1e-3);
    double yc = std::clamp(p.y, low, high);
    double y = yc * sy + (p.y - yc);
    double t = along(p.y, low, high);
    double m = std::clamp(o.maxExpandPos, 1e-3, 1.0 - 1e-3);
    // The bulge peaks at maxExpandPos and fades to nothing at both bounds.
    double bump = t < m ? smoothstep01(t / m) : smoothstep01((1.0 - t) / (1.0 - m));
    double sxz = 1.0 + o.expand * (1.0 / std::sqrt(sy) - 1.0) * bump;
    return {p.x * sxz, y, p.z * sxz};
}

namespace
{
double dropoffScale(double dropoff, double t)
{
    double d = std::clamp(dropoff, -1.0, 1.0);
    return d >= 0.0 ? 1.0 - d * t : 1.0 + d * (1.0 - t);
}
} // namespace

glm::dvec3 sine(const glm::dvec3& p, const SineParams& o)
{
    double low = std::min(o.low, o.high), high = std::max(o.low, o.high);
    double yc = std::clamp(p.y, low, high);
    double ext = std::max(std::abs(low), std::abs(high));
    double wl = std::abs(o.wavelength) < 1e-9 ? 1e-9 : o.wavelength;
    double amp = o.amplitude * dropoffScale(o.dropoff, ext > 0.0 ? std::abs(yc) / ext : 0.0);
    return {p.x + amp * std::sin(2.0 * std::numbers::pi * (yc / wl + o.offset)), p.y, p.z};
}

glm::dvec3 wave(const glm::dvec3& p, const WaveParams& o)
{
    double r = std::sqrt(p.x * p.x + p.z * p.z);
    if (r < o.minRadius || r > o.maxRadius)
        return p;
    double wl = std::abs(o.wavelength) < 1e-9 ? 1e-9 : o.wavelength;
    double amp = o.amplitude * dropoffScale(o.dropoff, o.maxRadius > 0.0 ? r / o.maxRadius : 0.0);
    return {p.x, p.y + amp * std::sin(2.0 * std::numbers::pi * (r / wl - o.offset)), p.z};
}

// ------------------------------------------------------------------------------------------------ lattice

glm::dvec3 latticeRest(const glm::ivec3& n, int i, int j, int k)
{
    auto at = [](int idx, int count) { return count < 2 ? 0.0 : -0.5 + double(idx) / double(count - 1); };
    return {at(i, n.x), at(j, n.y), at(k, n.z)};
}

geo::Mesh latticeMesh(const glm::ivec3& divisions)
{
    glm::ivec3 n = glm::max(divisions, glm::ivec3(2));
    std::vector<glm::dvec3> points;
    for (int k = 0; k < n.z; ++k)
        for (int j = 0; j < n.y; ++j)
            for (int i = 0; i < n.x; ++i)
                points.push_back(latticeRest(n, i, j, k));
    std::vector<Index> sizes, indices;
    auto quad = [&](size_t a, size_t b, size_t c, size_t d) {
        sizes.push_back(4);
        indices.insert(indices.end(), {Index(a), Index(b), Index(c), Index(d)});
    };
    auto id = [&](int i, int j, int k) { return latticeIndex(n, i, j, k); };
    for (int k = 0; k < n.z; ++k)
        for (int j = 0; j + 1 < n.y; ++j)
            for (int i = 0; i + 1 < n.x; ++i)
                quad(id(i, j, k), id(i, j + 1, k), id(i + 1, j + 1, k), id(i + 1, j, k));
    for (int j = 0; j < n.y; ++j)
        for (int k = 0; k + 1 < n.z; ++k)
            for (int i = 0; i + 1 < n.x; ++i)
                quad(id(i, j, k), id(i + 1, j, k), id(i + 1, j, k + 1), id(i, j, k + 1));
    for (int i = 0; i < n.x; ++i)
        for (int k = 0; k + 1 < n.z; ++k)
            for (int j = 0; j + 1 < n.y; ++j)
                quad(id(i, j, k), id(i, j, k + 1), id(i, j + 1, k + 1), id(i, j + 1, k));
    return geo::Mesh::fromPolygons(points, sizes, indices);
}

void ffdAxisWeights(int n, double s, FfdBasis basis, std::vector<std::pair<int, double>>& out)
{
    out.clear();
    if (n < 2)
    {
        out.push_back({0, 1.0});
        return;
    }
    s = std::clamp(s, 0.0, 1.0);
    switch (basis)
    {
    case FfdBasis::Trilinear:
    {
        double x = s * double(n - 1);
        int i0 = std::clamp(int(std::floor(x)), 0, n - 2);
        double f = x - double(i0);
        out.push_back({i0, 1.0 - f});
        out.push_back({i0 + 1, f});
        break;
    }
    case FfdBasis::Bernstein:
    {
        int d = n - 1;
        double binom = 1.0;
        for (int i = 0; i <= d; ++i)
        {
            out.push_back({i, binom * std::pow(s, i) * std::pow(1.0 - s, d - i)});
            binom = binom * double(d - i) / double(i + 1);
        }
        break;
    }
    case FfdBasis::BSpline:
    {
        double x = s * double(n - 1);
        int base = int(std::floor(x));
        for (int i = base - 1; i <= base + 2; ++i)
        {
            double t = std::abs(x - double(i));
            double w = t < 1.0 ? (4.0 - 6.0 * t * t + 3.0 * t * t * t) / 6.0 : t < 2.0 ? (2.0 - t) * (2.0 - t) * (2.0 - t) / 6.0 : 0.0;
            if (w <= 0.0)
                continue;
            // Missing points past the ends repeat the end point, so the weights still sum to 1.
            int idx = std::clamp(i, 0, n - 1);
            auto it = std::find_if(out.begin(), out.end(), [&](const auto& e) { return e.first == idx; });
            if (it != out.end())
                it->second += w;
            else
                out.push_back({idx, w});
        }
        break;
    }
    }
}

glm::dvec3 ffdDisplacement(const glm::ivec3& n, std::span<const glm::dvec3> displacement, const glm::dvec3& stu, FfdBasis basis)
{
    thread_local std::vector<std::pair<int, double>> wx, wy, wz;
    ffdAxisWeights(n.x, stu.x, basis, wx);
    ffdAxisWeights(n.y, stu.y, basis, wy);
    ffdAxisWeights(n.z, stu.z, basis, wz);
    glm::dvec3 out(0.0);
    for (auto [k, fk] : wz)
        for (auto [j, fj] : wy)
            for (auto [i, fi] : wx)
            {
                size_t idx = latticeIndex(n, i, j, k);
                if (idx < displacement.size())
                    out += displacement[idx] * (fi * fj * fk);
            }
    return out;
}

// ------------------------------------------------------------------------------------------------ delta mush

std::vector<glm::dmat3> pointFrames(const geo::Mesh& topology, std::span<const glm::dvec3> points, const Adjacency& adj)
{
    LILA_ZONE();

    const size_t n = std::min<size_t>(points.size(), topology.pointCapacity());
    std::vector<glm::dmat3> frames(n, glm::dmat3(1.0));
    parallelPoints(n, [&](size_t p) {
        if (!topology.pointValid(Index(p)))
            return;
        glm::dvec3 normal(0.0);
        topology.forEachPointCorner(Index(p), [&](Index c) {
            const glm::dvec3& o = points[p];
            glm::dvec3 a = points[topology.cornerNextPoint(c)] - o;
            glm::dvec3 b = points[topology.cornerPoint(topology.cornerPrev(c))] - o;
            normal += glm::cross(a, b);
        });
        double ln = glm::length(normal);
        if (ln < 1e-15 || adj.offsets[p] == adj.offsets[p + 1])
            return;
        normal /= ln;
        glm::dvec3 t(0.0);
        for (uint32_t k = adj.offsets[p]; k < adj.offsets[p + 1] && glm::length(t) < 1e-12; ++k)
        {
            glm::dvec3 e = points[adj.neighbors[k]] - points[p];
            t = e - normal * glm::dot(e, normal);
        }
        if (glm::length(t) < 1e-12)
            t = std::abs(normal.x) < 0.9 ? glm::cross(normal, glm::dvec3(1, 0, 0)) : glm::cross(normal, glm::dvec3(0, 1, 0));
        t = glm::normalize(t);
        frames[p] = glm::dmat3(t, glm::cross(normal, t), normal);
    });
    return frames;
}

DeltaMushBind deltaMushBind(const geo::Mesh& topology, std::span<const glm::dvec3> rest, const Adjacency& adj, int iterations, double step,
                            bool pinBoundary)
{
    LILA_ZONE();

    DeltaMushBind b;
    b.iterations = iterations;
    b.step = step;
    b.pinBoundary = pinBoundary;
    std::vector<glm::dvec3> smoothed(rest.begin(), rest.end());
    smooth(smoothed, adj, iterations, step, pinBoundary);
    std::vector<glm::dmat3> frames = pointFrames(topology, smoothed, adj);
    b.local.resize(rest.size());
    parallelPoints(rest.size(), [&](size_t p) {
        glm::dvec3 d = rest[p] - smoothed[p];
        b.local[p] = p < frames.size() ? glm::transpose(frames[p]) * d : d;
    });
    return b;
}

std::vector<glm::dvec3> deltaMushApply(const geo::Mesh& topology, std::span<const glm::dvec3> deformed, const Adjacency& adj,
                                       const DeltaMushBind& bind, double displacement)
{
    LILA_ZONE();

    std::vector<glm::dvec3> smoothed(deformed.begin(), deformed.end());
    smooth(smoothed, adj, bind.iterations, bind.step, bind.pinBoundary);
    std::vector<glm::dmat3> frames = pointFrames(topology, smoothed, adj);
    std::vector<glm::dvec3> out(deformed.size());
    parallelPoints(deformed.size(), [&](size_t p) {
        if (p >= bind.local.size() || p >= frames.size())
        {
            out[p] = deformed[p];
            return;
        }
        out[p] = smoothed[p] + frames[p] * bind.local[p] * displacement;
    });
    return out;
}

// ------------------------------------------------------------------------------------------------ wrap

WrapBinding wrapBind(const geo::Mesh& driver, std::span<const glm::dvec3> targets, int maxDrivers, double falloff)
{
    LILA_ZONE();

    WrapBinding b;
    b.driverPoints = driver.pointCapacity();
    geo::MeshBvh bvh(driver);
    if (bvh.empty())
    {
        b.offsets.assign(targets.size() + 1, 0);
        return b;
    }
    const geo::Triangulation& tris = bvh.triangles();
    auto triPoint = [&](Index t, int k) { return uint32_t(driver.cornerPoint(tris.corners[size_t(t) * 3 + size_t(k)])); };
    const double eps = std::max(1e-9, glm::length(driver.bounds().size()) * 1e-5);
    maxDrivers = std::clamp(maxDrivers, 1, 32);
    falloff = std::max(falloff, 1.0);

    std::vector<std::vector<WrapBinding::Influence>> per(targets.size());
    parallelPoints(targets.size(), [&](size_t i) {
        const glm::dvec3& p = targets[i];
        geo::BvhClosest hit;
        if (!bvh.closestPoint(p, hit))
            return;
        double reach = hit.distance * falloff + eps;
        geo::Bounds box;
        box.extend(p - glm::dvec3(reach));
        box.extend(p + glm::dvec3(reach));
        struct Cand
        {
            Index tri;
            double dist;
            glm::dvec3 point, bary;
        };
        std::vector<Cand> cands;
        for (Index t : bvh.overlapping(box))
        {
            glm::dvec3 a = driver.position(triPoint(t, 0)), bb = driver.position(triPoint(t, 1)), c = driver.position(triPoint(t, 2));
            glm::dvec3 bary;
            glm::dvec3 q = closestOnTriangle(p, a, bb, c, bary);
            double d = glm::length(p - q);
            if (d <= reach)
                cands.push_back({t, d, q, bary});
        }
        if (cands.empty())
        {
            glm::dvec3 a = driver.position(triPoint(hit.triangle, 0)), bb = driver.position(triPoint(hit.triangle, 1)),
                       c = driver.position(triPoint(hit.triangle, 2));
            glm::dvec3 bary;
            glm::dvec3 q = closestOnTriangle(p, a, bb, c, bary);
            cands.push_back({hit.triangle, glm::length(p - q), q, bary});
        }
        std::sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) { return x.dist != y.dist ? x.dist < y.dist : x.tri < y.tri; });
        if (int(cands.size()) > maxDrivers)
            cands.resize(size_t(maxDrivers));
        double total = 0.0;
        for (const Cand& c : cands)
        {
            WrapBinding::Influence inf;
            for (int k = 0; k < 3; ++k)
                inf.tri[k] = triPoint(c.tri, k);
            inf.bary = c.bary;
            glm::dmat3 f = triangleFrame(driver.position(inf.tri[0]), driver.position(inf.tri[1]), driver.position(inf.tri[2]));
            inf.offset = glm::transpose(f) * (p - c.point);
            inf.weight = 1.0 / ((c.dist + eps) * (c.dist + eps));
            total += inf.weight;
            per[i].push_back(inf);
        }
        for (auto& inf : per[i])
            inf.weight /= total;
    });
    for (auto& list : per)
    {
        b.influences.insert(b.influences.end(), list.begin(), list.end());
        b.offsets.push_back(uint32_t(b.influences.size()));
    }
    return b;
}

std::vector<glm::dvec3> wrapApply(const WrapBinding& bind, std::span<const glm::dvec3> driverPoints, std::span<const glm::dvec3> targets)
{
    LILA_ZONE();

    std::vector<glm::dvec3> out(targets.begin(), targets.end());
    const size_t n = std::min(targets.size(), bind.offsets.size() - 1);
    parallelPoints(n, [&](size_t i) {
        if (bind.offsets[i] == bind.offsets[i + 1])
            return;
        glm::dvec3 sum(0.0);
        for (uint32_t k = bind.offsets[i]; k < bind.offsets[i + 1]; ++k)
        {
            const WrapBinding::Influence& inf = bind.influences[k];
            if (inf.tri[0] >= driverPoints.size() || inf.tri[1] >= driverPoints.size() || inf.tri[2] >= driverPoints.size())
                return;
            const glm::dvec3 &a = driverPoints[inf.tri[0]], &b = driverPoints[inf.tri[1]], &c = driverPoints[inf.tri[2]];
            glm::dvec3 q = a * inf.bary.x + b * inf.bary.y + c * inf.bary.z;
            sum += (q + triangleFrame(a, b, c) * inf.offset) * inf.weight;
        }
        out[i] = sum;
    });
    return out;
}

// ------------------------------------------------------------------------------------------------ cage

void meanValueCoordinates(std::span<const glm::dvec3> cage, std::span<const uint32_t> triangles, const glm::dvec3& x, std::span<double> out)
{
    constexpr double kEps = 1e-10;
    std::fill(out.begin(), out.end(), 0.0);
    const size_t nv = cage.size();
    thread_local std::vector<double> dist;
    thread_local std::vector<glm::dvec3> unit;
    dist.resize(nv);
    unit.resize(nv);
    for (size_t j = 0; j < nv; ++j)
    {
        glm::dvec3 d = cage[j] - x;
        dist[j] = glm::length(d);
        if (dist[j] < kEps)
        {
            out[j] = 1.0;
            return;
        }
        unit[j] = d / dist[j];
    }
    for (size_t t = 0; t + 2 < triangles.size(); t += 3)
    {
        const uint32_t v[3] = {triangles[t], triangles[t + 1], triangles[t + 2]};
        double theta[3], c[3], s[3];
        double h = 0.0;
        for (int i = 0; i < 3; ++i)
        {
            double l = glm::length(unit[v[(i + 1) % 3]] - unit[v[(i + 2) % 3]]);
            theta[i] = 2.0 * std::asin(std::clamp(l / 2.0, -1.0, 1.0));
            h += theta[i];
        }
        h *= 0.5;
        if (std::numbers::pi - h < 1e-8)
        {
            // x lies on this triangle: plain 2D barycentric weights.
            std::fill(out.begin(), out.end(), 0.0);
            double sum = 0.0;
            for (int i = 0; i < 3; ++i)
            {
                double w = std::sin(theta[i]) * dist[v[(i + 2) % 3]] * dist[v[(i + 1) % 3]];
                out[v[i]] += w;
                sum += w;
            }
            if (sum > 0.0)
                for (int i = 0; i < 3; ++i)
                    out[v[i]] /= sum;
            return;
        }
        double det = glm::dot(unit[v[0]], glm::cross(unit[v[1]], unit[v[2]]));
        double sign = det < 0.0 ? -1.0 : 1.0;
        bool skip = false;
        for (int i = 0; i < 3; ++i)
        {
            c[i] = (2.0 * std::sin(h) * std::sin(h - theta[i])) / (std::sin(theta[(i + 1) % 3]) * std::sin(theta[(i + 2) % 3])) - 1.0;
            s[i] = sign * std::sqrt(std::max(0.0, 1.0 - c[i] * c[i]));
            if (std::abs(s[i]) <= kEps)
                skip = true;
        }
        // x is in the triangle's plane but outside it: the triangle adds nothing.
        if (skip)
            continue;
        for (int i = 0; i < 3; ++i)
        {
            int ip = (i + 1) % 3, im = (i + 2) % 3;
            out[v[i]] += (theta[i] - c[ip] * theta[im] - c[im] * theta[ip]) / (dist[v[i]] * std::sin(theta[ip]) * s[im]);
        }
    }
    double sum = 0.0;
    for (double w : out)
        sum += w;
    if (std::abs(sum) > 1e-300)
        for (double& w : out)
            w /= sum;
}

std::vector<float> meanValueCoordinates(std::span<const glm::dvec3> cage, std::span<const uint32_t> triangles, std::span<const glm::dvec3> points)
{
    LILA_ZONE();

    const size_t nv = cage.size();
    std::vector<float> out(points.size() * nv);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, points.size(), 64), [&](const tbb::blocked_range<size_t>& r) {
        std::vector<double> row(nv);
        for (size_t i = r.begin(); i != r.end(); ++i)
        {
            meanValueCoordinates(cage, triangles, points[i], row);
            for (size_t j = 0; j < nv; ++j)
                out[i * nv + j] = float(row[j]);
        }
    });
    return out;
}

std::vector<uint32_t> trianglePoints(const geo::Mesh& mesh)
{
    geo::Triangulation tris = geo::triangulate(mesh);
    std::vector<uint32_t> out(tris.corners.size());
    for (size_t i = 0; i < tris.corners.size(); ++i)
        out[i] = uint32_t(mesh.cornerPoint(tris.corners[i]));
    return out;
}
} // namespace lila::deform
