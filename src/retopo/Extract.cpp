#include "retopo/Internal.h"

#include "core/Profile.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>

namespace lila::retopo::detail
{
namespace
{
struct Node
{
    glm::dvec3 pos{0.0};
    glm::dvec3 normal{0.0};
    double weight = 0.0;
    double scale = 0.0;
    FeatureKind feature = FeatureKind::None;
    glm::dvec3 cornerPos{0.0};
    bool boundary = false;
    std::vector<Index> nbrs; // sorted counter-clockwise about the normal
};

/** Integer lattice step from vertex i's lattice point to j's, in i's frame. */
std::array<int, 2> latticeShift(const Surface& s, const std::vector<glm::dvec3>& q, const std::vector<glm::dvec3>& o,
                                const std::vector<double>& scale, Index i, Index j)
{
    glm::dvec3 bj;
    glm::dvec3 bi = compatLatticePair(s.pos[i], s.normal[i], q[i], o[i], scale[i], s.pos[j], s.normal[j], q[j], o[j], scale[j], bj);
    glm::dvec3 ti = glm::cross(s.normal[i], q[i]);
    glm::dvec3 da = bi - o[i];
    int ax = int(std::lround(glm::dot(da, q[i]) / scale[i])), ay = int(std::lround(glm::dot(da, ti) / scale[i]));
    // j's own axes turned to match i's, so the step reads right across creases.
    glm::dvec3 qj = matchQuarterRotation(q[i], q[j], s.normal[j]);
    glm::dvec3 tj = glm::cross(s.normal[j], qj);
    glm::dvec3 db = o[j] - bj;
    int bx = int(std::lround(glm::dot(db, qj) / scale[j])), by = int(std::lround(glm::dot(db, tj) / scale[j]));
    return {ax + bx, ay + by};
}
} // namespace

Extracted extractMesh(const Surface& s, const std::vector<glm::dvec3>& q, const std::vector<glm::dvec3>& o,
                      const std::vector<double>& scale, const std::vector<std::array<int, 2>>* edgeSteps)
{
    LILA_ZONE();

    const Index n = s.vertexCount();
    std::vector<Index> parent(n);
    std::iota(parent.begin(), parent.end(), Index(0));
    auto find = [&](Index x) {
        while (parent[x] != x)
            x = parent[x] = parent[parent[x]];
        return x;
    };

    // Vertices whose edge stays on one lattice point join one cluster, closest pairs first. A field that isn't quite
    // consistent can chain neighbouring lattice points together through a run of such edges, so two clusters only
    // join while their centres are well within a lattice step; otherwise the edge links them like a step would. Exact
    // steps from a seamless layout need no such guard.
    std::vector<std::array<Index, 2>> links;
    std::vector<std::pair<double, Index>> same;
    for (size_t e = 0; e < s.edges.size(); ++e)
    {
        Index i = s.edges[e][0], j = s.edges[e][1];
        auto d = edgeSteps ? (*edgeSteps)[e] : latticeShift(s, q, o, scale, i, j);
        int l1 = std::abs(d[0]) + std::abs(d[1]);
        if (l1 == 0)
            same.push_back({glm::length(o[i] - o[j]), Index(e)});
        else if (l1 == 1)
            links.push_back({i, j});
    }
    std::sort(same.begin(), same.end());
    std::vector<glm::dvec3> centre(o.begin(), o.end());
    std::vector<double> mass(n, 1.0);
    for (const auto& [gap, e] : same)
    {
        const Index i = s.edges[e][0], j = s.edges[e][1];
        Index a = find(i), b = find(j);
        if (a == b)
            continue;
        if (!edgeSteps && glm::length(centre[a] / mass[a] - centre[b] / mass[b]) > 0.7 * 0.5 * (scale[i] + scale[j]))
        {
            links.push_back({i, j});
            continue;
        }
        if (a > b)
            std::swap(a, b);
        parent[b] = a;
        centre[a] += centre[b];
        mass[a] += mass[b];
    }

    // One output vertex per cluster of vertices sharing a lattice point.
    std::vector<Index> nodeOf(n, kInvalid);
    std::vector<Node> nodes;
    for (Index v = 0; v < n; ++v)
    {
        Index r = find(v);
        if (nodeOf[r] == kInvalid)
        {
            nodeOf[r] = Index(nodes.size());
            nodes.emplace_back();
        }
        nodeOf[v] = nodeOf[r];
        Node& nd = nodes[nodeOf[v]];
        double w = std::max(s.area[v], 1e-300);
        nd.pos += o[v] * w;
        nd.normal += s.normal[v] * w;
        nd.scale += scale[v] * w;
        nd.weight += w;
        if (s.feature[v] > nd.feature)
        {
            nd.feature = s.feature[v];
            if (s.feature[v] == FeatureKind::Corner)
                nd.cornerPos = s.pos[v];
        }
        nd.boundary |= s.onBoundary[v] != 0;
    }
    for (Node& nd : nodes)
    {
        nd.pos /= nd.weight;
        nd.scale /= nd.weight;
        double len = glm::length(nd.normal);
        nd.normal = len > 0.0 ? nd.normal / len : glm::dvec3(0, 1, 0);
        if (nd.feature == FeatureKind::Corner)
            nd.pos = nd.cornerPos;
    }

    std::vector<uint64_t> edges;
    for (const auto& l : links)
    {
        Index a = nodeOf[l[0]], b = nodeOf[l[1]];
        if (a == b)
            continue;
        double len = glm::length(nodes[a].pos - nodes[b].pos);
        if (len > 1.8 * 0.5 * (nodes[a].scale + nodes[b].scale))
            continue;
        edges.push_back(edgeKey(a, b));
    }
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    for (uint64_t k : edges)
    {
        nodes[k >> 32].nbrs.push_back(Index(k & 0xffffffffu));
        nodes[k & 0xffffffffu].nbrs.push_back(Index(k >> 32));
    }

    auto sortNeighbours = [&](Index v) {
        Node& nd = nodes[v];
        glm::dvec3 e1 = anyTangent(nd.normal), e2 = glm::cross(nd.normal, e1);
        std::vector<std::pair<double, Index>> ang;
        for (Index u : nd.nbrs)
        {
            glm::dvec3 d = nodes[u].pos - nd.pos;
            ang.push_back({std::atan2(glm::dot(d, e2), glm::dot(d, e1)), u});
        }
        std::sort(ang.begin(), ang.end());
        for (size_t k = 0; k < ang.size(); ++k)
            nd.nbrs[k] = ang[k].second;
        return ang;
    };

    // Edges that leave a vertex nearly along another one are diagonals or duplicates: drop the longer.
    {
        std::vector<uint64_t> drop;
        const double minAngle = glm::radians(28.0);
        for (Index v = 0; v < Index(nodes.size()); ++v)
        {
            auto ang = sortNeighbours(v);
            if (ang.size() < 2)
                continue;
            for (size_t k = 0; k < ang.size(); ++k)
            {
                const auto& a = ang[k];
                const auto& b = ang[(k + 1) % ang.size()];
                double gap = b.first - a.first;
                if (gap < 0.0)
                    gap += 2.0 * 3.14159265358979323846;
                if (gap >= minAngle)
                    continue;
                double la = glm::length(nodes[a.second].pos - nodes[v].pos), lb = glm::length(nodes[b.second].pos - nodes[v].pos);
                drop.push_back(edgeKey(v, la > lb ? a.second : b.second));
            }
        }
        std::sort(drop.begin(), drop.end());
        for (Node& nd : nodes)
            nd.nbrs.clear();
        for (uint64_t k : edges)
            if (!std::binary_search(drop.begin(), drop.end(), k))
            {
                nodes[k >> 32].nbrs.push_back(Index(k & 0xffffffffu));
                nodes[k & 0xffffffffu].nbrs.push_back(Index(k >> 32));
            }
        for (Index v = 0; v < Index(nodes.size()); ++v)
            sortNeighbours(v);
    }

    // Trace faces: from u->v the face turns to the neighbour just clockwise of u around v.
    std::map<uint64_t, uint8_t> used;
    auto directed = [](Index a, Index b) { return (uint64_t(a) << 32) | b; };
    auto nextAround = [&](Index u, Index v) -> Index {
        const auto& nb = nodes[v].nbrs;
        auto it = std::find(nb.begin(), nb.end(), u);
        if (it == nb.end())
            return kInvalid;
        size_t k = size_t(it - nb.begin());
        return nb[(k + nb.size() - 1) % nb.size()];
    };
    std::vector<std::vector<Index>> faces;
    for (Index a = 0; a < Index(nodes.size()); ++a)
        for (Index b : nodes[a].nbrs)
        {
            if (used.contains(directed(a, b)))
                continue;
            std::vector<Index> face{a};
            Index u = a, v = b;
            bool closed = false;
            for (int step = 0; step < 8; ++step)
            {
                used[directed(u, v)] = 1;
                if (v == a)
                {
                    closed = true;
                    break;
                }
                face.push_back(v);
                Index w = nextAround(u, v);
                if (w == kInvalid || used.contains(directed(v, w)))
                    break;
                u = v;
                v = w;
            }
            if (!closed || face.size() < 3 || face.size() > 6)
                continue;
            std::vector<Index> sorted = face;
            std::sort(sorted.begin(), sorted.end());
            if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end())
                continue;
            faces.push_back(std::move(face));
        }

    Extracted ex;
    std::vector<Index> pointOf(nodes.size(), kInvalid);
    std::map<uint64_t, int> edgeUse;
    for (const auto& f : faces)
    {
        bool ok = true;
        for (size_t k = 0; k < f.size() && ok; ++k)
            ok = edgeUse[edgeKey(f[k], f[(k + 1) % f.size()])] < 2;
        if (!ok)
            continue;
        std::vector<Index> pts;
        for (Index v : f)
        {
            if (pointOf[v] == kInvalid)
            {
                pointOf[v] = ex.mesh.addPoint(nodes[v].pos);
                ex.feature.push_back(nodes[v].feature);
                ex.boundary.push_back(nodes[v].boundary);
            }
            pts.push_back(pointOf[v]);
        }
        if (ex.mesh.addFace(pts) == kInvalid)
            continue;
        for (size_t k = 0; k < f.size(); ++k)
            ++edgeUse[edgeKey(f[k], f[(k + 1) % f.size()])];
    }
    return ex;
}

} // namespace lila::retopo::detail
