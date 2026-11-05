#include "retopo/Retopo.h"

#include "core/Profile.h"
#include "geo/Bvh.h"
#include "retopo/Internal.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace lila::retopo::detail
{
namespace
{
/** The sheet seen down its normal: a 2D frame, and its border as segments in both 3D and that frame. */
struct Sheet
{
    glm::dvec3 normal{0.0}, u{0.0}, v{0.0};
    double top = 0.0;
    std::vector<std::array<glm::dvec3, 2>> border;
    std::vector<std::array<glm::dvec2, 2>> border2;
    std::vector<glm::dvec2> corners2;
    std::vector<glm::dvec3> corners;

    glm::dvec2 flat(const glm::dvec3& p) const { return {glm::dot(p, u), glm::dot(p, v)}; }
    glm::dvec3 above(const glm::dvec2& p) const { return u * p.x + v * p.y + normal * top; }
};

/** The closest border point to p in the sheet's frame, with the 3D point it stands for. */
glm::dvec2 nearestBorder(const Sheet& s, const glm::dvec2& p, glm::dvec3* at)
{
    double best = std::numeric_limits<double>::max();
    glm::dvec2 out = p;
    for (size_t i = 0; i < s.border2.size(); ++i)
    {
        const glm::dvec2 a = s.border2[i][0], ab = s.border2[i][1] - a;
        const double len2 = glm::dot(ab, ab);
        const double t = len2 > 0.0 ? std::clamp(glm::dot(p - a, ab) / len2, 0.0, 1.0) : 0.0;
        const glm::dvec2 q = a + ab * t;
        if (const double d = glm::dot(q - p, q - p); d < best)
        {
            best = d;
            out = q;
            if (at)
                *at = glm::mix(s.border[i][0], s.border[i][1], t);
        }
    }
    return out;
}

/** The sheet's frame, or an error when it doesn't face one way all over (overhangs would fold the grid). */
std::string sheetOf(const geo::Mesh& m, Sheet& s)
{
    double area = 0.0;
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
        {
            s.normal += m.faceAreaVector(f);
            area += m.faceArea(f);
        }
    if (!(glm::length(s.normal) > 1e-12 * area))
        return "the sheet faces no one way";
    s.normal = glm::normalize(s.normal);
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f) && m.faceArea(f) > 1e-9 * area && glm::dot(m.faceNormal(f), s.normal) < 0.1)
            return "the sheet turns back on itself; the grid needs one seen without overlap from one side";
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.isBoundaryEdge(e))
            s.border.push_back({m.position(m.edgePoint(e, 0)), m.position(m.edgePoint(e, 1))});
    if (s.border.empty())
        return "the grid needs an open sheet";

    // The grid runs along the outline's tightest bounding rectangle, so a rectangular tile gets rows parallel to its sides.
    const glm::dvec3 u0 = anyTangent(s.normal), v0 = glm::cross(s.normal, u0);
    double bestArea = std::numeric_limits<double>::max(), bestAngle = 0.0;
    const auto boxArea = [&](double angle) {
        const glm::dvec3 u = std::cos(angle) * u0 + std::sin(angle) * v0, v = glm::cross(s.normal, u);
        glm::dvec2 lo(std::numeric_limits<double>::max()), hi(-std::numeric_limits<double>::max());
        for (const auto& seg : s.border)
        {
            const glm::dvec2 p(glm::dot(seg[0], u), glm::dot(seg[0], v));
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
        return (hi.x - lo.x) * (hi.y - lo.y);
    };
    for (int k = 0; k < 90; ++k)
        if (const double a = boxArea(glm::radians(double(k))); a < bestArea * (1.0 - 1e-9))
        {
            bestArea = a;
            bestAngle = glm::radians(double(k));
        }
    for (int k = -10; k <= 10; ++k)
        if (const double angle = bestAngle + glm::radians(0.1 * k), a = boxArea(angle); a < bestArea * (1.0 - 1e-9))
        {
            bestArea = a;
            bestAngle = angle;
        }
    s.u = std::cos(bestAngle) * u0 + std::sin(bestAngle) * v0;
    s.v = glm::cross(s.normal, s.u);
    s.top = -std::numeric_limits<double>::max();
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            s.top = std::max(s.top, glm::dot(m.position(p), s.normal));
    s.top += 1.0 + 1e-3 * std::sqrt(area);
    for (const auto& seg : s.border)
        s.border2.push_back({s.flat(seg[0]), s.flat(seg[1])});

    // Outline corners: border points where the outline turns by more than 50 degrees.
    std::unordered_map<Index, std::vector<Index>> next;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.isBoundaryEdge(e))
        {
            next[m.edgePoint(e, 0)].push_back(m.edgePoint(e, 1));
            next[m.edgePoint(e, 1)].push_back(m.edgePoint(e, 0));
        }
    for (const auto& [p, nb] : next)
    {
        if (nb.size() != 2)
            continue;
        const glm::dvec2 at = s.flat(m.position(p));
        const glm::dvec2 a = at - s.flat(m.position(nb[0])), b = s.flat(m.position(nb[1])) - at;
        if (glm::length(a) > 0.0 && glm::length(b) > 0.0 && glm::dot(glm::normalize(a), glm::normalize(b)) < std::cos(glm::radians(50.0)))
        {
            s.corners2.push_back(at);
            s.corners.push_back(m.position(p));
        }
    }
    return {};
}

struct Lattice
{
    glm::dvec2 origin{0.0};
    double cell = 1.0;
    int nx = 0, ny = 0;
    std::vector<uint8_t> occupied;

    glm::dvec2 at(int x, int y) const { return origin + glm::dvec2(x, y) * cell; }
    uint8_t& cellAt(int x, int y) { return occupied[size_t(y) * nx + x]; }
    uint8_t cellOr0(int x, int y) const { return x < 0 || y < 0 || x >= nx || y >= ny ? 0 : occupied[size_t(y) * nx + x]; }
};

/** Cells whose centre has the sheet under it, centred on the outline's bounds. */
Lattice occupy(const Sheet& s, const geo::MeshBvh& bvh, double cell)
{
    glm::dvec2 lo(std::numeric_limits<double>::max()), hi(-std::numeric_limits<double>::max());
    for (const auto& seg : s.border2)
    {
        lo = glm::min(lo, glm::min(seg[0], seg[1]));
        hi = glm::max(hi, glm::max(seg[0], seg[1]));
    }
    Lattice g;
    g.cell = cell;
    g.nx = std::max(1, int(std::ceil((hi.x - lo.x) / cell - 1e-6)));
    g.ny = std::max(1, int(std::ceil((hi.y - lo.y) / cell - 1e-6)));
    g.origin = 0.5 * (lo + hi) - 0.5 * glm::dvec2(g.nx, g.ny) * cell;
    g.occupied.assign(size_t(g.nx) * g.ny, 0);
    tbb::parallel_for(0, g.ny, [&](int y) {
        for (int x = 0; x < g.nx; ++x)
        {
            geo::BvhHit hit;
            g.occupied[size_t(y) * g.nx + x] = bvh.raycast(s.above(g.at(x, y) + glm::dvec2(0.5 * cell)), -s.normal, hit);
        }
    });
    return g;
}

/** Fills a cell at every point where two cells meet only corner to corner, which would pinch the result there. */
void unpinch(Lattice& g)
{
    for (bool changed = true; changed;)
    {
        changed = false;
        for (int y = 0; y <= g.ny; ++y)
            for (int x = 0; x <= g.nx; ++x)
            {
                const uint8_t a = g.cellOr0(x - 1, y - 1), b = g.cellOr0(x, y - 1), c = g.cellOr0(x - 1, y), d = g.cellOr0(x, y);
                if (a && d && !b && !c)
                {
                    g.cellAt(x, y - 1) = 1;
                    changed = true;
                }
                else if (b && c && !a && !d)
                {
                    g.cellAt(x, y) = 1;
                    changed = true;
                }
            }
    }
}

Index occupiedCount(const Lattice& g) { return Index(std::count(g.occupied.begin(), g.occupied.end(), uint8_t(1))); }
} // namespace

QuadRemeshResult gridRemesh(const geo::Mesh& source, const QuadRemeshOptions& opts)
{
    LILA_ZONE();

    QuadRemeshResult out;
    out.layout = "grid";
    if (opts.symmetryAxis >= 0 || !opts.guides.empty() || !opts.featureGuides.empty() || opts.poles || !opts.pinnedPoints.empty())
    {
        out.error = "the grid takes no symmetry, guides, poles or pins";
        return out;
    }
    geo::Mesh src = source;
    src.compact();
    Sheet sheet;
    if (out.error = sheetOf(src, sheet); !out.error.empty())
        return out;
    const geo::MeshBvh bvh(src);

    // Cells cover the sheet's shadow, so the count follows the projected area; a few tries settle it on the target.
    double projected = 0.0;
    for (Index f = 0; f < src.faceCapacity(); ++f)
        if (src.faceValid(f))
            projected += glm::dot(src.faceAreaVector(f), sheet.normal);
    const double target = std::max(8, opts.targetFaces);
    double cell = opts.targetEdgeLength > 0.0 ? opts.targetEdgeLength : std::sqrt(projected / target);
    Lattice g = occupy(sheet, bvh, cell);
    if (opts.targetEdgeLength <= 0.0)
        for (int attempt = 0; attempt < 6; ++attempt)
        {
            const double count = double(std::max<Index>(1, occupiedCount(g)));
            if (std::abs(count / target - 1.0) <= std::min(0.01, std::max(opts.countTolerance, 1e-3)))
                break;
            cell *= std::sqrt(count / target);
            g = occupy(sheet, bvh, cell);
        }
    unpinch(g);
    if (occupiedCount(g) == 0)
    {
        out.error = "the grid found no cells on the sheet";
        return out;
    }

    // Grid points of occupied cells, in the sheet's frame.
    std::vector<Index> id(size_t(g.nx + 1) * (g.ny + 1), kInvalid);
    std::vector<glm::dvec2> flat;
    std::vector<Index> sizes, idx;
    const auto point = [&](int x, int y) {
        Index& slot = id[size_t(y) * (g.nx + 1) + x];
        if (slot == kInvalid)
        {
            slot = Index(flat.size());
            flat.push_back(g.at(x, y));
        }
        return slot;
    };
    for (int y = 0; y < g.ny; ++y)
        for (int x = 0; x < g.nx; ++x)
            if (g.cellAt(x, y))
            {
                sizes.push_back(4);
                idx.insert(idx.end(), {point(x, y), point(x + 1, y), point(x + 1, y + 1), point(x, y + 1)});
            }
    const Index n = Index(flat.size());
    std::vector<std::vector<Index>> nb(n), borderNb(n);
    std::vector<uint8_t> onBorder(n, 0), fixed(n, 0);
    {
        std::unordered_map<uint64_t, int> uses;
        for (size_t f = 0; f < sizes.size(); ++f)
            for (int k = 0; k < 4; ++k)
                ++uses[edgeKey(idx[f * 4 + k], idx[f * 4 + (k + 1) % 4])];
        for (const auto& [key, count] : uses)
        {
            const Index a = Index(key >> 32), b = Index(key & 0xffffffffu);
            nb[a].push_back(b);
            nb[b].push_back(a);
            if (count == 1)
            {
                onBorder[a] = onBorder[b] = 1;
                borderNb[a].push_back(b);
                borderNb[b].push_back(a);
            }
        }
    }

    // Each outline corner pins its nearest grid border point, so a rectangular tile keeps square corners.
    for (const glm::dvec2& c : sheet.corners2)
    {
        Index best = kInvalid;
        double bestD = std::numeric_limits<double>::max();
        for (Index p = 0; p < n; ++p)
            if (onBorder[p] && !fixed[p])
                if (const double d = glm::distance(flat[p], c); d < bestD)
                {
                    bestD = d;
                    best = p;
                }
        if (best != kInvalid && bestD < 1.5 * cell)
        {
            flat[best] = c;
            fixed[best] = 1;
        }
    }
    for (Index p = 0; p < n; ++p)
        if (onBorder[p] && !fixed[p])
            flat[p] = nearestBorder(sheet, flat[p], nullptr);

    // Relax in the sheet's frame: inner points to the middle of their neighbours, border points along the outline. A
    // move that would turn a face over is skipped.
    std::vector<std::vector<Index>> pointFaces(n);
    for (size_t f = 0; f < sizes.size(); ++f)
        for (int k = 0; k < 4; ++k)
            pointFaces[idx[f * 4 + k]].push_back(Index(f));
    const auto faceArea2 = [&](size_t f, Index moved, const glm::dvec2& to) {
        double a = 0.0;
        for (int k = 0; k < 4; ++k)
        {
            const Index i = idx[f * 4 + k], j = idx[f * 4 + (k + 1) % 4];
            const glm::dvec2 p = i == moved ? to : flat[i], q = j == moved ? to : flat[j];
            a += p.x * q.y - p.y * q.x;
        }
        return 0.5 * a;
    };
    const int iterations = std::max(4, 3 * opts.relaxIterations);
    for (int it = 0; it < iterations; ++it)
    {
        std::vector<glm::dvec2> next = flat;
        tbb::parallel_for(Index(0), n, [&](Index p) {
            if (fixed[p])
                return;
            const std::vector<Index>& pull = onBorder[p] ? borderNb[p] : nb[p];
            if (pull.empty())
                return;
            glm::dvec2 avg(0.0);
            for (Index o : pull)
                avg += flat[o];
            glm::dvec2 to = glm::mix(flat[p], avg / double(pull.size()), 0.5);
            if (onBorder[p])
                to = nearestBorder(sheet, to, nullptr);
            for (Index f : pointFaces[p])
                if (faceArea2(f, p, to) <= 0.05 * cell * cell)
                    return;
            next[p] = to;
        });
        flat = std::move(next);
    }

    // Lift onto the sheet: border points onto the source border, the rest straight down onto the surface.
    std::vector<glm::dvec3> pos(n);
    tbb::parallel_for(Index(0), n, [&](Index p) {
        if (onBorder[p])
        {
            nearestBorder(sheet, flat[p], &pos[p]);
            return;
        }
        geo::BvhHit hit;
        geo::BvhClosest c;
        if (bvh.raycast(sheet.above(flat[p]), -sheet.normal, hit))
            pos[p] = hit.point;
        else if (bvh.closestPoint(sheet.above(flat[p]) - sheet.normal * sheet.top, c))
            pos[p] = c.point;
    });
    out.mesh = geo::Mesh::fromPolygons(pos, sizes, idx);
    for (Index f = 0; f < out.mesh.faceCapacity(); ++f)
        if (out.mesh.faceValid(f) && glm::dot(out.mesh.faceAreaVector(f), sheet.normal) <= 0.0)
            ++out.defects;
    out.metrics = measureQuality(out.mesh, &bvh);
    out.attempts.push_back({out.layout, out.mesh.numFaces(), out.defects});
    return out;
}
} // namespace lila::retopo::detail
