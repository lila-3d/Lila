#include "curve/Network.h"

#include "core/Profile.h"
#include "curve/Intersect.h"
#include "curve/Tessellate.h"
#include "geo/Attribute.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <stdexcept>

namespace lila::curve
{
namespace
{
using D3 = glm::dvec3;
using geo::Index;

constexpr double kEps = 1e-12;

std::string str(int i) { return std::to_string(i); }

/** A curve's arc length against its span parameter, from a dense tessellation. */
struct Measured
{
    Polyline pl;

    explicit Measured(const Spline& s, double tol)
    {
        TessOptions o;
        o.mode = TessMode::Adaptive;
        o.chordError = std::clamp(tol * 0.02, 1e-5, 0.01);
        o.maxAngle = 0.03;
        pl = tessellate(s, o);
    }

    double at(double u) const
    {
        const auto& P = pl.params;
        if (P.empty())
            return 0.0;
        if (u <= P.front())
            return 0.0;
        if (u >= P.back())
            return pl.arcLength.back();
        size_t i = size_t(std::upper_bound(P.begin(), P.end(), u) - P.begin());
        double a = P[i - 1], b = P[i];
        double f = b - a > kEps ? (u - a) / (b - a) : 0.0;
        return pl.arcLength[i - 1] + (pl.arcLength[i] - pl.arcLength[i - 1]) * f;
    }

    /** Span parameter at arc length s, between the tessellation's points. */
    double paramAt(double s) const
    {
        const auto& A = pl.arcLength;
        if (A.empty())
            return 0.0;
        if (s <= 0.0)
            return pl.params.front();
        if (s >= A.back())
            return pl.params.back();
        size_t i = size_t(std::upper_bound(A.begin(), A.end(), s) - A.begin());
        double f = A[i] - A[i - 1] > kEps ? (s - A[i - 1]) / (A[i] - A[i - 1]) : 0.0;
        return pl.params[i - 1] + (pl.params[i] - pl.params[i - 1]) * f;
    }
};

/** Monotone cubic (Fritsch-Carlson) through increasing points: smooth and never overshooting, so arc length stays monotone. */
struct Pchip
{
    std::vector<double> x, y, m;

    Pchip(std::vector<double> xs, std::vector<double> ys) : x(std::move(xs)), y(std::move(ys))
    {
        const size_t n = x.size();
        m.assign(n, 0.0);
        if (n < 2)
            return;
        std::vector<double> d(n - 1);
        for (size_t k = 0; k + 1 < n; ++k)
            d[k] = (y[k + 1] - y[k]) / std::max(x[k + 1] - x[k], kEps);
        m[0] = d[0];
        m[n - 1] = d[n - 2];
        for (size_t k = 1; k + 1 < n; ++k)
        {
            if (d[k - 1] * d[k] <= 0.0)
                m[k] = 0.0;
            else
            {
                double h0 = x[k] - x[k - 1], h1 = x[k + 1] - x[k];
                double w1 = 2.0 * h1 + h0, w2 = h1 + 2.0 * h0;
                m[k] = (w1 + w2) / (w1 / d[k - 1] + w2 / d[k]);
            }
        }
    }

    double operator()(double t) const
    {
        const size_t n = x.size();
        if (n == 0)
            return 0.0;
        if (n == 1)
            return y[0];
        size_t k = std::min(size_t(std::upper_bound(x.begin(), x.end(), t) - x.begin()), n - 1);
        k = k == 0 ? 0 : k - 1;
        double h = std::max(x[k + 1] - x[k], kEps), s = std::clamp((t - x[k]) / h, 0.0, 1.0);
        double s2 = s * s, s3 = s2 * s;
        return (2 * s3 - 3 * s2 + 1) * y[k] + (s3 - 2 * s2 + s) * h * m[k] + (-2 * s3 + 3 * s2) * y[k + 1] + (s3 - s2) * h * m[k + 1];
    }
};

/** Cardinal functions of the natural cubic spline on the nodes: phi_i(x_k) = (i == k), C2 everywhere. */
struct Cardinal
{
    std::vector<double> x;
    std::vector<std::vector<double>> M; // second derivatives at the nodes, per function

    explicit Cardinal(std::vector<double> nodes) : x(std::move(nodes))
    {
        const size_t n = x.size();
        M.assign(n, std::vector<double>(n, 0.0));
        if (n < 3)
            return;
        for (size_t i = 0; i < n; ++i)
        {
            const size_t m = n - 2;
            std::vector<double> a(m), b(m), c(m), d(m);
            auto y = [&](size_t k) { return k == i ? 1.0 : 0.0; };
            for (size_t r = 0; r < m; ++r)
            {
                size_t k = r + 1;
                double h0 = x[k] - x[k - 1], h1 = x[k + 1] - x[k];
                a[r] = h0;
                b[r] = 2.0 * (h0 + h1);
                c[r] = h1;
                d[r] = 6.0 * ((y(k + 1) - y(k)) / h1 - (y(k) - y(k - 1)) / h0);
            }
            for (size_t r = 1; r < m; ++r)
            {
                double w = a[r] / b[r - 1];
                b[r] -= w * c[r - 1];
                d[r] -= w * d[r - 1];
            }
            std::vector<double> sol(m);
            for (size_t r = m; r-- > 0;)
                sol[r] = (d[r] - (r + 1 < m ? c[r] * sol[r + 1] : 0.0)) / b[r];
            for (size_t r = 0; r < m; ++r)
                M[i][r + 1] = sol[r];
        }
    }

    double operator()(size_t i, double t) const
    {
        const size_t n = x.size();
        if (n == 1)
            return 1.0;
        if (n == 2)
        {
            double f = std::clamp((t - x[0]) / std::max(x[1] - x[0], kEps), 0.0, 1.0);
            return i == 0 ? 1.0 - f : f;
        }
        size_t k = std::min(size_t(std::upper_bound(x.begin(), x.end(), t) - x.begin()), n - 1);
        k = k == 0 ? 0 : k - 1;
        double h = std::max(x[k + 1] - x[k], kEps);
        double A = (x[k + 1] - t) / h, B = (t - x[k]) / h;
        double yk = k == i ? 1.0 : 0.0, yk1 = k + 1 == i ? 1.0 : 0.0;
        return A * yk + B * yk1 + ((A * A * A - A) * M[i][k] + (B * B * B - B) * M[i][k + 1]) * h * h / 6.0;
    }
};

/** One line of the grid: a curve of one family (oriented to run with the other family's order) or a pole. */
struct Line
{
    int curve = -1; // input index, -1 for a pole
    Spline spline;
    D3 pole{0.0};
    std::vector<double> at; // span parameter where it meets each line of the other family
};

/** Evenly spread global parameters of the crossings: the mean of each line's normalised arc length to them. */
std::vector<double> globalParams(const std::vector<Line>& lines, const std::vector<Measured>& measured, size_t crossings)
{
    std::vector<double> g(crossings, 0.0);
    int count = 0;
    for (size_t r = 0; r < lines.size(); ++r)
    {
        if (lines[r].curve < 0)
            continue;
        double a = measured[r].at(lines[r].at.front()), b = measured[r].at(lines[r].at.back());
        if (b - a < kEps)
            continue;
        for (size_t c = 0; c < crossings; ++c)
            g[c] += (measured[r].at(lines[r].at[c]) - a) / (b - a);
        ++count;
    }
    for (size_t c = 0; c < crossings; ++c)
        g[c] = count ? g[c] / count : double(c) / std::max<size_t>(1, crossings - 1);
    g.front() = 0.0;
    g.back() = 1.0;
    for (size_t c = 1; c < crossings; ++c)
        g[c] = std::max(g[c], g[c - 1] + 1e-6);
    double top = g.back();
    for (double& v : g)
        v /= top;
    return g;
}

/** Samples a line at global parameters: arc length mapped smoothly so it reaches each crossing at its global parameter. */
std::vector<D3> sampleLine(const Line& line, const Measured* m, const std::vector<double>& global, const std::vector<double>& at)
{
    std::vector<D3> out;
    if (line.curve < 0)
    {
        out.assign(at.size(), line.pole);
        return out;
    }
    std::vector<double> sig;
    for (double u : line.at)
        sig.push_back(m->at(u));
    Pchip map(global, sig);
    // Exactly on the curve, so it becomes an edge loop of the mesh rather than a chord of its tessellation.
    for (double s : at)
        out.push_back(pointAt(line.spline, m->paramAt(map(s))));
    return out;
}
} // namespace

NetworkResult curveNetwork(const Splines& curves, const NetworkOptions& o)
{
    LILA_ZONE();

    NetworkResult r;
    const int N = int(curves.size());
    r.family.assign(size_t(N), -1);
    const double tol = std::max(o.tolerance, 1e-6);
    std::vector<bool> usable(size_t(N), false);
    for (int i = 0; i < N; ++i)
    {
        const Spline& s = curves[size_t(i)];
        if (!isValid(s) || spanCount(s) == 0 || length(s) <= tol)
            r.warnings.push_back("curve " + str(i) + " is degenerate; left out");
        else if (s.closed)
            r.warnings.push_back("curve " + str(i) + " is closed; a network takes open curves (detach it), left out");
        else
            usable[size_t(i)] = true;
    }

    struct Hit
    {
        int a, b;
        double ua, ub;
        D3 p;
        bool endEnd;
    };
    auto nearEnd = [&](const Spline& s, const D3& p) {
        return glm::distance(p, pointAt(s, 0.0)) < tol * 2.0 || glm::distance(p, pointAt(s, spanCount(s))) < tol * 2.0;
    };
    std::vector<Hit> hits;
    IntersectOptions io;
    io.tolerance = tol;
    for (int i = 0; i < N; ++i)
        for (int j = i + 1; j < N; ++j)
            if (usable[size_t(i)] && usable[size_t(j)])
                for (const CurveHit& h : intersect(curves[size_t(i)], curves[size_t(j)], io))
                    hits.push_back({i, j, h.ua, h.ub, (h.pa + h.pb) * 0.5,
                                    nearEnd(curves[size_t(i)], h.pa) && nearEnd(curves[size_t(j)], h.pb)});

    // Two families: crossing curves differ. End-to-end meetings colour only what crossings leave open, so curves of one
    // family may meet at a shared end (a pole).
    std::vector<int> color(size_t(N), -1);
    for (size_t i = 0; i < o.families.size() && i < size_t(N); ++i)
        if (usable[i] && (o.families[i] == 0 || o.families[i] == 1))
            color[i] = o.families[i];
    std::vector<std::vector<int>> hard(static_cast<size_t>(N)), soft(static_cast<size_t>(N));
    for (const Hit& h : hits)
    {
        (h.endEnd ? soft : hard)[size_t(h.a)].push_back(h.b);
        (h.endEnd ? soft : hard)[size_t(h.b)].push_back(h.a);
    }
    std::deque<int> q;
    auto flood = [&](const std::vector<std::vector<int>>& adj) {
        while (!q.empty())
        {
            int c = q.front();
            q.pop_front();
            for (int nb : adj[size_t(c)])
                if (color[size_t(nb)] < 0)
                {
                    color[size_t(nb)] = 1 - color[size_t(c)];
                    q.push_back(nb);
                }
        }
    };
    for (const auto* adj : {&hard, &soft})
    {
        for (int i = 0; i < N; ++i)
            if (color[size_t(i)] >= 0)
                q.push_back(i);
        flood(*adj);
        for (int i = 0; i < N; ++i)
            if (color[size_t(i)] < 0 && !(*adj)[size_t(i)].empty())
            {
                color[size_t(i)] = 0;
                q.push_back(i);
                flood(*adj);
            }
    }
    for (const Hit& h : hits)
        if (!h.endEnd && color[size_t(h.a)] == color[size_t(h.b)])
            throw std::invalid_argument("curves " + str(h.a) + " and " + str(h.b) +
                                        " cross but fall in the same family: the curves do not form a grid of two families");
    for (int i = 0; i < N; ++i)
        if (usable[size_t(i)] && color[size_t(i)] < 0)
            r.warnings.push_back("curve " + str(i) + " does not meet the network; left out");
    r.family = color;

    std::vector<int> fam[2];
    for (int i = 0; i < N; ++i)
        if (color[size_t(i)] >= 0)
            fam[color[size_t(i)]].push_back(i);
    if (fam[0].empty() || fam[1].empty())
        throw std::invalid_argument("a network needs curves running both ways that cross");

    // The crossing of each U and V curve.
    std::map<std::pair<int, int>, std::pair<double, double>> cross;
    std::map<std::pair<int, int>, D3> crossPt;
    for (const Hit& h : hits)
    {
        if (color[size_t(h.a)] == color[size_t(h.b)])
            continue;
        bool aU = color[size_t(h.a)] == 0;
        std::pair<int, int> key = aU ? std::pair{h.a, h.b} : std::pair{h.b, h.a};
        if (cross.contains(key))
            throw std::invalid_argument("curves " + str(key.first) + " and " + str(key.second) + " cross more than once");
        cross[key] = aU ? std::pair{h.ua, h.ub} : std::pair{h.ub, h.ua};
        crossPt[key] = h.p;
    }
    std::string missing;
    for (int u : fam[0])
        for (int v : fam[1])
            if (!cross.contains({u, v}))
                missing += (missing.empty() ? "" : ", ") + str(u) + " and " + str(v);
    if (!missing.empty())
        throw std::invalid_argument("every U curve must cross every V curve; these do not meet: " + missing);

    auto paramOn = [&](int curve, int other) {
        bool isU = color[size_t(curve)] == 0;
        auto& c = cross[isU ? std::pair{curve, other} : std::pair{other, curve}];
        return isU ? c.first : c.second;
    };
    // Order each family along the first curve of the other, then orient every curve to run with that order.
    std::vector<int> order[2];
    for (int f = 0; f < 2; ++f)
    {
        order[f] = fam[f];
        int ref = fam[1 - f].front();
        std::sort(order[f].begin(), order[f].end(), [&](int a, int b) { return paramOn(ref, a) < paramOn(ref, b); });
    }
    std::vector<Line> lines[2];
    for (int f = 0; f < 2; ++f)
        for (int c : order[f])
        {
            Line L;
            L.curve = c;
            L.spline = curves[size_t(c)];
            for (int other : order[1 - f])
                L.at.push_back(paramOn(c, other));
            bool up = true, down = true;
            for (size_t k = 1; k < L.at.size(); ++k)
            {
                up = up && L.at[k] > L.at[k - 1] + 1e-9;
                down = down && L.at[k] < L.at[k - 1] - 1e-9;
            }
            if (L.at.size() > 1 && !up && !down)
                throw std::invalid_argument("curve " + str(c) + " crosses the other family's curves out of order");
            if (L.at.size() > 1 && down)
            {
                double K = spanCount(L.spline);
                reverse(L.spline);
                for (double& u : L.at)
                    u = K - u;
            }
            lines[f].push_back(std::move(L));
        }

    // Poles: a family missing its first or last line where all the other family's curves run on to one shared end.
    bool anyPole[2] = {false, false};
    for (int f = 0; f < 2; ++f)
    {
        const std::vector<Line>& others = lines[1 - f];
        if (std::ranges::any_of(others, [](const Line& L) { return L.curve < 0; }))
            continue;
        for (bool atEnd : {false, true})
        {
            D3 first = atEnd ? pointAt(others.front().spline, spanCount(others.front().spline)) : pointAt(others.front().spline, 0.0);
            D3 sum(0.0);
            bool meet = others.size() >= 2;
            for (const Line& L : others)
            {
                double K = spanCount(L.spline);
                D3 e = atEnd ? pointAt(L.spline, K) : pointAt(L.spline, 0.0);
                double edgeParam = atEnd ? L.at.back() : L.at.front();
                bool runsOn = atEnd ? edgeParam < K - 1e-6 : edgeParam > 1e-6;
                bool beyond = glm::distance(pointAt(L.spline, edgeParam), e) > tol * 2.0;
                meet = meet && glm::distance(e, first) < tol * 2.0 && runsOn && beyond;
                sum += e;
            }
            if (!meet)
                continue;
            Line pole;
            pole.pole = sum / double(others.size());
            pole.at.assign(others.size(), 0.0);
            if (atEnd)
                lines[f].push_back(pole);
            else
                lines[f].insert(lines[f].begin(), pole);
            for (Line& L : lines[1 - f])
            {
                double K = spanCount(L.spline);
                if (atEnd)
                    L.at.push_back(K);
                else
                    L.at.insert(L.at.begin(), 0.0);
            }
            anyPole[f] = true;
        }
    }
    if (anyPole[0] && anyPole[1])
        throw std::invalid_argument("a network with poles in both directions is not supported");
    if (lines[0].size() < 2 || lines[1].size() < 2)
        throw std::invalid_argument("a network needs at least two curves (or a curve and a pole) each way");

    // Grid points: the crossings (the midpoint of the two curves there), or the pole.
    const size_t R = lines[0].size(), C = lines[1].size();
    std::vector<std::vector<D3>> X(R, std::vector<D3>(C, D3(0.0)));
    for (size_t i = 0; i < R; ++i)
        for (size_t j = 0; j < C; ++j)
        {
            const Line &u = lines[0][i], &v = lines[1][j];
            if (u.curve < 0)
                X[i][j] = u.pole;
            else if (v.curve < 0)
                X[i][j] = v.pole;
            else
                X[i][j] = crossPt[{u.curve, v.curve}];
        }

    std::vector<Measured> mu, mv;
    for (const Line& L : lines[0])
        mu.emplace_back(L.curve >= 0 ? L.spline : Spline(), tol);
    for (const Line& L : lines[1])
        mv.emplace_back(L.curve >= 0 ? L.spline : Spline(), tol);
    std::vector<double> sNodes = globalParams(lines[0], mu, C), tNodes = globalParams(lines[1], mv, R);

    const int nu = std::max(1, o.spansU), nv = std::max(1, o.spansV);
    std::vector<double> sv, tv;
    for (size_t c = 0; c + 1 < C; ++c)
        for (int k = 0; k < nu; ++k)
            sv.push_back(sNodes[c] + (sNodes[c + 1] - sNodes[c]) * k / nu);
    sv.push_back(1.0);
    for (size_t i = 0; i + 1 < R; ++i)
        for (int k = 0; k < nv; ++k)
            tv.push_back(tNodes[i] + (tNodes[i + 1] - tNodes[i]) * k / nv);
    tv.push_back(1.0);

    std::vector<std::vector<D3>> rowAt(R), colAt(C);
    for (size_t i = 0; i < R; ++i)
        rowAt[i] = sampleLine(lines[0][i], &mu[i], sNodes, sv);
    for (size_t j = 0; j < C; ++j)
        colAt[j] = sampleLine(lines[1][j], &mv[j], tNodes, tv);
    Cardinal phi(tNodes), psi(sNodes);

    // Gordon surface: rows blended across plus columns blended along, minus the blended grid they both count.
    const size_t S = sv.size(), T = tv.size();
    std::vector<std::vector<double>> ph(R, std::vector<double>(T)), ps(C, std::vector<double>(S));
    for (size_t i = 0; i < R; ++i)
        for (size_t b = 0; b < T; ++b)
            ph[i][b] = phi(i, tv[b]);
    for (size_t j = 0; j < C; ++j)
        for (size_t a = 0; a < S; ++a)
            ps[j][a] = psi(j, sv[a]);
    std::vector<std::vector<D3>> P(T, std::vector<D3>(S, D3(0.0)));
    for (size_t b = 0; b < T; ++b)
        for (size_t a = 0; a < S; ++a)
        {
            D3 p(0.0);
            for (size_t i = 0; i < R; ++i)
                p += rowAt[i][a] * ph[i][b];
            for (size_t j = 0; j < C; ++j)
                p += colAt[j][b] * ps[j][a];
            for (size_t i = 0; i < R; ++i)
                for (size_t j = 0; j < C; ++j)
                    p -= X[i][j] * (ph[i][b] * ps[j][a]);
            P[b][a] = p;
        }
    // Grid lines lie on their curves exactly.
    for (size_t i = 0; i < R; ++i)
        if (lines[0][i].curve >= 0)
            P[i * size_t(nv)] = rowAt[i];
    for (size_t j = 0; j < C; ++j)
        if (lines[1][j].curve >= 0)
            for (size_t b = 0; b < T; ++b)
                P[b][j * size_t(nu)] = colAt[j][b];
    // Where two curves meet within tolerance, the shared point sits between them.
    for (size_t i = 0; i < R; ++i)
        for (size_t j = 0; j < C; ++j)
            if (lines[0][i].curve >= 0 && lines[1][j].curve >= 0)
                P[i * size_t(nv)][j * size_t(nu)] = X[i][j];

    for (const auto& row : P)
        for (const D3& p : row)
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
                throw std::invalid_argument("the network surface is not finite");

    geo::Mesh& m = r.mesh;
    std::vector<std::vector<Index>> idx(T, std::vector<Index>(S, geo::kInvalid));
    for (size_t b = 0; b < T; ++b)
    {
        bool poleRow = b % size_t(nv) == 0 && lines[0][b / size_t(nv)].curve < 0;
        for (size_t a = 0; a < S; ++a)
        {
            bool poleCol = a % size_t(nu) == 0 && lines[1][a / size_t(nu)].curve < 0;
            if (poleRow && a > 0)
                idx[b][a] = idx[b][0];
            else if (poleCol && b > 0)
                idx[b][a] = idx[0][a];
            else
                idx[b][a] = m.addPoint(poleRow   ? lines[0][b / size_t(nv)].pole
                                       : poleCol ? lines[1][a / size_t(nu)].pole
                                                 : P[b][a]);
        }
    }
    for (size_t b = 0; b + 1 < T; ++b)
        for (size_t a = 0; a + 1 < S; ++a)
        {
            std::vector<Index> pts = {idx[b][a], idx[b][a + 1], idx[b + 1][a + 1], idx[b + 1][a]};
            std::vector<glm::vec2> uvs = {{float(a) / float(S - 1), float(b) / float(T - 1)},
                                          {float(a + 1) / float(S - 1), float(b) / float(T - 1)},
                                          {float(a + 1) / float(S - 1), float(b + 1) / float(T - 1)},
                                          {float(a) / float(S - 1), float(b + 1) / float(T - 1)}};
            std::vector<Index> fp;
            std::vector<glm::vec2> fu;
            for (size_t k = 0; k < 4; ++k)
                if (std::ranges::find(fp, pts[k]) == fp.end())
                {
                    fp.push_back(pts[k]);
                    fu.push_back(uvs[k]);
                }
            if (fp.size() < 3)
                continue;
            if (o.flip)
            {
                std::reverse(fp.begin(), fp.end());
                std::reverse(fu.begin(), fu.end());
            }
            Index f = m.addFace(fp);
            if (f == geo::kInvalid)
                continue;
            auto uv = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
            size_t k = 0;
            m.forEachFaceCorner(f, [&](Index c) { uv[c] = fu[k++]; });
        }
    for (const Line& L : lines[0])
        r.uOrder.push_back(L.curve);
    for (const Line& L : lines[1])
        r.vOrder.push_back(L.curve);
    return r;
}
} // namespace lila::curve
