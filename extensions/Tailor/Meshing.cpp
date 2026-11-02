#include "Pattern.h"
#include "TailorCore.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <set>
#include <utility>
#include <vector>

namespace tailor
{
namespace
{

struct P2
{
    double x = 0.0;
    double y = 0.0;
};

P2 toP2(const Vec2& v) { return {double(v[0]), double(v[1])}; }
P2 lerp(const P2& a, const P2& b, double t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; }
double dist(const P2& a, const P2& b) { return std::hypot(b.x - a.x, b.y - a.y); }
double orient(const P2& a, const P2& b, const P2& c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); }

/** Positive when d lies inside the circumcircle of counter-clockwise a, b, c. */
double inCircle(const P2& a, const P2& b, const P2& c, const P2& d)
{
    const double adx = a.x - d.x, ady = a.y - d.y;
    const double bdx = b.x - d.x, bdy = b.y - d.y;
    const double cdx = c.x - d.x, cdy = c.y - d.y;
    return (adx * adx + ady * ady) * (bdx * cdy - cdx * bdy) + (bdx * bdx + bdy * bdy) * (cdx * ady - adx * cdy) +
           (cdx * cdx + cdy * cdy) * (adx * bdy - bdx * ady);
}

double segmentDistance(const P2& p, const P2& a, const P2& b)
{
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double len2 = dx * dx + dy * dy;
    double t = len2 > 0.0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    return dist(p, {a.x + dx * t, a.y + dy * t});
}

bool insidePolygon(const std::vector<P2>& poly, const P2& p)
{
    bool in = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
    {
        const P2& a = poly[i];
        const P2& b = poly[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
            in = !in;
    }
    return in;
}

double wrapParam(double s, double length)
{
    s = std::fmod(s, length);
    if (s < 0.0)
        s += length;
    if (s >= length)
        s -= length;
    return s;
}

constexpr double kCornerCos = 0.93969262078; // 20 degrees of turn keeps a vertex

/** Arc-length parameterisation of a polyline. Closed polylines get S[n] = total length. */
struct ArcPoly
{
    std::vector<P2> pts;
    std::vector<double> S;
    std::vector<char> corner;
    bool closed = false;
    double length = 0.0;

    ArcPoly(const std::vector<Vec2>& src, bool isClosed) : closed(isClosed)
    {
        pts.reserve(src.size());
        for (const Vec2& v : src)
            pts.push_back(toP2(v));
        const size_t n = pts.size();
        if (n < 2)
            return;
        S.resize(closed ? n + 1 : n, 0.0);
        for (size_t i = 1; i < S.size(); ++i)
            S[i] = S[i - 1] + dist(pts[i - 1], pts[i % n]);
        length = S.back();
        corner.resize(n, 0);
        for (size_t i = 0; i < n; ++i)
            corner[i] = isCorner(int(i)) ? 1 : 0;
    }

    P2 at(double s) const
    {
        const size_t n = pts.size();
        if (n == 1)
            return pts[0];
        s = closed ? wrapParam(s, length) : std::clamp(s, 0.0, length);
        const size_t segs = closed ? n : n - 1;
        size_t i = size_t(std::upper_bound(S.begin(), S.end(), s) - S.begin());
        i = std::min(i == 0 ? 0 : i - 1, segs - 1);
        const double seg = S[i + 1] - S[i];
        return lerp(pts[i], pts[(i + 1) % n], seg > 0.0 ? (s - S[i]) / seg : 0.0);
    }

private:
    bool isCorner(int i) const
    {
        const int n = int(pts.size());
        const double e = length * 1e-9 + 1e-12;
        auto distinct = [&](int dir) {
            for (int k = 1; k < n; ++k)
            {
                int j = i + dir * k;
                if (closed)
                    j = ((j % n) + n) % n;
                else if (j < 0 || j >= n)
                    return -1;
                if (dist(pts[j], pts[i]) > e)
                    return j;
            }
            return -1;
        };
        const int p = distinct(-1), q = distinct(1);
        if (p < 0 || q < 0)
            return !closed;
        const P2 d0{pts[i].x - pts[p].x, pts[i].y - pts[p].y};
        const P2 d1{pts[q].x - pts[i].x, pts[q].y - pts[i].y};
        const double l = std::hypot(d0.x, d0.y) * std::hypot(d1.x, d1.y);
        return l > 0.0 && (d0.x * d1.x + d0.y * d1.y) / l < kCornerCos;
    }
};

/** Constrained Delaunay triangulation by incremental insertion, Lawson flips and flip-based segment recovery. */
class Cdt
{
public:
    struct Tri
    {
        std::array<int, 3> v{};
        std::array<int, 3> n{-1, -1, -1};
        std::array<bool, 3> c{};
    };

    static constexpr int kSuper = 3;

    std::vector<P2> pts;
    std::vector<Tri> tris;

    Cdt(const P2& lo, const P2& hi)
    {
        const double w = std::max({hi.x - lo.x, hi.y - lo.y, 1e-6});
        const P2 m{(lo.x + hi.x) * 0.5, (lo.y + hi.y) * 0.5};
        eps_ = w * 1e-9;
        pts = {{m.x - 20.0 * w, m.y - 10.0 * w}, {m.x + 20.0 * w, m.y - 10.0 * w}, {m.x, m.y + 20.0 * w}};
        vt_ = {0, 0, 0};
        tris.push_back(Tri{{0, 1, 2}});
    }

    /** Returns the vertex index, an existing one when p coincides with it, or -1 when p is outside. */
    int insert(const P2& p)
    {
        const int t = locate(p);
        if (t < 0)
            return -1;
        const Tri tr = tris[t];
        for (int k = 0; k < 3; ++k)
            if (dist(pts[tr.v[k]], p) <= eps_ * 10.0)
                return tr.v[k];
        int onEdge = -1;
        for (int i = 0; i < 3 && onEdge < 0; ++i)
            if (side(tr.v[i], tr.v[nxt(i)], p) == 0)
                onEdge = i;
        const int idx = int(pts.size());
        pts.push_back(p);
        vt_.push_back(t);
        std::vector<std::array<int, 3>> stack;
        if (onEdge >= 0)
            splitEdge(t, onEdge, idx, stack);
        else
            split3(t, idx, stack);
        legalize(stack);
        last_ = vt_[idx];
        return idx;
    }

    /** Forces segment a-b into the triangulation. Records the resulting mesh edges in `edges` when given. */
    bool constrain(int a, int b, std::vector<std::array<int, 2>>* edges, int depth = 0)
    {
        if (a == b)
            return true;
        if (a < 0 || b < 0 || depth > 64)
            return false;
        if (const auto [t, i] = findEdge(a, b); t >= 0)
            return markEdge(t, i, edges);

        const P2 A = pts[a], B = pts[b];
        int start = -1, x = -1, y = -1, through = -1;
        forRing(a, [&](int t, int k) {
            const int vx = tris[t].v[nxt(k)], vy = tris[t].v[prv(k)];
            for (int w : {vx, vy})
                if (onSegment(a, b, w))
                {
                    through = w;
                    return true;
                }
            if (orient(A, pts[vx], B) > 0.0 && orient(A, B, pts[vy]) > 0.0)
            {
                start = t;
                x = vx;
                y = vy;
                return true;
            }
            return false;
        });
        if (through >= 0)
            return splitConstraint(a, through, b, edges, depth);
        if (start < 0)
            return false;

        // Walk the edges crossed by a-b, keeping x on its right and y on its left.
        std::deque<std::array<int, 2>> crossed;
        int t = start;
        for (size_t guard = 0;; ++guard)
        {
            const int i = edgeOf(t, x, y);
            if (guard > tris.size() || i < 0 || tris[t].c[i] || tris[t].n[i] < 0)
                return false;
            crossed.push_back({x, y});
            const int u = tris[t].n[i];
            const int w = apex(u, y, x);
            if (w == b)
                break;
            if (onSegment(a, b, w))
                return splitConstraint(a, w, b, edges, depth);
            if (orient(A, B, pts[w]) > 0.0)
                y = w;
            else
                x = w;
            t = u;
        }

        const size_t cap = 64 * crossed.size() + 256;
        for (size_t guard = 0; !crossed.empty(); ++guard)
        {
            if (guard > cap)
                return false;
            const auto [p, q] = crossed.front();
            crossed.pop_front();
            const auto [te, ie] = findEdge(p, q);
            if (te < 0)
                continue;
            const int e0 = tris[te].v[ie], e1 = tris[te].v[nxt(ie)], c = tris[te].v[prv(ie)];
            const int d = apex(tris[te].n[ie], e1, e0);
            if (!properCross(c, d, e0, e1))
            {
                crossed.push_back({p, q});
                continue;
            }
            flip(te, ie);
            if (c != a && c != b && d != a && d != b && properCross(a, b, c, d))
                crossed.push_back({c, d});
        }
        const auto [t2, i2] = findEdge(a, b);
        return t2 >= 0 && markEdge(t2, i2, edges);
    }

    /** Lawson flips until every unconstrained edge is locally Delaunay. */
    void delaunay()
    {
        for (int pass = 0; pass < 64; ++pass)
        {
            int flips = 0;
            for (int t = 0; t < int(tris.size()); ++t)
                for (int i = 0; i < 3; ++i)
                    if (shouldFlip(t, i))
                    {
                        flip(t, i);
                        ++flips;
                        break;
                    }
            if (flips == 0)
                break;
        }
    }

    /** Laplacian relaxation of the movable vertices, rejecting moves that would fold a triangle. */
    void smooth(const std::vector<char>& movable, int iterations, double h)
    {
        const double minArea2 = 1e-3 * h * h;
        for (int it = 0; it < iterations; ++it)
        {
            std::vector<std::vector<int>> incident(pts.size());
            for (int t = 0; t < int(tris.size()); ++t)
                for (int v : tris[t].v)
                    incident[v].push_back(t);
            for (int v = kSuper; v < int(pts.size()); ++v)
            {
                if (v >= int(movable.size()) || !movable[v] || incident[v].empty())
                    continue;
                P2 sum{};
                int count = 0;
                bool skip = false;
                for (int t : incident[v])
                    for (int w : tris[t].v)
                    {
                        if (w == v)
                            continue;
                        skip |= w < kSuper;
                        sum.x += pts[w].x;
                        sum.y += pts[w].y;
                        ++count;
                    }
                if (skip || count == 0)
                    continue;
                const P2 old = pts[v];
                pts[v] = {sum.x / count, sum.y / count};
                for (int t : incident[v])
                {
                    const Tri& tr = tris[t];
                    if (orient(pts[tr.v[0]], pts[tr.v[1]], pts[tr.v[2]]) <= minArea2)
                    {
                        pts[v] = old;
                        break;
                    }
                }
            }
            delaunay();
        }
    }

private:
    std::vector<int> vt_; // one incident triangle per vertex
    int last_ = 0;
    double eps_ = 0.0;

    static int nxt(int i) { return i == 2 ? 0 : i + 1; }
    static int prv(int i) { return i == 0 ? 2 : i - 1; }

    int side(int a, int b, const P2& p) const
    {
        const double o = orient(pts[a], pts[b], p);
        const double tol = eps_ * dist(pts[a], pts[b]);
        return o > tol ? 1 : (o < -tol ? -1 : 0);
    }

    bool properCross(int p, int q, int r, int s) const
    {
        return side(p, q, pts[r]) * side(p, q, pts[s]) < 0 && side(r, s, pts[p]) * side(r, s, pts[q]) < 0;
    }

    bool onSegment(int a, int b, int w) const
    {
        if (w == a || w == b || w < kSuper)
            return false;
        const P2 &A = pts[a], &B = pts[b], &W = pts[w];
        const double len = dist(A, B);
        if (len <= 0.0 || std::abs(orient(A, B, W)) > eps_ * 10.0 * len)
            return false;
        const double t = ((W.x - A.x) * (B.x - A.x) + (W.y - A.y) * (B.y - A.y)) / (len * len);
        return t > 1e-9 && t < 1.0 - 1e-9;
    }

    int locate(const P2& p) const
    {
        int t = last_;
        const size_t cap = tris.size() + 64;
        for (size_t step = 0; step < cap; ++step)
        {
            const Tri& tr = tris[t];
            int i = 0;
            for (; i < 3; ++i)
                if (side(tr.v[i], tr.v[nxt(i)], p) < 0)
                    break;
            if (i == 3)
                return t;
            if (tr.n[i] < 0)
                break;
            t = tr.n[i];
        }
        for (int k = 0; k < int(tris.size()); ++k)
        {
            const Tri& tr = tris[k];
            if (side(tr.v[0], tr.v[1], p) >= 0 && side(tr.v[1], tr.v[2], p) >= 0 && side(tr.v[2], tr.v[0], p) >= 0)
                return k;
        }
        return -1;
    }

    int edgeOf(int t, int a, int b) const
    {
        const Tri& tr = tris[t];
        for (int i = 0; i < 3; ++i)
            if (tr.v[i] == a && tr.v[nxt(i)] == b)
                return i;
        return -1;
    }

    int apex(int u, int a, int b) const { return tris[u].v[prv(edgeOf(u, a, b))]; }

    void setTri(int t, int a, int b, int c, std::array<int, 3> n, std::array<bool, 3> cons)
    {
        tris[t] = Tri{{a, b, c}, n, cons};
        vt_[a] = vt_[b] = vt_[c] = t;
    }

    void relink(int u, int a, int b, int t)
    {
        if (u < 0)
            return;
        if (const int j = edgeOf(u, a, b); j >= 0)
            tris[u].n[j] = t;
    }

    template <class F>
    void forRing(int a, F&& visit) const
    {
        const int t0 = vt_[a];
        int t = t0;
        for (int guard = 0; guard < 4096; ++guard)
        {
            const Tri& tr = tris[t];
            int k = 0;
            while (k < 3 && tr.v[k] != a)
                ++k;
            if (k == 3 || visit(t, k))
                return;
            t = tr.n[prv(k)];
            if (t < 0 || t == t0)
                return;
        }
    }

    std::pair<int, int> findEdge(int a, int b) const
    {
        std::pair<int, int> r{-1, -1};
        forRing(a, [&](int t, int k) {
            if (tris[t].v[nxt(k)] == b)
                r = {t, k};
            else if (tris[t].v[prv(k)] == b)
                r = {t, prv(k)};
            return r.first >= 0;
        });
        return r;
    }

    bool markEdge(int t, int i, std::vector<std::array<int, 2>>* edges)
    {
        tris[t].c[i] = true;
        const int a = tris[t].v[i], b = tris[t].v[nxt(i)];
        if (const int u = tris[t].n[i]; u >= 0)
            if (const int j = edgeOf(u, b, a); j >= 0)
                tris[u].c[j] = true;
        if (edges)
            edges->push_back({a, b});
        return true;
    }

    bool splitConstraint(int a, int w, int b, std::vector<std::array<int, 2>>* edges, int depth)
    {
        const bool first = constrain(a, w, edges, depth + 1);
        const bool second = constrain(w, b, edges, depth + 1);
        return first && second;
    }

    bool shouldFlip(int t, int i) const
    {
        const Tri& tr = tris[t];
        if (tr.c[i] || tr.n[i] < 0)
            return false;
        const int a = tr.v[i], b = tr.v[nxt(i)], c = tr.v[prv(i)];
        const int d = apex(tr.n[i], b, a);
        const P2 &A = pts[a], &B = pts[b], &C = pts[c], &D = pts[d];
        auto d2 = [](const P2& p, const P2& q) { return (p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y); };
        const double scale = std::max({d2(A, D), d2(B, D), d2(C, D)});
        return inCircle(A, B, C, D) > 1e-10 * scale * scale && properCross(c, d, a, b);
    }

    void split3(int t, int p, std::vector<std::array<int, 3>>& stack)
    {
        const Tri old = tris[t];
        const int a = old.v[0], b = old.v[1], c = old.v[2];
        const int t1 = int(tris.size()), t2 = t1 + 1;
        tris.resize(tris.size() + 2);
        setTri(t, a, b, p, {old.n[0], t1, t2}, {old.c[0], false, false});
        setTri(t1, b, c, p, {old.n[1], t2, t}, {old.c[1], false, false});
        setTri(t2, c, a, p, {old.n[2], t, t1}, {old.c[2], false, false});
        relink(old.n[1], c, b, t1);
        relink(old.n[2], a, c, t2);
        stack.push_back({t, a, b});
        stack.push_back({t1, b, c});
        stack.push_back({t2, c, a});
    }

    void splitEdge(int t, int i, int p, std::vector<std::array<int, 3>>& stack)
    {
        const Tri old = tris[t];
        const int a = old.v[i], b = old.v[nxt(i)], c = old.v[prv(i)];
        const int nbc = old.n[nxt(i)], nca = old.n[prv(i)];
        const bool cab = old.c[i], cbc = old.c[nxt(i)], cca = old.c[prv(i)];
        const int u = old.n[i];
        const int t2 = int(tris.size());
        if (u < 0)
        {
            tris.resize(tris.size() + 1);
            setTri(t, a, p, c, {-1, t2, nca}, {cab, false, cca});
            setTri(t2, p, b, c, {-1, nbc, t}, {cab, cbc, false});
            relink(nbc, c, b, t2);
            stack.push_back({t, c, a});
            stack.push_back({t2, b, c});
            return;
        }
        const Tri ou = tris[u];
        const int j = edgeOf(u, b, a);
        const int d = ou.v[prv(j)];
        const int nad = ou.n[nxt(j)], ndb = ou.n[prv(j)];
        const bool cad = ou.c[nxt(j)], cdb = ou.c[prv(j)];
        const int u2 = t2 + 1;
        tris.resize(tris.size() + 2);
        setTri(t, a, p, c, {u2, t2, nca}, {cab, false, cca});
        setTri(t2, p, b, c, {u, nbc, t}, {cab, cbc, false});
        setTri(u, b, p, d, {t2, u2, ndb}, {cab, false, cdb});
        setTri(u2, p, a, d, {t, nad, u}, {cab, cad, false});
        relink(nbc, c, b, t2);
        relink(nad, d, a, u2);
        stack.push_back({t, c, a});
        stack.push_back({t2, b, c});
        stack.push_back({u, d, b});
        stack.push_back({u2, a, d});
    }

    /** Flips edge i of t. Afterwards t = (c, a, d) and its neighbour u = (d, b, c). */
    void flip(int t, int i)
    {
        const Tri ot = tris[t];
        const int a = ot.v[i], b = ot.v[nxt(i)], c = ot.v[prv(i)];
        const int u = ot.n[i];
        const Tri ou = tris[u];
        const int j = edgeOf(u, b, a);
        const int d = ou.v[prv(j)];
        const int nbc = ot.n[nxt(i)], nca = ot.n[prv(i)], nad = ou.n[nxt(j)], ndb = ou.n[prv(j)];
        setTri(t, c, a, d, {nca, nad, u}, {ot.c[prv(i)], ou.c[nxt(j)], false});
        setTri(u, d, b, c, {ndb, nbc, t}, {ou.c[prv(j)], ot.c[nxt(i)], false});
        relink(nad, d, a, t);
        relink(nbc, c, b, u);
    }

    void legalize(std::vector<std::array<int, 3>>& stack)
    {
        while (!stack.empty())
        {
            const auto [t, a, b] = stack.back();
            stack.pop_back();
            const int i = edgeOf(t, a, b);
            if (i < 0 || !shouldFlip(t, i))
                continue;
            const int u = tris[t].n[i];
            const int d = apex(u, b, a);
            flip(t, i);
            stack.push_back({t, a, d});
            stack.push_back({u, d, b});
        }
    }
};

/** Uniform bucket grid answering "is p within radius of any segment". */
class SegmentGrid
{
public:
    SegmentGrid(const P2& lo, const P2& hi, double cell, double radius) : lo_(lo), radius_(radius)
    {
        const double w = hi.x - lo.x + 2.0 * radius, h = hi.y - lo.y + 2.0 * radius;
        cell_ = std::max(cell, std::sqrt(w * h / 4.0e6));
        lo_ = {lo.x - radius, lo.y - radius};
        nx_ = std::max(1, int(std::ceil(w / cell_)));
        ny_ = std::max(1, int(std::ceil(h / cell_)));
        cells_.resize(size_t(nx_) * size_t(ny_));
    }

    void add(const P2& a, const P2& b)
    {
        const int id = int(segs_.size());
        segs_.push_back({a, b});
        const int x0 = cx(std::min(a.x, b.x) - radius_), x1 = cx(std::max(a.x, b.x) + radius_);
        const int y0 = cy(std::min(a.y, b.y) - radius_), y1 = cy(std::max(a.y, b.y) + radius_);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                cells_[size_t(y) * nx_ + x].push_back(id);
    }

    bool near(const P2& p) const
    {
        for (int id : cells_[size_t(cy(p.y)) * nx_ + cx(p.x)])
            if (segmentDistance(p, segs_[id][0], segs_[id][1]) < radius_)
                return true;
        return false;
    }

private:
    P2 lo_;
    double cell_ = 1.0;
    double radius_ = 0.0;
    int nx_ = 1, ny_ = 1;
    std::vector<std::array<P2, 2>> segs_;
    std::vector<std::vector<int>> cells_;

    int cx(double x) const { return std::clamp(int((x - lo_.x) / cell_), 0, nx_ - 1); }
    int cy(double y) const { return std::clamp(int((y - lo_.y) / cell_), 0, ny_ - 1); }
};

/** An outline sample at arc length s, carrying the seam slots sewn to it. */
struct LoopNode
{
    double s = 0.0;
    std::vector<int> slots;
};

/** Outline arc-length intervals [s0, s1] owned by seams; s1 may exceed the loop length. */
using Claims = std::vector<std::array<double, 2>>;

bool strictlyClaimed(double s, const Claims& claims, double length)
{
    const double e = length * 1e-9;
    for (const auto& c : claims)
    {
        double t = s;
        while (t < c[0])
            t += length;
        while (t >= c[0] + length)
            t -= length;
        if (t > c[0] + e && t < c[1] - e)
            return true;
    }
    return false;
}

/** Keeps corners and seam samples, fills the unclaimed gaps uniformly at spacing h. */
std::vector<LoopNode> resampleClosed(const ArcPoly& poly, std::vector<LoopNode> nodes, double h, const Claims& claims)
{
    const double L = poly.length, e = L * 1e-9;
    for (size_t i = 0; i < poly.pts.size(); ++i)
        if (poly.corner[i] && !strictlyClaimed(poly.S[i], claims, L))
            nodes.push_back({poly.S[i], {}});
    for (LoopNode& n : nodes)
        n.s = wrapParam(n.s, L);
    if (nodes.empty())
        nodes.push_back({0.0, {}});
    std::stable_sort(nodes.begin(), nodes.end(), [](const LoopNode& a, const LoopNode& b) { return a.s < b.s; });

    std::vector<LoopNode> merged;
    for (LoopNode& n : nodes)
    {
        if (!merged.empty() && n.s - merged.back().s < e)
            merged.back().slots.insert(merged.back().slots.end(), n.slots.begin(), n.slots.end());
        else
            merged.push_back(std::move(n));
    }
    if (merged.size() > 1 && merged.back().s + L - merged.front().s < e)
    {
        merged.front().slots.insert(merged.front().slots.end(), merged.back().slots.begin(), merged.back().slots.end());
        merged.pop_back();
    }

    std::vector<LoopNode> out;
    for (size_t i = 0; i < merged.size(); ++i)
    {
        const double s0 = merged[i].s;
        const double s1 = i + 1 < merged.size() ? merged[i + 1].s : merged[0].s + L;
        out.push_back(std::move(merged[i]));
        const double gap = s1 - s0;
        if (strictlyClaimed(s0 + gap * 0.5, claims, L))
            continue;
        int n = std::max(1, int(std::lround(gap / h)));
        if (merged.size() <= 2)
            n = std::max(n, merged.size() == 1 ? 3 : 2);
        for (int k = 1; k < n; ++k)
            out.push_back({s0 + gap * k / n, {}});
    }
    return out;
}

std::vector<P2> resampleOpen(const ArcPoly& poly, double h)
{
    std::vector<double> keep{0.0};
    for (size_t i = 1; i + 1 < poly.pts.size(); ++i)
        if (poly.corner[i])
            keep.push_back(poly.S[i]);
    keep.push_back(poly.length);
    std::vector<P2> out;
    const double e = poly.length * 1e-9;
    for (size_t i = 0; i + 1 < keep.size(); ++i)
    {
        const double gap = keep[i + 1] - keep[i];
        if (gap <= e)
            continue;
        const int n = std::max(1, int(std::lround(gap / h)));
        for (int k = 0; k < n; ++k)
            out.push_back(poly.at(keep[i] + gap * k / n));
    }
    out.push_back(poly.at(poly.length));
    return out;
}

struct PanelMesh
{
    std::vector<P2> verts;
    std::vector<std::array<int, 3>> tris;
    std::vector<std::array<int, 2>> creases;
    std::vector<float> creaseAngles;
    std::vector<std::pair<int, int>> slotVerts; // seam slot, local vertex
};

PanelMesh meshPanel(const Panel& panel, const ArcPoly& outline, std::vector<LoopNode> seamNodes, const Claims& claims)
{
    PanelMesh pm;
    const double h = std::max(double(panel.particleDistance), 1e-4);
    const std::vector<LoopNode> outer = resampleClosed(outline, std::move(seamNodes), h, claims);
    std::vector<P2> outerPts;
    for (const LoopNode& n : outer)
        outerPts.push_back(outline.at(n.s));
    if (outerPts.size() < 3)
        return pm;

    std::vector<std::vector<P2>> holes;
    for (const auto& hole : panel.holes)
    {
        if (hole.size() < 3)
            continue;
        const ArcPoly hp(hole, true);
        if (hp.length <= 1e-9)
            continue;
        std::vector<P2> loop;
        for (const LoopNode& n : resampleClosed(hp, {}, h, {}))
            loop.push_back(hp.at(n.s));
        if (loop.size() >= 3)
            holes.push_back(std::move(loop));
    }
    auto inDomain = [&](const P2& p) {
        if (!insidePolygon(outerPts, p))
            return false;
        for (const auto& hole : holes)
            if (insidePolygon(hole, p))
                return false;
        return true;
    };

    std::vector<P2> loopPts = outerPts;
    for (const auto& hole : holes)
        loopPts.insert(loopPts.end(), hole.begin(), hole.end());

    // Internal lines leaving the panel or crossing a hole are dropped; ends near the outline snap onto it.
    std::vector<std::vector<P2>> lines;
    std::vector<float> lineAngle;
    for (size_t li = 0; li < panel.internalLines.size(); ++li)
    {
        const auto& line = panel.internalLines[li];
        if (line.size() < 2)
            continue;
        const ArcPoly lp(line, false);
        if (lp.length <= 1e-9)
            continue;
        std::vector<P2> pts = resampleOpen(lp, h);
        bool ok = pts.size() >= 2;
        for (size_t k = 0; ok && k < pts.size(); ++k)
        {
            if (k == 0 || k + 1 == pts.size())
            {
                const P2* best = nullptr;
                double bestD = 0.35 * h;
                for (const P2& q : loopPts)
                    if (const double d = dist(q, pts[k]); d < bestD)
                    {
                        bestD = d;
                        best = &q;
                    }
                if (best)
                {
                    pts[k] = *best;
                    continue;
                }
            }
            ok = inDomain(pts[k]);
        }
        if (ok)
        {
            lines.push_back(std::move(pts));
            lineAngle.push_back(li < panel.lineAngles.size() ? panel.lineAngles[li] : std::nanf(""));
        }
    }

    P2 lo = outerPts[0], hi = outerPts[0];
    for (const P2& p : outerPts)
    {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y)};
    }

    const double clearance = 0.6 * h;
    SegmentGrid grid(lo, hi, h, clearance);
    for (const auto& loop : holes)
        for (size_t k = 0; k < loop.size(); ++k)
            grid.add(loop[k], loop[(k + 1) % loop.size()]);
    for (size_t k = 0; k < outerPts.size(); ++k)
        grid.add(outerPts[k], outerPts[(k + 1) % outerPts.size()]);
    for (const auto& line : lines)
        for (size_t k = 0; k + 1 < line.size(); ++k)
            grid.add(line[k], line[k + 1]);

    Cdt cdt(lo, hi);
    auto insertAll = [&](const std::vector<P2>& src) {
        std::vector<int> idx;
        idx.reserve(src.size());
        for (const P2& p : src)
            idx.push_back(cdt.insert(p));
        return idx;
    };
    const std::vector<int> outerIdx = insertAll(outerPts);
    std::vector<std::vector<int>> holeIdx, lineIdx;
    for (const auto& loop : holes)
        holeIdx.push_back(insertAll(loop));
    for (const auto& line : lines)
        lineIdx.push_back(insertAll(line));

    std::vector<char> movable(cdt.pts.size(), 0);
    const double rowH = h * std::sqrt(3.0) * 0.5;
    for (int j = 0; lo.y + j * rowH <= hi.y; ++j)
    {
        const double y = lo.y + j * rowH;
        for (int i = 0;; ++i)
        {
            const double x = lo.x + ((j & 1) ? 0.5 * h : 0.0) + i * h;
            if (x > hi.x)
                break;
            const P2 p{x, y};
            if (!inDomain(p) || grid.near(p))
                continue;
            const size_t before = cdt.pts.size();
            const int v = cdt.insert(p);
            if (v >= int(before))
            {
                movable.resize(cdt.pts.size(), 0);
                movable[v] = 1;
            }
        }
    }

    auto constrainLoop = [&](const std::vector<int>& idx) {
        for (size_t k = 0; k < idx.size(); ++k)
            cdt.constrain(idx[k], idx[(k + 1) % idx.size()], nullptr);
    };
    constrainLoop(outerIdx);
    for (const auto& idx : holeIdx)
        constrainLoop(idx);
    std::vector<std::array<int, 2>> creaseIdx;
    std::vector<float> creaseAngle;
    for (size_t li = 0; li < lineIdx.size(); ++li)
    {
        const auto& idx = lineIdx[li];
        for (size_t k = 0; k + 1 < idx.size(); ++k)
            cdt.constrain(idx[k], idx[k + 1], &creaseIdx);
        creaseAngle.resize(creaseIdx.size(), li < lineAngle.size() ? lineAngle[li] : std::nanf(""));
    }

    cdt.delaunay();
    cdt.smooth(movable, 3, h);

    std::vector<std::array<int, 3>> kept;
    for (const Cdt::Tri& t : cdt.tris)
    {
        if (t.v[0] < Cdt::kSuper || t.v[1] < Cdt::kSuper || t.v[2] < Cdt::kSuper)
            continue;
        const P2 &a = cdt.pts[t.v[0]], &b = cdt.pts[t.v[1]], &c = cdt.pts[t.v[2]];
        if (inDomain({(a.x + b.x + c.x) / 3.0, (a.y + b.y + c.y) / 3.0}))
            kept.push_back(t.v);
    }
    std::vector<int> local(cdt.pts.size(), -1);
    for (const auto& t : kept)
        for (int v : t)
            local[v] = 0;
    for (size_t v = 0; v < local.size(); ++v)
        if (local[v] == 0)
        {
            local[v] = int(pm.verts.size());
            pm.verts.push_back(cdt.pts[v]);
        }
    for (const auto& t : kept)
        pm.tris.push_back({local[t[0]], local[t[1]], local[t[2]]});
    for (size_t k = 0; k < creaseIdx.size(); ++k)
        if (const auto& e = creaseIdx[k]; local[e[0]] >= 0 && local[e[1]] >= 0)
        {
            pm.creases.push_back({local[e[0]], local[e[1]]});
            pm.creaseAngles.push_back(k < creaseAngle.size() ? creaseAngle[k] : std::nanf(""));
        }
    for (size_t k = 0; k < outer.size(); ++k)
    {
        const int v = outerIdx[k];
        if (v < 0 || local[v] < 0)
            continue;
        for (int slot : outer[k].slots)
            pm.slotVerts.push_back({slot, local[v]});
    }
    return pm;
}

struct Run
{
    int panel = -1;
    double s0 = 0.0;
    double len = 0.0;
    int from = 0;
    int to = 0;
};

struct Chain
{
    std::vector<Run> runs;
    double length = 0.0;
};

bool buildChain(const std::vector<SeamSide>& sides, const Garment& g, const std::vector<ArcPoly>& outlines,
                const std::vector<char>& usable, Chain& chain)
{
    for (const SeamSide& side : sides)
    {
        if (side.panel < 0 || side.panel >= int(g.panels.size()) || !usable[side.panel])
            return false;
        const ArcPoly& o = outlines[side.panel];
        const int n = int(o.pts.size());
        if (side.from < 0 || side.from >= n || side.to < 0 || side.to >= n || side.from == side.to)
            return false;
        double s1 = o.S[side.to];
        if (side.to < side.from)
            s1 += o.length;
        const Run r{side.panel, o.S[side.from], s1 - o.S[side.from], side.from, side.to};
        if (r.len <= o.length * 1e-9)
            return false;
        chain.runs.push_back(r);
        chain.length += r.len;
    }
    return !chain.runs.empty();
}

bool overlaps(const Run& r, const std::array<double, 2>& c, double length)
{
    const double e = length * 1e-9;
    for (int k = -1; k <= 1; ++k)
        if (std::min(r.s0 + r.len, c[1] + k * length) - std::max(r.s0, c[0] + k * length) > e)
            return true;
    return false;
}

/** Junctions between runs (hard) and outline corners inside runs (soft), as chain fractions. */
void chainBreaks(const Chain& chain, const std::vector<ArcPoly>& outlines, bool reversed, std::vector<double>& hard,
                 std::vector<double>& soft)
{
    auto map = [&](double f) { return std::clamp(reversed ? 1.0 - f : f, 0.0, 1.0); };
    double cum = 0.0;
    for (size_t k = 0; k < chain.runs.size(); ++k)
    {
        const Run& r = chain.runs[k];
        if (k > 0)
            hard.push_back(map(cum / chain.length));
        const ArcPoly& o = outlines[r.panel];
        const int n = int(o.pts.size());
        for (int i = (r.from + 1) % n; i != r.to; i = (i + 1) % n)
        {
            if (!o.corner[i])
                continue;
            double s = o.S[i];
            if (s < r.s0)
                s += o.length;
            soft.push_back(map((cum + s - r.s0) / chain.length));
        }
        cum += r.len;
    }
}

/** Sample fractions shared by both seam ends: every break hit exactly, intervals split to about spacing h. */
std::vector<double> seamSamples(std::vector<double> hard, std::vector<double> soft, double maxLength, double h)
{
    hard.push_back(0.0);
    hard.push_back(1.0);
    std::sort(hard.begin(), hard.end());
    std::vector<double> breaks;
    for (double f : hard)
        if (breaks.empty() || f - breaks.back() > 1e-9)
            breaks.push_back(f);
    std::sort(soft.begin(), soft.end());
    const double tol = 0.3 * h / maxLength;
    std::vector<double> accepted;
    for (double f : soft)
    {
        auto clear = [&](const std::vector<double>& list) {
            return std::all_of(list.begin(), list.end(), [&](double x) { return std::abs(x - f) > tol; });
        };
        if (clear(breaks) && clear(accepted))
            accepted.push_back(f);
    }
    breaks.insert(breaks.end(), accepted.begin(), accepted.end());
    std::sort(breaks.begin(), breaks.end());

    std::vector<double> samples;
    for (size_t i = 0; i + 1 < breaks.size(); ++i)
    {
        const double d = breaks[i + 1] - breaks[i];
        const int n = std::max(1, int(std::lround(maxLength * d / h)));
        for (int k = 0; k < n; ++k)
            samples.push_back(breaks[i] + d * k / n);
    }
    samples.push_back(1.0);
    return samples;
}

/** Seam slots per sample index; a sample on a junction of non-adjacent runs sews both run ends. */
struct SeamPlan
{
    std::vector<std::vector<int>> a, b;
    int seam = -1;
};

Vec3 placePoint(const Panel& p, const P2& q)
{
    const double x = q.x, y = q.y;
    const Vec3 &o = p.origin, &r = p.right, &u = p.up;
    if (std::abs(p.wrapRadius) < 1e-6f)
        return {float(o[0] + x * r[0] + y * u[0]), float(o[1] + x * r[1] + y * u[1]), float(o[2] + x * r[2] + y * u[2])};
    auto normalized = [](double a, double b, double c) {
        const double l = std::sqrt(a * a + b * b + c * c);
        return l > 0.0 ? std::array<double, 3>{a / l, b / l, c / l} : std::array<double, 3>{0.0, 0.0, 0.0};
    };
    const auto rn = normalized(r[0], r[1], r[2]);
    const auto n = normalized(r[1] * u[2] - r[2] * u[1], r[2] * u[0] - r[0] * u[2], r[0] * u[1] - r[1] * u[0]);
    // Positive radius curls the panel away from its facing side (right x up), around a body behind it.
    const double R = p.wrapRadius, th = x / R;
    const double along = R * std::sin(th), back = R * (std::cos(th) - 1.0);
    Vec3 out{};
    for (int k = 0; k < 3; ++k)
        out[k] = float(o[k] + rn[k] * along + n[k] * back + u[k] * y);
    return out;
}

} // namespace

Vec3 placePatternPoint(const Panel& p, const Vec2& at) { return placePoint(p, toP2(at)); }

ClothMesh buildClothMesh(const Garment& g)
{
    ClothMesh mesh;
    const int np = int(g.panels.size());
    std::vector<ArcPoly> outlines;
    outlines.reserve(np);
    std::vector<char> usable(np, 0);
    for (int p = 0; p < np; ++p)
    {
        const Panel& panel = g.panels[p];
        outlines.emplace_back(panel.outline, true);
        usable[p] = panel.state != PanelState::Deactivated && panel.outline.size() >= 3 && outlines[p].length > 1e-9;
    }

    std::vector<std::vector<LoopNode>> seamNodes(np);
    std::vector<Claims> claims(np);
    std::vector<SeamPlan> plans;
    int slotCount = 0;
    std::vector<std::pair<Seam, int>> seams;
    for (size_t si = 0; si < g.seams.size(); ++si)
        for (Seam& part : splitSeamAtNotches(g, g.seams[si]))
            seams.emplace_back(std::move(part), int(si));
    for (const auto& [seam, seamIndex] : seams)
    {
        Chain ca, cb;
        if (!buildChain(seam.a, g, outlines, usable, ca) || !buildChain(seam.b, g, outlines, usable, cb))
            continue;

        // A seam may not reuse outline already sewn by an earlier seam, nor overlap itself.
        std::vector<const Run*> runs;
        for (const Run& r : ca.runs)
            runs.push_back(&r);
        for (const Run& r : cb.runs)
            runs.push_back(&r);
        bool clash = false;
        for (size_t i = 0; i < runs.size() && !clash; ++i)
        {
            const double L = outlines[runs[i]->panel].length;
            for (const auto& c : claims[runs[i]->panel])
                clash |= overlaps(*runs[i], c, L);
            for (size_t j = 0; j < i; ++j)
                if (runs[j]->panel == runs[i]->panel)
                    clash |= overlaps(*runs[i], {runs[j]->s0, runs[j]->s0 + runs[j]->len}, L);
        }
        if (clash)
            continue;
        double h = 1e30;
        for (const Run* r : runs)
        {
            claims[r->panel].push_back({r->s0, r->s0 + r->len});
            h = std::min(h, std::max(double(g.panels[r->panel].particleDistance), 1e-4));
        }

        std::vector<double> hard, soft;
        chainBreaks(ca, outlines, false, hard, soft);
        chainBreaks(cb, outlines, seam.reversed, hard, soft);
        const std::vector<double> samples = seamSamples(hard, soft, std::max(ca.length, cb.length), h);

        auto place = [&](const Chain& chain, double f, std::vector<int>& slots) {
            const double t = f * chain.length, e = chain.length * 1e-9;
            double cum = 0.0;
            for (const Run& r : chain.runs)
            {
                if (t >= cum - e && t <= cum + r.len + e)
                {
                    const double s = r.s0 + std::clamp(t - cum, 0.0, r.len);
                    seamNodes[r.panel].push_back({wrapParam(s, outlines[r.panel].length), {slotCount}});
                    slots.push_back(slotCount++);
                }
                cum += r.len;
            }
        };
        SeamPlan plan;
        plan.seam = seamIndex;
        for (double f : samples)
        {
            plan.a.emplace_back();
            plan.b.emplace_back();
            place(ca, f, plan.a.back());
            place(cb, seam.reversed ? 1.0 - f : f, plan.b.back());
        }
        plans.push_back(std::move(plan));
    }

    std::vector<int> slotVertex(slotCount, -1);
    for (int p = 0; p < np; ++p)
    {
        if (!usable[p])
            continue;
        const Panel& panel = g.panels[p];
        const PanelMesh pm = meshPanel(panel, outlines[p], std::move(seamNodes[p]), claims[p]);
        const int base = int(mesh.rest3d.size());
        for (const P2& v : pm.verts)
        {
            mesh.rest3d.push_back(placePoint(panel, v));
            mesh.uv.push_back({float(v.x), float(v.y)});
            mesh.vertPanel.push_back(p);
        }
        for (const auto& t : pm.tris)
        {
            mesh.tris.push_back({base + t[0], base + t[1], base + t[2]});
            mesh.triPanel.push_back(p);
        }
        for (size_t k = 0; k < pm.creases.size(); ++k)
        {
            mesh.creaseEdges.push_back({base + pm.creases[k][0], base + pm.creases[k][1]});
            mesh.creaseAngles.push_back(pm.creaseAngles[k]);
        }
        for (const auto& [slot, v] : pm.slotVerts)
            slotVertex[slot] = base + v;
    }

    std::set<std::pair<int, int>> seen;
    for (const SeamPlan& plan : plans)
        for (size_t k = 0; k < plan.a.size(); ++k)
            for (int sa : plan.a[k])
                for (int sb : plan.b[k])
                {
                    const int va = slotVertex[sa], vb = slotVertex[sb];
                    if (va < 0 || vb < 0 || va == vb || !seen.insert(std::minmax(va, vb)).second)
                        continue;
                    mesh.seamPairs.push_back({va, vb});
                    mesh.seamRestLength.push_back(0.f);
                    mesh.seamOfPair.push_back(plan.seam);
                }
    return mesh;
}

} // namespace tailor
