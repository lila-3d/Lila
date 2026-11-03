#include "curve/Planar.h"

#include "core/Profile.h"
#include "curve/CurveMath.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace lila::curve
{
namespace
{
D3 newell(const std::vector<D3>& pts)
{
    D3 n(0.0);
    for (size_t i = 0; i < pts.size(); ++i)
    {
        const D3& a = pts[i];
        const D3& b = pts[(i + 1) % pts.size()];
        n += D3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y));
    }
    return n;
}

int orientSign(const D2& a, const D2& b, const D2& c)
{
    double v = cross2(b - a, c - a);
    double scale = std::max({std::abs(b.x - a.x), std::abs(b.y - a.y), std::abs(c.x - a.x), std::abs(c.y - a.y), 1e-300});
    if (std::abs(v) <= 1e-14 * scale * scale)
        return 0;
    return v > 0 ? 1 : -1;
}

bool onSegment(const D2& a, const D2& b, const D2& p)
{
    return std::min(a.x, b.x) <= p.x && p.x <= std::max(a.x, b.x) && std::min(a.y, b.y) <= p.y && p.y <= std::max(a.y, b.y);
}

/** True when every point lies on one line (a figure eight's signed area is zero too, so this looks at the spread). */
bool collinear(const std::vector<D2>& pts, double tol)
{
    size_t far = 0;
    for (size_t i = 1; i < pts.size(); ++i)
        if (glm::length(pts[i] - pts[0]) > glm::length(pts[far] - pts[0]))
            far = i;
    D2 d = pts[far] - pts[0];
    double len = glm::length(d);
    if (len <= tol)
        return true;
    d /= len;
    for (const D2& p : pts)
        if (std::abs(cross2(p - pts[0], d)) > tol)
            return false;
    return true;
}

/** Points just inside a loop by the middles of its three longest edges: loops that touch at a vertex still nest right. */
std::vector<D2> insideProbes(const std::vector<D2>& pts)
{
    std::vector<std::pair<double, size_t>> edges;
    for (size_t i = 0; i < pts.size(); ++i)
        edges.push_back({glm::length(pts[(i + 1) % pts.size()] - pts[i]), i});
    std::sort(edges.begin(), edges.end(), std::greater<>());
    double side = signedArea(pts) > 0.0 ? 1.0 : -1.0;
    std::vector<D2> out;
    for (size_t k = 0; k < std::min<size_t>(3, edges.size()); ++k)
    {
        size_t i = edges[k].second;
        D2 a = pts[i], b = pts[(i + 1) % pts.size()];
        D2 d = b - a;
        out.push_back((a + b) * 0.5 + D2(-d.y, d.x) * (side * 1e-6));
    }
    return out;
}

bool segmentsTouch(const D2& a, const D2& b, const D2& c, const D2& d)
{
    int o1 = orientSign(a, b, c), o2 = orientSign(a, b, d), o3 = orientSign(c, d, a), o4 = orientSign(c, d, b);
    if (o1 != o2 && o3 != o4)
        return true;
    return (o1 == 0 && onSegment(a, b, c)) || (o2 == 0 && onSegment(a, b, d)) || (o3 == 0 && onSegment(c, d, a)) || (o4 == 0 && onSegment(c, d, b));
}
} // namespace

PlaneFrame planeFrame(const glm::dvec3& origin, const glm::dvec3& normal)
{
    PlaneFrame f;
    f.origin = origin;
    double len = glm::length(normal);
    f.normal = len > 1e-12 ? normal / len : D3(0.0, 0.0, 1.0);
    const D3& n = f.normal;
    // Ground plane curves get x along world x and y along world -z, so 2D reads like a top view.
    D3 ref = std::abs(n.x) < 0.9 ? D3(1.0, 0.0, 0.0) : D3(0.0, 0.0, -1.0);
    f.x = glm::normalize(ref - n * glm::dot(ref, n));
    f.y = glm::cross(n, f.x);
    return f;
}

PlaneFrame fitPlane(std::span<const Spline> curves, const glm::dvec3& normal, double* deviation)
{
    LILA_ZONE();

    TessOptions to;
    to.chordError = 0.01;
    std::vector<std::vector<D3>> polys;
    D3 centroid(0.0);
    size_t count = 0;
    for (const Spline& s : curves)
    {
        if (spanCount(s) == 0)
            continue;
        Polyline pl = tessellate(s, to);
        for (const D3& p : pl.points)
            centroid += p;
        count += pl.size();
        polys.push_back(std::move(pl.points));
    }
    if (count)
        centroid /= double(count);
    D3 n = normal;
    if (glm::length(n) < 1e-12)
    {
        n = D3(0.0);
        D3 ref(0.0);
        for (const std::vector<D3>& pts : polys)
        {
            D3 m = newell(pts);
            if (glm::length(ref) < 1e-12)
                ref = m;
            n += glm::dot(m, ref) < 0.0 ? -m : m;
        }
        if (glm::length(n) < 1e-12)
        {
            // Open or straight curves: the plane through the spread of the points.
            std::vector<D3> all;
            for (const std::vector<D3>& pts : polys)
                all.insert(all.end(), pts.begin(), pts.end());
            n = planeNormal(all, 1e30);
        }
        if (glm::length(n) < 1e-12)
            n = D3(0.0, 1.0, 0.0);
        n = glm::normalize(n);
        int big = 0;
        for (int k = 1; k < 3; ++k)
            if (std::abs(n[k]) > std::abs(n[big]) + 1e-9)
                big = k;
        if (n[big] < 0.0)
            n = -n;
    }
    PlaneFrame f = planeFrame(centroid, n);
    if (deviation)
    {
        double worst = 0.0;
        for (const std::vector<D3>& pts : polys)
            for (const D3& p : pts)
                worst = std::max(worst, std::abs(f.height(p)));
        *deviation = worst;
    }
    return f;
}

double signedArea(std::span<const glm::dvec2> pts)
{
    double a = 0.0;
    for (size_t i = 0; i < pts.size(); ++i)
        a += cross2(pts[i], pts[(i + 1) % pts.size()]);
    return a * 0.5;
}

bool pointInPolygon(const glm::dvec2& p, std::span<const glm::dvec2> poly)
{
    bool in = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
    {
        const D2& a = poly[i];
        const D2& b = poly[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
            in = !in;
    }
    return in;
}

void reverseLoop(Loop2& l)
{
    std::reverse(l.points.begin() + 1, l.points.end());
    std::reverse(l.params.begin() + 1, l.params.end());
    std::reverse(l.kink.begin() + 1, l.kink.end());
}

Loops2 loopsOf(std::span<const Spline> curves, const PlaneFrame& plane, const TessOptions& tess, std::vector<std::string>* warnings)
{
    LILA_ZONE();

    Loops2 out;
    auto warn = [&](std::string w) {
        if (warnings)
            warnings->push_back(std::move(w));
    };
    for (size_t k = 0; k < curves.size(); ++k)
    {
        const Spline& s = curves[k];
        if (!s.closed)
        {
            warn("curve " + std::to_string(k + 1) + " is open; only closed curves make regions");
            continue;
        }
        if (spanCount(s) == 0)
        {
            warn("curve " + std::to_string(k + 1) + " is too short");
            continue;
        }
        Polyline pl = tessellate(s, tess);
        Loop2 l;
        l.source = int(k);
        double scale = 0.0;
        for (const D3& p : pl.points)
            scale = std::max(scale, glm::length(p - plane.origin));
        double merge = std::max(1e-9, scale * 1e-10);
        for (size_t i = 0; i < pl.size(); ++i)
        {
            D2 q = plane.to2(pl.points[i]);
            if (!l.points.empty() && glm::length(q - l.points.back()) <= merge)
            {
                l.kink.back() = uint8_t(l.kink.back() | pl.kink[i]);
                continue;
            }
            l.points.push_back(q);
            l.params.push_back(pl.params[i]);
            l.kink.push_back(pl.kink[i]);
        }
        while (l.points.size() > 1 && glm::length(l.points.front() - l.points.back()) <= merge)
        {
            l.kink.front() = uint8_t(l.kink.front() | l.kink.back());
            l.points.pop_back();
            l.params.pop_back();
            l.kink.pop_back();
        }
        if (l.points.size() < 3 || collinear(l.points, merge))
        {
            warn("curve " + std::to_string(k + 1) + " encloses no area in the plane");
            continue;
        }
        out.push_back(std::move(l));
    }
    return out;
}

std::vector<Region2> nestLoops(Loops2 loops)
{
    LILA_ZONE();

    size_t n = loops.size();
    std::vector<double> area(n);
    for (size_t i = 0; i < n; ++i)
        area[i] = std::abs(signedArea(loops[i].points));
    // parent: the smallest loop containing this one.
    std::vector<int> parent(n, -1), depth(n, 0);
    std::vector<size_t> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return area[a] > area[b]; });
    for (size_t oi = 0; oi < n; ++oi)
    {
        size_t i = order[oi];
        std::vector<D2> probes = insideProbes(loops[i].points);
        for (size_t oj = oi; oj-- > 0;)
        {
            size_t j = order[oj];
            int votes = 0;
            for (const D2& probe : probes)
                votes += pointInPolygon(probe, loops[j].points) ? 1 : -1;
            if (votes > 0)
            {
                parent[i] = int(j);
                depth[i] = depth[j] + 1;
                break;
            }
        }
    }
    std::vector<Region2> regions;
    std::vector<int> regionOf(n, -1);
    for (size_t oi = 0; oi < n; ++oi)
    {
        size_t i = order[oi];
        Loop2& l = loops[i];
        bool hole = depth[i] % 2 == 1;
        if ((signedArea(l.points) > 0.0) == hole)
            reverseLoop(l);
        if (!hole)
        {
            regionOf[i] = int(regions.size());
            regions.push_back({std::move(l), {}});
        }
        else if (parent[i] >= 0 && regionOf[size_t(parent[i])] >= 0)
            regions[size_t(regionOf[size_t(parent[i])])].holes.push_back(std::move(l));
    }
    return regions;
}

void vectorHandlesOnLines(Spline& s)
{
    if (s.kind != Kind::Bezier)
        return;
    std::vector<Piece> ps = pieces(s);
    for (size_t k = 0; k < ps.size() && k < s.size(); ++k)
        if (isStraight(ps[k], 1e-6))
        {
            size_t next = (k + 1) % s.size();
            s.typeR[k] = HandleType::Vector;
            s.typeL[next] = HandleType::Vector;
        }
}

bool loopsCross(const Loops2& loops)
{
    LILA_ZONE();

    struct Seg
    {
        D2 a, b;
        double lo, hi;
        size_t loop, index, count;
    };
    std::vector<Seg> segs;
    for (size_t l = 0; l < loops.size(); ++l)
    {
        const auto& p = loops[l].points;
        for (size_t i = 0; i < p.size(); ++i)
        {
            const D2& a = p[i];
            const D2& b = p[(i + 1) % p.size()];
            segs.push_back({a, b, std::min(a.x, b.x), std::max(a.x, b.x), l, i, p.size()});
        }
    }
    std::sort(segs.begin(), segs.end(), [](const Seg& x, const Seg& y) { return x.lo < y.lo; });
    for (size_t i = 0; i < segs.size(); ++i)
        for (size_t j = i + 1; j < segs.size() && segs[j].lo <= segs[i].hi; ++j)
        {
            const Seg& s = segs[i];
            const Seg& t = segs[j];
            if (std::max(std::min(s.a.y, s.b.y), std::min(t.a.y, t.b.y)) > std::min(std::max(s.a.y, s.b.y), std::max(t.a.y, t.b.y)))
                continue;
            if (s.loop == t.loop)
            {
                // Neighbours share a point by construction.
                size_t d = (s.index + s.count - t.index) % s.count;
                if (d == 1 || d == s.count - 1)
                    continue;
            }
            if (segmentsTouch(s.a, s.b, t.a, t.b))
                return true;
        }
    return false;
}
} // namespace lila::curve
