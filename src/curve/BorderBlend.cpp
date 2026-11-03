#include "curve/BorderBlend.h"

#include "core/Profile.h"
#include "curve/CurveMath.h"
#include "curve/Tessellate.h"
#include "geo/Attribute.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace lila::curve
{
namespace
{
using geo::Index;

/** Normalised arc length at each point; a closed side's total includes the closing edge. */
std::vector<double> arcParams(const std::vector<D3>& p, bool closed)
{
    std::vector<double> s(p.size(), 0.0);
    double acc = 0.0;
    for (size_t i = 1; i < p.size(); ++i)
        s[i] = acc += glm::distance(p[i - 1], p[i]);
    double total = acc + (closed && p.size() > 1 ? glm::distance(p.back(), p.front()) : 0.0);
    for (double& v : s)
        v = total > kEps ? v / total : 0.0;
    return s;
}

/** Rotation taking unit a onto unit b (Rodrigues). */
glm::dmat3 rotationBetween(const D3& a, const D3& b)
{
    D3 v = glm::cross(a, b);
    double c = glm::dot(a, b), s = glm::length(v);
    if (s < 1e-12)
    {
        if (c > 0.0)
            return glm::dmat3(1.0);
        D3 axis = unit(glm::cross(a, std::abs(a.x) < 0.9 ? D3(1, 0, 0) : D3(0, 1, 0)));
        return glm::dmat3(2.0 * glm::outerProduct(axis, axis)) - glm::dmat3(1.0);
    }
    D3 k = v / s;
    glm::dmat3 K(D3(0, k.z, -k.y), D3(-k.z, 0, k.x), D3(k.y, -k.x, 0));
    return glm::dmat3(1.0) + s * K + (1.0 - c) * K * K;
}

/** Points of a polyline at normalised arc lengths. */
std::vector<D3> byArcLength(const std::vector<D3>& dense, const std::vector<double>& at)
{
    std::vector<double> s = arcParams(dense, false);
    std::vector<D3> out;
    for (double x : at)
    {
        size_t k = std::min(size_t(std::upper_bound(s.begin(), s.end(), x) - s.begin()), dense.size() - 1);
        k = k == 0 ? 0 : k - 1;
        if (k + 1 >= dense.size())
        {
            out.push_back(dense.back());
            continue;
        }
        double f = s[k + 1] - s[k] > kEps ? std::clamp((x - s[k]) / (s[k + 1] - s[k]), 0.0, 1.0) : 0.0;
        out.push_back(glm::mix(dense[k], dense[k + 1], f));
    }
    return out;
}

/** Ease that is 0 at the sides and 1 across the middle, so profile shaping never tilts the strip where it meets them. */
double taper(double s)
{
    auto smooth = [](double x) {
        x = std::clamp(x, 0.0, 1.0);
        return x * x * (3.0 - 2.0 * x);
    };
    return smooth(std::min(s, 1.0 - s) / 0.2);
}

/** Columns of the side with more points (its points), and where each meets the other side. */
struct Pairing
{
    std::vector<size_t> smallOrder; // the small side's points in column order (original indices)
    std::vector<size_t> column;     // column of each of them
    bool reversed = false;
    double cost = std::numeric_limits<double>::max();
};

Pairing pairUp(const BorderSide& big, const BorderSide& small, bool reversed, size_t start)
{
    const size_t N = big.points.size(), n = small.points.size();
    Pairing p;
    p.reversed = reversed;
    for (size_t i = 0; i < n; ++i)
    {
        size_t k = reversed ? (start + n - i) % n : (start + i) % n;
        if (!small.closed)
            k = reversed ? n - 1 - i : i;
        p.smallOrder.push_back(k);
    }
    std::vector<D3> sp;
    for (size_t k : p.smallOrder)
        sp.push_back(small.points[k]);
    std::vector<double> qs = arcParams(sp, small.closed), Ps = arcParams(big.points, big.closed);
    p.column.resize(n);
    size_t prev = 0;
    for (size_t i = 0; i < n; ++i)
    {
        size_t lo = i == 0 ? 0 : prev + 1, hi = N - (n - i);
        if (!small.closed && i + 1 == n)
            lo = hi = N - 1;
        if (i == 0)
            hi = 0;
        size_t best = lo;
        double bd = std::numeric_limits<double>::max();
        for (size_t c = lo; c <= hi && c < N; ++c)
            if (double d = std::abs(Ps[c] - qs[i]); d < bd)
            {
                bd = d;
                best = c;
            }
        p.column[i] = prev = best;
    }
    p.cost = 0.0;
    for (size_t i = 0; i < n; ++i)
        p.cost += glm::dot(sp[i] - big.points[p.column[i]], sp[i] - big.points[p.column[i]]);
    return p;
}
} // namespace

BorderSide borderSide(const geo::Mesh& m, const geo::EdgePath& chain, const glm::dvec3& toward)
{
    BorderSide out;
    out.closed = chain.closed;
    const size_t n = chain.points.size();
    for (Index p : chain.points)
        out.points.push_back(m.position(p));
    int forward = 0, backward = 0;
    for (size_t k = 0; k < n; ++k)
    {
        const D3& p = out.points[k];
        D3 prev = k > 0 ? out.points[k - 1] : (chain.closed ? out.points[n - 1] : p);
        D3 next = k + 1 < n ? out.points[k + 1] : (chain.closed ? out.points[0] : p);
        D3 along = unit(next - prev);
        D3 nrm(0.0), away(0.0);
        bool border = false;
        auto visit = [&](size_t ei) {
            if (ei >= chain.edges.size())
                return;
            Index e = chain.edges[ei];
            bool isBorder = m.isBoundaryEdge(e);
            m.forEachEdgeCorner(e, [&](Index c) {
                Index f = m.cornerFace(c);
                nrm += m.faceNormal(f);
                if (isBorder)
                {
                    border = true;
                    away += p - m.faceCentroid(f);
                    (m.cornerPoint(c) == chain.points[ei] ? forward : backward) += 1;
                }
            });
        };
        if (k > 0 || chain.closed)
            visit(k > 0 ? k - 1 : n - 1);
        if (k + 1 < n || chain.closed)
            visit(k);
        D3 d = unit(glm::cross(along, nrm));
        D3 want = border ? away : toward - p;
        if (glm::length(d) < 0.5)
            d = unit(want - along * glm::dot(want, along));
        else if (glm::dot(d, want) < 0.0)
            d = -d;
        out.out.push_back(d);
    }
    out.facesRunForward = forward > backward;
    return out;
}

BorderBlendResult borderBlend(const BorderSide& A, const BorderSide& B, const Splines& profiles, const BorderBlendOptions& o)
{
    LILA_ZONE();

    if (A.closed != B.closed)
        throw std::invalid_argument("one side is a closed loop and the other is not");
    const size_t minPts = A.closed ? 3 : 2;
    if (A.points.size() < minPts || B.points.size() < minPts || A.out.size() != A.points.size() || B.out.size() != B.points.size())
        throw std::invalid_argument("each side needs at least " + std::to_string(minPts) + " points");
    BorderBlendResult r;
    const bool aBig = A.points.size() >= B.points.size();
    const BorderSide& big = aBig ? A : B;
    const BorderSide& small = aBig ? B : A;
    const size_t N = big.points.size(), n = small.points.size();

    Pairing best;
    for (bool rev : {false, true})
        for (size_t st = 0; st < (small.closed ? n : 1); ++st)
            if (Pairing p = pairUp(big, small, rev, st); p.cost < best.cost)
                best = p;

    // The small side's station for every column: its own points where paired, in between elsewhere.
    std::vector<double> Pb = arcParams(big.points, big.closed);
    std::vector<SideStation> smallSt(N);
    std::vector<D3> smallP(N), smallOut(N);
    const size_t segs = small.closed ? n : n - 1;
    for (size_t i = 0; i < segs; ++i)
    {
        size_t c0 = best.column[i], c1 = i + 1 < n ? best.column[i + 1] : N;
        size_t k0 = best.smallOrder[i], k1 = best.smallOrder[(i + 1) % n];
        double p0 = Pb[c0], p1 = c1 < N ? Pb[c1] : 1.0;
        // In original order the segment runs from the lower index (the closing one from the last point to the first).
        bool forwardSeg = (k1 == k0 + 1) || (small.closed && k0 == n - 1 && k1 == 0);
        for (size_t c = c0; c < c1; ++c)
        {
            double t = p1 - p0 > kEps ? (Pb[c] - p0) / (p1 - p0) : 0.0;
            smallP[c] = glm::mix(small.points[k0], small.points[k1], t);
            smallOut[c] = unit(glm::mix(small.out[k0], small.out[k1], t));
            smallSt[c] = forwardSeg ? SideStation{k0, t} : (t < 1e-12 ? SideStation{k0, 0.0} : SideStation{k1, 1.0 - t});
        }
    }
    if (!small.closed)
    {
        size_t k = best.smallOrder.back();
        smallP[N - 1] = small.points[k];
        smallOut[N - 1] = small.out[k];
        smallSt[N - 1] = {k, 0.0};
    }
    std::vector<D3> PA(N), PB(N), dA(N), dB(N);
    for (size_t c = 0; c < N; ++c)
    {
        (aBig ? PA : PB)[c] = big.points[c];
        (aBig ? dA : dB)[c] = big.out[c];
        (aBig ? PB : PA)[c] = smallP[c];
        (aBig ? dB : dA)[c] = smallOut[c];
        (aBig ? r.stationsA : r.stationsB).push_back({c, 0.0});
        (aBig ? r.stationsB : r.stationsA).push_back(smallSt[c]);
    }
    // Whether the columns run along A in its own direction (A is the small side and may have been reversed).
    bool aForward = aBig || !best.reversed;

    const int S = std::max(1, o.segments);
    std::vector<double> rowS;
    for (int i = 0; i <= S; ++i)
        rowS.push_back(double(i) / S);
    constexpr int kDense = 64;
    std::vector<double> denseS;
    for (int i = 0; i <= kDense; ++i)
        denseS.push_back(double(i) / kDense);

    // Base cross-sections: cubics leaving each side along its out direction.
    std::vector<std::vector<D3>> base(N), baseDense(N);
    std::vector<glm::dmat3> frame(N);
    std::vector<double> chord(N);
    int touching = 0;
    for (size_t c = 0; c < N; ++c)
    {
        double L = glm::distance(PA[c], PB[c]);
        chord[c] = L;
        touching += L < 1e-9;
        D3 q0 = PA[c], q1 = PA[c] + dA[c] * (o.tensionA * L / 3.0), q2 = PB[c] + dB[c] * (o.tensionB * L / 3.0), q3 = PB[c];
        std::vector<D3> dense;
        for (int i = 0; i <= kDense * 2; ++i)
        {
            double t = double(i) / (kDense * 2), u = 1.0 - t;
            dense.push_back(u * u * u * q0 + 3.0 * u * u * t * q1 + 3.0 * u * t * t * q2 + t * t * t * q3);
        }
        baseDense[c] = byArcLength(dense, denseS);
        base[c] = byArcLength(dense, rowS);
        D3 e1 = unit(PB[c] - PA[c]);
        size_t cp = c > 0 ? c - 1 : (big.closed ? N - 1 : c), cn = c + 1 < N ? c + 1 : (big.closed ? 0 : c);
        D3 tan = PA[cn] - PA[cp];
        D3 e2 = unit(tan - e1 * glm::dot(tan, e1));
        if (glm::length(e2) < 0.5)
            e2 = unit(glm::cross(e1, std::abs(e1.y) < 0.9 ? D3(0, 1, 0) : D3(1, 0, 0)));
        frame[c] = glm::dmat3(e1, e2, glm::cross(e1, e2));
    }
    if (touching == int(N))
        throw std::invalid_argument("the two sides lie on each other");
    if (touching)
        r.warnings.push_back(std::to_string(touching) + " columns have the sides touching");

    // Profiles: each shapes the column it spans, as an offset from the base in that column's frame per unit of gap.
    std::vector<std::pair<size_t, std::vector<D3>>> shaped;
    for (const Spline& ps : profiles)
    {
        if (!isValid(ps) || ps.closed)
        {
            r.warnings.push_back("a profile curve is closed or invalid; ignored");
            continue;
        }
        Polyline pl = tessellate(ps);
        if (pl.size() < 2 || pl.length < 1e-9)
            continue;
        std::vector<D3> pts = pl.points;
        D3 e0 = pts.front(), e1 = pts.back();
        size_t bestC = 0;
        bool flipP = false;
        double bd = std::numeric_limits<double>::max();
        for (size_t c = 0; c < N; ++c)
        {
            double d0 = glm::distance(e0, PA[c]) + glm::distance(e1, PB[c]), d1 = glm::distance(e1, PA[c]) + glm::distance(e0, PB[c]);
            if (std::min(d0, d1) < bd)
            {
                bd = std::min(d0, d1);
                bestC = c;
                flipP = d1 < d0;
            }
        }
        if (flipP)
        {
            std::reverse(pts.begin(), pts.end());
            std::swap(e0, e1);
        }
        if (chord[bestC] < 1e-9 || glm::distance(e0, e1) < 1e-9)
            continue;
        // Similarity putting the profile's ends on the column's ends, then its offset from the base.
        glm::dmat3 rot = rotationBetween(unit(e1 - e0), unit(PB[bestC] - PA[bestC]));
        double scale = chord[bestC] / glm::distance(e0, e1);
        std::vector<D3> along = byArcLength(pts, denseS), off;
        glm::dmat3 toLocal = glm::transpose(frame[bestC]);
        for (size_t i = 0; i < along.size(); ++i)
        {
            D3 mapped = PA[bestC] + rot * (along[i] - e0) * scale;
            off.push_back(toLocal * (mapped - baseDense[bestC][i]) / chord[bestC]);
        }
        shaped.push_back({bestC, std::move(off)});
    }
    std::sort(shaped.begin(), shaped.end(), [](const auto& x, const auto& y) { return x.first < y.first; });

    auto offsetAt = [&](size_t c, double s) -> D3 {
        if (shaped.empty())
            return D3(0.0);
        auto sample = [&](const std::vector<D3>& off) {
            double x = std::clamp(s, 0.0, 1.0) * kDense;
            size_t k = std::min(size_t(x), size_t(kDense - 1));
            return glm::mix(off[k], off[k + 1], x - double(k));
        };
        if (shaped.size() == 1)
            return sample(shaped[0].second);
        // Between the profiles on either side of the column, by column distance (round the loop when closed).
        size_t after = 0;
        while (after < shaped.size() && shaped[after].first < c)
            ++after;
        size_t i0, i1;
        double d0, d1;
        if (after == 0 || after == shaped.size())
        {
            if (!big.closed)
                return sample(shaped[after == 0 ? 0 : shaped.size() - 1].second);
            i0 = shaped.size() - 1;
            i1 = 0;
            d0 = double((c + N - shaped[i0].first) % N);
            d1 = double((shaped[i1].first + N - c) % N);
        }
        else
        {
            i0 = after - 1;
            i1 = after;
            d0 = double(c - shaped[i0].first);
            d1 = double(shaped[i1].first - c);
        }
        double f = d0 + d1 > 0.0 ? d0 / (d0 + d1) : 0.0;
        return glm::mix(sample(shaped[i0].second), sample(shaped[i1].second), f);
    };

    geo::Mesh& m = r.mesh;
    std::vector<std::vector<Index>> grid(size_t(S) + 1, std::vector<Index>(N));
    for (size_t c = 0; c < N; ++c)
        for (int i = 0; i <= S; ++i)
        {
            D3 p = base[c][size_t(i)];
            if (i > 0 && i < S)
                p += frame[c] * offsetAt(c, rowS[size_t(i)]) * (chord[c] * taper(rowS[size_t(i)]));
            grid[size_t(i)][c] = m.addPoint(p);
        }
    const bool flip = (aForward == A.facesRunForward) != o.flip;
    const size_t cols = big.closed ? N : N - 1;
    const float uDen = float(big.closed ? N : N - 1);
    for (int i = 0; i < S; ++i)
        for (size_t c = 0; c < cols; ++c)
        {
            size_t c1 = (c + 1) % N;
            std::vector<Index> q = {grid[size_t(i)][c], grid[size_t(i)][c1], grid[size_t(i) + 1][c1], grid[size_t(i) + 1][c]};
            std::vector<glm::vec2> uv = {{float(c) / uDen, float(i) / float(S)},
                                         {float(c + 1) / uDen, float(i) / float(S)},
                                         {float(c + 1) / uDen, float(i + 1) / float(S)},
                                         {float(c) / uDen, float(i + 1) / float(S)}};
            if (flip)
            {
                std::reverse(q.begin(), q.end());
                std::reverse(uv.begin(), uv.end());
            }
            Index f = m.addFace(q);
            if (f == geo::kInvalid)
                continue;
            auto uvs = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
            size_t k = 0;
            m.forEachFaceCorner(f, [&](Index cr) { uvs[cr] = uv[k++]; });
        }
    for (size_t c = 0; c < N; ++c)
    {
        r.rowA.push_back(grid[0][c]);
        r.rowB.push_back(grid[size_t(S)][c]);
    }
    return r;
}
} // namespace lila::curve
