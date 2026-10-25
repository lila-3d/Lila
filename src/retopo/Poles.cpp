#include "retopo/Internal.h"

#include "core/Profile.h"

#include <Eigen/Sparse>
#include <Eigen/SparseCholesky>

#include <glm/gtc/constants.hpp>

#include <climits>
#include <cmath>
#include <complex>
#include <map>
#include <numeric>
#include <queue>

namespace lila::retopo::detail
{
namespace
{
constexpr double kTwoPi = glm::two_pi<double>();

double wrapAngle(double a) { return std::remainder(a, kTwoPi); }

/** The edge between a and b, or kInvalid. */
Index edgeBetween(const Surface& s, Index a, Index b)
{
    for (Index k = s.adjStart[a]; k < s.adjStart[a + 1]; ++k)
        if (s.adj[k] == b)
            return s.adjEdge[k];
    return kInvalid;
}

/** A triangle's sides in winding order, with +1 where the side runs along its edge's stored direction. */
struct TriSides
{
    std::array<Index, 3> edge{kInvalid, kInvalid, kInvalid};
    std::array<int, 3> sign{0, 0, 0};
};

std::vector<TriSides> triSides(const Surface& s)
{
    std::vector<TriSides> out(s.tris.size());
    for (size_t t = 0; t < s.tris.size(); ++t)
        for (int k = 0; k < 3; ++k)
        {
            Index a = s.tris[t][k], b = s.tris[t][(k + 1) % 3];
            Index e = edgeBetween(s, a, b);
            out[t].edge[k] = e;
            out[t].sign[k] = e == kInvalid ? 0 : s.edges[e][0] == a ? 1 : -1;
        }
    return out;
}

/** Per triangle, the transport's turning round it, wrapped to a half turn either way: the curvature it holds. */
std::vector<double> holonomy(const Surface& s, const std::vector<TriSides>& sides, const std::vector<double>& theta)
{
    std::vector<double> omega(s.tris.size(), 0.0);
    for (size_t t = 0; t < s.tris.size(); ++t)
    {
        double sum = 0.0;
        for (int k = 0; k < 3; ++k)
            if (sides[t].edge[k] != kInvalid)
                sum += sides[t].sign[k] * theta[sides[t].edge[k]];
        omega[t] = wrapAngle(sum);
    }
    return omega;
}
} // namespace

std::vector<FieldSingularity> fieldSingularities(const Surface& s, const std::vector<glm::dvec3>& q)
{
    LILA_ZONE();

    std::vector<double> theta = transportAngles(s), phi = fieldAngles(s, q);
    std::vector<TriSides> sides = triSides(s);
    std::vector<double> omega = holonomy(s, sides, theta);
    std::vector<FieldSingularity> out;
    for (size_t t = 0; t < s.tris.size(); ++t)
    {
        double turn = omega[t];
        for (int k = 0; k < 3; ++k)
        {
            if (sides[t].edge[k] == kInvalid)
                continue;
            Index a = s.tris[t][k], b = s.tris[t][(k + 1) % 3];
            turn += wrapAngle(phi[b] - phi[a] - sides[t].sign[k] * theta[sides[t].edge[k]]);
        }
        int index = int(std::lround(turn / kTwoPi));
        if (index != 0)
            out.push_back({Index(t), index});
    }
    return out;
}

Shells shellIndexTotals(const Surface& s)
{
    LILA_ZONE();

    const Index n = Index(s.tris.size());
    std::vector<Index> parent(n);
    std::iota(parent.begin(), parent.end(), Index(0));
    auto find = [&](Index x) {
        while (parent[x] != x)
            x = parent[x] = parent[parent[x]];
        return x;
    };
    for (size_t e = 0; e < s.edges.size(); ++e)
        if (s.edgeTriCount[e] == 2)
            parent[find(s.edgeTris[e][0])] = find(s.edgeTris[e][1]);

    Shells out;
    out.shellOfTri.resize(n);
    std::map<Index, Index> shellOfRoot;
    for (Index t = 0; t < n; ++t)
    {
        auto [it, fresh] = shellOfRoot.try_emplace(find(t), Index(shellOfRoot.size()));
        out.shellOfTri[t] = it->second;
    }
    std::vector<double> sum(shellOfRoot.size(), 0.0);
    std::vector<uint8_t> open(shellOfRoot.size(), 0);
    std::vector<double> theta = transportAngles(s);
    std::vector<double> omega = holonomy(s, triSides(s), theta);
    for (Index t = 0; t < n; ++t)
        sum[out.shellOfTri[t]] += omega[t];
    for (size_t e = 0; e < s.edges.size(); ++e)
        if (s.edgeTriCount[e] != 2 && s.edgeTriCount[e] > 0)
            open[out.shellOfTri[s.edgeTris[e][0]]] = 1;
    out.indexTotal.resize(sum.size());
    for (size_t k = 0; k < sum.size(); ++k)
        out.indexTotal[k] = open[k] ? INT_MIN : int(std::lround(sum[k] / kTwoPi));
    return out;
}

PrescribedTwist prescribeSingularities(const Surface& s, std::span<const FieldSingularity> singularities)
{
    LILA_ZONE();

    PrescribedTwist out;
    const Index nt = Index(s.tris.size());
    std::vector<int> index(nt, 0);
    for (const FieldSingularity& f : singularities)
        if (f.tri < nt)
            index[f.tri] += f.index;

    Shells shells = shellIndexTotals(s);
    std::vector<int> have(shells.indexTotal.size(), 0);
    for (Index t = 0; t < nt; ++t)
        have[shells.shellOfTri[t]] += index[t];
    for (size_t k = 0; k < have.size(); ++k)
        if (shells.indexTotal[k] != INT_MIN && have[k] != shells.indexTotal[k])
        {
            out.error = "the poles on a closed shell must add up to " + std::to_string(shells.indexTotal[k]) +
                        " (three-edge poles count +1, five-edge -1), not " + std::to_string(have[k]);
            return out;
        }

    // Least-norm twist x with A x = b: each triangle's sides turn 2pi times its index more than the curvature it holds.
    // x = A^T y with (A A^T) y = b, a Laplacian over the triangles.
    std::vector<TriSides> sides = triSides(s);
    std::vector<double> theta = transportAngles(s);
    std::vector<double> omega = holonomy(s, sides, theta);
    std::vector<Eigen::Triplet<double>> trip;
    trip.reserve(size_t(nt) * 4);
    for (Index t = 0; t < nt; ++t)
    {
        double diag = 1e-9;
        for (int k = 0; k < 3; ++k)
        {
            Index e = sides[t].edge[k];
            if (e == kInvalid)
                continue;
            diag += 1.0;
            if (s.edgeTriCount[e] != 2)
                continue;
            Index other = s.edgeTris[e][0] == t ? s.edgeTris[e][1] : s.edgeTris[e][0];
            int otherSign = 0;
            for (int j = 0; j < 3; ++j)
                if (sides[other].edge[j] == e)
                    otherSign = sides[other].sign[j];
            trip.emplace_back(t, other, double(sides[t].sign[k] * otherSign));
        }
        trip.emplace_back(t, t, diag);
    }
    Eigen::SparseMatrix<double> m(nt, nt);
    m.setFromTriplets(trip.begin(), trip.end());
    Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> solver(m);
    if (solver.info() != Eigen::Success)
    {
        out.error = "couldn't solve for the twist";
        return out;
    }
    Eigen::VectorXd b(nt);
    for (Index t = 0; t < nt; ++t)
        b[t] = kTwoPi * index[t] - omega[t];
    Eigen::VectorXd y = solver.solve(b);
    out.twist.assign(s.edges.size(), 0.0);
    for (Index t = 0; t < nt; ++t)
        for (int k = 0; k < 3; ++k)
            if (sides[t].edge[k] != kInvalid)
                out.twist[sides[t].edge[k]] += sides[t].sign[k] * y[t];
    return out;
}

std::vector<glm::dvec3> propagateField(const Surface& s, const std::vector<double>& twist)
{
    LILA_ZONE();

    const Index n = s.vertexCount();
    std::vector<double> theta = transportAngles(s);
    for (size_t e = 0; e < theta.size() && e < twist.size(); ++e)
        theta[e] += twist[e];

    // Features pull the turn of each component: the mean of where they want the field against where it is.
    std::vector<double> want = fieldAngles(s, s.featureDir);
    std::vector<double> phi(n, 0.0);
    std::vector<uint8_t> seen(n, 0);
    std::vector<Index> members;
    for (Index root = 0; root < n; ++root)
    {
        if (seen[root])
            continue;
        members.clear();
        std::queue<Index> todo;
        todo.push(root);
        seen[root] = 1;
        while (!todo.empty())
        {
            Index a = todo.front();
            todo.pop();
            members.push_back(a);
            for (Index k = s.adjStart[a]; k < s.adjStart[a + 1]; ++k)
            {
                Index b = s.adj[k];
                if (seen[b])
                    continue;
                Index e = s.adjEdge[k];
                phi[b] = phi[a] + (s.edges[e][0] == a ? theta[e] : -theta[e]);
                seen[b] = 1;
                todo.push(b);
            }
        }
        std::complex<double> pull(0.0);
        for (Index v : members)
            if (s.feature[v] == FeatureKind::Line)
                pull += std::polar(s.area[v], want[v] - phi[v]);
        double turn = std::abs(pull) > 0.0 ? std::arg(pull) : 0.0;
        for (Index v : members)
            phi[v] += turn;
    }
    return fieldFromAngles(s, phi);
}
} // namespace lila::retopo::detail
