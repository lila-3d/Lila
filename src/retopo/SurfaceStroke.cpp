#include "retopo/SurfaceStroke.h"

#include "core/Profile.h"

#include <algorithm>
#include <cmath>

namespace lila::retopo
{
namespace
{
glm::dvec3 onSurface(const geo::MeshBvh* surface, const glm::dvec3& p)
{
    geo::BvhClosest c;
    return surface && !surface->empty() && surface->closestPoint(p, c) ? c.point : p;
}

double strokeTurnDegrees(const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c)
{
    glm::dvec3 u = b - a, v = c - b;
    double lu = glm::length(u), lv = glm::length(v);
    if (lu <= 0.0 || lv <= 0.0)
        return 0.0;
    return glm::degrees(std::acos(std::clamp(glm::dot(u, v) / (lu * lv), -1.0, 1.0)));
}
} // namespace

double polylineLength(std::span<const glm::dvec3> points, bool closed)
{
    double len = 0.0;
    for (size_t i = 1; i < points.size(); ++i)
        len += glm::length(points[i] - points[i - 1]);
    if (closed && points.size() > 2)
        len += glm::length(points.front() - points.back());
    return len;
}

glm::dvec3 pointAlong(std::span<const glm::dvec3> points, double fraction, bool closed)
{
    if (points.empty())
        return glm::dvec3(0.0);
    double total = polylineLength(points, closed);
    if (total <= 0.0)
        return points.front();
    double want = std::clamp(fraction, 0.0, 1.0) * total;
    size_t n = points.size(), segs = closed ? n : n - 1;
    for (size_t i = 0; i < segs; ++i)
    {
        const glm::dvec3& a = points[i];
        const glm::dvec3& b = points[(i + 1) % n];
        double len = glm::length(b - a);
        if (want <= len || i + 1 == segs)
            return len > 0.0 ? glm::mix(a, b, std::clamp(want / len, 0.0, 1.0)) : a;
        want -= len;
    }
    return points.back();
}

std::vector<glm::dvec3> resamplePolyline(std::span<const glm::dvec3> points, int segments, bool closed, const geo::MeshBvh* surface)
{
    LILA_ZONE();

    std::vector<glm::dvec3> out;
    if (points.empty() || segments < 1)
        return out;
    double total = polylineLength(points, closed);
    size_t n = points.size(), segs = closed ? n : n - 1;
    int count = closed ? segments : segments + 1;
    out.reserve(count);
    size_t seg = 0;
    double segStart = 0.0;
    for (int k = 0; k < count; ++k)
    {
        if (!closed && k == count - 1)
        {
            out.push_back(points.back());
            break;
        }
        double want = total * double(k) / double(segments);
        while (seg < segs)
        {
            double len = glm::length(points[(seg + 1) % n] - points[seg]);
            if (want <= segStart + len || seg + 1 == segs)
            {
                double t = len > 0.0 ? std::clamp((want - segStart) / len, 0.0, 1.0) : 0.0;
                out.push_back(glm::mix(points[seg], points[(seg + 1) % n], t));
                break;
            }
            segStart += len;
            ++seg;
        }
        if (seg >= segs)
            out.push_back(points.back());
    }
    if (surface)
        for (size_t i = 0; i < out.size(); ++i)
        {
            // Ends of an open polyline are exact on purpose (they may be existing vertices).
            if (!closed && (i == 0 || i + 1 == out.size()))
                continue;
            out[i] = onSurface(surface, out[i]);
        }
    return out;
}

std::vector<int> findCorners(std::span<const glm::dvec3> points, bool closed, double angleDegrees, int window)
{
    std::vector<int> corners;
    const int n = int(points.size());
    if (angleDegrees < 0.0 || n < 3)
        return corners;
    window = std::max(1, window);
    std::vector<double> turn(n, 0.0);
    for (int i = 0; i < n; ++i)
    {
        int a = i - window, b = i + window;
        if (!closed)
        {
            if (i == 0 || i == n - 1)
                continue;
            a = std::max(a, 0);
            b = std::min(b, n - 1);
        }
        turn[i] = strokeTurnDegrees(points[((a % n) + n) % n], points[i], points[((b % n) + n) % n]);
    }
    // One corner per run of points above the angle: the sharpest.
    std::vector<uint8_t> sharp(n, 0);
    for (int i = 0; i < n; ++i)
        sharp[i] = turn[i] >= angleDegrees;
    int start = 0;
    if (closed)
    {
        bool all = std::all_of(sharp.begin(), sharp.end(), [](uint8_t s) { return s; });
        if (all)
            return corners;
        while (sharp[start])
            ++start;
    }
    for (int k = 0; k < n;)
    {
        int i = (start + k) % n;
        if (!sharp[i])
        {
            ++k;
            continue;
        }
        int best = i;
        while (k < n && sharp[(start + k) % n])
        {
            int j = (start + k) % n;
            if (turn[j] > turn[best])
                best = j;
            ++k;
        }
        corners.push_back(best);
    }
    std::sort(corners.begin(), corners.end());
    return corners;
}

SurfaceStroke processStroke(std::span<const glm::dvec3> raw, const geo::MeshBvh* surface, const StrokeOptions& opts)
{
    LILA_ZONE();

    SurfaceStroke s;
    std::vector<glm::dvec3> pts;
    for (const glm::dvec3& p : raw)
        if (pts.empty() || glm::length(p - pts.back()) > 1e-9)
            pts.push_back(p);
    if (pts.size() < 2)
        return s;
    double total = polylineLength(pts);
    if (total <= 0.0)
        return s;
    s.closed = opts.closeDistance > 0.0 && pts.size() > 3 && glm::length(pts.back() - pts.front()) <= opts.closeDistance &&
               total > opts.closeDistance * 4.0;
    if (s.closed)
        pts.pop_back();
    double spacing = opts.spacing > 0.0 ? opts.spacing : total / 50.0;
    int segments = std::max(2, int(std::lround(polylineLength(pts, s.closed) / spacing)));
    std::vector<glm::dvec3> even = resamplePolyline(pts, segments, s.closed, nullptr);

    // Corners from the unsmoothed points, so smoothing can't round them away.
    std::vector<int> corners = findCorners(even, s.closed, opts.cornerAngle, opts.cornerWindow);
    std::vector<uint8_t> held(even.size(), 0);
    for (int c : corners)
        held[c] = 1;
    if (!s.closed)
        held.front() = held.back() = 1;
    const size_t n = even.size();
    for (int pass = 0; pass < opts.smoothing; ++pass)
    {
        std::vector<glm::dvec3> next = even;
        for (size_t i = 0; i < n; ++i)
        {
            if (held[i])
                continue;
            const glm::dvec3& a = even[(i + n - 1) % n];
            const glm::dvec3& b = even[(i + 1) % n];
            next[i] = glm::mix(even[i], (a + b) * 0.5, 0.5);
        }
        even.swap(next);
    }
    for (size_t i = 0; i < n; ++i)
        if (!(!s.closed && (i == 0 || i + 1 == n)))
            even[i] = onSurface(surface, even[i]);
    s.points = std::move(even);
    s.corners = std::move(corners);
    return s;
}

int nearestPointIndex(std::span<const glm::dvec3> points, const glm::dvec3& p)
{
    int best = -1;
    double bestD = 0.0;
    for (size_t i = 0; i < points.size(); ++i)
        if (double d = glm::length(points[i] - p); best < 0 || d < bestD)
        {
            best = int(i);
            bestD = d;
        }
    return best;
}

std::vector<glm::dvec3> polylineSpan(std::span<const glm::dvec3> points, int a, int b, bool closed)
{
    std::vector<glm::dvec3> out;
    const int n = int(points.size());
    if (n == 0 || a < 0 || b < 0 || a >= n || b >= n)
        return out;
    if (!closed)
    {
        int step = a <= b ? 1 : -1;
        for (int i = a;; i += step)
        {
            out.push_back(points[i]);
            if (i == b)
                break;
        }
        return out;
    }
    for (int i = a;; i = (i + 1) % n)
    {
        out.push_back(points[i]);
        if (i == b)
            break;
    }
    return out;
}
} // namespace lila::retopo
