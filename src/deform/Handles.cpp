#include "deform/Handles.h"

#include "core/Profile.h"
#include "deform/Deform.h"
#include "deform/Shape.h"

#include <Eigen/Sparse>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <map>

namespace lila::deform
{
namespace
{
using SpMat = Eigen::SparseMatrix<double>;
using CholeskySolver = Eigen::SimplicialLDLT<SpMat>;
using Rows = Eigen::Matrix<double, Eigen::Dynamic, 3>;

/** The positive semidefinite cotangent Laplacian: L_ii = sum of w_ij, L_ij = -w_ij. */
SpMat laplacianMatrix(const CotanLaplacian& c, size_t n)
{
    std::vector<Eigen::Triplet<double>> t;
    t.reserve(c.weight.size() * 4);
    for (size_t e = 0; e < c.weight.size(); ++e)
    {
        const int a = int(c.edgeA[e]), b = int(c.edgeB[e]);
        const double w = c.weight[e];
        t.emplace_back(a, a, w);
        t.emplace_back(b, b, w);
        t.emplace_back(a, b, -w);
        t.emplace_back(b, a, -w);
    }
    SpMat L(static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(n));
    L.setFromTriplets(t.begin(), t.end());
    return L;
}

/** L M^-1 L, the biharmonic operator. */
SpMat biharmonicMatrix(const SpMat& L, const CotanLaplacian& c)
{
    double mean = 0.0;
    for (double a : c.area)
        mean += a;
    mean = c.area.empty() ? 1.0 : mean / double(c.area.size());
    Eigen::VectorXd inv(L.rows());
    for (Eigen::Index i = 0; i < L.rows(); ++i)
        inv[i] = 1.0 / std::max(c.area[size_t(i)], 1e-8 * mean + 1e-300);
    SpMat Q = L * inv.asDiagonal() * L;
    Q.makeCompressed();
    return Q;
}

/** A[free, free]; freeIdx maps a point to its free row or -1. */
SpMat restrictFree(const SpMat& A, const std::vector<int>& freeIdx, int nFree)
{
    std::vector<Eigen::Triplet<double>> t;
    t.reserve(size_t(A.nonZeros()));
    for (Eigen::Index k = 0; k < A.outerSize(); ++k)
        for (SpMat::InnerIterator it(A, k); it; ++it)
        {
            int r = freeIdx[size_t(it.row())], c = freeIdx[size_t(it.col())];
            if (r >= 0 && c >= 0)
                t.emplace_back(r, c, it.value());
        }
    SpMat out(nFree, nFree);
    out.setFromTriplets(t.begin(), t.end());
    return out;
}
} // namespace

CotanLaplacian cotanLaplacian(const geo::Mesh& topology, std::span<const glm::dvec3> points)
{
    LILA_ZONE();

    CotanLaplacian out;
    out.area.assign(points.size(), 0.0);
    std::map<std::pair<uint32_t, uint32_t>, double> edges;
    std::vector<uint32_t> tris = trianglePoints(topology);
    for (size_t t = 0; t + 2 < tris.size(); t += 3)
    {
        const uint32_t id[3] = {tris[t], tris[t + 1], tris[t + 2]};
        if (id[0] >= points.size() || id[1] >= points.size() || id[2] >= points.size())
            continue;
        const glm::dvec3 p[3] = {points[id[0]], points[id[1]], points[id[2]]};
        const double area2 = glm::length(glm::cross(p[1] - p[0], p[2] - p[0]));
        if (area2 < 1e-30)
            continue;
        for (int k = 0; k < 3; ++k)
        {
            const glm::dvec3& o = p[k];
            const glm::dvec3 a = p[(k + 1) % 3] - o, b = p[(k + 2) % 3] - o;
            const double cot = glm::dot(a, b) / area2;
            uint32_t i = id[(k + 1) % 3], j = id[(k + 2) % 3];
            edges[{std::min(i, j), std::max(i, j)}] += 0.5 * cot;
            out.area[id[k]] += area2 / 6.0;
        }
    }
    out.edgeA.reserve(edges.size());
    out.edgeB.reserve(edges.size());
    out.weight.reserve(edges.size());
    for (const auto& [e, w] : edges)
    {
        out.edgeA.push_back(e.first);
        out.edgeB.push_back(e.second);
        // Obtuse triangles give negative cotangents; clamping keeps the systems positive definite.
        out.weight.push_back(std::max(w, 1e-3));
    }
    return out;
}

// ------------------------------------------------------------------------------------------------ HandleSolver

namespace
{
/** Connected piece of every point over the Laplacian's edges. */
std::vector<uint32_t> pieces(const CotanLaplacian& c, size_t n)
{
    std::vector<uint32_t> parent(n);
    for (size_t i = 0; i < n; ++i)
        parent[i] = uint32_t(i);
    auto find = [&](uint32_t i) {
        while (parent[i] != i)
            i = parent[i] = parent[parent[i]];
        return i;
    };
    for (size_t e = 0; e < c.weight.size(); ++e)
    {
        uint32_t a = find(c.edgeA[e]), b = find(c.edgeB[e]);
        if (a != b)
            parent[std::max(a, b)] = std::min(a, b);
    }
    for (size_t i = 0; i < n; ++i)
        parent[i] = find(uint32_t(i));
    return parent;
}

/** Holds every point of a piece no group reaches in group `stay`. */
void holdLoosePieces(const CotanLaplacian& c, std::vector<int>& group, int stay)
{
    std::vector<uint32_t> piece = pieces(c, group.size());
    std::vector<char> reached(group.size(), 0);
    for (size_t p = 0; p < group.size(); ++p)
        if (group[p] >= 0)
            reached[piece[p]] = 1;
    for (size_t p = 0; p < group.size(); ++p)
        if (!reached[piece[p]])
            group[p] = stay;
}
} // namespace

struct HandleSolver::Impl
{
    Mode mode = Mode::Laplacian;
    size_t n = 0;
    std::vector<int> group; // per point: -1 free, else its group
    std::vector<int> freeIdx;
    std::vector<uint32_t> freePoints;
    int groups = 0;
    SpMat L, Q;
    CholeskySolver biharmonic, rigid;
    Eigen::MatrixXd blend; // free points x groups: biharmonic weights of each group, for the ARAP start
    // Weighted neighbours, CSR, for the ARAP local step.
    std::vector<uint32_t> offsets, nbr;
    std::vector<double> w;
    bool ok = false;
};

HandleSolver::HandleSolver() : mImpl(std::make_unique<Impl>()) {}
HandleSolver::~HandleSolver() = default;
HandleSolver::HandleSolver(HandleSolver&&) noexcept = default;
HandleSolver& HandleSolver::operator=(HandleSolver&&) noexcept = default;

bool HandleSolver::ready() const { return mImpl->ok; }

bool HandleSolver::prepare(const geo::Mesh& topology, std::span<const glm::dvec3> rest, std::span<const int> groups, Mode mode)
{
    LILA_ZONE();

    mImpl = std::make_unique<Impl>();
    Impl& m = *mImpl;
    m.mode = mode;
    m.n = rest.size();
    m.group.assign(m.n, -1);
    int maxGroup = -1;
    for (size_t p = 0; p < m.n && p < groups.size(); ++p)
        if (groups[p] >= 0)
        {
            m.group[p] = groups[p];
            maxGroup = std::max(maxGroup, groups[p]);
        }
    // Held points no transform is given for: past every real group, so they get the identity.
    const int stay = maxGroup + 1;
    for (size_t p = 0; p < m.n; ++p)
        if (!topology.pointValid(geo::Index(p)))
            m.group[p] = stay;
    CotanLaplacian c = cotanLaplacian(topology, rest);
    holdLoosePieces(c, m.group, stay);
    m.groups = stay + 1;

    m.freeIdx.assign(m.n, -1);
    for (size_t p = 0; p < m.n; ++p)
        if (m.group[p] < 0)
        {
            m.freeIdx[p] = int(m.freePoints.size());
            m.freePoints.push_back(uint32_t(p));
        }
    const int nFree = int(m.freePoints.size());
    if (nFree == 0)
        return false;

    m.L = laplacianMatrix(c, m.n);
    m.Q = biharmonicMatrix(m.L, c);
    m.biharmonic.compute(restrictFree(m.Q, m.freeIdx, nFree));
    if (m.biharmonic.info() != Eigen::Success)
        return false;
    if (mode == Mode::Arap)
    {
        m.rigid.compute(restrictFree(m.L, m.freeIdx, nFree));
        if (m.rigid.info() != Eigen::Success)
            return false;
        Eigen::MatrixXd held = Eigen::MatrixXd::Zero(Eigen::Index(m.n), m.groups);
        for (size_t p = 0; p < m.n; ++p)
            if (m.group[p] >= 0)
                held(Eigen::Index(p), m.group[p]) = 1.0;
        Eigen::MatrixXd qh = m.Q * held;
        Eigen::MatrixXd rhs(nFree, m.groups);
        for (int i = 0; i < nFree; ++i)
            rhs.row(i) = -qh.row(m.freePoints[size_t(i)]);
        m.blend = m.biharmonic.solve(rhs);
        std::vector<std::vector<std::pair<uint32_t, double>>> adj(m.n);
        for (size_t e = 0; e < c.weight.size(); ++e)
        {
            adj[c.edgeA[e]].push_back({c.edgeB[e], c.weight[e]});
            adj[c.edgeB[e]].push_back({c.edgeA[e], c.weight[e]});
        }
        m.offsets.assign(1, 0);
        for (const auto& list : adj)
        {
            for (const auto& [j, wt] : list)
            {
                m.nbr.push_back(j);
                m.w.push_back(wt);
            }
            m.offsets.push_back(uint32_t(m.nbr.size()));
        }
    }
    m.ok = true;
    return true;
}

std::vector<glm::dvec3> HandleSolver::solve(std::span<const glm::dvec3> input, std::span<const glm::dmat4> transforms, int iterations) const
{
    LILA_ZONE();

    const Impl& m = *mImpl;
    std::vector<glm::dvec3> out(input.begin(), input.end());
    if (input.size() != m.n || m.n == 0)
        return out;
    auto moved = [&](int g, const glm::dvec3& p) {
        return g >= 0 && size_t(g) < transforms.size() ? glm::dvec3(transforms[size_t(g)] * glm::dvec4(p, 1.0)) : p;
    };
    for (size_t p = 0; p < m.n; ++p)
        if (m.group[p] >= 0)
            out[p] = moved(m.group[p], input[p]);
    if (!m.ok)
        return out;
    const int nFree = int(m.freePoints.size());

    if (m.mode == Mode::Laplacian)
    {
        // Biharmonic displacement: Q_ff d_f = -Q_fc d_c.
        Rows dc = Rows::Zero(Eigen::Index(m.n), 3);
        for (size_t p = 0; p < m.n; ++p)
            if (m.group[p] >= 0)
                for (int k = 0; k < 3; ++k)
                    dc(Eigen::Index(p), k) = out[p][k] - input[p][k];
        Rows qd = m.Q * dc;
        Rows rhs(nFree, 3);
        for (int i = 0; i < nFree; ++i)
            rhs.row(i) = -qd.row(m.freePoints[size_t(i)]);
        Rows df = m.biharmonic.solve(rhs);
        for (int i = 0; i < nFree; ++i)
            out[m.freePoints[size_t(i)]] = input[m.freePoints[size_t(i)]] + glm::dvec3(df(i, 0), df(i, 1), df(i, 2));
        return out;
    }

    tbb::parallel_for(tbb::blocked_range<int>(0, nFree, 256), [&](const tbb::blocked_range<int>& r) {
        for (int f = r.begin(); f != r.end(); ++f)
        {
            const uint32_t p = m.freePoints[size_t(f)];
            glm::dvec3 acc(0.0);
            for (int g = 0; g < m.groups; ++g)
                if (double wt = m.blend(f, g); wt != 0.0)
                    acc += wt * moved(g, input[p]);
            out[p] = acc;
        }
    });
    Rows xc = Rows::Zero(Eigen::Index(m.n), 3);
    for (size_t p = 0; p < m.n; ++p)
        if (m.group[p] >= 0)
            for (int k = 0; k < 3; ++k)
                xc(Eigen::Index(p), k) = out[p][k];
    const Rows lxc = m.L * xc;
    std::vector<glm::dmat3> rot(m.n, glm::dmat3(1.0));
    for (int it = 0; it < std::max(iterations, 0); ++it)
    {
        tbb::parallel_for(tbb::blocked_range<size_t>(0, m.n, 256), [&](const tbb::blocked_range<size_t>& r) {
            for (size_t i = r.begin(); i != r.end(); ++i)
            {
                glm::dmat3 cov(0.0);
                for (uint32_t k = m.offsets[i]; k < m.offsets[i + 1]; ++k)
                {
                    const uint32_t j = m.nbr[k];
                    const glm::dvec3 e0 = input[i] - input[j], e1 = out[i] - out[j];
                    cov += m.w[k] * glm::dmat3(e1 * e0.x, e1 * e0.y, e1 * e0.z);
                }
                glm::dmat3 S;
                polarDecompose(cov, rot[i], S);
            }
        });
        Rows b(nFree, 3);
        tbb::parallel_for(tbb::blocked_range<int>(0, nFree, 256), [&](const tbb::blocked_range<int>& r) {
            for (int f = r.begin(); f != r.end(); ++f)
            {
                const uint32_t i = m.freePoints[size_t(f)];
                glm::dvec3 acc(0.0);
                for (uint32_t k = m.offsets[i]; k < m.offsets[i + 1]; ++k)
                {
                    const uint32_t j = m.nbr[k];
                    acc += 0.5 * m.w[k] * ((rot[i] + rot[j]) * (input[i] - input[j]));
                }
                for (int c = 0; c < 3; ++c)
                    b(f, c) = acc[c] - lxc(i, c);
            }
        });
        Rows x = m.rigid.solve(b);
        for (int f = 0; f < nFree; ++f)
            out[m.freePoints[size_t(f)]] = glm::dvec3(x(f, 0), x(f, 1), x(f, 2));
    }
    return out;
}

// ------------------------------------------------------------------------------------------------ BBW

std::vector<float> boundedBiharmonicWeights(const geo::Mesh& topology, std::span<const glm::dvec3> points, const std::vector<std::vector<uint32_t>>& handles)
{
    LILA_ZONE();

    const size_t n = points.size(), h = handles.size();
    std::vector<float> out(n * h, 0.0f);
    if (n == 0 || h == 0)
        return out;
    // -1 free, a handle index, or h for points held at 0 by every handle (invalid, or on a piece no handle reaches).
    std::vector<int> owner(n, -1);
    for (size_t k = 0; k < h; ++k)
        for (uint32_t p : handles[k])
            if (p < n && owner[p] < 0)
                owner[p] = int(k);
    for (size_t p = 0; p < n; ++p)
        if (!topology.pointValid(geo::Index(p)))
            owner[p] = int(h);
    CotanLaplacian c = cotanLaplacian(topology, points);
    holdLoosePieces(c, owner, int(h));
    SpMat L = laplacianMatrix(c, n);
    SpMat Q = biharmonicMatrix(L, c);

    tbb::parallel_for(size_t(0), h, [&](size_t k) {
        // Fixed points and their values: the handles, then any point that left [0, 1] in an earlier pass.
        std::vector<double> fixedValue(n, 0.0);
        std::vector<char> fixed(n, 0);
        for (size_t p = 0; p < n; ++p)
            if (owner[p] >= 0)
            {
                fixed[p] = 1;
                fixedValue[p] = owner[p] == int(k) ? 1.0 : 0.0;
            }
        std::vector<double> wk(n, 0.0);
        for (int pass = 0; pass < 12; ++pass)
        {
            std::vector<int> freeIdx(n, -1);
            std::vector<uint32_t> freePts;
            for (size_t p = 0; p < n; ++p)
                if (!fixed[p])
                {
                    freeIdx[p] = int(freePts.size());
                    freePts.push_back(uint32_t(p));
                }
            for (size_t p = 0; p < n; ++p)
                wk[p] = fixed[p] ? fixedValue[p] : 0.0;
            if (freePts.empty())
                break;
            Eigen::VectorXd xc(static_cast<Eigen::Index>(n));
            for (size_t p = 0; p < n; ++p)
                xc[Eigen::Index(p)] = fixed[p] ? fixedValue[p] : 0.0;
            Eigen::VectorXd qx = Q * xc;
            Eigen::VectorXd rhs(Eigen::Index(freePts.size()));
            for (size_t i = 0; i < freePts.size(); ++i)
                rhs[Eigen::Index(i)] = -qx[freePts[i]];
            CholeskySolver s(restrictFree(Q, freeIdx, int(freePts.size())));
            if (s.info() != Eigen::Success)
                break;
            Eigen::VectorXd x = s.solve(rhs);
            bool violated = false;
            for (size_t i = 0; i < freePts.size(); ++i)
            {
                double v = x[Eigen::Index(i)];
                wk[freePts[i]] = v;
                if (v < -1e-9 || v > 1.0 + 1e-9)
                {
                    fixed[freePts[i]] = 1;
                    fixedValue[freePts[i]] = v < 0.0 ? 0.0 : 1.0;
                    violated = true;
                }
            }
            if (!violated)
                break;
        }
        for (size_t p = 0; p < n; ++p)
            out[p * h + k] = float(std::clamp(wk[p], 0.0, 1.0));
    });

    for (size_t p = 0; p < n; ++p)
    {
        double sum = 0.0;
        for (size_t k = 0; k < h; ++k)
            sum += out[p * h + k];
        for (size_t k = 0; k < h; ++k)
            out[p * h + k] = sum > 1e-12 ? float(out[p * h + k] / sum) : float(1.0 / double(h));
    }
    return out;
}
} // namespace lila::deform
