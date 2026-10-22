#include "uv/Optimize.h"

#include "core/Profile.h"
#include "uv/Layout.h"
#include "uv/OptimizeMath.h"
#include "uv/Seams.h"
#include "uv/Unwrap.h"

#include <Eigen/Sparse>
#include <Eigen/SparseCholesky>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>

namespace lila::uv
{
namespace
{
using namespace detail;
using Triplet = Eigen::Triplet<double>;
using SpMat = Eigen::SparseMatrix<double>;

/** Per-triangle target singular values for a mode, from the current ones (s2 taken unsigned). */
std::pair<double, double> targetStretch(OptimizeMode mode, double mix, double s1, double s2)
{
    s2 = std::max(std::abs(s2), 1e-9);
    s1 = std::max(s1, s2);
    const double conformal = 0.5 * (s1 + s2);
    if (mode == OptimizeMode::Conformal)
        return {conformal, conformal};
    // Nearest (e^x, e^-x) to (s1, s2) by Newton on x.
    double x = 0.5 * std::log(s1 / s2);
    for (int it = 0; it < 8; ++it)
    {
        double ep = std::exp(x), em = std::exp(-x);
        double g = 2.0 * (ep - s1) * ep - 2.0 * (em - s2) * em;
        double h = 2.0 * (2.0 * ep * ep - s1 * ep) + 2.0 * (2.0 * em * em - s2 * em);
        double dx = h > 1e-12 ? -g / h : -0.1 * g;
        x += std::clamp(dx, -1.0, 1.0);
        if (std::abs(dx) < 1e-12)
            break;
    }
    const double t1 = std::exp(x), t2 = std::exp(-x);
    if (mode == OptimizeMode::Authalic)
        return {t1, t2};
    const double m = std::clamp(mix, 0.0, 1.0);
    return {(1.0 - m) * conformal + m * t1, (1.0 - m) * conformal + m * t2};
}

/** Stiffness matrix sum(area * g_p . g_q) over the free vertices, factorised once. */
class Poisson
{
public:
    Poisson(const Chart& ch, const std::vector<TriFrame>& fr, const std::vector<uint8_t>& fixed)
    {
        const size_t V = ch.pos.size();
        mFree.assign(V, -1);
        int n = 0;
        for (size_t v = 0; v < V; ++v)
            if (!fixed[v])
                mFree[v] = n++;
        mN = n;
        std::vector<Triplet> trips;
        trips.reserve(ch.tris.size() * 9 + size_t(n));
        double diag = 0.0;
        for (size_t i = 0; i < ch.tris.size(); ++i)
        {
            if (fr[i].area <= 0.0)
                continue;
            const auto& t = ch.tris[i];
            for (int p = 0; p < 3; ++p)
            {
                int r = mFree[t[p]];
                if (r < 0)
                    continue;
                for (int q = 0; q < 3; ++q)
                {
                    double w = fr[i].area * glm::dot(fr[i].g[p], fr[i].g[q]);
                    int c = mFree[t[q]];
                    if (c >= 0)
                    {
                        trips.push_back(Triplet(r, c, w));
                        if (r == c)
                            diag += w;
                    }
                    else
                        mCoupling.push_back({r, int(t[q]), w});
                }
            }
        }
        double eps = 1e-10 * std::max(diag / std::max(n, 1), 1e-30);
        for (int i = 0; i < n; ++i)
            trips.push_back(Triplet(i, i, eps));
        SpMat m(n, n);
        m.setFromTriplets(trips.begin(), trips.end());
        mSolver.compute(m);
        mOk = n > 0 && mSolver.info() == Eigen::Success;
    }

    bool ok() const { return mOk; }

    /** Solves for one coordinate: rhs is per vertex, x holds the fixed values and receives the free ones. */
    bool solve(const std::vector<double>& rhs, std::vector<double>& x) const
    {
        Eigen::VectorXd b = Eigen::VectorXd::Zero(mN);
        for (size_t v = 0; v < rhs.size(); ++v)
            if (mFree[v] >= 0)
                b[mFree[v]] = rhs[v];
        for (const Coupling& c : mCoupling)
            b[c.row] -= c.w * x[size_t(c.fixedVertex)];
        Eigen::VectorXd s = mSolver.solve(b);
        if (mSolver.info() != Eigen::Success || !s.allFinite())
            return false;
        for (size_t v = 0; v < x.size(); ++v)
            if (mFree[v] >= 0)
                x[v] = s[mFree[v]];
        return true;
    }

private:
    struct Coupling
    {
        int row;
        int fixedVertex;
        double w;
    };
    std::vector<int> mFree;
    std::vector<Coupling> mCoupling;
    int mN = 0;
    Eigen::SimplicialLDLT<SpMat> mSolver;
    bool mOk = false;
};

double signedAreaOf(const Chart& ch, const std::vector<glm::dvec2>& uv)
{
    double a = 0.0;
    for (const auto& t : ch.tris)
        a += signedArea2(uv[t[0]], uv[t[1]], uv[t[2]]);
    return a;
}

glm::dvec2 meanOf(const std::vector<glm::dvec2>& uv)
{
    glm::dvec2 s(0.0);
    for (const glm::dvec2& p : uv)
        s += p;
    return uv.empty() ? s : s / double(uv.size());
}
} // namespace

ChartStats chartStats(const Chart& ch)
{
    ChartStats s;
    std::vector<TriFrame> fr = triFrames(ch);
    double a3 = 0.0, au = 0.0;
    for (size_t i = 0; i < ch.tris.size(); ++i)
    {
        a3 += fr[i].area;
        au += signedArea2(ch.uv[ch.tris[i][0]], ch.uv[ch.tris[i][1]], ch.uv[ch.tris[i][2]]);
    }
    if (a3 <= 0.0 || au == 0.0)
        return s;
    const double sign = au < 0.0 ? -1.0 : 1.0;
    const double ratio = std::abs(au) / a3;
    double w = 0.0;
    for (size_t i = 0; i < ch.tris.size(); ++i)
    {
        if (fr[i].area <= 0.0)
            continue;
        const auto& t = ch.tris[i];
        double a = sign * signedArea2(ch.uv[t[0]], ch.uv[t[1]], ch.uv[t[2]]);
        if (a <= 0.0)
        {
            ++s.folded;
            continue;
        }
        double la = std::abs(std::log2(a / fr[i].area / ratio));
        Svd2 d = svd2(jacobian(fr[i], ch.uv[t[0]], ch.uv[t[1]], ch.uv[t[2]]));
        double an = d.s1 / std::max(std::abs(d.s2), 1e-12) - 1.0;
        s.area += fr[i].area * la;
        s.angle += fr[i].area * an;
        s.maxArea = std::max(s.maxArea, la);
        s.maxAngle = std::max(s.maxAngle, an);
        w += fr[i].area;
    }
    if (w > 0.0)
    {
        s.area /= w;
        s.angle /= w;
    }
    return s;
}

OptimizeResult optimizeChart(Chart& ch, const OptimizeOptions& o)
{
    LILA_ZONE();

    OptimizeResult r;
    const size_t V = ch.pos.size();
    if (V < 3 || ch.tris.empty())
        return r;
    r.before = chartStats(ch);
    size_t pins = 0;
    if (o.usePins)
        for (uint8_t p : ch.pinned)
            pins += p ? 1 : 0;

    if (o.mode == OptimizeMode::Isometric)
    {
        glm::dvec2 c0 = ch.uvCentroid();
        double area0 = std::abs(ch.areaUv());
        int n = slim(ch, o.iterations, pins > 0);
        if (n < 0)
            return r;
        if (pins < 2)
            fitChart(ch, c0, area0);
        r.ok = true;
        r.iterations = n;
        r.after = chartStats(ch);
        return r;
    }

    const double a3 = ch.area3d(), au = ch.areaUv();
    if (a3 <= 0.0 || std::abs(au) <= 1e-300)
        return r;
    const bool mirror = au < 0.0 && pins < 2;
    std::vector<glm::dvec2> x(ch.uv.begin(), ch.uv.end());
    if (mirror)
        for (glm::dvec2& p : x)
            p.x = -p.x;

    // 3D frames scaled to the island's own uv scale, so 1 means "as big as it is now".
    const double scale = std::sqrt(std::abs(au) / a3);
    std::vector<TriFrame> fr(ch.tris.size());
    for (size_t i = 0; i < ch.tris.size(); ++i)
        fr[i] = triFrame(ch.pos[ch.tris[i][0]] * scale, ch.pos[ch.tris[i][1]] * scale, ch.pos[ch.tris[i][2]] * scale);

    std::vector<uint8_t> fixed(V, 0);
    if (pins > 0)
        for (size_t v = 0; v < V; ++v)
            fixed[v] = ch.pinned[v];
    else
        fixed[0] = 1;
    Poisson solver(ch, fr, fixed);
    if (!solver.ok())
        return r;

    const bool keepArea = pins < 2 && o.mode != OptimizeMode::Authalic;
    const double area0 = std::abs(au);
    const glm::dvec2 mean0 = meanOf(x);
    size_t folded0 = 0;
    for (size_t i = 0; i < ch.tris.size(); ++i)
        if (fr[i].area > 0.0 && signedArea2(x[ch.tris[i][0]], x[ch.tris[i][1]], x[ch.tris[i][2]]) <= 0.0)
            ++folded0;

    auto energy = [&](const std::vector<glm::dvec2>& s) {
        double e = 0.0;
        for (size_t i = 0; i < ch.tris.size(); ++i)
        {
            if (fr[i].area <= 0.0)
                continue;
            const auto& t = ch.tris[i];
            Svd2 d = svd2(jacobian(fr[i], s[t[0]], s[t[1]], s[t[2]]));
            auto [t1, t2] = targetStretch(o.mode, o.mix, d.s1, d.s2);
            e += fr[i].area * ((d.s1 - t1) * (d.s1 - t1) + (d.s2 - t2) * (d.s2 - t2));
        }
        return e;
    };
    auto restoreArea = [&](std::vector<glm::dvec2>& s) {
        double a = std::abs(signedAreaOf(ch, s));
        if (a <= 0.0)
            return;
        glm::dvec2 c = meanOf(s);
        double k = std::sqrt(area0 / a);
        for (glm::dvec2& p : s)
            p = c + (p - c) * k;
    };

    double e0 = energy(x);
    std::vector<double> rhsU(V), rhsV(V), xu(V), xv(V);
    std::vector<Jac2> target(ch.tris.size());
    for (int it = 0; it < o.iterations; ++it)
    {
        tbb::parallel_for(size_t(0), ch.tris.size(), [&](size_t i) {
            if (fr[i].area <= 0.0)
                return;
            const auto& t = ch.tris[i];
            Svd2 d = svd2(jacobian(fr[i], x[t[0]], x[t[1]], x[t[2]]));
            // Positive targets, so a folded triangle is pulled towards its unflipped shape.
            auto [t1, t2] = targetStretch(o.mode, o.mix, d.s1, d.s2);
            target[i] = compose(d, t1, t2);
        });
        std::fill(rhsU.begin(), rhsU.end(), 0.0);
        std::fill(rhsV.begin(), rhsV.end(), 0.0);
        for (size_t i = 0; i < ch.tris.size(); ++i)
        {
            if (fr[i].area <= 0.0)
                continue;
            const auto& t = ch.tris[i];
            for (int p = 0; p < 3; ++p)
            {
                rhsU[t[p]] += fr[i].area * (fr[i].g[p].x * target[i].a + fr[i].g[p].y * target[i].b);
                rhsV[t[p]] += fr[i].area * (fr[i].g[p].x * target[i].c + fr[i].g[p].y * target[i].d);
            }
        }
        for (size_t v = 0; v < V; ++v)
        {
            xu[v] = x[v].x;
            xv[v] = x[v].y;
        }
        if (!solver.solve(rhsU, xu) || !solver.solve(rhsV, xv))
            break;
        std::vector<glm::dvec2> dir(V);
        for (size_t v = 0; v < V; ++v)
            dir[v] = glm::dvec2(xu[v], xv[v]) - x[v];
        double step = folded0 ? 1.0 : maxFlipFreeStep(ch, fr, x, dir);
        std::vector<glm::dvec2> trial(V);
        double e1 = 0.0;
        for (int back = 0; back < 12; ++back)
        {
            for (size_t v = 0; v < V; ++v)
                trial[v] = x[v] + step * dir[v];
            if (keepArea)
                restoreArea(trial);
            e1 = energy(trial);
            if (e1 <= e0)
                break;
            step *= 0.5;
        }
        if (!(e1 <= e0))
            break;
        x.swap(trial);
        ++r.iterations;
        bool settled = e0 - e1 < 1e-6 * std::max(e0, 1e-30);
        e0 = e1;
        if (settled)
            break;
    }

    if (pins == 0)
    {
        glm::dvec2 shift = mean0 - meanOf(x);
        for (glm::dvec2& p : x)
            p += shift;
    }
    if (mirror)
        for (glm::dvec2& p : x)
            p.x = -p.x;
    for (size_t v = 0; v < V; ++v)
        ch.uv[v] = x[v];
    r.ok = true;
    r.after = chartStats(ch);
    return r;
}

MosaicResult mosaicFaces(geo::Mesh& mesh, std::span<const Index> faceIds, std::string_view uvSet, const MosaicOptions& opts)
{
    LILA_ZONE();

    MosaicResult res;
    std::vector<Index> faces = liveFaces(mesh, faceIds);
    if (faces.empty() || mesh.cornerAttrs().get<glm::vec2>(uvSet).empty())
        return res;
    ChartOptions co;
    co.source = IslandSource::Uvs;
    co.uvSet = uvSet;
    std::vector<Chart> charts = buildCharts(mesh, faces, co);
    res.islandsBefore = charts.size();

    std::vector<uint8_t> bad(charts.size(), 0);
    tbb::parallel_for(size_t(0), charts.size(), size_t(1), [&](size_t i) {
        Chart probe = charts[i];
        std::fill(probe.pinned.begin(), probe.pinned.end(), uint8_t(0));
        UnwrapOptions uo;
        uo.usePins = false;
        uo.iterations = 8;
        bool disk = probe.boundaryLoops().size() == 1 && probe.euler() == 1;
        UnwrapResult u = disk ? unwrapChart(probe, uo) : UnwrapResult{};
        bad[i] = !u.ok || u.distortion > opts.threshold ? 1 : 0;
    });

    std::vector<Index> splitFaces;
    for (size_t i = 0; i < charts.size(); ++i)
        if (bad[i])
        {
            splitFaces.insert(splitFaces.end(), charts[i].faces.begin(), charts[i].faces.end());
            ++res.split;
        }

    std::vector<Chart> out;
    if (!splitFaces.empty())
    {
        markSeamsFromUvs(mesh, faces, uvSet);
        AutoSeamOptions so;
        so.hardAngle = 180.0;
        so.useHardEdges = false;
        so.keepSeams = true;
        so.maxDistortion = opts.threshold;
        so.visibility = opts.visibility;
        so.merge = true;
        std::sort(splitFaces.begin(), splitFaces.end());
        std::vector<Chart> pieces = autoSeams(mesh, splitFaces, so).charts;

        std::vector<Index> owner(mesh.faceCapacity(), kInvalid);
        for (size_t i = 0; i < charts.size(); ++i)
            for (Index f : charts[i].faces)
                owner[f] = Index(i);
        std::vector<std::vector<size_t>> byOwner(charts.size());
        for (size_t k = 0; k < pieces.size(); ++k)
            if (!pieces[k].faces.empty())
                byOwner[owner[pieces[k].faces.front()]].push_back(k);

        for (size_t i = 0; i < charts.size(); ++i)
        {
            if (!bad[i])
            {
                out.push_back(std::move(charts[i]));
                continue;
            }
            const Chart& old = charts[i];
            const double a3 = old.area3d();
            const double s = a3 > 0.0 ? std::sqrt(std::abs(old.areaUv()) / a3) : 1.0;
            const glm::dvec2 c0 = old.uvCentroid();
            const glm::dvec2 hi = old.uvMax();
            std::vector<size_t>& mine = byOwner[i];
            std::stable_sort(mine.begin(), mine.end(), [&](size_t a, size_t b) { return pieces[a].area3d() > pieces[b].area3d(); });
            double cursor = hi.x;
            for (size_t n = 0; n < mine.size(); ++n)
            {
                Chart& p = pieces[mine[n]];
                std::fill(p.pinned.begin(), p.pinned.end(), uint8_t(0));
                for (glm::dvec2& q : p.uv)
                    q *= s;
                glm::dvec2 lo = p.uvMin(), phi = p.uvMax();
                glm::dvec2 at = n == 0 ? c0 - p.uvCentroid() : glm::dvec2(cursor + 0.02 * s * std::sqrt(a3) - lo.x, c0.y - 0.5 * (lo.y + phi.y));
                for (glm::dvec2& q : p.uv)
                    q += at;
                if (n > 0)
                    cursor = p.uvMax().x;
                out.push_back(std::move(p));
            }
        }
    }
    else
        out = std::move(charts);

    tbb::parallel_for(size_t(0), out.size(), size_t(1), [&](size_t i) {
        OptimizeOptions oo = opts.optimize;
        oo.usePins = false;
        optimizeChart(out[i], oo);
    });
    writeCharts(mesh, out, uvSet);
    if (res.split > 0)
        markSeamsFromUvs(mesh, faces, uvSet);
    res.islandsAfter = out.size();
    return res;
}
} // namespace lila::uv
