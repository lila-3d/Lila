#include "uv/Bff.h"

#include "core/Profile.h"
#include "uv/Unwrap.h"

#include <Eigen/Sparse>
#include <Eigen/SparseCholesky>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace lila::uv
{
namespace
{
using Triplet = Eigen::Triplet<double>;
using SpMat = Eigen::SparseMatrix<double>;
constexpr double kTwoPi = glm::two_pi<double>();

uint64_t edgeKey(Index a, Index b)
{
    return (uint64_t(std::min(a, b)) << 32) | std::max(a, b);
}

/** Cotan Laplacian (positive semi-definite) and the angle sum at each vertex, from 3D lengths. */
struct Geometry
{
    std::vector<Triplet> laplacian;
    std::vector<double> angleSum;
};

Geometry geometry(const Chart& ch)
{
    Geometry g;
    g.angleSum.assign(ch.pos.size(), 0.0);
    g.laplacian.reserve(ch.tris.size() * 12);
    for (const auto& t : ch.tris)
        for (int k = 0; k < 3; ++k)
        {
            Index i = t[k], j = t[(k + 1) % 3], l = t[(k + 2) % 3];
            glm::dvec3 e1 = ch.pos[j] - ch.pos[i], e2 = ch.pos[l] - ch.pos[i];
            double cr = glm::length(glm::cross(e1, e2)), d = glm::dot(e1, e2);
            g.angleSum[i] += std::atan2(cr, d);
            if (cr <= 1e-14 * glm::length(e1) * glm::length(e2))
                continue;
            double w = 0.5 * d / cr;
            g.laplacian.push_back(Triplet(int(j), int(j), w));
            g.laplacian.push_back(Triplet(int(l), int(l), w));
            g.laplacian.push_back(Triplet(int(j), int(l), -w));
            g.laplacian.push_back(Triplet(int(l), int(j), -w));
        }
    return g;
}

/** A Laplacian restricted to its free vertices, factorised; fixed vertices enter the right-hand side. */
class Restricted
{
public:
    Restricted() = default;

    Restricted(const std::vector<Triplet>& full, size_t n, const std::vector<uint8_t>& fixed)
    {
        mFree.assign(n, -1);
        int nf = 0;
        for (size_t v = 0; v < n; ++v)
            if (!fixed[v])
                mFree[v] = nf++;
        mN = nf;
        std::vector<Triplet> ff;
        ff.reserve(full.size());
        double diag = 0.0;
        for (const Triplet& t : full)
        {
            int r = mFree[size_t(t.row())];
            if (r < 0)
                continue;
            int c = mFree[size_t(t.col())];
            if (c < 0)
                mCoupling.push_back(Triplet(r, t.col(), t.value()));
            else
            {
                ff.push_back(Triplet(r, c, t.value()));
                if (r == c)
                    diag += t.value();
            }
        }
        // A whisper of regularisation keeps vertices touched only by degenerate triangles solvable.
        double eps = 1e-12 * std::max(diag / std::max(nf, 1), 1e-30);
        for (int i = 0; i < nf; ++i)
            ff.push_back(Triplet(i, i, eps));
        if (nf == 0)
        {
            mOk = true;
            return;
        }
        SpMat m(nf, nf);
        m.setFromTriplets(ff.begin(), ff.end());
        mSolver = std::make_unique<Eigen::SimplicialLDLT<SpMat>>(m);
        mOk = mSolver->info() == Eigen::Success;
    }

    bool ok() const { return mOk; }

    /** Solves L_ff x_f = b_f - L_fc x_c. b is per vertex; x holds the fixed values and receives the free ones. */
    bool solve(const std::vector<double>& b, std::vector<double>& x) const
    {
        if (mN == 0)
            return true;
        Eigen::VectorXd rhs(mN);
        for (size_t v = 0; v < mFree.size(); ++v)
            if (mFree[v] >= 0)
                rhs[mFree[v]] = b[v];
        for (const Triplet& t : mCoupling)
            rhs[t.row()] -= t.value() * x[size_t(t.col())];
        Eigen::VectorXd s = mSolver->solve(rhs);
        if (mSolver->info() != Eigen::Success || !s.allFinite())
            return false;
        for (size_t v = 0; v < mFree.size(); ++v)
            if (mFree[v] >= 0)
                x[v] = s[mFree[v]];
        return true;
    }

private:
    std::vector<int> mFree;
    std::vector<Triplet> mCoupling; // (free row, fixed vertex, value)
    int mN = 0;
    std::unique_ptr<Eigen::SimplicialLDLT<SpMat>> mSolver;
    bool mOk = false;
};

/** Log scale factors with u = 0 on the boundary and at the cones, flat everywhere else. */
std::vector<double> coneScale(const Chart& ch, const Geometry& g, const std::vector<uint8_t>& boundary, std::span<const Index> cones)
{
    const size_t V = ch.pos.size();
    std::vector<uint8_t> fixed = boundary;
    for (Index c : cones)
        fixed[c] = 1;
    Restricted r(g.laplacian, V, fixed);
    std::vector<double> u(V, 0.0), b(V, 0.0);
    for (size_t v = 0; v < V; ++v)
        b[v] = -(kTwoPi - g.angleSum[v]);
    if (!r.ok() || !r.solve(b, u))
        return {};
    return u;
}

std::vector<uint8_t> boundaryMask(const Chart& ch)
{
    std::vector<uint8_t> mask(ch.pos.size(), 0);
    for (const auto& loop : ch.boundaryLoops())
        for (Index v : loop)
            mask[v] = 1;
    return mask;
}

/** Shortest paths along real (polygon) edges from each cone to the boundary or an earlier path. */
std::vector<std::pair<Index, Index>> conePaths(const Chart& ch, std::span<const Index> cones)
{
    const size_t V = ch.pos.size();
    std::vector<std::vector<Index>> adj(V);
    for (size_t f = 0; f + 1 < ch.faceStart.size(); ++f)
        for (Index k = ch.faceStart[f]; k < ch.faceStart[f + 1]; ++k)
        {
            Index a = ch.cornerVertex[k];
            Index b = ch.cornerVertex[k + 1 < ch.faceStart[f + 1] ? k + 1 : ch.faceStart[f]];
            adj[a].push_back(b);
            adj[b].push_back(a);
        }
    std::vector<uint8_t> reached = boundaryMask(ch);
    std::vector<std::pair<Index, Index>> out;
    std::vector<double> dist(V);
    std::vector<Index> prev(V);
    for (Index cone : cones)
    {
        if (reached[cone])
            continue;
        std::fill(dist.begin(), dist.end(), std::numeric_limits<double>::infinity());
        std::fill(prev.begin(), prev.end(), kInvalid);
        using Item = std::pair<double, Index>;
        std::priority_queue<Item, std::vector<Item>, std::greater<>> pq;
        dist[cone] = 0.0;
        pq.emplace(0.0, cone);
        Index hit = kInvalid;
        while (!pq.empty())
        {
            auto [d, v] = pq.top();
            pq.pop();
            if (d > dist[v])
                continue;
            if (reached[v])
            {
                hit = v;
                break;
            }
            for (Index w : adj[v])
            {
                double nd = d + glm::length(ch.pos[w] - ch.pos[v]);
                if (nd < dist[w])
                {
                    dist[w] = nd;
                    prev[w] = v;
                    pq.emplace(nd, w);
                }
            }
        }
        for (Index v = hit; v != kInvalid && prev[v] != kInvalid; v = prev[v])
        {
            out.emplace_back(prev[v], v);
            reached[prev[v]] = 1;
        }
    }
    return out;
}

/** Up to n boundary indices with the sharpest turning, kept apart along the loop. */
std::vector<size_t> sharpestCorners(const Chart& ch, const std::vector<Index>& loop, const std::vector<double>& curvature, size_t n)
{
    const size_t nb = loop.size();
    std::vector<double> at(nb, 0.0);
    double total = 0.0;
    for (size_t i = 0; i < nb; ++i)
    {
        at[i] = total;
        total += glm::length(ch.pos[loop[(i + 1) % nb]] - ch.pos[loop[i]]);
    }
    std::vector<size_t> order(nb);
    std::iota(order.begin(), order.end(), size_t(0));
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return curvature[loop[a]] > curvature[loop[b]]; });
    std::vector<size_t> picked;
    for (double spacing : {total / (2.0 * double(n)), total / (4.0 * double(n)), 0.0})
    {
        for (size_t i : order)
        {
            if (picked.size() >= n)
                break;
            bool apart = std::find(picked.begin(), picked.end(), i) == picked.end();
            for (size_t p : picked)
            {
                double d = std::abs(at[i] - at[p]);
                apart &= std::min(d, total - d) > spacing;
            }
            if (apart)
                picked.push_back(i);
        }
        if (picked.size() >= n)
            break;
    }
    std::sort(picked.begin(), picked.end());
    return picked;
}
} // namespace

struct BffSolver::Impl
{
    const Chart* chart = nullptr;
    std::vector<Index> loop;
    std::vector<double> curvature;
    SpMat laplacian;
    Restricted interior; // boundary fixed
    Restricted gauge;    // one boundary vertex fixed
    bool ok = false;
};

BffSolver::BffSolver(const Chart& ch) : m(std::make_unique<Impl>())
{
    LILA_ZONE();

    m->chart = &ch;
    const size_t V = ch.pos.size();
    auto loops = ch.boundaryLoops();
    if (V < 3 || loops.size() != 1 || ch.euler() != 1)
        return;
    m->loop = std::move(loops.front());
    std::vector<uint8_t> isB(V, 0);
    for (Index v : m->loop)
    {
        if (isB[v])
            return; // pinched boundary
        isB[v] = 1;
    }
    Geometry g = geometry(ch);
    m->curvature.resize(V);
    for (size_t v = 0; v < V; ++v)
        m->curvature[v] = (isB[v] ? glm::pi<double>() : kTwoPi) - g.angleSum[v];
    m->laplacian.resize(Eigen::Index(V), Eigen::Index(V));
    m->laplacian.setFromTriplets(g.laplacian.begin(), g.laplacian.end());
    m->interior = Restricted(g.laplacian, V, isB);
    std::vector<uint8_t> one(V, 0);
    one[m->loop.front()] = 1;
    m->gauge = Restricted(g.laplacian, V, one);
    m->ok = m->interior.ok() && m->gauge.ok();
}

BffSolver::~BffSolver() = default;
BffSolver::BffSolver(BffSolver&&) noexcept = default;
BffSolver& BffSolver::operator=(BffSolver&&) noexcept = default;

bool BffSolver::ok() const
{
    return m->ok;
}

const std::vector<Index>& BffSolver::boundary() const
{
    return m->loop;
}

const std::vector<double>& BffSolver::curvature() const
{
    return m->curvature;
}

std::vector<double> BffSolver::dirichletToNeumann(std::span<const double> uB) const
{
    const size_t V = m->chart->pos.size();
    std::vector<double> u(V, 0.0), b(V, 0.0);
    for (size_t i = 0; i < m->loop.size(); ++i)
        u[m->loop[i]] = uB[i];
    for (size_t v = 0; v < V; ++v)
        b[v] = -m->curvature[v];
    if (!m->interior.solve(b, u))
        return {};
    Eigen::Map<const Eigen::VectorXd> uv(u.data(), Eigen::Index(V));
    Eigen::VectorXd lu = m->laplacian * uv;
    std::vector<double> k(m->loop.size());
    for (size_t i = 0; i < m->loop.size(); ++i)
        k[i] = m->curvature[m->loop[i]] + lu[Eigen::Index(m->loop[i])];
    return k;
}

std::vector<double> BffSolver::neumannToDirichlet(std::span<const double> kB) const
{
    const size_t V = m->chart->pos.size();
    std::vector<double> b(V), u(V, 0.0);
    for (size_t v = 0; v < V; ++v)
        b[v] = -m->curvature[v];
    for (size_t i = 0; i < m->loop.size(); ++i)
        b[m->loop[i]] = kB[i] - m->curvature[m->loop[i]];
    if (!m->gauge.solve(b, u))
        return {};
    std::vector<double> out(m->loop.size());
    for (size_t i = 0; i < m->loop.size(); ++i)
        out[i] = u[m->loop[i]];
    return out;
}

bool BffSolver::flatten(std::span<const double> uB, std::span<const double> kB, bool exactLengths, std::vector<glm::dvec2>& uv) const
{
    LILA_ZONE();

    const Chart& ch = *m->chart;
    const size_t nb = m->loop.size(), V = ch.pos.size();
    if (uB.size() != nb || kB.size() != nb)
        return false;
    std::vector<double> len(nb), phi(nb);
    for (size_t i = 0; i < nb; ++i)
    {
        Index a = m->loop[i], b = m->loop[(i + 1) % nb];
        len[i] = std::exp(0.5 * (uB[i] + uB[(i + 1) % nb])) * glm::length(ch.pos[b] - ch.pos[a]);
    }
    phi[0] = 0.0;
    for (size_t i = 1; i < nb; ++i)
        phi[i] = phi[i - 1] + kB[i];

    if (exactLengths)
    {
        // Gauss-Newton on the edge directions: the smallest turn changes that close the loop.
        double total = std::accumulate(len.begin(), len.end(), 0.0);
        for (int it = 0; it < 30; ++it)
        {
            glm::dvec2 r(0.0);
            double g00 = 0.0, g01 = 0.0, g11 = 0.0;
            for (size_t i = 0; i < nb; ++i)
            {
                double c = std::cos(phi[i]), s = std::sin(phi[i]);
                r += len[i] * glm::dvec2(c, s);
                g00 += len[i] * len[i] * s * s;
                g01 -= len[i] * len[i] * s * c;
                g11 += len[i] * len[i] * c * c;
            }
            if (glm::length(r) < 1e-13 * total)
                break;
            double det = g00 * g11 - g01 * g01;
            if (std::abs(det) < 1e-300)
                return false;
            glm::dvec2 lam((g11 * r.x - g01 * r.y) / det, (-g01 * r.x + g00 * r.y) / det);
            for (size_t i = 0; i < nb; ++i)
                phi[i] -= len[i] * (-std::sin(phi[i]) * lam.x + std::cos(phi[i]) * lam.y);
        }
    }
    else
    {
        // Smallest length change (weighted by the lengths) that closes the polygon.
        glm::dvec2 r(0.0);
        double g00 = 0.0, g01 = 0.0, g11 = 0.0;
        for (size_t i = 0; i < nb; ++i)
        {
            double c = std::cos(phi[i]), s = std::sin(phi[i]);
            r += len[i] * glm::dvec2(c, s);
            g00 += len[i] * c * c;
            g01 += len[i] * c * s;
            g11 += len[i] * s * s;
        }
        double det = g00 * g11 - g01 * g01;
        if (std::abs(det) > 1e-300)
        {
            glm::dvec2 lam((g11 * r.x - g01 * r.y) / det, (-g01 * r.x + g00 * r.y) / det);
            for (size_t i = 0; i < nb; ++i)
                len[i] -= len[i] * (std::cos(phi[i]) * lam.x + std::sin(phi[i]) * lam.y);
        }
    }

    std::vector<double> x(V, 0.0), y(V, 0.0), zero(V, 0.0);
    glm::dvec2 at(0.0);
    for (size_t i = 0; i < nb; ++i)
    {
        x[m->loop[i]] = at.x;
        y[m->loop[i]] = at.y;
        at += len[i] * glm::dvec2(std::cos(phi[i]), std::sin(phi[i]));
    }
    // A conformal map's coordinates are harmonic, so the harmonic extension of its boundary is the map.
    if (!m->interior.solve(zero, x) || !m->interior.solve(zero, y))
        return false;
    uv.resize(V);
    for (size_t v = 0; v < V; ++v)
        uv[v] = {x[v], y[v]};
    return true;
}

std::vector<Index> placeCones(const Chart& ch, int count, std::span<const Index> fixedCones)
{
    LILA_ZONE();

    std::vector<Index> cones(fixedCones.begin(), fixedCones.end());
    if (count <= 0 || ch.tris.empty())
        return cones;
    Geometry g = geometry(ch);
    std::vector<uint8_t> boundary = boundaryMask(ch);
    for (int n = 0; n < count; ++n)
    {
        std::vector<double> u = coneScale(ch, g, boundary, cones);
        if (u.empty())
            break;
        Index best = kInvalid;
        double bestU = 1e-9;
        for (Index v = 0; v < ch.vertexCount(); ++v)
            if (!boundary[v] && std::find(cones.begin(), cones.end(), v) == cones.end() && std::abs(u[v]) > bestU)
            {
                bestU = std::abs(u[v]);
                best = v;
            }
        if (best == kInvalid)
            break;
        cones.push_back(best);
    }
    return cones;
}

std::vector<Index> cutChart(Chart& ch, std::span<const std::pair<Index, Index>> edges)
{
    LILA_ZONE();

    const Index V0 = ch.vertexCount();
    std::vector<Index> origin(V0);
    std::iota(origin.begin(), origin.end(), Index(0));
    if (edges.empty())
        return origin;
    std::unordered_set<uint64_t> cut;
    std::vector<Index> touched;
    for (auto [a, b] : edges)
    {
        cut.insert(edgeKey(a, b));
        touched.push_back(a);
        touched.push_back(b);
    }
    std::sort(touched.begin(), touched.end());
    touched.erase(std::unique(touched.begin(), touched.end()), touched.end());

    std::vector<std::vector<std::pair<size_t, int>>> incident(V0);
    for (size_t i = 0; i < ch.tris.size(); ++i)
        for (int k = 0; k < 3; ++k)
            incident[ch.tris[i][k]].emplace_back(i, k);

    for (Index v : touched)
    {
        const auto& list = incident[v];
        std::vector<size_t> parent(list.size());
        std::iota(parent.begin(), parent.end(), size_t(0));
        auto find = [&](size_t x) {
            while (parent[x] != x)
                x = parent[x] = parent[parent[x]];
            return x;
        };
        std::unordered_map<Index, size_t> seen;
        for (size_t e = 0; e < list.size(); ++e)
        {
            const auto& t = ch.tris[list[e].first];
            for (int s : {1, 2})
            {
                Index w = t[size_t((list[e].second + s) % 3)];
                if (cut.count(edgeKey(v, w)))
                    continue;
                auto [it, fresh] = seen.try_emplace(w, e);
                if (!fresh)
                    parent[find(e)] = find(it->second);
            }
        }
        std::unordered_map<size_t, Index> groupVertex;
        groupVertex[find(0)] = v;
        for (size_t e = 0; e < list.size(); ++e)
        {
            auto [it, fresh] = groupVertex.try_emplace(find(e), kInvalid);
            if (fresh)
            {
                it->second = Index(ch.pos.size());
                ch.pos.push_back(ch.pos[v]);
                ch.uv.push_back(ch.uv[v]);
                ch.pinned.push_back(ch.pinned[v]);
                ch.vertexPoint.push_back(ch.vertexPoint[v]);
                origin.push_back(v);
            }
            ch.tris[list[e].first][size_t(list[e].second)] = it->second;
        }
    }

    std::unordered_map<uint64_t, Index> faceVertex;
    for (size_t i = 0; i < ch.tris.size(); ++i)
        for (Index w : ch.tris[i])
            faceVertex[(uint64_t(ch.triFace[i]) << 32) | origin[w]] = w;
    for (size_t f = 0; f + 1 < ch.faceStart.size(); ++f)
        for (Index k = ch.faceStart[f]; k < ch.faceStart[f + 1]; ++k)
        {
            auto it = faceVertex.find((uint64_t(ch.faces[f]) << 32) | ch.cornerVertex[k]);
            if (it != faceVertex.end())
                ch.cornerVertex[k] = it->second;
        }
    return origin;
}

BffResult bff(Chart& ch, const BffOptions& opts)
{
    LILA_ZONE();

    BffResult r;
    if (ch.tris.empty())
        return r;

    std::vector<double> coneU;
    if (opts.boundary == BffBoundary::MinimalArea && (opts.autoCones > 0 || !opts.cones.empty()))
    {
        std::vector<uint8_t> boundary = boundaryMask(ch);
        std::vector<Index> fixed;
        for (Index c : opts.cones)
            if (c < ch.vertexCount() && !boundary[c])
                fixed.push_back(c);
        std::vector<Index> cones = placeCones(ch, opts.autoCones, fixed);
        if (!cones.empty())
        {
            Geometry g = geometry(ch);
            coneU = coneScale(ch, g, boundary, cones);
            if (coneU.empty())
                return r;
            std::vector<std::pair<Index, Index>> paths = conePaths(ch, cones);
            std::vector<Index> origin = cutChart(ch, paths);
            std::vector<double> u(origin.size());
            for (size_t v = 0; v < origin.size(); ++v)
                u[v] = coneU[origin[v]];
            coneU = std::move(u);
            r.cones = std::move(cones);
            r.cutEdges = paths.size();
        }
    }

    BffSolver s(ch);
    if (!s.ok())
        return r;
    const std::vector<Index>& loop = s.boundary();
    const size_t nb = loop.size();
    std::vector<double> u(nb, 0.0), k;
    auto lengthsFor = [&](const std::vector<double>& uu) {
        std::vector<double> l(nb);
        for (size_t i = 0; i < nb; ++i)
            l[i] = std::exp(0.5 * (uu[i] + uu[(i + 1) % nb])) * glm::length(ch.pos[loop[(i + 1) % nb]] - ch.pos[loop[i]]);
        return l;
    };
    bool lengthsMatter = false;
    switch (opts.boundary)
    {
    case BffBoundary::MinimalArea:
        if (!coneU.empty())
            for (size_t i = 0; i < nb; ++i)
                u[i] = coneU[loop[i]];
        k = s.dirichletToNeumann(u);
        lengthsMatter = true;
        break;
    case BffBoundary::ScaleFactors:
        if (opts.boundaryValues.size() != nb)
            return r;
        u = opts.boundaryValues;
        k = s.dirichletToNeumann(u);
        lengthsMatter = true;
        break;
    case BffBoundary::Curvature:
    {
        if (opts.boundaryValues.size() != nb)
            return r;
        k = opts.boundaryValues;
        double shift = (kTwoPi - std::accumulate(k.begin(), k.end(), 0.0)) / double(nb);
        for (double& x : k)
            x += shift;
        u = s.neumannToDirichlet(k);
        break;
    }
    case BffBoundary::Disk:
        // Curvature proportional to length makes a circle; a few rounds settle the lengths it implies.
        for (int it = 0; it < 4 && u.size() == nb; ++it)
        {
            std::vector<double> l = lengthsFor(u);
            double total = std::accumulate(l.begin(), l.end(), 0.0);
            k.assign(nb, 0.0);
            for (size_t i = 0; i < nb; ++i)
                k[i] = kTwoPi * 0.5 * (l[(i + nb - 1) % nb] + l[i]) / total;
            u = s.neumannToDirichlet(k);
        }
        break;
    case BffBoundary::Polygon:
    {
        std::vector<size_t> corners;
        for (Index c : opts.corners)
            if (auto it = std::find(loop.begin(), loop.end(), c); it != loop.end())
                corners.push_back(size_t(it - loop.begin()));
        std::sort(corners.begin(), corners.end());
        corners.erase(std::unique(corners.begin(), corners.end()), corners.end());
        if (corners.size() < 3)
            corners = sharpestCorners(ch, loop, s.curvature(), std::min<size_t>(4, nb));
        if (corners.size() < 3)
            return r;
        k.assign(nb, 0.0);
        for (size_t c : corners)
            k[c] = kTwoPi / double(corners.size());
        u = s.neumannToDirichlet(k);
        break;
    }
    }
    if (k.size() != nb || u.size() != nb)
        return r;

    std::vector<glm::dvec2> uv;
    if (!s.flatten(u, k, opts.exactLengths && opts.boundary != BffBoundary::Polygon && opts.boundary != BffBoundary::Disk, uv))
        return r;
    for (const glm::dvec2& p : uv)
        if (!std::isfinite(p.x) || !std::isfinite(p.y))
            return r;
    ch.uv = std::move(uv);
    if (ch.areaUv() < 0.0)
        for (glm::dvec2& p : ch.uv)
            p.x = -p.x;
    if (!lengthsMatter)
        matchSurfaceArea(ch);
    glm::dvec2 lo = ch.uvMin();
    for (glm::dvec2& p : ch.uv)
        p -= lo;
    r.ok = true;
    return r;
}
} // namespace lila::uv
