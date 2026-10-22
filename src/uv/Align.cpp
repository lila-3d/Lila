#include "uv/Align.h"

#include "core/Profile.h"
#include "uv/Layout.h"
#include "uv/Unwrap.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <set>

namespace lila::uv
{
namespace
{
/** Chart vertex pairs joined by a selected polygon edge, each once. */
std::set<std::pair<Index, Index>> selectedChartEdges(const Chart& ch, const geo::Mesh& mesh, std::span<const uint8_t> sel)
{
    std::set<std::pair<Index, Index>> out;
    for (size_t f = 0; f + 1 < ch.faceStart.size(); ++f)
        for (Index k = ch.faceStart[f]; k < ch.faceStart[f + 1]; ++k)
        {
            Index e = mesh.cornerEdge(ch.corners[k]);
            if (e >= sel.size() || !sel[e])
                continue;
            Index a = ch.cornerVertex[k];
            Index b = ch.cornerVertex[k + 1 < ch.faceStart[f + 1] ? k + 1 : ch.faceStart[f]];
            out.emplace(std::min(a, b), std::max(a, b));
        }
    return out;
}

/** Maximal paths of the edges between vertices that are not simple chain links. Closed loops are left out. */
std::vector<std::vector<Index>> chains(size_t vertexCount, const std::set<std::pair<Index, Index>>& edges)
{
    std::vector<std::vector<Index>> adj(vertexCount);
    for (auto [a, b] : edges)
    {
        adj[a].push_back(b);
        adj[b].push_back(a);
    }
    std::set<std::pair<Index, Index>> used;
    auto take = [&](Index a, Index b) { return used.emplace(std::min(a, b), std::max(a, b)).second; };
    std::vector<std::vector<Index>> out;
    for (Index v = 0; v < vertexCount; ++v)
    {
        if (adj[v].empty() || adj[v].size() == 2)
            continue;
        for (Index w : adj[v])
        {
            if (!take(v, w))
                continue;
            std::vector<Index> path{v, w};
            Index prev = v, cur = w;
            while (adj[cur].size() == 2)
            {
                Index next = adj[cur][0] == prev ? adj[cur][1] : adj[cur][0];
                if (!take(cur, next))
                    break;
                path.push_back(next);
                prev = cur;
                cur = next;
            }
            out.push_back(std::move(path));
        }
    }
    return out;
}

/** Lays the chain's uvs on their best-fit line, in order, with the chain's uv arc length kept. */
void layOnLine(Chart& ch, const std::vector<Index>& path)
{
    glm::dvec2 c(0.0);
    for (Index v : path)
        c += ch.uv[v];
    c /= double(path.size());
    double sxx = 0.0, sxy = 0.0, syy = 0.0;
    for (Index v : path)
    {
        glm::dvec2 d = ch.uv[v] - c;
        sxx += d.x * d.x;
        sxy += d.x * d.y;
        syy += d.y * d.y;
    }
    const double angle = 0.5 * std::atan2(2.0 * sxy, sxx - syy);
    glm::dvec2 dir(std::cos(angle), std::sin(angle));
    if (glm::dot(ch.uv[path.back()] - ch.uv[path.front()], dir) < 0.0)
        dir = -dir;
    std::vector<double> s(path.size(), 0.0);
    for (size_t i = 1; i < path.size(); ++i)
        s[i] = s[i - 1] + glm::length(ch.uv[path[i]] - ch.uv[path[i - 1]]);
    double mean = 0.0;
    for (double x : s)
        mean += x;
    mean /= double(s.size());
    for (size_t i = 0; i < path.size(); ++i)
        ch.uv[path[i]] = c + (s[i] - mean) * dir;
}
} // namespace

size_t straightenEdges(Chart& ch, const geo::Mesh& mesh, std::span<const uint8_t> sel, int iterations)
{
    LILA_ZONE();

    std::vector<std::vector<Index>> paths = chains(ch.pos.size(), selectedChartEdges(ch, mesh, sel));
    if (paths.empty())
        return 0;
    std::vector<uint8_t> pinnedBefore = ch.pinned;
    for (const auto& p : paths)
    {
        layOnLine(ch, p);
        for (Index v : p)
            ch.pinned[v] = 1;
    }
    std::vector<glm::dvec2> laid = ch.uv;
    if (lscm(ch, true) && foldedTriangles(ch) == 0)
        slim(ch, iterations, true);
    // Chain vertices exactly on their lines, whatever the solve did with them.
    for (const auto& p : paths)
        for (Index v : p)
            ch.uv[v] = laid[v];
    ch.pinned = std::move(pinnedBefore);
    return paths.size();
}

bool alignToEdges(Chart& ch, const geo::Mesh& mesh, std::span<const uint8_t> sel, AlignAxis axis)
{
    auto edges = selectedChartEdges(ch, mesh, sel);
    if (edges.empty())
        return false;
    // Doubled angles, so an edge and its reverse agree.
    double c2 = 0.0, s2 = 0.0;
    for (auto [a, b] : edges)
    {
        glm::dvec2 d = ch.uv[b] - ch.uv[a];
        double len = glm::length(d);
        if (len <= 0.0)
            continue;
        double t = std::atan2(d.y, d.x);
        c2 += len * std::cos(2.0 * t);
        s2 += len * std::sin(2.0 * t);
    }
    if (c2 == 0.0 && s2 == 0.0)
        return false;
    const double angle = 0.5 * std::atan2(s2, c2);
    const double half = glm::half_pi<double>();
    auto wrap = [&](double r) {
        while (r > half)
            r -= glm::pi<double>();
        while (r <= -half)
            r += glm::pi<double>();
        return r;
    };
    double toU = wrap(-angle), toV = wrap(half - angle);
    double turn = axis == AlignAxis::U ? toU : axis == AlignAxis::V ? toV : (std::abs(toU) <= std::abs(toV) ? toU : toV);
    transformChart(ch, ch.uvCentroid(), turn);
    return true;
}
} // namespace lila::uv
