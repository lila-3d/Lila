#include "uv/OptCuts.h"

#include "core/Profile.h"
#include "uv/Bff.h"
#include "uv/Metrics.h"
#include "uv/OptimizeMath.h"
#include "uv/Unwrap.h"
#include "uv/UvCommon.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <queue>
#include <unordered_map>

namespace lila::uv
{
namespace
{
using namespace detail;

constexpr int kSolveIterations = 8;
constexpr int kTrialIterations = 10;
constexpr int kTrialRings = 3;
constexpr size_t kTrialsPerStep = 8;
constexpr int kMaxSplits = 400;
constexpr int kMaxMergeTrials = 600;
constexpr double kInf = std::numeric_limits<double>::infinity();

/** Symmetric Dirichlet energy of one triangle times its area, with the uvs scaled by scale. */
double triEnergy(const Chart& ch, const TriFrame& f, size_t i, double sign, double scale = 1.0)
{
    if (f.area <= 0.0)
        return 0.0;
    const auto& t = ch.tris[i];
    Jac2 j = jacobian(f, ch.uv[t[0]], ch.uv[t[1]], ch.uv[t[2]]);
    if (sign * j.det() <= 0.0)
        return kInf;
    Svd2 s = svd2(j);
    double a = s.s1 * s.s1 * scale * scale, b = s.s2 * s.s2 * scale * scale;
    return f.area * (a + 1.0 / a + b + 1.0 / b);
}

double orientation(const Chart& ch)
{
    return ch.areaUv() < 0.0 ? -1.0 : 1.0;
}

double chartEnergy(const Chart& ch)
{
    std::vector<TriFrame> fr = triFrames(ch);
    double sign = orientation(ch), e = 0.0;
    for (size_t i = 0; i < ch.tris.size(); ++i)
        e += triEnergy(ch, fr[i], i, sign);
    return e;
}

struct State
{
    geo::Mesh& mesh;
    std::vector<Index> faces;
    std::vector<uint8_t> inSet, cut, mandatory;
    std::vector<double> cost;
    std::vector<Chart> charts;
    std::vector<double> energy;
    std::vector<glm::dvec2> cornerUv;
    double area = 0.0;
    double sqrtArea = 1.0;
    double lambda = 0.5;

    explicit State(geo::Mesh& m) : mesh(m) {}

    double distortion() const
    {
        double e = 0.0;
        for (double x : energy)
            e += x;
        return area > 0.0 ? e / area / 4.0 - 1.0 : 0.0;
    }

    void store(const Chart& ch)
    {
        for (size_t k = 0; k < ch.corners.size(); ++k)
            cornerUv[ch.corners[k]] = ch.uv[ch.cornerVertex[k]];
    }

    /** Islands of the faces under a cut, with uvs carried over from the corners (averaged where corners join). */
    std::vector<Chart> rebuild(std::span<const Index> islandFaces, std::span<const uint8_t> cuts) const
    {
        ChartOptions co;
        co.useSeamAttr = false;
        co.extraSeams = cuts;
        co.uvSet = "__optcutsNone";
        co.pinAttr = "__optcutsNone";
        std::vector<Chart> out = buildCharts(mesh, islandFaces, co);
        for (Chart& ch : out)
        {
            std::vector<glm::dvec2> sum(ch.pos.size(), glm::dvec2(0.0));
            std::vector<int> n(ch.pos.size(), 0);
            for (size_t k = 0; k < ch.corners.size(); ++k)
            {
                sum[ch.cornerVertex[k]] += cornerUv[ch.corners[k]];
                ++n[ch.cornerVertex[k]];
            }
            for (size_t v = 0; v < ch.pos.size(); ++v)
                ch.uv[v] = n[v] ? sum[v] / double(n[v]) : glm::dvec2(0.0);
        }
        return out;
    }
};

/** SLIM from the current uvs, or a fresh unwrap when they fold. Returns the chart's energy. */
double solve(Chart& ch)
{
    if (foldedTriangles(ch) > 0 || slim(ch, kSolveIterations, false) < 0)
    {
        UnwrapOptions uo;
        uo.usePins = false;
        uo.iterations = kSolveIterations;
        if (!unwrapChart(ch, uo).ok)
            return kInf;
    }
    return chartEnergy(ch);
}

/** Polygon edges of a chart: chart vertex pair -> mesh edge and how many of the chart's faces use it. */
struct ChartEdges
{
    std::unordered_map<uint64_t, std::pair<Index, int>> edges;
    std::vector<std::vector<Index>> adj;
};

ChartEdges chartEdges(const geo::Mesh& mesh, const Chart& ch)
{
    ChartEdges ce;
    ce.adj.resize(ch.pos.size());
    for (size_t f = 0; f + 1 < ch.faceStart.size(); ++f)
        for (Index k = ch.faceStart[f]; k < ch.faceStart[f + 1]; ++k)
        {
            Index a = ch.cornerVertex[k];
            Index b = ch.cornerVertex[k + 1 < ch.faceStart[f + 1] ? k + 1 : ch.faceStart[f]];
            auto [it, fresh] = ce.edges.try_emplace(edgeKey(a, b), mesh.cornerEdge(ch.corners[k]), 0);
            if (++it->second.second == 1)
            {
                ce.adj[a].push_back(b);
                ce.adj[b].push_back(a);
            }
        }
    return ce;
}

struct Candidate
{
    size_t chart = 0;
    Index from = kInvalid, to = kInvalid; // chart vertices; from is on the border
    Index edge = kInvalid;                // mesh edge
    double guess = 0.0;
    double gain = 0.0; // distortion drop from the local trial
};

/**
 * Distortion drop from cutting from -> to, estimated by re-solving the triangles within a few rings of
 * the cut with everything around them held. Energies are taken at the patch's own scale, the one SLIM
 * relaxes it towards, so the estimate measures shape relieved rather than a scale it cannot change.
 */
double trialGain(const Chart& ch, const std::vector<std::vector<size_t>>& vertexTris, Index from, Index to, double area)
{
    const double sign = orientation(ch);
    std::vector<uint8_t> inRing(ch.pos.size(), 0);
    std::vector<Index> frontier{to, from};
    inRing[to] = inRing[from] = 1;
    for (int ring = 0; ring < kTrialRings; ++ring)
    {
        std::vector<Index> next;
        for (Index v : frontier)
            for (size_t t : vertexTris[v])
                for (Index w : ch.tris[t])
                    if (!inRing[w])
                    {
                        inRing[w] = 1;
                        next.push_back(w);
                    }
        frontier = std::move(next);
    }
    std::vector<size_t> tris;
    for (size_t t = 0; t < ch.tris.size(); ++t)
        if (inRing[ch.tris[t][0]] || inRing[ch.tris[t][1]] || inRing[ch.tris[t][2]])
            tris.push_back(t);
    Chart sub;
    std::unordered_map<Index, Index> local;
    for (size_t t : tris)
    {
        std::array<Index, 3> lt;
        for (int k = 0; k < 3; ++k)
        {
            Index v = ch.tris[t][size_t(k)];
            auto [it, fresh] = local.try_emplace(v, Index(sub.pos.size()));
            if (fresh)
            {
                sub.pos.push_back(ch.pos[v]);
                sub.uv.push_back(glm::dvec2(sign * ch.uv[v].x, ch.uv[v].y));
                sub.pinned.push_back(inRing[v] ? 0 : 1);
                sub.vertexPoint.push_back(ch.vertexPoint[v]);
            }
            lt[size_t(k)] = it->second;
        }
        sub.tris.push_back(lt);
        sub.triFace.push_back(0);
    }
    auto patchEnergy = [](const Chart& c) {
        std::vector<TriFrame> f = triFrames(c);
        double au = std::abs(c.areaUv()), a3 = c.area3d();
        double scale = au > 0.0 ? std::sqrt(a3 / au) : 1.0, e = 0.0;
        for (size_t t = 0; t < c.tris.size(); ++t)
            e += triEnergy(c, f[t], t, 1.0, scale);
        return e;
    };
    const double before = patchEnergy(sub);
    std::pair<Index, Index> e{local[from], local[to]};
    cutChart(sub, std::span(&e, 1));
    if (slim(sub, kTrialIterations, true) < 0)
        return 0.0;
    const double after = patchEnergy(sub);
    if (!std::isfinite(before) || !std::isfinite(after))
        return 0.0;
    return (before - after) / area / 4.0;
}

/** The split that best trades distortion relieved against seam price, or none. */
std::optional<Candidate> bestSplit(const State& s)
{
    LILA_ZONE();

    std::vector<Candidate> all;
    std::vector<std::vector<TriFrame>> frames(s.charts.size());
    std::vector<std::vector<std::vector<size_t>>> vertexTris(s.charts.size());
    for (size_t c = 0; c < s.charts.size(); ++c)
    {
        const Chart& ch = s.charts[c];
        if (ch.tris.size() < 4)
            continue;
        frames[c] = triFrames(ch);
        const double sign = orientation(ch);
        std::vector<double> excess(ch.pos.size(), 0.0);
        vertexTris[c].resize(ch.pos.size());
        for (size_t t = 0; t < ch.tris.size(); ++t)
        {
            double e = triEnergy(ch, frames[c][t], t, sign) - 4.0 * frames[c][t].area;
            for (Index v : ch.tris[t])
            {
                excess[v] += e / 3.0;
                vertexTris[c][v].push_back(t);
            }
        }
        ChartEdges ce = chartEdges(s.mesh, ch);
        std::vector<uint8_t> border(ch.pos.size(), 0);
        for (const auto& [key, info] : ce.edges)
            if (info.second == 1)
                border[Index(key >> 32)] = border[Index(key & 0xffffffffu)] = 1;
        for (Index v = 0; v < ch.vertexCount(); ++v)
        {
            if (!border[v])
                continue;
            for (Index q : ce.adj[v])
            {
                const auto& info = ce.edges.at(edgeKey(v, q));
                Index e = info.first;
                if (info.second != 2 || s.cut[e] || s.mandatory[e])
                    continue;
                double ring = excess[q];
                for (Index w : ce.adj[q])
                    ring += excess[w];
                double guess = ring / s.area / 4.0 - s.lambda * s.cost[e] / s.sqrtArea;
                all.push_back({c, v, q, e, guess, 0.0});
            }
        }
    }
    if (all.empty())
        return std::nullopt;
    size_t keep = std::min(kTrialsPerStep, all.size());
    std::partial_sort(all.begin(), all.begin() + std::ptrdiff_t(keep), all.end(),
                      [](const Candidate& a, const Candidate& b) { return a.guess > b.guess; });
    all.resize(keep);
    tbb::parallel_for(size_t(0), all.size(), size_t(1), [&](size_t i) {
        Candidate& c = all[i];
        c.gain = trialGain(s.charts[c.chart], vertexTris[c.chart], c.from, c.to, s.area);
    });
    auto score = [&](const Candidate& c) { return c.gain - s.lambda * s.cost[c.edge] / s.sqrtArea; };
    auto best = std::max_element(all.begin(), all.end(), [&](const Candidate& a, const Candidate& b) { return score(a) < score(b); });
    if (best->gain <= 0.0)
        return std::nullopt;
    return *best;
}

void replaceChart(State& s, size_t c, std::vector<Chart>&& parts, std::vector<double>&& energies)
{
    for (const Chart& ch : parts)
        s.store(ch);
    s.charts[c] = std::move(parts[0]);
    s.energy[c] = energies[0];
    for (size_t i = 1; i < parts.size(); ++i)
    {
        s.charts.push_back(std::move(parts[i]));
        s.energy.push_back(energies[i]);
    }
}

bool applySplit(State& s, const Candidate& cand)
{
    s.cut[cand.edge] = 1;
    std::vector<Chart> parts = s.rebuild(s.charts[cand.chart].faces, s.cut);
    std::vector<double> e(parts.size());
    tbb::parallel_for(size_t(0), parts.size(), size_t(1), [&](size_t i) { e[i] = solve(parts[i]); });
    for (double x : e)
        if (!std::isfinite(x))
        {
            s.cut[cand.edge] = 0;
            return false;
        }
    replaceChart(s, cand.chart, std::move(parts), std::move(e));
    return true;
}

/** Edges at a point that end the cut there: cut ones plus borders of the set and of the mesh. */
int cutDegree(const State& s, Index p)
{
    int n = 0;
    s.mesh.forEachPointEdge(p, [&](Index e) {
        int count = 0;
        bool outside = false;
        s.mesh.forEachEdgeCorner(e, [&](Index c) {
            ++count;
            outside |= !s.inSet[s.mesh.cornerFace(c)];
        });
        if (count == 0)
            return;
        if (s.cut[e] || count != 2 || outside)
            ++n;
    });
    return n;
}

/** Zips seam tips back, most expensive first, while the distortion stays under the bound. */
int zipTips(State& s, double bound)
{
    LILA_ZONE();

    const geo::Mesh& m = s.mesh;
    std::vector<Index> chartOf(m.faceCapacity(), kInvalid);
    auto relabel = [&] {
        for (size_t c = 0; c < s.charts.size(); ++c)
            for (Index f : s.charts[c].faces)
                chartOf[f] = Index(c);
    };
    relabel();
    using Item = std::pair<double, Index>;
    std::priority_queue<Item> queue;
    auto consider = [&](Index e) {
        if (!s.cut[e] || s.mandatory[e] || !m.isManifoldEdge(e) || m.isBoundaryEdge(e))
            return;
        Index c = m.edgeCorner(e);
        Index f1 = m.cornerFace(c), f2 = m.cornerFace(m.cornerRadial(c));
        if (!s.inSet[f1] || !s.inSet[f2] || chartOf[f1] != chartOf[f2])
            return;
        if (cutDegree(s, m.edgePoint(e, 0)) == 1 || cutDegree(s, m.edgePoint(e, 1)) == 1)
            queue.emplace(s.cost[e], e);
    };
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && s.cut[e])
            consider(e);

    int merges = 0, trials = 0;
    while (!queue.empty() && trials < kMaxMergeTrials)
    {
        Index e = queue.top().second;
        queue.pop();
        if (!s.cut[e])
            continue;
        Index c = m.edgeCorner(e);
        Index chart = chartOf[m.cornerFace(c)];
        if (chart == kInvalid || chart != chartOf[m.cornerFace(m.cornerRadial(c))])
            continue;
        Index a = m.edgePoint(e, 0), b = m.edgePoint(e, 1);
        if (cutDegree(s, a) != 1 && cutDegree(s, b) != 1)
            continue;
        ++trials;
        s.cut[e] = 0;
        std::vector<Chart> parts = s.rebuild(s.charts[chart].faces, s.cut);
        bool ok = parts.size() == 1 && parts[0].boundaryLoops().size() == 1 && parts[0].euler() == 1 && foldedTriangles(parts[0]) == 0;
        double energy = kInf;
        if (ok && slim(parts[0], kSolveIterations, false) >= 0)
            energy = chartEnergy(parts[0]);
        double next = s.distortion() + (energy - s.energy[chart]) / s.area / 4.0;
        if (!std::isfinite(energy) || next > bound)
        {
            s.cut[e] = 1;
            continue;
        }
        std::vector<double> en{energy};
        replaceChart(s, chart, std::move(parts), std::move(en));
        ++merges;
        // The cut's next edge back may now be a tip.
        for (Index p : {a, b})
            m.forEachPointEdge(p, [&](Index f) { consider(f); });
    }
    return merges;
}

/** Hard and sharp edges, plus kept seams: always cut. */
void mandatoryCuts(State& s, const AutoSeamOptions& opts)
{
    const geo::Mesh& m = s.mesh;
    std::span<const bool> hard = m.edgeAttrs().get<bool>(geo::attr::kHard);
    std::span<const bool> seam = m.edgeAttrs().get<bool>(geo::attr::kSeam);
    s.cut.assign(m.edgeCapacity(), 0);
    s.mandatory.assign(m.edgeCapacity(), 0);
    for (Index e = 0; e < m.edgeCapacity(); ++e)
    {
        if (!m.edgeValid(e))
            continue;
        bool must = dihedralAngle(m, e) > opts.hardAngle || (opts.useHardEdges && !hard.empty() && hard[e]) ||
                    (opts.keepSeams && !seam.empty() && seam[e]);
        s.cut[e] = s.mandatory[e] = must ? 1 : 0;
    }
}

/** The fewest further cuts that make every island a disk, worked out on a copy so the mesh's seams stay. */
void cutToDisksInto(State& s)
{
    geo::Mesh scratch = s.mesh;
    std::span<bool> seam = scratch.edgeAttrs().getOrAdd<bool>(geo::attr::kSeam);
    for (Index e = 0; e < scratch.edgeCapacity(); ++e)
        seam[e] = s.cut[e] != 0;
    cutToDisks(scratch, s.faces);
    std::span<const bool> after = std::as_const(scratch).edgeAttrs().get<bool>(geo::attr::kSeam);
    for (Index e = 0; e < scratch.edgeCapacity(); ++e)
        if (after[e])
            s.cut[e] = 1;
}

Index writeCutSeams(State& s, bool keep)
{
    geo::Mesh& m = s.mesh;
    std::span<bool> seam = m.edgeAttrs().getOrAdd<bool>(geo::attr::kSeam);
    Index n = 0;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
    {
        if (!m.edgeValid(e) || m.edgeCorner(e) == kInvalid)
            continue;
        int count = 0;
        bool touches = false, outside = false;
        m.forEachEdgeCorner(e, [&](Index c) {
            ++count;
            (s.inSet[m.cornerFace(c)] ? touches : outside) = true;
        });
        if (!touches)
            continue;
        seam[e] = (count > 1 && (s.cut[e] || outside)) || (keep && seam[e]);
        n += seam[e] ? 1 : 0;
    }
    return n;
}
} // namespace

double seamLength(const geo::Mesh& mesh, std::span<const Index> faceIds)
{
    std::vector<Index> faces = liveFaces(mesh, faceIds);
    std::vector<uint8_t> inSet(mesh.faceCapacity(), 0);
    for (Index f : faces)
        inSet[f] = 1;
    std::span<const bool> seam = mesh.edgeAttrs().get<bool>(geo::attr::kSeam);
    if (seam.empty())
        return 0.0;
    double len = 0.0;
    for (Index e = 0; e < mesh.edgeCapacity(); ++e)
    {
        if (!mesh.edgeValid(e) || !seam[e] || !mesh.isManifoldEdge(e) || mesh.isBoundaryEdge(e))
            continue;
        Index c = mesh.edgeCorner(e);
        if (inSet[mesh.cornerFace(c)] && inSet[mesh.cornerFace(mesh.cornerRadial(c))])
            len += mesh.edgeLength(e);
    }
    return len;
}

OptCutsResult optCuts(geo::Mesh& mesh, std::span<const Index> faceIds, const AutoSeamOptions& opts)
{
    LILA_ZONE();

    OptCutsResult res;
    State s(mesh);
    s.faces = liveFaces(mesh, faceIds);
    if (s.faces.empty())
        return res;
    s.inSet.assign(mesh.faceCapacity(), 0);
    for (Index f : s.faces)
    {
        s.inSet[f] = 1;
        s.area += mesh.faceArea(f);
    }
    if (s.area <= 0.0)
        return res;
    s.sqrtArea = std::sqrt(s.area);
    s.lambda = std::max(0.0, opts.seamWeight);
    s.cost = seamCosts(mesh, s.faces, opts.visibility, opts.views);
    s.cornerUv.assign(mesh.cornerCapacity(), glm::dvec2(0.0));
    mandatoryCuts(s, opts);
    cutToDisksInto(s);

    // Fresh start: every island unwrapped from scratch.
    {
        ChartOptions co;
        co.useSeamAttr = false;
        co.extraSeams = s.cut;
        co.uvSet = "__optcutsNone";
        co.pinAttr = "__optcutsNone";
        s.charts = buildCharts(mesh, s.faces, co);
        s.energy.assign(s.charts.size(), kInf);
        tbb::parallel_for(size_t(0), s.charts.size(), size_t(1), [&](size_t i) {
            UnwrapOptions uo;
            uo.usePins = false;
            if (unwrapChart(s.charts[i], uo).ok)
                s.energy[i] = chartEnergy(s.charts[i]);
        });
        for (const Chart& ch : s.charts)
            s.store(ch);
    }

    const double bound = std::max(0.0, opts.distortionBound);
    for (int step = 0; step < kMaxSplits && s.distortion() > bound; ++step)
    {
        std::optional<Candidate> c = bestSplit(s);
        if (!c || !applySplit(s, *c))
            break;
        ++res.splits;
    }
    res.reachedBound = s.distortion() <= bound;
    if (res.reachedBound)
        res.merges = zipTips(s, bound);

    std::vector<Index> overlapping;
    for (const Chart& ch : s.charts)
        if (chartOverlaps(ch))
        {
            overlapping.insert(overlapping.end(), ch.faces.begin(), ch.faces.end());
            ++res.overlapFixes;
        }
    res.seams.seamEdges = writeCutSeams(s, opts.keepSeams);
    if (!overlapping.empty())
    {
        std::sort(overlapping.begin(), overlapping.end());
        AutoSeamOptions classic;
        classic.keepSeams = true;
        classic.hardAngle = 180.0;
        classic.useHardEdges = false;
        classic.maxDistortion = std::max(opts.maxDistortion, bound);
        classic.visibility = opts.visibility;
        classic.views = opts.views;
        classic.merge = false;
        std::vector<Chart> fixed = autoSeams(mesh, overlapping, classic).charts;
        std::erase_if(s.charts, [](const Chart& ch) { return chartOverlaps(ch); });
        for (Chart& ch : fixed)
            s.charts.push_back(std::move(ch));
        std::span<const bool> seam = std::as_const(mesh).edgeAttrs().get<bool>(geo::attr::kSeam);
        res.seams.seamEdges = 0;
        for (Index e = 0; e < mesh.edgeCapacity(); ++e)
            if (mesh.edgeValid(e) && seam[e])
            {
                bool touches = false;
                mesh.forEachEdgeCorner(e, [&](Index c) { touches |= s.inSet[mesh.cornerFace(c)] != 0; });
                res.seams.seamEdges += touches ? 1 : 0;
            }
    }

    double e = 0.0;
    for (const Chart& ch : s.charts)
        e += chartEnergy(ch);
    res.distortion = e / s.area / 4.0 - 1.0;
    res.seamLength = seamLength(mesh, s.faces);
    res.seams.charts = std::move(s.charts);
    return res;
}
} // namespace lila::uv
