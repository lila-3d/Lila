#include "sculpt/Reproject.h"

#include "core/Profile.h"
#include "retopo/Retopo.h"

#include <Eigen/Sparse>
#include <Eigen/SparseCholesky>

#include <tbb/parallel_for.h>

#include <stdexcept>

namespace lila::sculpt
{
namespace
{
using SpMat = Eigen::SparseMatrix<double>;

SpMat toEigen(const geo::PointStencil& s)
{
    std::vector<Eigen::Triplet<double>> t;
    t.reserve(s.cols.size());
    for (geo::Index r = 0; r < s.rows(); ++r)
        for (geo::Index k = s.offsets[r]; k < s.offsets[r + 1]; ++k)
            t.emplace_back(int(r), int(s.cols[k]), s.weights[k]);
    SpMat m(int(s.rows()), int(s.inputs));
    m.setFromTriplets(t.begin(), t.end());
    return m;
}
} // namespace

void symmetrizePositions(std::span<glm::dvec3> positions, const geo::SymmetryMap& map)
{
    if (map.isRadial())
    {
        map.averageOrbits(positions, true);
        return;
    }
    const geo::SymmetryPlane& pl = map.plane();
    for (geo::Index p = 0; p < positions.size(); ++p)
    {
        const geo::Index q = map.mirrorPoint(p);
        if (q == geo::kInvalid || q < p || q >= positions.size())
            continue;
        if (q == p)
        {
            positions[p] = pl.project(positions[p]);
            continue;
        }
        const glm::dvec3 avg = (positions[p] + pl.reflect(positions[q])) * 0.5;
        positions[p] = avg;
        positions[q] = pl.reflect(avg);
    }
}

struct CageFitter::Impl
{
    int n = 0;
    std::vector<uint8_t> held;
    std::vector<SpMat> transposed;
    std::span<const geo::PointStencil> stencils;
    const geo::SymmetryMap* symmetry = nullptr;
    double maxMoveRatio = 0.0;
    SpMat normal;
    Eigen::SimplicialLDLT<SpMat> solver;

    /** Per cage point, the largest of the values over the subdivided points it reaches. */
    Eigen::VectorXd reachMax(Eigen::VectorXd v) const
    {
        for (size_t l = transposed.size(); l-- > 0;)
        {
            const SpMat& t = transposed[l];
            Eigen::VectorXd out = Eigen::VectorXd::Zero(t.rows());
            for (int k = 0; k < t.outerSize(); ++k)
                for (SpMat::InnerIterator it(t, k); it; ++it)
                    if (it.value() != 0.0)
                        out(it.row()) = std::max(out(it.row()), v(it.col()));
            v = std::move(out);
        }
        return v;
    }

    /**
     * Refits with every point the fit moves further than maxMoveRatio times the largest move of the surface it shapes
     * held at that limit, along its own direction. A few rounds, as holding some points pushes others out.
     */
    Eigen::MatrixX3d capMoves(Eigen::MatrixX3d dx, const Eigen::MatrixX3d& rhs, const Eigen::VectorXd& reach) const
    {
        LILA_ZONE();

        std::vector<double> weight(size_t(n), 0.0);
        std::vector<Eigen::RowVector3d> goal(size_t(n), Eigen::RowVector3d::Zero());
        for (int round = 0; round < 4; ++round)
        {
            bool capped = false;
            for (int i = 0; i < n; ++i)
            {
                const double limit = maxMoveRatio * reach(i);
                const double len = dx.row(i).norm();
                if (held[size_t(i)] || len <= limit * 1.001)
                    continue;
                weight[size_t(i)] = 1e3 * normal.coeff(i, i);
                goal[size_t(i)] = len > 0.0 ? Eigen::RowVector3d(dx.row(i) * (limit / len)) : Eigen::RowVector3d::Zero();
                capped = true;
            }
            if (!capped)
                break;
            std::vector<Eigen::Triplet<double>> diag;
            Eigen::MatrixX3d b = rhs;
            for (int i = 0; i < n; ++i)
                if (weight[size_t(i)] > 0.0)
                {
                    diag.emplace_back(i, i, weight[size_t(i)]);
                    b.row(i) += weight[size_t(i)] * goal[size_t(i)];
                }
            SpMat W(n, n);
            W.setFromTriplets(diag.begin(), diag.end());
            Eigen::SimplicialLDLT<SpMat> refit(SpMat(normal + W));
            if (refit.info() != Eigen::Success)
                break;
            dx = refit.solve(b);
        }
        return dx;
    }
};

CageFitter::CageFitter(const geo::Mesh& cage, std::span<const geo::PointStencil> stencils, const FitOptions& opts) : m(std::make_unique<Impl>())
{
    LILA_ZONE();

    const int n = int(cage.pointCapacity());
    if (stencils.empty() || int(stencils.front().inputs) != n)
        throw std::invalid_argument("fitCage: the stencils don't match the cage");
    m->n = n;
    m->stencils = stencils;
    m->symmetry = opts.symmetry;
    m->maxMoveRatio = std::max(opts.maxMoveRatio, 0.0);

    // Normal equations without forming the full product: M = S1' (S2' (... SL' SL ...) S2) S1, rhs likewise.
    SpMat last = toEigen(stencils.back());
    m->transposed.resize(stencils.size());
    m->transposed.back() = last.transpose();
    SpMat M = m->transposed.back() * last;
    for (size_t l = stencils.size() - 1; l-- > 0;)
    {
        SpMat s = toEigen(stencils[l]);
        m->transposed[l] = s.transpose();
        M = SpMat(m->transposed[l] * M) * s;
    }
    M.prune(0.0);

    double meanDiag = 0.0;
    int counted = 0;
    for (int i = 0; i < n; ++i)
        if (cage.pointValid(geo::Index(i)))
        {
            meanDiag += M.coeff(i, i);
            ++counted;
        }
    meanDiag = counted ? meanDiag / counted : 1.0;
    const double lambda = std::max(opts.regularization, 0.0) * meanDiag;
    std::vector<Eigen::Triplet<double>> diag;
    m->held.assign(size_t(n), 0);
    for (int i = 0; i < n; ++i)
    {
        double w = lambda;
        const bool valid = cage.pointValid(geo::Index(i));
        const bool pinned = size_t(i) < opts.pinned.size() && opts.pinned[size_t(i)];
        // Points the subdivision never reads (deleted ones) and pinned ones are held at their start.
        if (!valid || M.coeff(i, i) == 0.0)
            w = 1.0;
        else if (pinned)
            w = 1e8 * meanDiag;
        if (w > 0.0)
            diag.emplace_back(i, i, w);
        m->held[size_t(i)] = !valid || pinned;
    }
    SpMat D(n, n);
    D.setFromTriplets(diag.begin(), diag.end());
    M += D;

    m->solver.compute(M);
    if (m->solver.info() != Eigen::Success)
        throw std::runtime_error("fitCage: the normal equations are singular");
    if (m->maxMoveRatio > 0.0)
        m->normal = std::move(M);
}

CageFitter::~CageFitter() = default;

std::vector<glm::dvec3> CageFitter::solve(std::span<const glm::dvec3> start, std::span<const glm::dvec3> target) const
{
    LILA_ZONE();

    std::vector<glm::dvec3> x(start.begin(), start.end());
    if (int(x.size()) != m->n || target.size() != m->stencils.back().rows())
        throw std::invalid_argument("fitCage: the stencils don't match the cage and target");

    // Solved for the change from the starting cage: a cage that already fits then gives a zero right-hand side, and
    // the parts of the cage the subdivision cannot see (a cube's alternating corners) stay exactly where they were.
    std::vector<glm::dvec3> fitted = x;
    for (const geo::PointStencil& s : m->stencils)
        fitted = s.apply(fitted);
    Eigen::MatrixX3d rhs(Eigen::Index(target.size()), 3);
    Eigen::VectorXd gap(Eigen::Index(target.size()));
    for (size_t i = 0; i < target.size(); ++i)
    {
        const glm::dvec3 r = target[i] - fitted[i];
        rhs.row(Eigen::Index(i)) << r.x, r.y, r.z;
        gap(Eigen::Index(i)) = glm::length(r);
    }
    for (size_t l = m->transposed.size(); l-- > 0;)
        rhs = Eigen::MatrixX3d(m->transposed[l] * rhs);
    for (int i = 0; i < m->n; ++i)
        if (m->held[size_t(i)])
            rhs.row(i).setZero();

    Eigen::MatrixX3d dx = m->solver.solve(rhs);
    if (m->maxMoveRatio > 0.0)
        dx = m->capMoves(std::move(dx), rhs, m->reachMax(std::move(gap)));
    for (int i = 0; i < m->n; ++i)
        if (!m->held[size_t(i)])
            x[size_t(i)] += glm::dvec3(dx(i, 0), dx(i, 1), dx(i, 2));
    if (m->symmetry)
        symmetrizePositions(x, *m->symmetry);
    return x;
}

std::vector<glm::dvec3> fitCage(const geo::Mesh& cage, std::span<const geo::PointStencil> stencils, std::span<const glm::dvec3> target,
                                const FitOptions& opts)
{
    LILA_ZONE();

    return CageFitter(cage, stencils, opts).solve(cage.positions(), target);
}

std::vector<glm::dvec3> fitToSurface(const CageFitter& fitter, std::span<const geo::PointStencil> stencils, std::span<const glm::dvec3> start,
                                     const geo::MeshBvh& bvh, int iterations)
{
    LILA_ZONE();

    std::vector<glm::dvec3> x(start.begin(), start.end());
    for (int it = 0; it < std::max(1, iterations); ++it)
    {
        std::vector<glm::dvec3> pts = x;
        for (const geo::PointStencil& s : stencils)
            pts = s.apply(pts);
        tbb::parallel_for(size_t(0), pts.size(), [&](size_t i) {
            geo::BvhClosest c;
            if (bvh.closestPoint(pts[i], c))
                pts[i] = c.point;
        });
        x = fitter.solve(x, pts);
    }
    return x;
}

geo::Mesh fitCageToSurface(const geo::Mesh& cage, const geo::Mesh& surface, const geo::MeshBvh& bvh, const FitOptions& opts)
{
    LILA_ZONE();

    geo::Mesh out = cage;
    if (opts.method != FitMethod::LeastSquares)
    {
        retopo::ProjectOptions po;
        po.method = opts.method == FitMethod::Normal ? retopo::ProjectMethod::Normal : retopo::ProjectMethod::Closest;
        if (!opts.pinned.empty())
        {
            po.active.assign(out.pointCapacity(), 1);
            for (size_t p = 0; p < opts.pinned.size() && p < po.active.size(); ++p)
                po.active[p] = !opts.pinned[p];
        }
        retopo::projectMesh(out, surface, bvh, po);
        if (opts.symmetry)
            symmetrizePositions(out.positions(), *opts.symmetry);
        return out;
    }

    const std::vector<geo::PointStencil> stencils = geo::subdivideStencils(cage, opts.subdivide);
    const CageFitter fitter(cage, stencils, opts);
    std::vector<glm::dvec3> x = fitToSurface(fitter, stencils, cage.positions(), bvh, opts.iterations);
    for (geo::Index p = 0; p < out.pointCapacity(); ++p)
        out.setPosition(p, x[p]);
    return out;
}
} // namespace lila::sculpt
