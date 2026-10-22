#include "uv/Uv.h"

#include "core/Profile.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace lila::uv
{
namespace
{
bool usable(const Chart& ch)
{
    double a = std::abs(ch.areaUv());
    return std::isfinite(a) && a > 1e-14 * std::max(1.0, ch.area3d()) && foldedTriangles(ch) == 0;
}

size_t pinCount(const Chart& ch)
{
    size_t n = 0;
    for (uint8_t p : ch.pinned)
        n += p ? 1 : 0;
    return n;
}

/** Turns the chart about its centroid to best match a previous layout of the same vertices. */
void matchRotation(Chart& ch, const std::vector<glm::dvec2>& old)
{
    glm::dvec2 cn(0.0), co(0.0);
    for (size_t i = 0; i < old.size(); ++i)
    {
        cn += ch.uv[i];
        co += old[i];
    }
    cn /= double(old.size());
    co /= double(old.size());
    double s = 0.0, c = 0.0;
    for (size_t i = 0; i < old.size(); ++i)
    {
        glm::dvec2 a = ch.uv[i] - cn, b = old[i] - co;
        s += a.x * b.y - a.y * b.x;
        c += glm::dot(a, b);
    }
    if (s == 0.0 && c == 0.0)
        return;
    transformChart(ch, cn, std::atan2(s, c));
}

std::string& currentSet()
{
    static std::string name = "uv0";
    return name;
}
} // namespace

std::vector<Index> uvCornersOfFaces(const geo::Mesh& mesh, std::string_view uvSet, std::span<const Index> faces)
{
    std::span<const glm::vec2> uv = mesh.cornerAttrs().get<glm::vec2>(uvSet);
    std::vector<Index> out;
    for (Index f : liveFaces(mesh, faces))
        mesh.forEachFaceCorner(f, [&](Index c) {
            out.push_back(c);
            if (uv.empty())
                return;
            mesh.forEachPointCorner(mesh.cornerPoint(c), [&](Index k) {
                if (glm::all(glm::lessThanEqual(glm::abs(uv[k] - uv[c]), glm::vec2(1e-6f))))
                    out.push_back(k);
            });
        });
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<Index> cornersOfPoints(const geo::Mesh& mesh, std::span<const Index> points)
{
    std::vector<Index> out;
    for (Index p : points)
        if (mesh.pointValid(p))
            mesh.forEachPointCorner(p, [&](Index c) { out.push_back(c); });
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

const std::string& currentUvSet()
{
    return currentSet();
}

void setCurrentUvSet(std::string name)
{
    currentSet() = name.empty() ? std::string("uv0") : std::move(name);
}

UnwrapFacesResult unwrapFaces(geo::Mesh& mesh, std::span<const Index> faceIds, std::string_view uvSet, const UnwrapFacesOptions& opts)
{
    LILA_ZONE();

    UnwrapFacesResult res;
    std::vector<Index> faces = liveFaces(mesh, faceIds);
    if (faces.empty())
        return res;
    res.seamsAdded = cutToDisks(mesh, faces);
    ChartOptions co;
    co.uvSet = uvSet;
    std::vector<Chart> charts = buildCharts(mesh, faces, co);
    res.islands = charts.size();

    std::vector<uint8_t> ok(charts.size(), 0), hadUvs(charts.size(), 0);
    std::vector<std::vector<glm::dvec2>> old(charts.size());
    std::vector<glm::dvec2> oldCentroid(charts.size());
    std::vector<double> oldArea(charts.size());
    tbb::parallel_for(size_t(0), charts.size(), size_t(1), [&](size_t i) {
        Chart& ch = charts[i];
        hadUvs[i] = usable(ch) ? 1 : 0;
        old[i] = ch.uv;
        oldCentroid[i] = ch.uvCentroid();
        oldArea[i] = std::abs(ch.areaUv());
        ok[i] = unwrapChart(ch, opts.unwrap).ok ? 1 : 0;
        if (!ok[i])
            ch.uv = old[i];
    });

    std::vector<Chart*> toPack;
    for (size_t i = 0; i < charts.size(); ++i)
    {
        if (!ok[i])
        {
            ++res.failed;
            continue;
        }
        Chart& ch = charts[i];
        if (opts.unwrap.usePins && pinCount(ch) >= 2)
            continue;
        if (!opts.pack && opts.keepPlacement && hadUvs[i])
        {
            matchRotation(ch, old[i]);
            fitChart(ch, oldCentroid[i], oldArea[i]);
        }
        else
            toPack.push_back(&ch);
    }
    if (!toPack.empty())
    {
        std::vector<Chart> packed;
        packed.reserve(toPack.size());
        for (Chart* c : toPack)
            packed.push_back(std::move(*c));
        packCharts(packed, opts.packing);
        for (size_t i = 0; i < toPack.size(); ++i)
            *toPack[i] = std::move(packed[i]);
    }
    writeCharts(mesh, charts, uvSet);
    return res;
}

AutoUvResult autoUv(geo::Mesh& mesh, std::span<const Index> faces, std::string_view uvSet, const AutoUvOptions& opts)
{
    LILA_ZONE();

    AutoUvResult res;
    AutoSeamResult seams = autoSeams(mesh, faces, opts.seams);
    res.seams = seams.seamEdges;
    std::vector<Chart>& charts = seams.charts;
    res.islands = charts.size();
    UnwrapOptions uo = opts.unwrap;
    uo.usePins = false;
    tbb::parallel_for(size_t(0), charts.size(), size_t(1), [&](size_t i) {
        Chart& ch = charts[i];
        std::fill(ch.pinned.begin(), ch.pinned.end(), uint8_t(0));
        // Islands come back already flattened once; refine those, start over on the rest.
        if (uo.method == UnwrapMethod::Slim && usable(ch) && slim(ch, uo.iterations, false) >= 0)
            return;
        unwrapChart(ch, uo);
    });
    res.pack = packCharts(charts, opts.pack);
    writeCharts(mesh, charts, uvSet);
    return res;
}

LightmapResult generateLightmapUvs(geo::Mesh& mesh, const LightmapOptions& opts)
{
    LILA_ZONE();

    LightmapResult res;
    std::vector<Chart> charts;
    if (!mesh.cornerAttrs().get<glm::vec2>(opts.source).empty())
    {
        ChartOptions co;
        co.source = IslandSource::Uvs;
        co.uvSet = opts.source;
        charts = buildCharts(mesh, {}, co);
    }
    else
    {
        // Seams are worked out on a copy so the mesh's own seams stay as they are.
        geo::Mesh scratch = mesh;
        charts = autoSeams(scratch).charts;
    }
    res.islands = charts.size();
    std::atomic<size_t> rewrapped{0};
    tbb::parallel_for(size_t(0), charts.size(), size_t(1), [&](size_t i) {
        Chart& ch = charts[i];
        std::fill(ch.pinned.begin(), ch.pinned.end(), uint8_t(0));
        if (usable(ch) && !chartOverlaps(ch))
            return;
        if (ch.boundaryLoops().size() == 1 && ch.euler() == 1 && unwrapChart(ch).ok)
            ++rewrapped;
    });
    res.rewrapped = rewrapped;

    PackOptions po;
    po.resolution = std::max(4, opts.resolution);
    po.padding = opts.padding;
    po.margin = opts.padding * 0.5;
    po.rotateStep = opts.rotateStep;
    po.effort = opts.effort;
    po.normalizeScale = true;
    po.workingResolution = std::min(po.resolution, 4096);
    res.pack = packCharts(charts, po);
    if (!res.pack.ok)
        return res;
    writeCharts(mesh, charts, opts.target);
    res.ok = true;
    return res;
}
} // namespace lila::uv
