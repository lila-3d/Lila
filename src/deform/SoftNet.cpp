#include "deform/SoftNet.h"

#include "core/Profile.h"

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <Eigen/Sparse>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <algorithm>
#include <cmath>

namespace lila::deform::softnet
{
namespace
{
using SpMat = Eigen::SparseMatrix<double>;
using Rows = Eigen::Matrix<double, Eigen::Dynamic, 3>;
} // namespace

struct Solver::Impl
{
    size_t points = 0;
    size_t samples = 0;
    double weight = 0.0;
    SpMat c;
    Eigen::SimplicialLDLT<SpMat> solver;
};

Solver::Solver() = default;
Solver::~Solver() = default;
Solver::Solver(Solver&&) noexcept = default;
Solver& Solver::operator=(Solver&&) noexcept = default;

bool Solver::prepare(const geo::Mesh& topology, std::span<const Sample> samples, double edgeLength, double reach, double stiffness)
{
    LILA_ZONE();
    mImpl.reset();
    const size_t n = topology.pointCapacity();
    if (n == 0 || samples.empty())
        return false;

    // The graph Laplacian: it depends on the topology alone, so the factor is the same whatever pose it is made in.
    std::vector<Eigen::Triplet<double>> lt;
    std::vector<double> degree(n, 0.0);
    for (geo::Index e = 0; e < topology.edgeCapacity(); ++e)
    {
        if (!topology.edgeValid(e))
            continue;
        const geo::Index a = topology.edgePoint(e, 0), b = topology.edgePoint(e, 1);
        if (a >= n || b >= n || a == b)
            continue;
        lt.emplace_back(Eigen::Index(a), Eigen::Index(b), -1.0);
        lt.emplace_back(Eigen::Index(b), Eigen::Index(a), -1.0);
        degree[a] += 1.0;
        degree[b] += 1.0;
    }
    for (size_t i = 0; i < n; ++i)
        lt.emplace_back(Eigen::Index(i), Eigen::Index(i), degree[i]);
    const Eigen::Index size = Eigen::Index(n);
    SpMat l(size, size);
    l.setFromTriplets(lt.begin(), lt.end());

    std::vector<Eigen::Triplet<double>> ct;
    for (size_t s = 0; s < samples.size(); ++s)
        for (int k = 0; k < 3; ++k)
            if (samples[s].points[size_t(k)] < n && samples[s].bary[k] != 0.0)
                ct.emplace_back(Eigen::Index(s), Eigen::Index(samples[s].points[size_t(k)]), samples[s].bary[k]);
    auto impl = std::make_unique<Impl>();
    impl->c = SpMat(Eigen::Index(samples.size()), size);
    impl->c.setFromTriplets(ct.begin(), ct.end());

    // In edge units: the screening makes a displacement fade over about reach, and the samples pull far harder than
    // bending resists, so the surface meets them closely.
    const double h = std::max(edgeLength, 1e-9);
    const double r = std::max(reach, h) / h;
    impl->weight = std::max(stiffness, 1e-6) * 100.0;
    SpMat a = SpMat(l.transpose()) * l;
    SpMat id(size, size);
    id.setIdentity();
    a += (1.0 / (r * r * r * r)) * id;
    a += impl->weight * SpMat(impl->c.transpose() * impl->c);
    impl->solver.compute(a);
    if (impl->solver.info() != Eigen::Success)
        return false;
    impl->points = n;
    impl->samples = samples.size();
    mImpl = std::move(impl);
    return true;
}

bool Solver::ready() const { return mImpl != nullptr; }

std::vector<glm::dvec3> Solver::solve(std::span<const glm::dvec3> deltas) const
{
    LILA_ZONE();
    std::vector<glm::dvec3> out(mImpl ? mImpl->points : 0, glm::dvec3(0.0));
    if (!mImpl || deltas.size() != mImpl->samples)
        return out;
    Rows d(Eigen::Index(deltas.size()), 3);
    for (size_t s = 0; s < deltas.size(); ++s)
        d.row(Eigen::Index(s)) << deltas[s].x, deltas[s].y, deltas[s].z;
    const Rows rhs = mImpl->weight * (mImpl->c.transpose() * d);
    const Rows x = mImpl->solver.solve(rhs);
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = {x(Eigen::Index(i), 0), x(Eigen::Index(i), 1), x(Eigen::Index(i), 2)};
    return out;
}

std::array<double, 2> knotWeights(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    const double b = t * t * (3.0 - 2.0 * t);
    return {1.0 - b, b};
}
} // namespace lila::deform::softnet
