#include "uv/Unwrap.h"

#include "core/Profile.h"
#include "uv/Bff.h"
#include "uv/OptimizeMath.h"

#include <Eigen/Sparse>
#include <Eigen/SparseCholesky>

#include <tbb/parallel_for.h>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

namespace lila::uv
{
namespace
{
using detail::Jac2;
using detail::SpMat;
using detail::Svd2;
using detail::TriFrame;
using detail::Triplet;
using detail::jacobian;
using detail::signedArea2;
using detail::svd2;
using detail::triFrame;

std::vector<TriFrame> frames(const Chart& ch, double scale = 1.0)
{
    std::vector<TriFrame> out(ch.tris.size());
    for (size_t i = 0; i < ch.tris.size(); ++i)
    {
        const auto& t = ch.tris[i];
        out[i] = triFrame(ch.pos[t[0]] * scale, ch.pos[t[1]] * scale, ch.pos[t[2]] * scale);
    }
    return out;
}

double dirichlet(double s1, double s2)
{
    return s1 * s1 + 1.0 / (s1 * s1) + s2 * s2 + 1.0 / (s2 * s2);
}

std::vector<double> distortions(const Chart& ch, const std::vector<TriFrame>& fr, std::span<const glm::dvec2> uv)
{
    std::vector<double> out(ch.tris.size(), 0.0);
    for (size_t i = 0; i < ch.tris.size(); ++i)
    {
        if (fr[i].area <= 0.0)
            continue;
        const auto& t = ch.tris[i];
        Jac2 j = jacobian(fr[i], uv[t[0]], uv[t[1]], uv[t[2]]);
        if (j.det() <= 0.0)
        {
            out[i] = std::numeric_limits<double>::infinity();
            continue;
        }
        Svd2 s = svd2(j);
        out[i] = dirichlet(s.s1, s.s2);
    }
    return out;
}

std::pair<Index, Index> farthestPair(const Chart& ch)
{
    glm::dvec3 lo(std::numeric_limits<double>::max()), hi(-std::numeric_limits<double>::max());
    for (const glm::dvec3& p : ch.pos)
    {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    glm::dvec3 ext = hi - lo;
    int axis = ext.x >= ext.y && ext.x >= ext.z ? 0 : ext.y >= ext.z ? 1 : 2;
    Index a = 0, b = 0;
    for (Index v = 0; v < ch.vertexCount(); ++v)
    {
        if (ch.pos[v][axis] < ch.pos[a][axis])
            a = v;
        if (ch.pos[v][axis] > ch.pos[b][axis])
            b = v;
    }
    // One sweep refines the pair towards the true diameter.
    for (int pass = 0; pass < 2; ++pass)
    {
        Index far = a;
        double best = -1.0;
        for (Index v = 0; v < ch.vertexCount(); ++v)
            if (double d = glm::length(ch.pos[v] - ch.pos[a]); d > best)
            {
                best = d;
                far = v;
            }
        b = far;
        std::swap(a, b);
    }
    return {std::min(a, b), std::max(a, b)};
}

/** Solves M x = rhs over free dofs; dofs with fixed[i] keep x[i]. M is assembled from triplets over all dofs. */
bool solveConstrained(const std::vector<Triplet>& trips, size_t n, std::span<const uint8_t> fixed, Eigen::VectorXd& x,
                      const Eigen::VectorXd* rhsFull = nullptr)
{
    std::vector<Index> freeId(n, kInvalid);
    Index nf = 0;
    for (size_t i = 0; i < n; ++i)
        if (!fixed[i])
            freeId[i] = nf++;
    if (nf == 0)
        return true;
    std::vector<Triplet> ff;
    ff.reserve(trips.size());
    Eigen::VectorXd rhs = Eigen::VectorXd::Zero(nf);
    if (rhsFull)
        for (size_t i = 0; i < n; ++i)
            if (freeId[i] != kInvalid)
                rhs[freeId[i]] = (*rhsFull)[i];
    double diag = 0.0;
    for (const Triplet& t : trips)
    {
        Index r = freeId[t.row()], c = freeId[t.col()];
        if (r == kInvalid)
            continue;
        if (c == kInvalid)
            rhs[r] -= t.value() * x[t.col()];
        else
        {
            ff.push_back(Triplet(int(r), int(c), t.value()));
            if (r == c)
                diag += t.value();
        }
    }
    // A whisper of regularisation keeps vertices only touched by degenerate triangles solvable.
    double eps = 1e-10 * std::max(diag / nf, 1e-30);
    for (Index i = 0; i < nf; ++i)
        ff.push_back(Triplet(int(i), int(i), eps));
    SpMat m(nf, nf);
    m.setFromTriplets(ff.begin(), ff.end());
    Eigen::SimplicialLDLT<SpMat> solver(m);
    if (solver.info() != Eigen::Success)
        return false;
    Eigen::VectorXd sol = solver.solve(rhs);
    if (solver.info() != Eigen::Success || !sol.allFinite())
        return false;
    for (size_t i = 0; i < n; ++i)
        if (freeId[i] != kInvalid)
            x[i] = sol[freeId[i]];
    return true;
}
/** BFF with lengths kept along the boundary; false (chart untouched) when it fails or folds. */
bool flattenBff(Chart& ch)
{
    Chart trial = ch;
    if (!bff(trial).ok || foldedTriangles(trial) > 0)
        return false;
    ch.uv = std::move(trial.uv);
    return true;
}
} // namespace

bool lscm(Chart& ch, bool usePins)
{
    LILA_ZONE();

    const size_t V = ch.vertexCount();
    if (V < 3 || ch.tris.empty())
        return false;
    std::vector<TriFrame> fr = frames(ch);
    std::vector<Triplet> trips;
    trips.reserve(ch.tris.size() * 72);
    for (size_t i = 0; i < ch.tris.size(); ++i)
    {
        const TriFrame& f = fr[i];
        if (f.area <= 0.0)
            continue;
        const auto& t = ch.tris[i];
        // du/dx - dv/dy and du/dy + dv/dx, over dofs u0 u1 u2 v0 v1 v2.
        double r1[6] = {f.g[0].x, f.g[1].x, f.g[2].x, -f.g[0].y, -f.g[1].y, -f.g[2].y};
        double r2[6] = {f.g[0].y, f.g[1].y, f.g[2].y, f.g[0].x, f.g[1].x, f.g[2].x};
        int dof[6];
        for (int j = 0; j < 3; ++j)
        {
            dof[j] = int(t[j]);
            dof[3 + j] = int(V + t[j]);
        }
        for (int r = 0; r < 6; ++r)
            for (int c = 0; c < 6; ++c)
                trips.push_back(Triplet(dof[r], dof[c], f.area * (r1[r] * r1[c] + r2[r] * r2[c])));
    }

    std::vector<uint8_t> fixed(2 * V, 0);
    Eigen::VectorXd x = Eigen::VectorXd::Zero(Eigen::Index(2 * V));
    size_t pins = 0;
    if (usePins)
        for (Index v = 0; v < V; ++v)
            pins += ch.pinned[v] ? 1 : 0;
    if (pins >= 2)
    {
        for (Index v = 0; v < V; ++v)
            if (ch.pinned[v])
            {
                fixed[v] = fixed[V + v] = 1;
                x[v] = ch.uv[v].x;
                x[V + v] = ch.uv[v].y;
            }
    }
    else
    {
        auto [a, b] = farthestPair(ch);
        if (a == b)
            return false;
        fixed[a] = fixed[V + a] = fixed[b] = fixed[V + b] = 1;
        x[b] = glm::length(ch.pos[b] - ch.pos[a]);
    }
    if (!solveConstrained(trips, 2 * V, fixed, x))
        return false;
    for (Index v = 0; v < V; ++v)
        ch.uv[v] = {x[v], x[V + v]};
    return true;
}

bool tutte(Chart& ch)
{
    LILA_ZONE();

    const size_t V = ch.vertexCount();
    auto loops = ch.boundaryLoops();
    if (loops.empty() || loops[0].size() < 3)
        return false;
    const std::vector<Index>& outer = loops[0];
    double total = 0.0;
    std::vector<double> along(outer.size(), 0.0);
    for (size_t i = 0; i < outer.size(); ++i)
    {
        along[i] = total;
        total += glm::length(ch.pos[outer[(i + 1) % outer.size()]] - ch.pos[outer[i]]);
    }
    if (total <= 0.0)
        return false;
    double radius = total / glm::two_pi<double>();
    std::vector<uint8_t> fixed(V, 0);
    Eigen::VectorXd xu = Eigen::VectorXd::Zero(Eigen::Index(V)), xv = xu;
    for (size_t i = 0; i < outer.size(); ++i)
    {
        double a = along[i] / total * glm::two_pi<double>();
        fixed[outer[i]] = 1;
        xu[outer[i]] = radius * std::cos(a);
        xv[outer[i]] = radius * std::sin(a);
    }
    std::vector<uint64_t> edges;
    for (const auto& t : ch.tris)
        for (int i = 0; i < 3; ++i)
        {
            Index a = t[i], b = t[(i + 1) % 3];
            edges.push_back((uint64_t(std::min(a, b)) << 32) | std::max(a, b));
        }
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    std::vector<Triplet> trips;
    for (uint64_t e : edges)
    {
        int a = int(e >> 32), b = int(e & 0xffffffffu);
        trips.push_back(Triplet(a, a, 1.0));
        trips.push_back(Triplet(b, b, 1.0));
        trips.push_back(Triplet(a, b, -1.0));
        trips.push_back(Triplet(b, a, -1.0));
    }
    if (!solveConstrained(trips, V, fixed, xu) || !solveConstrained(trips, V, fixed, xv))
        return false;
    for (Index v = 0; v < V; ++v)
        ch.uv[v] = {xu[v], xv[v]};
    return true;
}

size_t foldedTriangles(const Chart& ch)
{
    double total = ch.areaUv();
    double sign = total < 0.0 ? -1.0 : 1.0;
    size_t n = 0;
    std::vector<TriFrame> fr = frames(ch);
    for (size_t i = 0; i < ch.tris.size(); ++i)
    {
        if (fr[i].area <= 0.0)
            continue;
        const auto& t = ch.tris[i];
        if (sign * signedArea2(ch.uv[t[0]], ch.uv[t[1]], ch.uv[t[2]]) <= 0.0)
            ++n;
    }
    return n;
}

void matchSurfaceArea(Chart& ch)
{
    double a3 = ch.surfaceArea(), au = std::abs(ch.areaUv());
    if (a3 <= 0.0 || au <= 0.0)
        return;
    double s = std::sqrt(a3 / au);
    for (glm::dvec2& p : ch.uv)
        p *= s;
}

double chartDistortion(const Chart& ch)
{
    double a3 = ch.area3d(), au = std::abs(ch.areaUv());
    if (a3 <= 0.0 || au <= 0.0)
        return std::numeric_limits<double>::infinity();
    std::vector<TriFrame> fr = frames(ch, 1.0 / std::sqrt(a3));
    std::vector<glm::dvec2> uv(ch.uv.begin(), ch.uv.end());
    double s = 1.0 / std::sqrt(au);
    double flip = ch.areaUv() < 0.0 ? -1.0 : 1.0;
    for (glm::dvec2& p : uv)
        p = glm::dvec2(p.x * flip, p.y) * s;
    std::vector<double> d = distortions(ch, fr, uv);
    double e = 0.0, w = 0.0;
    for (size_t i = 0; i < d.size(); ++i)
    {
        e += fr[i].area * d[i];
        w += fr[i].area;
    }
    return w > 0.0 ? e / w / 4.0 - 1.0 : 0.0;
}

int slim(Chart& ch, int iterations, bool usePins)
{
    LILA_ZONE();

    const size_t V = ch.vertexCount();
    const size_t T = ch.tris.size();
    if (V < 3 || T == 0)
        return 0;
    double a3 = ch.area3d(), au = std::abs(ch.areaUv());
    if (a3 <= 0.0 || au <= 0.0)
        return -1;

    bool pinned = false;
    if (usePins)
        for (uint8_t p : ch.pinned)
            pinned |= p != 0;

    // Work at unit area so energies, weights and tolerances are scale free.
    std::vector<TriFrame> fr = frames(ch, 1.0 / std::sqrt(a3));
    const double su = 1.0 / std::sqrt(au);
    const bool mirror = ch.areaUv() < 0.0 && !pinned;
    Eigen::VectorXd x(Eigen::Index(2 * V));
    for (Index v = 0; v < V; ++v)
    {
        x[v] = ch.uv[v].x * su * (mirror ? -1.0 : 1.0);
        x[V + v] = ch.uv[v].y * su;
    }
    Eigen::VectorXd pinX = x;

    auto uvAt = [&](const Eigen::VectorXd& s, Index v) { return glm::dvec2(s[v], s[V + v]); };
    for (size_t i = 0; i < T; ++i)
        if (fr[i].area > 0.0)
        {
            const auto& t = ch.tris[i];
            if (signedArea2(uvAt(x, t[0]), uvAt(x, t[1]), uvAt(x, t[2])) <= 0.0)
                return -1;
        }

    const double pinWeight = 1e5;
    auto energy = [&](const Eigen::VectorXd& s) {
        double e = 0.0;
        for (size_t i = 0; i < T; ++i)
        {
            if (fr[i].area <= 0.0)
                continue;
            const auto& t = ch.tris[i];
            Jac2 j = jacobian(fr[i], uvAt(s, t[0]), uvAt(s, t[1]), uvAt(s, t[2]));
            if (j.det() <= 0.0)
                return std::numeric_limits<double>::infinity();
            Svd2 d = svd2(j);
            e += fr[i].area * dirichlet(d.s1, d.s2);
        }
        if (pinned)
            for (Index v = 0; v < V; ++v)
                if (ch.pinned[v])
                {
                    double du = s[v] - pinX[v], dv = s[V + v] - pinX[V + v];
                    e += pinWeight * (du * du + dv * dv);
                }
        return e;
    };

    // Big charts drop the anisotropic coupling between u and v: half the unknowns per solve and one
    // factorisation serves both, at the price of a few more iterations.
    const bool coupled = V <= 10000;
    const int bs = coupled ? 6 : 3;
    const size_t n = coupled ? 2 * V : V;
    auto dof = [&](const std::array<Index, 3>& t, int k) { return int(k < 3 ? t[k] : V + t[k - 3]); };

    // Fixed sparsity: one bs x bs block per triangle, plus the diagonal.
    SpMat m(static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(n));
    {
        std::vector<Triplet> trips;
        trips.reserve(T * bs * bs + n);
        for (size_t i = 0; i < T; ++i)
            for (int r = 0; r < bs; ++r)
                for (int c = 0; c < bs; ++c)
                    trips.push_back(Triplet(dof(ch.tris[i], r), dof(ch.tris[i], c), 0.0));
        for (size_t i = 0; i < n; ++i)
            trips.push_back(Triplet(int(i), int(i), 0.0));
        m.setFromTriplets(trips.begin(), trips.end());
        m.makeCompressed();
    }
    auto slot = [&](int row, int col) {
        const int* inner = m.innerIndexPtr();
        int lo = m.outerIndexPtr()[col], hi = m.outerIndexPtr()[col + 1];
        return int(std::lower_bound(inner + lo, inner + hi, row) - inner);
    };
    const size_t bb = size_t(bs) * bs;
    std::vector<int> slots(T * bb);
    std::vector<int> diagSlot(n);
    tbb::parallel_for(size_t(0), T, [&](size_t i) {
        for (int r = 0; r < bs; ++r)
            for (int c = 0; c < bs; ++c)
                slots[i * bb + r * bs + c] = slot(dof(ch.tris[i], r), dof(ch.tris[i], c));
    });
    for (size_t i = 0; i < n; ++i)
        diagSlot[i] = slot(int(i), int(i));

    Eigen::SimplicialLDLT<SpMat> solver;
    solver.analyzePattern(m);

    double e0 = energy(x);
    int done = 0;
    std::vector<double> local(T * bb);
    std::vector<double> localRhs(T * 6);
    for (int it = 0; it < iterations; ++it)
    {
        // Local step: per-triangle proxy rotation and weights.
        tbb::parallel_for(size_t(0), T, [&](size_t i) {
            double* K = &local[i * bb];
            double* B = &localRhs[i * 6];
            std::fill(K, K + bb, 0.0);
            std::fill(B, B + 6, 0.0);
            const TriFrame& f = fr[i];
            if (f.area <= 0.0)
                return;
            const auto& t = ch.tris[i];
            Jac2 j = jacobian(f, uvAt(x, t[0]), uvAt(x, t[1]), uvAt(x, t[2]));
            Svd2 s = svd2(j);
            double s1 = std::max(s.s1, 1e-8), s2 = std::max(s.s2, 1e-8);
            auto weight = [](double sv) {
                if (std::abs(sv - 1.0) < 1e-6)
                    return 2.0;
                return std::sqrt((sv - 1.0 / (sv * sv * sv)) / (sv - 1.0));
            };
            double w1 = weight(s1), w2 = weight(s2);
            double ang = s.phi + s.theta;
            double R[2][2] = {{std::cos(ang), -std::sin(ang)}, {std::sin(ang), std::cos(ang)}};
            if (!coupled)
            {
                // Isotropic proxy: w^2 |grad u - R0|^2 + w^2 |grad v - R1|^2.
                double w = f.area * 0.5 * (w1 * w1 + w2 * w2);
                for (int p = 0; p < 3; ++p)
                {
                    B[p] += w * (f.g[p].x * R[0][0] + f.g[p].y * R[0][1]);
                    B[3 + p] += w * (f.g[p].x * R[1][0] + f.g[p].y * R[1][1]);
                    for (int q = 0; q < 3; ++q)
                        K[p * 3 + q] = w * glm::dot(f.g[p], f.g[q]);
                }
                return;
            }
            double cp = std::cos(s.phi), sp = std::sin(s.phi);
            // W = U diag(w) U^T with U = Rot(phi); R = Rot(phi + theta).
            double W[2][2] = {{w1 * cp * cp + w2 * sp * sp, (w1 - w2) * cp * sp}, {(w1 - w2) * cp * sp, w1 * sp * sp + w2 * cp * cp}};
            for (int r = 0; r < 2; ++r)
                for (int c = 0; c < 2; ++c)
                {
                    double a[6];
                    for (int k = 0; k < 3; ++k)
                    {
                        a[k] = W[r][0] * f.g[k][c];
                        a[3 + k] = W[r][1] * f.g[k][c];
                    }
                    double b = W[r][0] * R[0][c] + W[r][1] * R[1][c];
                    for (int p = 0; p < 6; ++p)
                    {
                        B[p] += f.area * b * a[p];
                        for (int q = 0; q < 6; ++q)
                            K[p * 6 + q] += f.area * a[p] * a[q];
                    }
                }
        });

        // Global step.
        double* val = m.valuePtr();
        std::fill(val, val + m.nonZeros(), 0.0);
        Eigen::VectorXd rhs = Eigen::VectorXd::Zero(Eigen::Index(2 * V));
        for (size_t i = 0; i < T; ++i)
        {
            const auto& t = ch.tris[i];
            const double* K = &local[i * bb];
            for (size_t k = 0; k < bb; ++k)
                val[slots[i * bb + k]] += K[k];
            for (int p = 0; p < 6; ++p)
                rhs[Eigen::Index(p < 3 ? t[p] : V + t[p - 3])] += localRhs[i * 6 + p];
        }
        double trace = 0.0;
        for (size_t i = 0; i < n; ++i)
            trace += val[diagSlot[i]];
        // Proximal term: removes the translation null space without biasing the shape.
        double prox = 1e-8 * trace / double(n);
        for (size_t i = 0; i < n; ++i)
            val[diagSlot[i]] += prox;
        for (size_t i = 0; i < 2 * V; ++i)
            rhs[Eigen::Index(i)] += prox * x[Eigen::Index(i)];
        if (pinned)
            for (Index v = 0; v < V; ++v)
                if (ch.pinned[v])
                {
                    if (coupled)
                    {
                        val[diagSlot[v]] += pinWeight;
                        val[diagSlot[V + v]] += pinWeight;
                    }
                    else
                        val[diagSlot[v]] += pinWeight;
                    rhs[Eigen::Index(v)] += pinWeight * pinX[Eigen::Index(v)];
                    rhs[Eigen::Index(V + v)] += pinWeight * pinX[Eigen::Index(V + v)];
                }
        solver.factorize(m);
        if (solver.info() != Eigen::Success)
            break;
        Eigen::VectorXd target(Eigen::Index(2 * V));
        if (coupled)
            target = solver.solve(rhs);
        else
        {
            target.head(Eigen::Index(V)) = solver.solve(rhs.head(Eigen::Index(V)));
            target.tail(Eigen::Index(V)) = solver.solve(rhs.tail(Eigen::Index(V)));
        }
        if (!target.allFinite())
            break;
        Eigen::VectorXd dir = target - x;

        // Largest step before any triangle flips.
        double tMax = 1.0;
        for (size_t i = 0; i < T; ++i)
        {
            if (fr[i].area <= 0.0)
                continue;
            const auto& t = ch.tris[i];
            Jac2 j0 = jacobian(fr[i], uvAt(x, t[0]), uvAt(x, t[1]), uvAt(x, t[2]));
            Jac2 jd = jacobian(fr[i], uvAt(dir, t[0]), uvAt(dir, t[1]), uvAt(dir, t[2]));
            double qa = jd.det();
            double qb = j0.a * jd.d + j0.d * jd.a - j0.b * jd.c - j0.c * jd.b;
            double qc = j0.det();
            double root = std::numeric_limits<double>::infinity();
            if (std::abs(qa) < 1e-14)
            {
                if (qb < 0.0)
                    root = -qc / qb;
            }
            else
            {
                double disc = qb * qb - 4.0 * qa * qc;
                if (disc >= 0.0)
                {
                    double sq = std::sqrt(disc);
                    double r1 = (-qb - sq) / (2.0 * qa), r2 = (-qb + sq) / (2.0 * qa);
                    if (r1 > 0.0)
                        root = std::min(root, r1);
                    if (r2 > 0.0)
                        root = std::min(root, r2);
                }
            }
            tMax = std::min(tMax, 0.8 * root);
        }
        double step = std::min(1.0, tMax);
        Eigen::VectorXd trial = x + step * dir;
        double e1 = energy(trial);
        while (e1 > e0 && step > 1e-6)
        {
            step *= 0.5;
            trial = x + step * dir;
            e1 = energy(trial);
        }
        if (!(e1 <= e0))
            break;
        x = trial;
        ++done;
        bool settled = (e0 - e1) < 2e-4 * e0;
        e0 = e1;
        if (settled)
            break;
    }

    // Back to 3D scale, or to the pins' own frame.
    double back = pinned ? 1.0 / su : std::sqrt(a3);
    for (Index v = 0; v < V; ++v)
        ch.uv[v] = glm::dvec2(x[v], x[V + v]) * back;
    if (pinned)
        for (Index v = 0; v < V; ++v)
            if (ch.pinned[v])
                ch.uv[v] = glm::dvec2(pinX[v], pinX[V + v]) / su;
    return done;
}

UnwrapResult unwrapChart(Chart& ch, const UnwrapOptions& opts)
{
    LILA_ZONE();

    UnwrapResult r;
    if (ch.tris.empty())
        return r;
    size_t pins = 0;
    if (opts.usePins)
        for (uint8_t p : ch.pinned)
            pins += p ? 1 : 0;
    std::vector<glm::dvec2> pinUv;
    for (Index v = 0; v < ch.vertexCount(); ++v)
        if (ch.pinned[v])
            pinUv.push_back(ch.uv[v]);

    const bool viaBff = opts.method == UnwrapMethod::Bff && pins < 2 && flattenBff(ch);
    bool ok = viaBff || lscm(ch, opts.usePins);
    if (!viaBff && ok && pins < 2)
    {
        // LSCM pins two points arbitrarily; mirror if it came out inverted and fix the scale.
        if (ch.areaUv() < 0.0)
            for (glm::dvec2& p : ch.uv)
                p.x = -p.x;
        matchSurfaceArea(ch);
    }
    if (!viaBff && opts.method != UnwrapMethod::Conformal)
    {
        if (!ok || foldedTriangles(ch) > 0)
        {
            if (tutte(ch))
            {
                matchSurfaceArea(ch);
                ok = true;
                r.usedTutte = true;
            }
        }
        if (ok)
        {
            int n = slim(ch, opts.iterations, opts.usePins && pins >= 1);
            r.iterations = std::max(0, n);
        }
    }
    if (!ok)
        return r;

    if (pins == 1)
    {
        glm::dvec2 offset(0.0);
        for (Index v = 0; v < ch.vertexCount(); ++v)
            if (ch.pinned[v])
                offset = pinUv.front() - ch.uv[v];
        for (glm::dvec2& p : ch.uv)
            p += offset;
    }
    else if (pins == 0)
    {
        glm::dvec2 lo = ch.uvMin();
        for (glm::dvec2& p : ch.uv)
            p -= lo;
    }
    r.ok = true;
    r.distortion = chartDistortion(ch);
    return r;
}

size_t unwrapCharts(std::span<Chart> charts, const UnwrapOptions& opts)
{
    LILA_ZONE();

    std::atomic<size_t> ok{0};
    // Big charts first so the pool does not finish on one straggler.
    std::vector<size_t> order(charts.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return charts[a].tris.size() > charts[b].tris.size(); });
    tbb::parallel_for(size_t(0), order.size(), size_t(1), [&](size_t i) {
        if (unwrapChart(charts[order[i]], opts).ok)
            ++ok;
    });
    return ok;
}
} // namespace lila::uv
