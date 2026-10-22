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

std::vector<glm::dvec3> fitCage(const geo::Mesh& cage, std::span<const geo::PointStencil> stencils, std::span<const glm::dvec3> target,
                                const FitOptions& opts)
{
    LILA_ZONE();

    const int n = int(cage.pointCapacity());
    std::vector<glm::dvec3> x(cage.positions().begin(), cage.positions().end());
    if (stencils.empty() || int(stencils.front().inputs) != n || target.size() != stencils.back().rows())
        throw std::invalid_argument("fitCage: the stencils don't match the cage and target");

    // Solved for the change from the starting cage: a cage that already fits then gives a zero right-hand side, and
    // the parts of the cage the subdivision cannot see (a cube's alternating corners) stay exactly where they were.
    std::vector<glm::dvec3> fitted = x;
    for (const geo::PointStencil& s : stencils)
        fitted = s.apply(fitted);
    Eigen::MatrixX3d residual(Eigen::Index(target.size()), 3);
    for (size_t i = 0; i < target.size(); ++i)
    {
        const glm::dvec3 r = target[i] - fitted[i];
        residual.row(Eigen::Index(i)) << r.x, r.y, r.z;
    }

    // Normal equations without forming the full product: M = S1' (S2' (... SL' SL ...) S2) S1, rhs likewise.
    SpMat last = toEigen(stencils.back());
    SpMat M = SpMat(last.transpose()) * last;
    Eigen::MatrixX3d rhs = last.transpose() * residual;
    for (size_t l = stencils.size() - 1; l-- > 0;)
    {
        SpMat s = toEigen(stencils[l]);
        SpMat st = s.transpose();
        M = SpMat(st * M) * s;
        rhs = st * rhs;
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
        if (!valid || pinned)
            rhs.row(i).setZero();
    }
    SpMat D(n, n);
    D.setFromTriplets(diag.begin(), diag.end());
    M += D;

    Eigen::SimplicialLDLT<SpMat> solver(M);
    if (solver.info() != Eigen::Success)
        throw std::runtime_error("fitCage: the normal equations are singular");
    Eigen::MatrixX3d dx = solver.solve(rhs);
    for (int i = 0; i < n; ++i)
        if (cage.pointValid(geo::Index(i)) && !(size_t(i) < opts.pinned.size() && opts.pinned[size_t(i)]))
            x[size_t(i)] += glm::dvec3(dx(i, 0), dx(i, 1), dx(i, 2));
    if (opts.symmetry)
        symmetrizePositions(x, *opts.symmetry);
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

    std::vector<geo::PointStencil> stencils = geo::subdivideStencils(cage, opts.subdivide);
    for (int it = 0; it < std::max(1, opts.iterations); ++it)
    {
        std::vector<glm::dvec3> pts(out.positions().begin(), out.positions().end());
        for (const geo::PointStencil& s : stencils)
            pts = s.apply(pts);
        tbb::parallel_for(size_t(0), pts.size(), [&](size_t i) {
            geo::BvhClosest c;
            if (bvh.closestPoint(pts[i], c))
                pts[i] = c.point;
        });
        std::vector<glm::dvec3> x = fitCage(out, stencils, pts, opts);
        for (geo::Index p = 0; p < out.pointCapacity(); ++p)
            out.setPosition(p, x[p]);
    }
    return out;
}
} // namespace lila::sculpt
