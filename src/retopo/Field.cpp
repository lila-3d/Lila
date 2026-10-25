#include "retopo/Internal.h"

#include "core/Profile.h"

#include <Eigen/Sparse>
#include <Eigen/SparseCholesky>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <numeric>

namespace lila::retopo::detail
{
namespace
{
struct Frame
{
    glm::dvec3 e1, e2;
};

std::vector<Frame> frames(const Surface& s)
{
    std::vector<Frame> f(s.vertexCount());
    for (Index v = 0; v < s.vertexCount(); ++v)
    {
        f[v].e1 = anyTangent(s.normal[v]);
        f[v].e2 = glm::cross(s.normal[v], f[v].e1);
    }
    return f;
}

double angleIn(const Frame& f, const glm::dvec3& d) { return std::atan2(glm::dot(d, f.e2), glm::dot(d, f.e1)); }

/** Cotangent weights per edge, clamped so slivers from decimation can't dominate. */
std::vector<double> edgeWeights(const Surface& s)
{
    std::vector<double> w(s.edges.size(), 0.0);
    auto edgeOf = [&](Index a, Index b) {
        for (Index k = s.adjStart[a]; k < s.adjStart[a + 1]; ++k)
            if (s.adj[k] == b)
                return s.adjEdge[k];
        return kInvalid;
    };
    for (const auto& t : s.tris)
        for (int k = 0; k < 3; ++k)
        {
            const glm::dvec3& p = s.pos[t[k]];
            glm::dvec3 a = s.pos[t[(k + 1) % 3]] - p, b = s.pos[t[(k + 2) % 3]] - p;
            double sn = glm::length(glm::cross(a, b));
            double cot = sn > 1e-300 ? glm::dot(a, b) / sn : 0.0;
            Index e = edgeOf(t[(k + 1) % 3], t[(k + 2) % 3]);
            if (e != kInvalid)
                w[e] += 0.5 * cot;
        }
    for (double& x : w)
        x = std::clamp(x, 0.02, 10.0);
    return w;
}

/** Union-find root per vertex over the surface edges. */
std::vector<Index> components(const Surface& s)
{
    std::vector<Index> parent(s.vertexCount());
    std::iota(parent.begin(), parent.end(), Index(0));
    auto find = [&](Index x) {
        while (parent[x] != x)
            x = parent[x] = parent[parent[x]];
        return x;
    };
    for (const auto& e : s.edges)
    {
        Index a = find(e[0]), b = find(e[1]);
        if (a != b)
            parent[std::max(a, b)] = std::min(a, b);
    }
    for (Index v = 0; v < s.vertexCount(); ++v)
        parent[v] = find(v);
    return parent;
}

glm::dvec3 rotate90(const glm::dvec3& q, const glm::dvec3& n) { return glm::cross(n, q); }

/** q1's rotation by a multiple of 90 degrees closest to q0. */
glm::dvec3 matchRotation(const glm::dvec3& q0, const glm::dvec3& q1, const glm::dvec3& n1)
{
    glm::dvec3 t1 = rotate90(q1, n1);
    double a = glm::dot(q0, q1), b = glm::dot(q0, t1);
    if (std::abs(a) >= std::abs(b))
        return a >= 0.0 ? q1 : -q1;
    return b >= 0.0 ? t1 : -t1;
}

double distToSegment(const glm::dvec3& p, const glm::dvec3& a, const glm::dvec3& b, glm::dvec3& dir)
{
    glm::dvec3 ab = b - a;
    double len2 = glm::dot(ab, ab);
    double t = len2 > 0.0 ? std::clamp(glm::dot(p - a, ab) / len2, 0.0, 1.0) : 0.0;
    dir = len2 > 0.0 ? ab / std::sqrt(len2) : glm::dvec3(0.0);
    return glm::length(p - (a + ab * t));
}
} // namespace

Curvature estimateCurvature(const Surface& s)
{
    LILA_ZONE();

    const Index n = s.vertexCount();
    std::vector<glm::dmat3> tensor(n, glm::dmat3(0.0));
    for (size_t e = 0; e < s.edges.size(); ++e)
    {
        if (s.edgeTriCount[e] != 2)
            continue;
        Index a = s.edges[e][0], b = s.edges[e][1];
        Index t0 = s.edgeTris[e][0], t1 = s.edgeTris[e][1];
        glm::dvec3 d = s.pos[b] - s.pos[a];
        double len = glm::length(d);
        if (len <= 0.0)
            continue;
        d /= len;
        // Orient the edge as t0 runs it, so convex and concave creases get opposite signs.
        const auto& f = s.tris[t0];
        bool forward = (f[0] == a && f[1] == b) || (f[1] == a && f[2] == b) || (f[2] == a && f[0] == b);
        const glm::dvec3 &n0 = s.triNormal[t0], &n1 = s.triNormal[t1];
        double sn = glm::dot(glm::cross(n0, n1), forward ? d : -d);
        double beta = std::atan2(sn, glm::dot(n0, n1));
        glm::dmat3 m = glm::outerProduct(d, d) * (0.5 * beta * len);
        tensor[a] += m;
        tensor[b] += m;
    }
    // One ring of smoothing takes out the noise of a single edge.
    std::vector<glm::dmat3> smooth(n);
    std::vector<double> ringArea(n);
    tbb::parallel_for(Index(0), n, [&](Index v) {
        glm::dmat3 sum = tensor[v];
        double ar = s.area[v];
        for (Index k = s.adjStart[v]; k < s.adjStart[v + 1]; ++k)
        {
            sum += tensor[s.adj[k]];
            ar += s.area[s.adj[k]];
        }
        smooth[v] = sum;
        ringArea[v] = ar;
    });
    Curvature c;
    c.maxDir.resize(n);
    c.anisotropy.resize(n);
    c.magnitude.resize(n);
    tbb::parallel_for(Index(0), n, [&](Index v) {
        glm::dvec3 e1 = anyTangent(s.normal[v]), e2 = glm::cross(s.normal[v], e1);
        const glm::dmat3& T = smooth[v];
        double inv = ringArea[v] > 0.0 ? 1.0 / ringArea[v] : 0.0;
        double a = glm::dot(e1, T * e1) * inv, b = glm::dot(e1, T * e2) * inv, cc = glm::dot(e2, T * e2) * inv;
        double mean = 0.5 * (a + cc), r = std::sqrt(0.25 * (a - cc) * (a - cc) + b * b);
        double l1 = mean + r, l2 = mean - r;
        double alpha = 0.5 * std::atan2(2.0 * b, a - cc);
        c.maxDir[v] = std::cos(alpha) * e1 + std::sin(alpha) * e2;
        c.anisotropy[v] = 2.0 * r;
        c.magnitude[v] = std::sqrt(l1 * l1 + l2 * l2);
    });
    return c;
}

std::vector<glm::dvec3> solveCrossField(const Surface& s, const FieldOptions& opts)
{
    LILA_ZONE();

    using SpMat = Eigen::SparseMatrix<double>;
    const Index n = s.vertexCount();
    std::vector<Frame> fr = frames(s);
    const double scale = std::max(opts.scale, 1e-12);

    // Hard constraints: feature lines and guides, as unit 4th powers in the vertex frame.
    std::vector<uint8_t> fixed(n, 0);
    std::vector<std::complex<double>> fixedValue(n);
    auto constrain = [&](Index v, glm::dvec3 dir) {
        dir -= s.normal[v] * glm::dot(s.normal[v], dir);
        if (glm::length(dir) < 1e-9)
            return;
        fixed[v] = 1;
        fixedValue[v] = std::polar(1.0, 4.0 * angleIn(fr[v], dir));
    };
    for (Index v = 0; v < n; ++v)
        if (s.feature[v] == FeatureKind::Line)
            constrain(v, s.featureDir[v]);
    if (!opts.guides.empty())
        for (Index v = 0; v < n; ++v)
        {
            if (s.feature[v] != FeatureKind::None)
                continue;
            double best = 0.35 * scale;
            glm::dvec3 bestDir(0.0);
            for (const auto& g : opts.guides)
                for (size_t i = 0; i + 1 < g.size(); ++i)
                {
                    glm::dvec3 dir;
                    double d = distToSegment(s.pos[v], g[i], g[i + 1], dir);
                    if (d < best)
                    {
                        best = d;
                        bestDir = dir;
                    }
                }
            if (glm::length(bestDir) > 0.0)
                constrain(v, bestDir);
        }

    // Curvature alignment, weighted by how clearly the surface bends one way.
    Curvature curv = estimateCurvature(s);
    std::vector<double> align(n, 0.0);
    std::vector<std::complex<double>> alignValue(n);
    for (Index v = 0; v < n; ++v)
    {
        double g = std::clamp(curv.anisotropy[v] * scale * 2.0, 0.0, 1.0) * opts.curvatureWeight;
        align[v] = g * s.area[v] * 10.0 / (scale * scale);
        alignValue[v] = std::polar(1.0, 4.0 * angleIn(fr[v], curv.maxDir[v]));
    }

    // Soft creases (a bevel's segment edges, too gentle to be features) pull harder than curvature, which faceting blurs.
    if (opts.softCreaseAngle > 0.0)
    {
        const double minAngle = glm::radians(opts.softCreaseAngle);
        std::vector<double> bend(s.edges.size(), 0.0);
        for (size_t e = 0; e < s.edges.size(); ++e)
            if (s.edgeTriCount[e] == 2)
                bend[e] = std::acos(std::clamp(glm::dot(s.triNormal[s.edgeTris[e][0]], s.triNormal[s.edgeTris[e][1]]), -1.0, 1.0));
        // Per triangle, its edges' summed bend, so an edge's neighbours' mean comes cheaply.
        std::vector<double> triBend(s.tris.size(), 0.0);
        for (size_t e = 0; e < s.edges.size(); ++e)
            for (int k = 0; k < std::min<int>(2, s.edgeTriCount[e]); ++k)
                triBend[s.edgeTris[e][k]] += bend[e];
        std::vector<std::complex<double>> pull(n, 0.0);
        std::vector<double> weight(n, 0.0);
        for (size_t e = 0; e < s.edges.size(); ++e)
        {
            if (s.edgeTriCount[e] != 2 || s.edgeFeature[e])
                continue;
            double angle = bend[e];
            if (angle < minAngle)
                continue;
            // A crease concentrates its bend on the edge; on a faceted or noisy surface every edge bends alike.
            double around = (triBend[s.edgeTris[e][0]] + triBend[s.edgeTris[e][1]] - 2.0 * angle) / 4.0;
            if (angle < 2.0 * around)
                continue;
            glm::dvec3 d = s.pos[s.edges[e][1]] - s.pos[s.edges[e][0]];
            for (Index v : s.edges[e])
            {
                pull[v] += angle * std::polar(1.0, 4.0 * angleIn(fr[v], d));
                weight[v] += angle;
            }
        }
        for (Index v = 0; v < n; ++v)
        {
            if (weight[v] <= 0.0 || std::abs(pull[v]) < 1e-12)
                continue;
            double g = 2.0 * std::clamp(weight[v] / (4.0 * minAngle), 0.0, 1.0);
            align[v] = std::max(align[v], g * s.area[v] * 10.0 / (scale * scale));
            alignValue[v] = pull[v] / std::abs(pull[v]);
        }
    }

    std::vector<Index> unknown(n, kInvalid);
    Index free = 0;
    for (Index v = 0; v < n; ++v)
        if (!fixed[v])
            unknown[v] = free++;
    std::vector<std::complex<double>> u(n);
    for (Index v = 0; v < n; ++v)
        u[v] = fixed[v] ? fixedValue[v] : std::complex<double>(0.0);
    if (free == 0)
    {
        std::vector<glm::dvec3> q(n);
        for (Index v = 0; v < n; ++v)
        {
            double phi = std::arg(u[v]) / 4.0;
            q[v] = std::cos(phi) * fr[v].e1 + std::sin(phi) * fr[v].e2;
        }
        return q;
    }

    // Transport per edge: a direction at a reads as rotated by r[e] at b.
    std::vector<double> w = edgeWeights(s);
    std::vector<double> theta = transportAngles(s);
    std::vector<std::complex<double>> r(s.edges.size());
    for (size_t e = 0; e < s.edges.size(); ++e)
        r[e] = std::polar(1.0, theta[e] + (e < opts.twist.size() ? opts.twist[e] : 0.0));

    const double mu = 1.0 / (4.0 * scale * scale);
    auto assemble = [&](double inertia, bool withAlign) {
        std::vector<Eigen::Triplet<double>> trip;
        trip.reserve(s.edges.size() * 8 + free * 2);
        std::vector<double> diag(n, 0.0);
        for (size_t e = 0; e < s.edges.size(); ++e)
        {
            Index a = s.edges[e][0], b = s.edges[e][1];
            diag[a] += w[e];
            diag[b] += w[e];
            if (unknown[a] == kInvalid || unknown[b] == kInvalid)
                continue;
            // Block at row b, column a is the real form of -w r.
            double x = -w[e] * r[e].real(), y = -w[e] * r[e].imag();
            Index ra = unknown[a] * 2, rb = unknown[b] * 2;
            trip.emplace_back(rb, ra, x);
            trip.emplace_back(rb, ra + 1, -y);
            trip.emplace_back(rb + 1, ra, y);
            trip.emplace_back(rb + 1, ra + 1, x);
            trip.emplace_back(ra, rb, x);
            trip.emplace_back(ra, rb + 1, y);
            trip.emplace_back(ra + 1, rb, -y);
            trip.emplace_back(ra + 1, rb + 1, x);
        }
        for (Index v = 0; v < n; ++v)
            if (unknown[v] != kInvalid)
            {
                double d = diag[v] + inertia * s.area[v] + (withAlign ? align[v] : 0.0);
                trip.emplace_back(unknown[v] * 2, unknown[v] * 2, d);
                trip.emplace_back(unknown[v] * 2 + 1, unknown[v] * 2 + 1, d);
            }
        SpMat m(free * 2, free * 2);
        m.setFromTriplets(trip.begin(), trip.end());
        return m;
    };
    // Constant right-hand side from fixed neighbours: -A_fc u_c.
    std::vector<std::complex<double>> boundaryRhs(n, 0.0);
    for (size_t e = 0; e < s.edges.size(); ++e)
    {
        Index a = s.edges[e][0], b = s.edges[e][1];
        if (unknown[a] != kInvalid && unknown[b] == kInvalid)
            boundaryRhs[a] += w[e] * std::conj(r[e]) * u[b];
        else if (unknown[b] != kInvalid && unknown[a] == kInvalid)
            boundaryRhs[b] += w[e] * r[e] * u[a];
    }

    auto unpack = [&](const Eigen::VectorXd& x) {
        for (Index v = 0; v < n; ++v)
            if (unknown[v] != kInvalid)
                u[v] = {x[unknown[v] * 2], x[unknown[v] * 2 + 1]};
    };
    auto normalizeEach = [&]() {
        for (Index v = 0; v < n; ++v)
            if (unknown[v] != kInvalid)
            {
                double m = std::abs(u[v]);
                u[v] = m > 1e-300 ? u[v] / m : std::complex<double>(1.0, 0.0);
            }
    };

    bool anyFixed = free < n;
    if (!anyFixed)
    {
        // No constraints: the smoothest field is the smallest eigenvector (Knoppel et al.), by inverse iteration.
        SpMat a0 = assemble(1e-6 / (scale * scale), false);
        Eigen::SimplicialLDLT<SpMat> solver(a0);
        if (solver.info() != Eigen::Success)
            return std::vector<glm::dvec3>(n, glm::dvec3(1, 0, 0));
        std::vector<Index> comp = components(s);
        for (Index v = 0; v < n; ++v)
        {
            // Deterministic start that every component sees differently from the trivial field.
            double h = std::sin(double(v) * 12.9898 + 78.233) * 43758.5453;
            u[v] = std::polar(1.0, (h - std::floor(h)) * 6.283185307179586);
        }
        Eigen::VectorXd b(free * 2);
        for (int it = 0; it < 12; ++it)
        {
            for (Index v = 0; v < n; ++v)
            {
                b[unknown[v] * 2] = s.area[v] * u[v].real();
                b[unknown[v] * 2 + 1] = s.area[v] * u[v].imag();
            }
            unpack(solver.solve(b));
            // Normalize per component so small parts don't vanish next to big ones.
            std::vector<double> norm(n, 0.0);
            for (Index v = 0; v < n; ++v)
                norm[comp[v]] += s.area[v] * std::norm(u[v]);
            for (Index v = 0; v < n; ++v)
                if (norm[comp[v]] > 0.0)
                    u[v] /= std::sqrt(norm[comp[v]]);
        }
        normalizeEach();
    }

    // Implicit smoothing of the unit field toward curvature directions, with the constraints held.
    SpMat a1 = assemble(mu, true);
    Eigen::SimplicialLDLT<SpMat> solver(a1);
    if (solver.info() != Eigen::Success)
        return std::vector<glm::dvec3>(n, glm::dvec3(1, 0, 0));
    Eigen::VectorXd b(free * 2);
    for (int it = 0; it < 10; ++it)
    {
        Budget::checkpoint();
        for (Index v = 0; v < n; ++v)
            if (unknown[v] != kInvalid)
            {
                std::complex<double> rhs = mu * s.area[v] * u[v] + align[v] * alignValue[v] + boundaryRhs[v];
                b[unknown[v] * 2] = rhs.real();
                b[unknown[v] * 2 + 1] = rhs.imag();
            }
        unpack(solver.solve(b));
        normalizeEach();
    }

    std::vector<glm::dvec3> q(n);
    for (Index v = 0; v < n; ++v)
    {
        double phi = std::arg(u[v]) / 4.0;
        q[v] = std::cos(phi) * fr[v].e1 + std::sin(phi) * fr[v].e2;
    }
    return q;
}

std::vector<double> transportAngles(const Surface& s)
{
    std::vector<Frame> fr = frames(s);
    std::vector<double> theta(s.edges.size());
    for (size_t e = 0; e < s.edges.size(); ++e)
    {
        Index a = s.edges[e][0], b = s.edges[e][1];
        glm::dvec3 d = s.pos[b] - s.pos[a];
        theta[e] = 4.0 * (angleIn(fr[b], d) - angleIn(fr[a], d));
    }
    return theta;
}

std::vector<double> fieldAngles(const Surface& s, const std::vector<glm::dvec3>& q)
{
    std::vector<Frame> fr = frames(s);
    std::vector<double> phi(s.vertexCount());
    for (Index v = 0; v < s.vertexCount(); ++v)
        phi[v] = 4.0 * angleIn(fr[v], q[v]);
    return phi;
}

std::vector<glm::dvec3> fieldFromAngles(const Surface& s, const std::vector<double>& phi)
{
    std::vector<Frame> fr = frames(s);
    std::vector<glm::dvec3> q(s.vertexCount());
    for (Index v = 0; v < s.vertexCount(); ++v)
        q[v] = std::cos(phi[v] / 4.0) * fr[v].e1 + std::sin(phi[v] / 4.0) * fr[v].e2;
    return q;
}

// Position field: Instant Meshes style lattice smoothing on a multiresolution hierarchy.

namespace
{
struct Level
{
    std::vector<glm::dvec3> v, n, q;
    std::vector<double> a, s;
    std::vector<Index> adjStart, adj;
    std::vector<FeatureKind> kind;
    std::vector<glm::dvec3> cpos, cdir;
    std::vector<Index> parent; // into the next coarser level
    Index size() const { return Index(v.size()); }
};

glm::dvec3 middlePoint(const glm::dvec3& p0, const glm::dvec3& n0, const glm::dvec3& p1, const glm::dvec3& n1)
{
    // Point closest to both, on both tangent planes (Lagrange multipliers); the epsilon covers parallel planes.
    double n0p0 = glm::dot(n0, p0), n0p1 = glm::dot(n0, p1), n1p0 = glm::dot(n1, p0), n1p1 = glm::dot(n1, p1);
    double n0n1 = glm::dot(n0, n1);
    double denom = 1.0 / (1.0 - n0n1 * n0n1 + 1e-4);
    double l0 = 2.0 * (n0p1 - n0p0 - n0n1 * (n1p0 - n1p1)) * denom;
    double l1 = 2.0 * (n1p0 - n1p1 - n0n1 * (n0p1 - n0p0)) * denom;
    return 0.5 * (p0 + p1) - 0.25 * (n0 * l0 + n1 * l1);
}

glm::dvec3 latticeFloor(const glm::dvec3& o, const glm::dvec3& q, const glm::dvec3& n, const glm::dvec3& p, double s)
{
    glm::dvec3 t = glm::cross(n, q), d = p - o;
    return o + q * (std::floor(glm::dot(q, d) / s) * s) + t * (std::floor(glm::dot(t, d) / s) * s);
}

glm::dvec3 latticeRound(const glm::dvec3& o, const glm::dvec3& q, const glm::dvec3& n, const glm::dvec3& p, double s)
{
    glm::dvec3 t = glm::cross(n, q), d = p - o;
    return o + q * (std::round(glm::dot(q, d) / s) * s) + t * (std::round(glm::dot(t, d) / s) * s);
}

void applyConstraint(const Level& L, Index i, glm::dvec3& o)
{
    if (L.kind[i] == FeatureKind::Corner)
        o = L.cpos[i];
    else if (L.kind[i] == FeatureKind::Line)
    {
        glm::dvec3 d = L.cdir[i];
        glm::dvec3 c = L.cpos[i];
        o = c + d * glm::dot(o - c, d);
        o -= L.n[i] * glm::dot(L.n[i], o - L.v[i]);
    }
}

Level coarsen(Level& fine)
{
    LILA_ZONE();

    const Index n = fine.size();
    struct Pair
    {
        double score;
        Index a, b;
    };
    std::vector<Pair> pairs;
    for (Index i = 0; i < n; ++i)
        for (Index k = fine.adjStart[i]; k < fine.adjStart[i + 1]; ++k)
        {
            Index j = fine.adj[k];
            if (j <= i)
                continue;
            if (fine.kind[i] == FeatureKind::Corner && fine.kind[j] == FeatureKind::Corner)
                continue;
            double ratio = std::min(fine.a[i], fine.a[j]) / std::max(fine.a[i], fine.a[j]);
            pairs.push_back({glm::dot(fine.n[i], fine.n[j]) * ratio, i, j});
        }
    std::stable_sort(pairs.begin(), pairs.end(), [](const Pair& x, const Pair& y) { return x.score > y.score; });
    fine.parent.assign(n, kInvalid);
    Level c;
    auto addCoarse = [&](Index i, Index j) {
        Index id = c.size();
        double ai = fine.a[i], aj = j != kInvalid ? fine.a[j] : 0.0;
        double sum = std::max(ai + aj, 1e-300);
        glm::dvec3 v = fine.v[i] * ai, nn = fine.n[i] * ai, q = fine.q[i];
        double sc = fine.s[i] * ai;
        if (j != kInvalid)
        {
            v += fine.v[j] * aj;
            nn += fine.n[j] * aj;
            q += matchRotation(fine.q[i], fine.q[j], fine.n[j]);
            sc += fine.s[j] * aj;
        }
        v /= sum;
        double len = glm::length(nn);
        nn = len > 0.0 ? nn / len : fine.n[i];
        q -= nn * glm::dot(nn, q);
        len = glm::length(q);
        q = len > 1e-12 ? q / len : anyTangent(nn);
        c.v.push_back(v);
        c.n.push_back(nn);
        c.q.push_back(q);
        c.a.push_back(ai + aj);
        c.s.push_back(sc / sum);
        // Corners win over lines; a line keeps the child that has it.
        Index src = i;
        if (j != kInvalid && (fine.kind[j] > fine.kind[i] || (fine.kind[j] == fine.kind[i] && aj > ai)))
            src = j;
        c.kind.push_back(fine.kind[src]);
        c.cpos.push_back(fine.cpos[src]);
        glm::dvec3 d = fine.cdir[src] - nn * glm::dot(nn, fine.cdir[src]);
        len = glm::length(d);
        c.cdir.push_back(len > 1e-12 ? d / len : fine.cdir[src]);
        if (c.kind.back() == FeatureKind::Line && len <= 1e-12)
            c.kind.back() = FeatureKind::Corner;
        fine.parent[i] = id;
        if (j != kInvalid)
            fine.parent[j] = id;
    };
    for (const Pair& p : pairs)
        if (fine.parent[p.a] == kInvalid && fine.parent[p.b] == kInvalid)
            addCoarse(p.a, p.b);
    for (Index i = 0; i < n; ++i)
        if (fine.parent[i] == kInvalid)
            addCoarse(i, kInvalid);

    std::vector<uint64_t> links;
    for (Index i = 0; i < n; ++i)
        for (Index k = fine.adjStart[i]; k < fine.adjStart[i + 1]; ++k)
        {
            Index a = fine.parent[i], b = fine.parent[fine.adj[k]];
            if (a != b)
                links.push_back((uint64_t(a) << 32) | b);
        }
    std::sort(links.begin(), links.end());
    links.erase(std::unique(links.begin(), links.end()), links.end());
    c.adjStart.assign(c.size() + 1, 0);
    for (uint64_t l : links)
        ++c.adjStart[(l >> 32) + 1];
    for (Index i = 0; i < c.size(); ++i)
        c.adjStart[i + 1] += c.adjStart[i];
    c.adj.resize(links.size());
    for (size_t k = 0; k < links.size(); ++k)
        c.adj[k] = Index(links[k] & 0xffffffffu);
    return c;
}

void smoothPositions(const Level& L, std::vector<glm::dvec3>& o, int sweeps)
{
    LILA_ZONE();

    for (int it = 0; it < sweeps; ++it, Budget::checkpoint())
        for (Index i = 0; i < L.size(); ++i)
        {
            const glm::dvec3 &vi = L.v[i], &ni = L.n[i], &qi = L.q[i];
            const double si = L.s[i];
            glm::dvec3 sum = o[i];
            double weight = 0.0;
            for (Index k = L.adjStart[i]; k < L.adjStart[i + 1]; ++k)
            {
                Index j = L.adj[k];
                const glm::dvec3 &vj = L.v[j], &nj = L.n[j], &qj = L.q[j];
                const double sj = L.s[j];
                glm::dvec3 ti = glm::cross(ni, qi), tj = glm::cross(nj, qj);
                glm::dvec3 mid = middlePoint(vi, ni, vj, nj);
                glm::dvec3 a0 = latticeFloor(sum, qi, ni, mid, si), a1 = latticeFloor(o[j], qj, nj, mid, sj);
                double best = std::numeric_limits<double>::max();
                glm::dvec3 bi(0.0), bj(0.0);
                for (int x = 0; x < 4; ++x)
                {
                    glm::dvec3 c0 = a0 + (qi * double(x & 1) + ti * double((x >> 1) & 1)) * si;
                    for (int y = 0; y < 4; ++y)
                    {
                        glm::dvec3 c1 = a1 + (qj * double(y & 1) + tj * double((y >> 1) & 1)) * sj;
                        double cost = glm::dot(c0 - c1, c0 - c1);
                        if (cost < best)
                        {
                            best = cost;
                            bi = c0;
                            bj = c1;
                        }
                    }
                }
                // Both lattice points join the running average, so the result stays a lattice point of i.
                sum = bi * weight + bj;
                weight += 1.0;
                sum /= weight;
                sum -= ni * glm::dot(ni, sum - vi);
            }
            if (weight > 0.0)
                o[i] = latticeRound(sum, qi, ni, vi, si);
            applyConstraint(L, i, o[i]);
        }
}
} // namespace

glm::dvec3 compatLatticePair(const glm::dvec3& vi, const glm::dvec3& ni, const glm::dvec3& qi, const glm::dvec3& oi, double si,
                             const glm::dvec3& vj, const glm::dvec3& nj, const glm::dvec3& qj, const glm::dvec3& oj, double sj,
                             glm::dvec3& outJ)
{
    glm::dvec3 ti = glm::cross(ni, qi), tj = glm::cross(nj, qj);
    glm::dvec3 mid = middlePoint(vi, ni, vj, nj);
    glm::dvec3 a0 = latticeFloor(oi, qi, ni, mid, si), a1 = latticeFloor(oj, qj, nj, mid, sj);
    double best = std::numeric_limits<double>::max();
    glm::dvec3 bi(0.0);
    for (int x = 0; x < 4; ++x)
    {
        glm::dvec3 c0 = a0 + (qi * double(x & 1) + ti * double((x >> 1) & 1)) * si;
        for (int y = 0; y < 4; ++y)
        {
            glm::dvec3 c1 = a1 + (qj * double(y & 1) + tj * double((y >> 1) & 1)) * sj;
            double cost = glm::dot(c0 - c1, c0 - c1);
            if (cost < best)
            {
                best = cost;
                bi = c0;
                outJ = c1;
            }
        }
    }
    return bi;
}

glm::dvec3 matchQuarterRotation(const glm::dvec3& q0, const glm::dvec3& q1, const glm::dvec3& n1) { return matchRotation(q0, q1, n1); }

std::vector<glm::dvec3> solvePositionField(const Surface& s, const std::vector<glm::dvec3>& q, const std::vector<double>& scale)
{
    LILA_ZONE();

    std::vector<Level> levels(1);
    Level& L0 = levels[0];
    L0.v = s.pos;
    L0.n = s.normal;
    L0.q = q;
    L0.a = s.area;
    L0.s = scale;
    L0.adjStart = s.adjStart;
    L0.adj = s.adj;
    L0.kind = s.feature;
    L0.cpos = s.pos;
    L0.cdir = s.featureDir;
    while (levels.size() < 24 && levels.back().size() > 64)
    {
        Level c = coarsen(levels.back());
        bool stalled = c.size() > levels.back().size() * 9 / 10;
        levels.push_back(std::move(c));
        if (stalled)
            break;
    }

    std::vector<glm::dvec3> o = levels.back().v;
    for (size_t li = levels.size(); li-- > 0;)
    {
        Level& L = levels[li];
        if (li + 1 < levels.size())
        {
            // Prolong: each vertex takes its parent's lattice, moved into its own tangent plane near itself.
            std::vector<glm::dvec3> fineO(L.size());
            for (Index i = 0; i < L.size(); ++i)
            {
                glm::dvec3 p = o[L.parent[i]];
                p -= L.n[i] * glm::dot(L.n[i], p - L.v[i]);
                fineO[i] = latticeRound(p, L.q[i], L.n[i], L.v[i], L.s[i]);
                applyConstraint(L, i, fineO[i]);
            }
            o = std::move(fineO);
        }
        else
            for (Index i = 0; i < L.size(); ++i)
                applyConstraint(L, i, o[i]);
        smoothPositions(L, o, li == 0 ? 32 : 16);
    }
    return o;
}
} // namespace lila::retopo::detail
