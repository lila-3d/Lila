#include "uv/Chart.h"

#include "core/Profile.h"
#include "geo/Triangulate.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>

namespace lila::uv
{
namespace
{
struct UnionFind
{
    std::vector<Index> parent;

    explicit UnionFind(size_t n) : parent(n) { std::iota(parent.begin(), parent.end(), Index(0)); }

    Index find(Index x)
    {
        while (parent[x] != x)
        {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    }

    void unite(Index a, Index b)
    {
        a = find(a);
        b = find(b);
        if (a == b)
            return;
        // Lower id wins so roots do not depend on join order.
        if (a < b)
            parent[b] = a;
        else
            parent[a] = b;
    }
};

bool sameUv(const glm::vec2& a, const glm::vec2& b)
{
    return std::abs(a.x - b.x) <= 1e-6f && std::abs(a.y - b.y) <= 1e-6f;
}

/** Mesh id to local index: a dense table when the subset is a good part of the mesh, else a hash map. */
class IdMap
{
public:
    IdMap(size_t capacity, size_t count) : mDense(count * 8 > capacity)
    {
        if (mDense)
            mTable.assign(capacity, kInvalid);
        else
            mHash.reserve(count * 2);
    }

    void set(Index key, Index value)
    {
        if (mDense)
            mTable[key] = value;
        else
            mHash[key] = value;
    }

    Index get(Index key) const
    {
        if (mDense)
            return mTable[key];
        auto it = mHash.find(key);
        return it == mHash.end() ? kInvalid : it->second;
    }

private:
    bool mDense;
    std::vector<Index> mTable;
    std::unordered_map<Index, Index> mHash;
};

/** Islands of a face subset, in local indices so the cost follows the subset, not the mesh. */
struct Grouping
{
    std::vector<Index> faceChart;  // per input face
    std::vector<Index> corners;    // local corner -> mesh corner, face by face
    std::vector<Index> faceStart;  // per input face, plus one
    std::vector<Index> cornerRoot; // per local corner
    IdMap cornerLocal;
    Index chartCount = 0;

    Grouping(size_t cornerCapacity, size_t count) : cornerLocal(cornerCapacity, count) {}
};

Grouping group(const geo::Mesh& mesh, std::span<const Index> faces, const ChartOptions& opts)
{
    LILA_ZONE();

    Grouping out(mesh.cornerCapacity(), faces.size() * 4);
    IdMap faceLocal(mesh.faceCapacity(), faces.size());
    for (size_t i = 0; i < faces.size(); ++i)
    {
        faceLocal.set(faces[i], Index(i));
        out.faceStart.push_back(Index(out.corners.size()));
        mesh.forEachFaceCorner(faces[i], [&](Index c) {
            out.cornerLocal.set(c, Index(out.corners.size()));
            out.corners.push_back(c);
        });
    }
    out.faceStart.push_back(Index(out.corners.size()));

    std::span<const bool> seam = opts.useSeamAttr ? mesh.edgeAttrs().get<bool>(geo::attr::kSeam) : std::span<const bool>{};
    std::span<const glm::vec2> uv = mesh.cornerAttrs().get<glm::vec2>(opts.uvSet);
    const bool byUv = opts.source == IslandSource::Uvs && !uv.empty();

    UnionFind faceSets(faces.size());
    UnionFind cornerSets(out.corners.size());
    for (Index lc = 0; lc < out.corners.size(); ++lc)
    {
        const Index self = out.corners[lc];
        const Index e = mesh.cornerEdge(self);
        const bool extra = e < opts.extraSeams.size() && opts.extraSeams[e];
        if (extra || (!byUv && !seam.empty() && seam[e]))
            continue;
        Index c[3];
        int n = 0;
        mesh.forEachEdgeCorner(e, [&](Index r) {
            if (n < 3 && faceLocal.get(mesh.cornerFace(r)) != kInvalid)
                c[n++] = r;
        });
        // Each edge once: from its lowest corner in the set.
        if (n != 2 || std::min(c[0], c[1]) != self || mesh.cornerFace(c[0]) == mesh.cornerFace(c[1]))
            continue;
        const bool opposite = mesh.cornerPoint(c[0]) != mesh.cornerPoint(c[1]);
        // Corner pairs that sit on the same point on either side of the edge.
        const Index a0 = c[0], a1 = opposite ? mesh.cornerNext(c[1]) : c[1];
        const Index b0 = mesh.cornerNext(c[0]), b1 = opposite ? c[1] : mesh.cornerNext(c[1]);
        if (byUv && (!sameUv(uv[a0], uv[a1]) || !sameUv(uv[b0], uv[b1])))
            continue;
        faceSets.unite(faceLocal.get(mesh.cornerFace(c[0])), faceLocal.get(mesh.cornerFace(c[1])));
        cornerSets.unite(out.cornerLocal.get(a0), out.cornerLocal.get(a1));
        cornerSets.unite(out.cornerLocal.get(b0), out.cornerLocal.get(b1));
    }

    out.faceChart.resize(faces.size());
    std::vector<Index> rootChart(faces.size(), kInvalid);
    for (Index i = 0; i < faces.size(); ++i)
    {
        Index r = faceSets.find(i);
        if (rootChart[r] == kInvalid)
            rootChart[r] = out.chartCount++;
        out.faceChart[i] = rootChart[r];
    }
    out.cornerRoot.resize(out.corners.size());
    for (Index c = 0; c < out.corners.size(); ++c)
        out.cornerRoot[c] = cornerSets.find(c);
    return out;
}
} // namespace

std::vector<Index> liveFaces(const geo::Mesh& mesh, std::span<const Index> faces)
{
    std::vector<Index> out;
    if (faces.empty())
    {
        out.reserve(mesh.numFaces());
        for (Index f = 0; f < mesh.faceCapacity(); ++f)
            if (mesh.faceValid(f))
                out.push_back(f);
        return out;
    }
    out.assign(faces.begin(), faces.end());
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    std::erase_if(out, [&](Index f) { return !mesh.faceValid(f); });
    return out;
}

std::vector<std::vector<Index>> islandFaces(const geo::Mesh& mesh, std::span<const Index> faces, const ChartOptions& opts)
{
    LILA_ZONE();

    std::vector<Index> live = liveFaces(mesh, faces);
    Grouping g = group(mesh, live, opts);
    std::vector<std::vector<Index>> out(g.chartCount);
    for (size_t i = 0; i < live.size(); ++i)
        out[g.faceChart[i]].push_back(live[i]);
    return out;
}

std::vector<Index> islandsOfFaces(const geo::Mesh& mesh, std::span<const Index> seeds, const ChartOptions& opts)
{
    LILA_ZONE();

    std::vector<Index> out;
    if (seeds.empty())
        return out;
    std::vector<uint8_t> seed(mesh.faceCapacity(), 0);
    for (Index f : seeds)
        if (f < seed.size())
            seed[f] = 1;
    for (const std::vector<Index>& island : islandFaces(mesh, {}, opts))
        if (std::any_of(island.begin(), island.end(), [&](Index f) { return seed[f] != 0; }))
            out.insert(out.end(), island.begin(), island.end());
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<Chart> buildCharts(const geo::Mesh& mesh, std::span<const Index> faces, const ChartOptions& opts)
{
    LILA_ZONE();

    std::vector<Index> live = liveFaces(mesh, faces);
    Grouping g = group(mesh, live, opts);
    std::vector<Chart> charts(g.chartCount);
    std::vector<std::vector<Index>> chartLocalFaces(g.chartCount);
    for (size_t i = 0; i < live.size(); ++i)
    {
        charts[g.faceChart[i]].faces.push_back(live[i]);
        chartLocalFaces[g.faceChart[i]].push_back(Index(i));
    }

    std::span<const glm::vec2> uv = mesh.cornerAttrs().get<glm::vec2>(opts.uvSet);
    std::span<const bool> pin = mesh.cornerAttrs().get<bool>(opts.pinAttr);
    // Charts own disjoint corners, so this map is filled in parallel safely.
    std::vector<Index> cornerVertex(g.corners.size(), kInvalid);

    tbb::parallel_for(size_t(0), charts.size(), [&](size_t ci) {
        Chart& ch = charts[ci];
        std::unordered_map<Index, Index> rootVertex;
        for (Index lf : chartLocalFaces[ci])
        {
            ch.faceStart.push_back(Index(ch.corners.size()));
            for (Index lc = g.faceStart[lf]; lc < g.faceStart[lf + 1]; ++lc)
            {
                const Index c = g.corners[lc];
                auto [it, fresh] = rootVertex.try_emplace(g.cornerRoot[lc], Index(ch.pos.size()));
                if (fresh)
                {
                    ch.vertexPoint.push_back(mesh.cornerPoint(c));
                    ch.pos.push_back(mesh.position(mesh.cornerPoint(c)));
                    ch.uv.push_back(uv.empty() ? glm::dvec2(0.0) : glm::dvec2(uv[c]));
                    ch.pinned.push_back(0);
                }
                ch.corners.push_back(c);
                ch.cornerVertex.push_back(it->second);
                cornerVertex[lc] = it->second;
                if (!pin.empty() && pin[c])
                    ch.pinned[it->second] = 1;
            }
        }
        ch.faceStart.push_back(Index(ch.corners.size()));
        std::vector<Index> tri;
        for (Index f : ch.faces)
        {
            tri.clear();
            geo::triangulateFace(mesh, f, tri);
            for (size_t i = 0; i + 2 < tri.size(); i += 3)
            {
                ch.tris.push_back({cornerVertex[g.cornerLocal.get(tri[i])], cornerVertex[g.cornerLocal.get(tri[i + 1])],
                                   cornerVertex[g.cornerLocal.get(tri[i + 2])]});
                ch.triFace.push_back(f);
            }
        }
    });
    return charts;
}

std::span<glm::vec2> uvLayer(geo::Mesh& mesh, std::string_view uvSet)
{
    return mesh.cornerAttrs().getOrAdd<glm::vec2>(uvSet);
}

void writeCharts(geo::Mesh& mesh, std::span<const Chart> charts, std::string_view uvSet)
{
    LILA_ZONE();

    std::span<glm::vec2> uv = uvLayer(mesh, uvSet);
    for (const Chart& ch : charts)
        for (size_t i = 0; i < ch.corners.size(); ++i)
            uv[ch.corners[i]] = glm::vec2(ch.uv[ch.cornerVertex[i]]);
}

Index markSeamsFromUvs(geo::Mesh& mesh, std::span<const Index> faces, std::string_view uvSet)
{
    LILA_ZONE();

    std::vector<Index> live = liveFaces(mesh, faces);
    std::vector<uint8_t> inSet(mesh.faceCapacity(), 0);
    for (Index f : live)
        inSet[f] = 1;
    std::span<const glm::vec2> uv = std::as_const(mesh).cornerAttrs().get<glm::vec2>(uvSet);
    std::vector<int8_t> state(mesh.edgeCapacity(), -1);
    for (Index f : live)
        mesh.forEachFaceCorner(f, [&](Index c) {
            Index e = mesh.cornerEdge(c);
            if (state[e] >= 0)
                return;
            bool split = false;
            int count = 0;
            mesh.forEachEdgeCorner(e, [&](Index r) {
                ++count;
                if (r == c)
                    return;
                if (!inSet[mesh.cornerFace(r)])
                {
                    split = true;
                    return;
                }
                if (uv.empty())
                    return;
                const bool same = mesh.cornerPoint(r) == mesh.cornerPoint(c);
                const glm::vec2 a = same ? uv[r] : uv[mesh.cornerNext(r)];
                const glm::vec2 b = same ? uv[mesh.cornerNext(r)] : uv[r];
                if (!sameUv(a, uv[c]) || !sameUv(b, uv[mesh.cornerNext(c)]))
                    split = true;
            });
            state[e] = (split && count > 1) ? 1 : 0;
        });
    std::span<bool> seam = mesh.edgeAttrs().getOrAdd<bool>(geo::attr::kSeam);
    Index n = 0;
    for (Index e = 0; e < mesh.edgeCapacity(); ++e)
        if (state[e] >= 0)
        {
            seam[e] = state[e] == 1;
            n += state[e] == 1 ? 1 : 0;
        }
    return n;
}

double dihedralAngle(const geo::Mesh& mesh, Index e)
{
    Index c = mesh.edgeCorner(e);
    if (c == kInvalid)
        return 0.0;
    Index r = mesh.cornerRadial(c);
    if (r == c)
        return 0.0;
    double d = glm::dot(mesh.faceNormal(mesh.cornerFace(c)), mesh.faceNormal(mesh.cornerFace(r)));
    return glm::degrees(std::acos(std::clamp(d, -1.0, 1.0)));
}

double Chart::area3d() const
{
    double a = 0.0;
    for (const auto& t : tris)
        a += 0.5 * glm::length(glm::cross(pos[t[1]] - pos[t[0]], pos[t[2]] - pos[t[0]]));
    return a;
}

double Chart::areaUv() const
{
    double a = 0.0;
    for (const auto& t : tris)
    {
        glm::dvec2 e1 = uv[t[1]] - uv[t[0]], e2 = uv[t[2]] - uv[t[0]];
        a += 0.5 * (e1.x * e2.y - e1.y * e2.x);
    }
    return a;
}

std::vector<std::vector<Index>> Chart::boundaryLoops() const
{
    std::unordered_map<uint64_t, int> directed;
    auto key = [](Index a, Index b) { return (uint64_t(a) << 32) | b; };
    for (const auto& t : tris)
        for (int i = 0; i < 3; ++i)
            ++directed[key(t[i], t[(i + 1) % 3])];
    std::unordered_multimap<Index, Index> next;
    std::vector<uint64_t> edges;
    for (const auto& t : tris)
        for (int i = 0; i < 3; ++i)
        {
            Index a = t[i], b = t[(i + 1) % 3];
            if (!directed.count(key(b, a)))
            {
                next.emplace(a, b);
                edges.push_back(key(a, b));
            }
        }
    std::sort(edges.begin(), edges.end());
    std::unordered_map<uint64_t, uint8_t> used;
    std::vector<std::vector<Index>> loops;
    std::vector<double> lengths;
    for (uint64_t start : edges)
    {
        if (used[start])
            continue;
        std::vector<Index> loop;
        double len = 0.0;
        Index a = Index(start >> 32), b = Index(start & 0xffffffffu);
        used[start] = 1;
        loop.push_back(a);
        len += glm::length(pos[b] - pos[a]);
        Index first = a;
        while (b != first && loop.size() <= edges.size())
        {
            loop.push_back(b);
            Index nb = kInvalid;
            auto [lo, hi] = next.equal_range(b);
            for (auto it = lo; it != hi; ++it)
                if (!used[key(b, it->second)])
                {
                    nb = it->second;
                    break;
                }
            if (nb == kInvalid)
                break;
            used[key(b, nb)] = 1;
            len += glm::length(pos[nb] - pos[b]);
            b = nb;
        }
        loops.push_back(std::move(loop));
        lengths.push_back(len);
    }
    std::vector<size_t> order(loops.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) { return lengths[x] > lengths[y]; });
    std::vector<std::vector<Index>> sorted;
    for (size_t i : order)
        sorted.push_back(std::move(loops[i]));
    return sorted;
}

int Chart::euler() const
{
    std::vector<uint64_t> edges;
    edges.reserve(tris.size() * 3);
    for (const auto& t : tris)
        for (int i = 0; i < 3; ++i)
        {
            Index a = t[i], b = t[(i + 1) % 3];
            edges.push_back((uint64_t(std::min(a, b)) << 32) | std::max(a, b));
        }
    std::sort(edges.begin(), edges.end());
    size_t e = size_t(std::unique(edges.begin(), edges.end()) - edges.begin());
    return int(pos.size()) - int(e) + int(tris.size());
}

glm::dvec2 Chart::uvMin() const
{
    glm::dvec2 m(std::numeric_limits<double>::max());
    for (const glm::dvec2& p : uv)
        m = glm::min(m, p);
    return m;
}

glm::dvec2 Chart::uvMax() const
{
    glm::dvec2 m(-std::numeric_limits<double>::max());
    for (const glm::dvec2& p : uv)
        m = glm::max(m, p);
    return m;
}

glm::dvec2 Chart::uvCentroid() const
{
    glm::dvec2 s(0.0);
    double w = 0.0;
    for (const auto& t : tris)
    {
        glm::dvec2 e1 = uv[t[1]] - uv[t[0]], e2 = uv[t[2]] - uv[t[0]];
        double a = std::abs(e1.x * e2.y - e1.y * e2.x);
        s += a * (uv[t[0]] + uv[t[1]] + uv[t[2]]) / 3.0;
        w += a;
    }
    if (w > 0.0)
        return s / w;
    return uv.empty() ? glm::dvec2(0.0) : (uvMin() + uvMax()) * 0.5;
}
} // namespace lila::uv
