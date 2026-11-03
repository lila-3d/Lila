#include "curve/PipeRoute.h"

#include "core/Profile.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <queue>
#include <unordered_map>

namespace lila::curve
{
namespace
{
using D3 = glm::dvec3;
using I3 = glm::i64vec3;

constexpr int kNoDir = 6;

/** The axis direction (0 +x, 1 -x, 2 +y, 3 -y, 4 +z, 5 -z) nearest v, or kNoDir for zero. */
int axisDir(const D3& v)
{
    if (glm::length(v) < 1e-12)
        return kNoDir;
    int axis = 0;
    for (int k = 1; k < 3; ++k)
        if (std::abs(v[k]) > std::abs(v[axis]))
            axis = k;
    return axis * 2 + (v[axis] < 0.0 ? 1 : 0);
}

/** The grid: per-axis steps chosen so start and end both land on cell centres. */
struct Grid
{
    D3 origin{0.0}, step{1.0};
    I3 size{1};

    size_t count() const { return size_t(size.x) * size_t(size.y) * size_t(size.z); }
    size_t index(const I3& c) const { return size_t(c.x) + size_t(size.x) * (size_t(c.y) + size_t(size.y) * size_t(c.z)); }
    I3 cell(size_t i) const
    {
        return I3(int64_t(i % size_t(size.x)), int64_t((i / size_t(size.x)) % size_t(size.y)), int64_t(i / (size_t(size.x) * size_t(size.y))));
    }
    D3 centre(const I3& c) const { return origin + D3(c) * step; }
    bool inside(const I3& c) const { return c.x >= 0 && c.y >= 0 && c.z >= 0 && c.x < size.x && c.y < size.y && c.z < size.z; }
};

Grid makeGrid(const D3& a, const D3& b, double cell, double margin)
{
    Grid g;
    for (int k = 0; k < 3; ++k)
    {
        const double span = std::abs(b[k] - a[k]);
        const double n = std::round(span / cell);
        g.step[k] = n >= 1.0 ? span / n : cell;
        const int64_t below = int64_t(std::ceil((a[k] - std::min(a[k], b[k]) + margin) / g.step[k]));
        const int64_t above = int64_t(std::ceil((std::max(a[k], b[k]) + margin - a[k]) / g.step[k]));
        g.origin[k] = a[k] - double(below) * g.step[k];
        g.size[k] = below + above + 1;
    }
    return g;
}
} // namespace

PipeRouteResult routePipe(const D3& a, const D3& b, const ObstacleDistance& obstacles, const PipeRouteOptions& o)
{
    LILA_ZONE();

    PipeRouteResult r;
    r.points = {a, b};
    r.length = glm::distance(a, b);
    const double clear = std::max(o.clearance, 0.0);
    const double chord = glm::distance(a, b);
    if (chord < 1e-9)
    {
        r.found = true;
        return r;
    }
    double cell = o.cellSize > 0.0 ? o.cellSize : std::max({clear, chord / 256.0, 1e-3});
    auto marginFor = [&](double c) { return o.margin > 0.0 ? o.margin : std::max(chord * 0.25, 4.0 * c + clear); };
    Grid grid = makeGrid(a, b, cell, marginFor(cell));
    while (grid.count() > std::max<size_t>(o.maxCells, 64))
    {
        cell *= 1.25;
        grid = makeGrid(a, b, cell, marginFor(cell));
    }
    r.cell = grid.step;

    const D3 rel = (b - a) / grid.step;
    const I3 startCell = I3(glm::round((a - grid.origin) / grid.step));
    const I3 endCell = startCell + I3(glm::round(rel));
    const double maxStep = std::max({grid.step.x, grid.step.y, grid.step.z});
    // Ends often sit on the wall they come out of, so the space right around them is always open.
    const double endFree = clear + maxStep * 1.01;
    auto nearEnd = [&](const D3& p) { return glm::distance(p, a) <= endFree || glm::distance(p, b) <= endFree; };
    auto open = [&](const D3& p) { return !obstacles || nearEnd(p) || obstacles(p, clear * 2.0 + maxStep) >= clear; };

    std::vector<uint8_t> state(grid.count(), 0); // 0 unknown, 1 open, 2 blocked
    auto cellOpen = [&](const I3& c) {
        uint8_t& s = state[grid.index(c)];
        if (s == 0)
            s = open(grid.centre(c)) ? 1 : 2;
        return s == 1;
    };

    const std::array<I3, 6> dirs = {I3(1, 0, 0), I3(-1, 0, 0), I3(0, 1, 0), I3(0, -1, 0), I3(0, 0, 1), I3(0, 0, -1)};
    const int firstDir = axisDir(o.startDir), lastDir = axisDir(o.endDir);
    auto heuristic = [&](const I3& c) {
        double h = 0.0;
        for (int k = 0; k < 3; ++k)
            h += double(std::abs(endCell[k] - c[k])) * grid.step[k] * std::max(o.axisCost[k], 1e-6);
        return h;
    };

    struct Open
    {
        double f, g;
        uint64_t s;
        bool operator>(const Open& x) const { return f > x.f; }
    };
    auto key = [](size_t cellIndex, int dir) { return uint64_t(cellIndex) * 7 + uint64_t(dir); };
    std::priority_queue<Open, std::vector<Open>, std::greater<>> queue;
    std::unordered_map<uint64_t, double> best;
    std::unordered_map<uint64_t, uint64_t> from;
    const uint64_t start = key(grid.index(startCell), kNoDir);
    best[start] = 0.0;
    queue.push({heuristic(startCell), 0.0, start});
    uint64_t goal = UINT64_MAX;
    size_t expanded = 0;
    while (!queue.empty() && expanded < o.maxExpanded)
    {
        Open cur = queue.top();
        queue.pop();
        if (auto it = best.find(cur.s); it != best.end() && cur.g > it->second)
            continue;
        ++expanded;
        const I3 c = grid.cell(size_t(cur.s / 7));
        const int dir = int(cur.s % 7);
        if (c == endCell && (lastDir == kNoDir || dir == lastDir))
        {
            goal = cur.s;
            break;
        }
        for (int d = 0; d < 6; ++d)
        {
            if (dir != kNoDir && d == (dir ^ 1))
                continue;
            if (dir == kNoDir && firstDir != kNoDir && d != firstDir)
                continue;
            const I3 n = c + dirs[size_t(d)];
            if (!grid.inside(n) || !cellOpen(n))
                continue;
            const int axis = d / 2;
            if (!open((grid.centre(c) + grid.centre(n)) * 0.5))
                continue;
            const double g = cur.g + grid.step[axis] * std::max(o.axisCost[axis], 1e-6) + (dir != kNoDir && dir != d ? std::max(o.bendCost, 0.0) : 0.0);
            const uint64_t s = key(grid.index(n), d);
            auto it = best.find(s);
            if (it != best.end() && it->second <= g)
                continue;
            best[s] = g;
            from[s] = cur.s;
            queue.push({g + heuristic(n), g, s});
        }
    }
    if (goal == UINT64_MAX)
    {
        r.warning = expanded >= o.maxExpanded ? std::format("no route found within {} cells searched: raise the cell size", expanded)
                                              : "no route found: the obstacles close off every way between the ends";
        return r;
    }

    std::vector<I3> cells;
    for (uint64_t s = goal;; s = from[s])
    {
        cells.push_back(grid.cell(size_t(s / 7)));
        if (s == start)
            break;
    }
    std::reverse(cells.begin(), cells.end());
    r.points.clear();
    for (size_t k = 0; k < cells.size(); ++k)
    {
        if (k > 0 && k + 1 < cells.size() && cells[k] - cells[k - 1] == cells[k + 1] - cells[k])
            continue;
        r.points.push_back(grid.centre(cells[k]));
    }
    r.points.front() = a;
    r.points.back() = b;
    r.found = true;
    r.bends = int(r.points.size()) - 2;
    r.length = 0.0;
    for (size_t k = 1; k < r.points.size(); ++k)
        r.length += glm::distance(r.points[k - 1], r.points[k]);
    return r;
}

Spline routeSpline(const PipeRouteResult& r)
{
    Spline s;
    s.kind = Kind::Poly;
    s.points = r.points;
    s.closed = false;
    s.normalize();
    return s;
}
} // namespace lila::curve
