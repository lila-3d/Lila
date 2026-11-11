#include "uv/FinalMesh.h"

#include "core/Profile.h"
#include "uv/Pack.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <optional>

namespace lila::uv
{
namespace
{
/** Raster cells per uv unit the overlap between islands is measured in. */
constexpr int kFine = 2048;
/** Fine cells per coarse cell side, for the far search. */
constexpr int kCoarse = 4;
/** Fine cells an island may move in the near search. */
constexpr int kFineReach = 12;
/** Coarse cells an island may move in the far search (0.125 of a tile) before it is packed instead. */
constexpr int kCoarseReach = 64;
/** Coarse cells an island no move clears may shift to overlap less. */
constexpr int kLeastReach = 16;

using ExportTri = std::array<Index, 3>;
using Cell = glm::ivec2;

/** Calls fn(x, y) for each fine cell whose centre the triangle covers, as bake lint samples them. */
template <class Fn>
void rasterize(const glm::dvec2& a, const glm::dvec2& b, const glm::dvec2& c, Fn&& fn)
{
    const glm::dvec2 p[3] = {a * double(kFine), b * double(kFine), c * double(kFine)};
    auto edge = [](const glm::dvec2& s, const glm::dvec2& t, const glm::dvec2& q) { return (t.x - s.x) * (q.y - s.y) - (t.y - s.y) * (q.x - s.x); };
    const double area = edge(p[0], p[1], p[2]);
    if (std::abs(area) < 1e-12)
        return;
    const double sign = area > 0.0 ? 1.0 : -1.0;
    const int x0 = int(std::floor(std::min({p[0].x, p[1].x, p[2].x}))), x1 = int(std::ceil(std::max({p[0].x, p[1].x, p[2].x})));
    const int y0 = int(std::floor(std::min({p[0].y, p[1].y, p[2].y}))), y1 = int(std::ceil(std::max({p[0].y, p[1].y, p[2].y})));
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
        {
            const glm::dvec2 q(x + 0.5, y + 0.5);
            if (sign * edge(p[0], p[1], q) >= 0.0 && sign * edge(p[1], p[2], q) >= 0.0 && sign * edge(p[2], p[0], q) >= 0.0)
                fn(x, y);
        }
}

/** A set of cells over a fixed region (outside it nothing is set). */
struct Bitmap
{
    Cell lo{0}, size{0};
    std::vector<uint8_t> bits;

    Bitmap() = default;
    Bitmap(Cell l, Cell s) : lo(l), size(s), bits(size_t(s.x) * size_t(s.y), 0) {}
    bool inside(const Cell& c) const { return c.x >= lo.x && c.y >= lo.y && c.x < lo.x + size.x && c.y < lo.y + size.y; }
    size_t index(const Cell& c) const { return size_t(c.y - lo.y) * size_t(size.x) + size_t(c.x - lo.x); }
    bool get(const Cell& c) const { return inside(c) && bits[index(c)] != 0; }
};

/** Distinct cells the triangles cover, row by row; with mask, also the cells as a bitmap over their box. */
template <class Corners>
std::vector<Cell> cellsOf(size_t count, Corners&& corners, Bitmap* mask = nullptr)
{
    glm::dvec2 lo(std::numeric_limits<double>::max()), hi(std::numeric_limits<double>::lowest());
    for (size_t t = 0; t < count; ++t)
        for (const glm::dvec2& p : corners(t))
        {
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
    if (count == 0)
        return {};
    const Cell blo = Cell(glm::floor(lo * double(kFine))) - 1, bhi = Cell(glm::ceil(hi * double(kFine))) + 1;
    Bitmap local(blo, bhi - blo + 1);
    for (size_t t = 0; t < count; ++t)
    {
        const std::array<glm::dvec2, 3> p = corners(t);
        rasterize(p[0], p[1], p[2], [&](int x, int y) { local.bits[local.index({x, y})] = 1; });
    }
    std::vector<Cell> out;
    for (int y = 0; y < local.size.y; ++y)
        for (int x = 0; x < local.size.x; ++x)
            if (local.bits[size_t(y) * size_t(local.size.x) + size_t(x)])
                out.push_back(blo + Cell(x, y));
    if (mask)
        *mask = std::move(local);
    return out;
}

std::vector<Cell> cellsOf(std::span<const ExportTri> tris, std::span<const glm::dvec2> uv, Bitmap* mask = nullptr)
{
    return cellsOf(tris.size(), [&](size_t t) { return std::array{uv[tris[t][0]], uv[tris[t][1]], uv[tris[t][2]]}; }, mask);
}

Cell coarseOf(const Cell& c)
{
    return {int(std::floor(double(c.x) / kCoarse)), int(std::floor(double(c.y) / kCoarse))};
}

/** Integer offsets within reach, nearest first. */
std::vector<Cell> offsetsWithin(int reach)
{
    std::vector<Cell> o;
    for (int y = -reach; y <= reach; ++y)
        for (int x = -reach; x <= reach; ++x)
            if (x * x + y * y <= reach * reach)
                o.push_back({x, y});
    std::ranges::stable_sort(o, [](const Cell& a, const Cell& b) { return a.x * a.x + a.y * a.y < b.x * b.x + b.y * b.y; });
    return o;
}

/** Cells of the set (its mask given) with a side neighbour outside it, nearest the point first. */
std::vector<Cell> outlineOf(std::span<const Cell> cells, const Bitmap& mask, const glm::dvec2& near)
{
    std::vector<Cell> out;
    for (const Cell& c : cells)
        for (const Cell& d : {Cell(1, 0), Cell(-1, 0), Cell(0, 1), Cell(0, -1)})
            if (!mask.get(c + d))
            {
                out.push_back(c);
                break;
            }
    std::ranges::sort(out, [&](const Cell& a, const Cell& b) {
        const glm::dvec2 da = glm::dvec2(a) - near, db = glm::dvec2(b) - near;
        return glm::dot(da, da) < glm::dot(db, db);
    });
    return out;
}

/** Islands stay in their tile; one already past its edge may not go further. */
bool staysInTile(const Cell& boxLo, const Cell& boxHi, const Cell& d, const Cell& boundLo, const Cell& boundHi)
{
    const Cell a = boxLo + d, b = boxHi + d;
    return !((a.x < boundLo.x && d.x < 0) || (a.y < boundLo.y && d.y < 0) || (b.x >= boundHi.x && d.x > 0) || (b.y >= boundHi.y && d.y > 0));
}

/** The first offset (nearest first) that lands every cell on a free one and keeps the island's box inside bounds. */
template <class Blocked>
std::optional<Cell> firstFree(std::span<const Cell> offsets, std::span<const Cell> outline, std::span<const Cell> cells, Blocked&& blocked,
                              const Cell& boxLo, const Cell& boxHi, const Cell& boundLo, const Cell& boundHi, int skipWithin)
{
    for (const Cell& d : offsets)
    {
        if (d.x * d.x + d.y * d.y <= skipWithin * skipWithin || !staysInTile(boxLo, boxHi, d, boundLo, boundHi))
            continue;
        auto hits = [&](const Cell& c) { return blocked(c + d); };
        if (std::ranges::any_of(outline, hits) || std::ranges::any_of(cells, hits))
            continue;
        return d;
    }
    return std::nullopt;
}

/**
 * Per cage island, the exported triangles whose uvs it gives. The exported mesh's own uv islands are the cage's, but a
 * face can carry a neighbouring island's uvs (a bevel strip on a seam), so each is matched by the cage corners its rows read.
 */
std::vector<std::vector<ExportTri>> exportTrianglesOf(const FinalMap& map, const geo::Mesh& cage, std::span<const Chart> charts,
                                                      std::string_view uvSet)
{
    std::vector<int> cornerIsland(cage.cornerCapacity(), -1);
    for (size_t i = 0; i < charts.size(); ++i)
        for (Index c : charts[i].corners)
            cornerIsland[c] = int(i);
    const geo::Mesh& ex = *map.exported;
    ChartOptions co;
    co.source = IslandSource::Uvs;
    co.uvSet = uvSet;
    std::vector<std::vector<ExportTri>> out(charts.size());
    for (const std::vector<Index>& faces : islandFaces(ex, {}, co))
    {
        std::map<int, size_t> votes;
        for (Index f : faces)
            ex.forEachFaceCorner(f, [&](Index c) {
                for (Index k = map.rowStart[c]; k < map.rowStart[c + 1]; ++k)
                    if (int i = cornerIsland[map.cols[k]]; i >= 0)
                        ++votes[i];
            });
        if (votes.empty())
            continue;
        const int island = std::ranges::max_element(votes, {}, [](const auto& v) { return v.second; })->first;
        for (Index f : faces)
        {
            const std::vector<Index> cs = ex.faceCorners(f);
            for (size_t k = 1; k + 1 < cs.size(); ++k)
                out[size_t(island)].push_back({cs[0], cs[k], cs[k + 1]});
        }
    }
    return out;
}

/** A chart standing for triangles in uv, for packing around them. */
Chart proxyChart(std::span<const ExportTri> tris, std::span<const glm::dvec2> uv, const geo::Mesh& ex)
{
    Chart p;
    std::map<Index, Index> local;
    for (const ExportTri& t : tris)
    {
        std::array<Index, 3> lt{};
        for (size_t k = 0; k < 3; ++k)
        {
            auto [it, added] = local.try_emplace(t[k], Index(p.uv.size()));
            if (added)
            {
                p.uv.push_back(uv[t[k]]);
                p.pos.push_back(ex.position(ex.cornerPoint(t[k])));
                p.pinned.push_back(0);
            }
            lt[k] = it->second;
        }
        p.tris.push_back(lt);
        p.triFace.push_back(0);
    }
    return p;
}

/** How many owners cover each fine cell, and each coarse cell's sum of those, over a frame round every island. */
struct Coverage
{
    Cell lo{0}, size{0}, coarseLo{0}, coarseSize{0};
    std::vector<uint16_t> fine;
    std::vector<uint32_t> coarse;

    Coverage(Cell l, Cell s) : lo(l), size(s), coarseLo(coarseOf(l)), coarseSize(coarseOf(l + s - 1) - coarseOf(l) + 1)
    {
        fine.assign(size_t(size.x) * size_t(size.y), 0);
        coarse.assign(size_t(coarseSize.x) * size_t(coarseSize.y), 0);
    }
    bool inside(const Cell& c) const { return c.x >= lo.x && c.y >= lo.y && c.x < lo.x + size.x && c.y < lo.y + size.y; }
    int at(const Cell& c) const { return inside(c) ? fine[size_t(c.y - lo.y) * size_t(size.x) + size_t(c.x - lo.x)] : 0; }
    uint32_t coarseAt(const Cell& cc) const
    {
        const Cell q = cc - coarseLo;
        if (q.x < 0 || q.y < 0 || q.x >= coarseSize.x || q.y >= coarseSize.y)
            return 0;
        return coarse[size_t(q.y) * size_t(coarseSize.x) + size_t(q.x)];
    }
    void add(std::span<const Cell> cells, int by)
    {
        for (const Cell& c : cells)
        {
            if (!inside(c))
                continue;
            fine[size_t(c.y - lo.y) * size_t(size.x) + size_t(c.x - lo.x)] += uint16_t(by);
            const Cell q = coarseOf(c) - coarseLo;
            coarse[size_t(q.y) * size_t(coarseSize.x) + size_t(q.x)] += uint32_t(by);
        }
    }
};

/** Per coarse cell over a fine cell set's box, how many of the set's cells it holds. */
struct CoarseCounts
{
    Cell lo{0}, size{0};
    std::vector<uint32_t> count;
    /** The coarse cells holding any, in row order. */
    std::vector<Cell> cells;

    CoarseCounts(std::span<const Cell> fine, const Cell& boxLo, const Cell& boxHi)
        : lo(coarseOf(boxLo)), size(coarseOf(boxHi) - coarseOf(boxLo) + 1), count(size_t(size.x) * size_t(size.y), 0)
    {
        for (const Cell& c : fine)
            ++count[index(coarseOf(c))];
        for (int y = 0; y < size.y; ++y)
            for (int x = 0; x < size.x; ++x)
                if (count[size_t(y) * size_t(size.x) + size_t(x)])
                    cells.push_back(lo + Cell(x, y));
    }
    bool inside(const Cell& q) const { return q.x >= lo.x && q.y >= lo.y && q.x < lo.x + size.x && q.y < lo.y + size.y; }
    size_t index(const Cell& q) const { return size_t(q.y - lo.y) * size_t(size.x) + size_t(q.x - lo.x); }
    uint32_t at(const Cell& q) const { return inside(q) ? count[index(q)] : 0; }
};
} // namespace

size_t keepIslandsApart(geo::Mesh& cage, const FinalMap& map, std::string_view uvSet, std::span<const glm::vec2> before,
                        std::span<const std::array<glm::dvec2, 3>> others)
{
    LILA_ZONE();
    const std::span<const glm::vec2> nowUv = cage.cornerAttrs().get<glm::vec2>(uvSet);
    if (!map.active() || !map.exported || nowUv.empty() || before.size() != nowUv.size())
        return 0;
    ChartOptions co;
    co.source = IslandSource::Uvs;
    co.uvSet = uvSet;
    std::vector<Chart> charts = buildCharts(cage, {}, co);
    const size_t n = charts.size();

    // Islands the tool moved, unpinned.
    std::vector<uint8_t> movable(n, 0);
    for (size_t i = 0; i < n; ++i)
        if (std::ranges::none_of(charts[i].pinned, [](uint8_t p) { return p != 0; }))
            movable[i] = std::ranges::any_of(charts[i].corners, [&](Index c) { return before[c] != nowUv[c]; });
    if (std::ranges::none_of(movable, [](uint8_t m) { return m != 0; }))
        return 0;

    const geo::Mesh& ex = *map.exported;
    const std::vector<std::vector<ExportTri>> tris = exportTrianglesOf(map, cage, charts, uvSet);
    const std::vector<glm::dvec2> was = exportedUvs(map, before);
    std::vector<glm::dvec2> uv = exportedUvs(map, nowUv);

    // Per island its cells before and now (and as bitmaps); the other meshes' triangles are one more owner, n.
    std::vector<std::vector<Cell>> cellsWas(n + 1), cells(n + 1);
    std::vector<Bitmap> maskWas(n + 1), mask(n + 1);
    tbb::parallel_for(size_t(0), 2 * n + 1, size_t(1), [&](size_t k) {
        if (k == 2 * n)
            cells[n] = cellsOf(others.size(), [&](size_t t) { return others[t]; }, &mask[n]);
        else if (k < n)
            cellsWas[k] = cellsOf(tris[k], was, &maskWas[k]);
        else
            cells[k - n] = cellsOf(tris[k - n], uv, &mask[k - n]);
    });
    cellsWas[n] = cells[n];
    maskWas[n] = mask[n];

    Cell lo(std::numeric_limits<int>::max()), hi(std::numeric_limits<int>::min());
    for (const auto* set : {&cellsWas, &cells})
        for (const std::vector<Cell>& cs : *set)
            for (const Cell& c : cs)
            {
                lo = glm::min(lo, c);
                hi = glm::max(hi, c);
            }
    if (lo.x > hi.x)
        return 0;
    const int margin = kCoarseReach * kCoarse + 2 * kCoarse;
    const Cell frameLo = lo - margin, frameSize = hi - lo + 2 * margin + 1;
    if (double(frameSize.x) * double(frameSize.y) > 2.5e7)
        return 0;

    // Islands that overlapped before (or touched) may go on doing so where they did, and no further.
    Coverage now(frameLo, frameSize);
    {
        Coverage then(frameLo, frameSize);
        for (size_t i = 0; i <= n; ++i)
        {
            then.add(cellsWas[i], 1);
            now.add(cells[i], 1);
        }
        // Kept cells: an island's cells another owner shared before; the coverage keeps them out of its blocked set.
        for (size_t i = 0; i < n; ++i)
            std::erase_if(cellsWas[i], [&](const Cell& c) { return then.at(c) < 2; });
    }
    auto keptFor = [&](size_t i) {
        Bitmap kept;
        if (!cellsWas[i].empty())
        {
            const auto [blo, bhi] = std::ranges::minmax(cellsWas[i], [](const Cell& a, const Cell& b) { return a.y < b.y; });
            Cell klo = blo, khi = bhi;
            for (const Cell& c : cellsWas[i])
            {
                klo.x = std::min(klo.x, c.x);
                khi.x = std::max(khi.x, c.x);
            }
            kept = Bitmap(Cell(klo.x, blo.y), Cell(khi.x - klo.x + 1, bhi.y - blo.y + 1));
            for (const Cell& c : cellsWas[i])
                kept.bits[kept.index(c)] = 1;
        }
        return kept;
    };
    // A cell is blocked for an island when another owner covers it, unless the island shared it before.
    auto blockedFor = [&](size_t i, const Bitmap& kept) {
        return [&, i](const Cell& c) { return now.at(c) > int(mask[i].get(c)) && !kept.get(c); };
    };

    std::vector<size_t> order;
    std::vector<Bitmap> kept(n);
    for (size_t i = 0; i < n; ++i)
        if (movable[i])
        {
            kept[i] = keptFor(i);
            const auto blocked = blockedFor(i, kept[i]);
            if (std::ranges::any_of(cells[i], blocked))
                order.push_back(i);        }
    if (order.empty())
        return 0;
    // Small islands first: a big one then often finds the small ones it grew into already gone.
    std::ranges::stable_sort(order, [&](size_t a, size_t b) { return cells[a].size() < cells[b].size(); });

    static const std::vector<Cell> nearOffsets = offsetsWithin(kFineReach), farOffsets = offsetsWithin(kCoarseReach),
                                   leastOffsets = offsetsWithin(kLeastReach);
    std::vector<glm::dvec2> shift(n, glm::dvec2(0.0));
    auto boxOf = [&](size_t i) { return std::pair{mask[i].lo, mask[i].lo + mask[i].size - 1}; };
    auto tileOf = [&](size_t i) { return Cell(glm::floor(charts[i].uvCentroid())) * kFine; };
    auto coarseOwn = [&](size_t i) {
        const auto [boxLo, boxHi] = boxOf(i);
        return CoarseCounts(cells[i], boxLo, boxHi);
    };
    // Fine cells of other owners in a coarse cell, kept cells included, so a free coarse move is free cell by cell too.
    auto crowdOf = [&](const CoarseCounts& own) { return [&](const Cell& q) { return now.coarseAt(q) - own.at(q); }; };
    auto moveBy = [&](size_t i, const glm::dvec2& t) {
        shift[i] += t;
        std::vector<uint8_t> done(ex.cornerCapacity(), 0);
        for (const ExportTri& tr : tris[i])
            for (Index c : tr)
                if (!done[c])
                {
                    done[c] = 1;
                    uv[c] += t;
                }
        now.add(cells[i], -1);
        cells[i] = cellsOf(tris[i], uv, &mask[i]);
        now.add(cells[i], 1);
    };

    // Moves the island the least distance that clears it; false when it is blocked and no move within reach clears it.
    auto place = [&](size_t i) {
        const auto blocked = blockedFor(i, kept[i]);
        glm::dvec2 at(0.0);
        size_t hits = 0;
        for (const Cell& c : cells[i])
            if (blocked(c))
            {
                at += glm::dvec2(c);
                ++hits;
            }
        if (hits == 0)
            return true;
        at /= double(hits);
        const auto [boxLo, boxHi] = boxOf(i);
        const Cell tileLo = tileOf(i), tileHi = tileLo + kFine;
        std::optional<Cell> found = firstFree(nearOffsets, outlineOf(cells[i], mask[i], at), cells[i], blocked, boxLo, boxHi, tileLo, tileHi, -1);
        if (!found)
        {
            const CoarseCounts own = coarseOwn(i);
            const auto crowd = crowdOf(own);
            const std::vector<Cell>& mine = own.cells;
            std::vector<Cell> rim;
            for (const Cell& c : mine)
                for (const Cell& d : {Cell(1, 0), Cell(-1, 0), Cell(0, 1), Cell(0, -1)})
                    if (!own.at(c + d))
                    {
                        rim.push_back(c);
                        break;
                    }
            const glm::dvec2 cat = at / double(kCoarse);
            std::ranges::sort(rim, {}, [&](const Cell& c) { return glm::dot(glm::dvec2(c) - cat, glm::dvec2(c) - cat); });
            // The coarse box rounds outwards; keep it inside the tile's coarse cells.
            if (auto d = firstFree(farOffsets, rim, mine, [&](const Cell& q) { return crowd(q) > 0; }, coarseOf(boxLo), coarseOf(boxHi),
                                   coarseOf(tileLo), coarseOf(tileHi - 1) + 1, kFineReach / kCoarse))
                found = *d * kCoarse;
        }
        if (!found)
            return false;
        moveBy(i, glm::dvec2(*found) / double(kFine));
        return true;
    };

    // Where nothing has room for the island at its size, the nearby move that leaves it overlapping least, if that is less.
    auto leastOverlap = [&](size_t i) {
        const auto blocked = blockedFor(i, kept[i]);
        auto hitsAt = [&](const Cell& d) { return size_t(std::ranges::count_if(cells[i], [&](const Cell& c) { return blocked(c + d); })); };
        const CoarseCounts own = coarseOwn(i);
        const auto crowd = crowdOf(own);
        const std::vector<Cell>& mine = own.cells;
        const auto [boxLo, boxHi] = boxOf(i);
        const Cell tileLo = tileOf(i), tileHi = tileLo + kFine;
        uint64_t best = std::numeric_limits<uint64_t>::max();
        Cell bestD(0);
        for (const Cell& d : leastOffsets)
        {
            const Cell f = d * kCoarse;
            if (!staysInTile(boxLo, boxHi, f, tileLo, tileHi))
                continue;
            uint64_t sum = 0;
            for (const Cell& c : mine)
                if ((sum += crowd(c + d)) >= best)
                    break;
            if (sum < best)
            {
                best = sum;
                bestD = f;
            }
        }
        if (bestD != Cell(0) && hitsAt(bestD) < hitsAt(Cell(0)))
            moveBy(i, glm::dvec2(bestD) / double(kFine));
    };

    std::vector<size_t> unplaced, movesBefore;
    size_t moves = 0;
    for (size_t i : order)
        if (place(i))
            ++moves;
        else
        {
            unplaced.push_back(i);
            movesBefore.push_back(moves);
        }
    // Again for those left that others moved after: the islands they hit may have moved off.
    for (size_t k = unplaced.size(); k-- > 0;)
        if (movesBefore[k] < moves && place(unplaced[k]))
            unplaced.erase(unplaced.begin() + std::ptrdiff_t(k));

    // Those no short move clears are packed at their size around the rest, through their exported shapes.
    if (!unplaced.empty())
    {
        std::vector<Chart> proxies;
        std::vector<PackChartInfo> info;
        for (size_t i = 0; i < n; ++i)
        {
            proxies.push_back(proxyChart(tris[i], uv, ex));
            info.push_back({0, std::ranges::find(unplaced, i) == unplaced.end()});
        }
        if (!others.empty())
        {
            Chart o;
            for (const auto& t : others)
            {
                const Index b = Index(o.uv.size());
                for (const glm::dvec2& p : t)
                {
                    o.uv.push_back(p);
                    o.pos.push_back(glm::dvec3(p, 0.0));
                    o.pinned.push_back(0);
                }
                o.tris.push_back({b, b + 1, b + 2});
                o.triFace.push_back(0);
            }
            proxies.push_back(std::move(o));
            info.push_back({0, true});
        }
        PackOptions po;
        po.lockScale = true;
        po.rotateStep = 0;
        po.preOrient = false;
        po.layout = PackLayout::KeepTile;
        po.resolution = kFine;
        // Two texels apart: one lint cell at half this resolution.
        po.padding = 2.0;
        po.margin = 1.0;
        po.effort = 0;
        std::vector<glm::dvec2> centre(n);
        for (size_t i : unplaced)
            centre[i] = proxies[i].uvCentroid();
        if (packCharts(proxies, po, info).ok)
            for (size_t i : unplaced)
                moveBy(i, proxies[i].uvCentroid() - centre[i]);
        else
            for (size_t i : unplaced)
                leastOverlap(i);
    }

    std::vector<Chart> moved;
    for (size_t i = 0; i < n; ++i)
        if (shift[i] != glm::dvec2(0.0))
        {
            for (glm::dvec2& p : charts[i].uv)
                p += shift[i];
            moved.push_back(std::move(charts[i]));
        }
    if (!moved.empty())
        writeCharts(cage, moved, uvSet);
    return moved.size();
}
} // namespace lila::uv
