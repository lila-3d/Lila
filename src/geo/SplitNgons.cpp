#include "geo/Ops.h"

#include "core/Profile.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace lila::geo
{
namespace
{
constexpr double kTwoPi = glm::two_pi<double>();

double ngonCross2(const glm::dvec2& a, const glm::dvec2& b) { return a.x * b.y - a.y * b.x; }

/** Counter-clockwise angle from a to b, in [0, 2pi). */
double ccwAngle(const glm::dvec2& a, const glm::dvec2& b)
{
    double t = std::atan2(ngonCross2(a, b), glm::dot(a, b));
    return t < 0.0 ? t + kTwoPi : t;
}

bool segmentsCross(const glm::dvec2& a, const glm::dvec2& b, const glm::dvec2& c, const glm::dvec2& d, double eps)
{
    double d1 = ngonCross2(b - a, c - a), d2 = ngonCross2(b - a, d - a);
    double d3 = ngonCross2(d - c, a - c), d4 = ngonCross2(d - c, b - c);
    return ((d1 > eps && d2 < -eps) || (d1 < -eps && d2 > eps)) && ((d3 > eps && d4 < -eps) || (d3 < -eps && d4 > eps));
}

double pointSegmentDistance(const glm::dvec2& p, const glm::dvec2& a, const glm::dvec2& b)
{
    glm::dvec2 ab = b - a;
    double t = std::clamp(glm::dot(p - a, ab) / std::max(glm::dot(ab, ab), 1e-300), 0.0, 1.0);
    return glm::length(p - (a + t * ab));
}

/** Smallest sine of the corner angles of a polygon given in order; 0 when any corner is reflex. */
double shapeQuality(std::span<const glm::dvec2> p)
{
    const size_t n = p.size();
    double q = 1.0;
    for (size_t i = 0; i < n; ++i)
    {
        glm::dvec2 a = p[(i + n - 1) % n] - p[i], b = p[(i + 1) % n] - p[i];
        double la = glm::length(a), lb = glm::length(b);
        if (la < 1e-300 || lb < 1e-300)
            return 0.0;
        double s = ngonCross2(b, a) / (la * lb);
        if (s <= 0.0)
            return 0.0;
        q = std::min(q, s);
    }
    return q;
}

struct Chord
{
    size_t i = 0, j = 0;
    double score = -1e300;
};

/**
 * The best cut of one polygon. A cut scores for carrying on an edge that arrives at either end from outside the face,
 * for leaving both pieces with an even side count, for cutting off a well shaped quad, for not making thin corners,
 * and for being short.
 */
Chord bestChord(const Mesh& mesh, Index f, std::span<const Index> corners, double minAngle)
{
    const size_t n = corners.size();
    std::vector<Index> pts(n);
    std::vector<glm::dvec3> pos(n);
    for (size_t k = 0; k < n; ++k)
    {
        pts[k] = mesh.cornerPoint(corners[k]);
        pos[k] = mesh.position(pts[k]);
    }
    glm::dvec3 nrm = mesh.faceAreaVector(f);
    double area = glm::length(nrm);
    if (area < 1e-300)
        return {};
    nrm /= area;
    glm::dvec3 ax = pos[1] - pos[0];
    ax -= nrm * glm::dot(nrm, ax);
    if (glm::length(ax) < 1e-300)
        ax = std::abs(nrm.x) < 0.6 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 1, 0);
    ax = glm::normalize(ax - nrm * glm::dot(nrm, ax));
    glm::dvec3 ay = glm::cross(nrm, ax);
    std::vector<glm::dvec2> p(n);
    double perimeter = 0.0;
    for (size_t k = 0; k < n; ++k)
    {
        glm::dvec3 d = pos[k] - pos[0];
        p[k] = {glm::dot(d, ax), glm::dot(d, ay)};
    }
    for (size_t k = 0; k < n; ++k)
        perimeter += glm::length(p[(k + 1) % n] - p[k]);
    const double meanEdge = perimeter / double(n);
    const double eps = 1e-9 * meanEdge * meanEdge;

    // Directions arriving at each point along edges outside this face, flattened into the face plane.
    std::vector<std::vector<glm::dvec2>> arriving(n);
    for (size_t k = 0; k < n; ++k)
    {
        Index prev = pts[(k + n - 1) % n], next = pts[(k + 1) % n];
        mesh.forEachPointEdge(pts[k], [&](Index e) {
            Index o = mesh.edgeOtherPoint(e, pts[k]);
            if (o == prev || o == next)
                return;
            glm::dvec3 d = pos[k] - mesh.position(o);
            glm::dvec2 d2(glm::dot(d, ax), glm::dot(d, ay));
            double l = glm::length(d2);
            if (l > 1e-12 * meanEdge)
                arriving[k].push_back(d2 / l);
        });
    }
    auto carryOn = [&](size_t k, const glm::dvec2& dir) {
        double best = 0.0;
        for (const glm::dvec2& a : arriving[k])
            best = std::max(best, glm::dot(a, dir));
        return best * best;
    };

    const double minRad = glm::radians(minAngle);
    const bool evenTotal = n % 2 == 0;
    Chord best;
    std::vector<glm::dvec2> piece;
    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 2; j < n; ++j)
        {
            if (i == 0 && j == n - 1)
                continue;
            if (mesh.findEdge(pts[i], pts[j]) != kInvalid || pts[i] == pts[j])
                continue;
            glm::dvec2 c = p[j] - p[i];
            double len = glm::length(c);
            if (len < 1e-12 * meanEdge)
                continue;
            glm::dvec2 dir = c / len;

            // Inside the corner wedge at both ends, and the corners left on both sides open enough.
            double smallest = kTwoPi;
            bool inside = true;
            for (int end = 0; end < 2 && inside; ++end)
            {
                size_t k = end == 0 ? i : j;
                glm::dvec2 v = end == 0 ? dir : -dir;
                glm::dvec2 toNext = p[(k + 1) % n] - p[k], toPrev = p[(k + n - 1) % n] - p[k];
                double wedge = ccwAngle(toNext, toPrev);
                double a = ccwAngle(toNext, v);
                if (a <= 0.0 || a >= wedge)
                    inside = false;
                smallest = std::min({smallest, a, wedge - a});
            }
            if (!inside || smallest < minRad)
                continue;
            bool clear = true;
            for (size_t k = 0; k < n && clear; ++k)
            {
                size_t k1 = (k + 1) % n;
                if (k != i && k != j && k1 != i && k1 != j && segmentsCross(p[i], p[j], p[k], p[k1], eps))
                    clear = false;
                if (k != i && k != j && pointSegmentDistance(p[k], p[i], p[j]) < 1e-6 * meanEdge)
                    clear = false;
            }
            if (!clear)
                continue;

            double score = 2.0 * (carryOn(i, dir) + carryOn(j, -dir));
            size_t a = j - i + 1, b = n - (j - i) + 1;
            if (evenTotal && a % 2 == 0 && b % 2 == 0)
                score += 1.0;
            for (int side = 0; side < 2; ++side)
            {
                size_t size = side == 0 ? a : b;
                if (size > 4)
                    continue;
                piece.clear();
                for (size_t k = 0; k < size; ++k)
                    piece.push_back(p[side == 0 ? i + k : (j + k) % n]);
                double q = shapeQuality(piece);
                score += size == 4 ? 1.0 * q : 0.2 * q;
            }
            score += 0.5 * std::sin(std::min(smallest, glm::half_pi<double>()));
            score -= 0.3 * len / meanEdge;
            if (score > best.score)
                best = {i, j, score};
        }
    return best;
}
} // namespace

std::vector<Index> splitNgons(Mesh& mesh, std::span<const Index> faces, const NgonSplitOptions& opts)
{
    LILA_ZONE();

    const Index maxSides = Index(std::max(opts.maxSides, 3));
    std::vector<Index> work(faces.begin(), faces.end()), out;
    while (!work.empty())
    {
        Index f = work.back();
        work.pop_back();
        if (!mesh.faceValid(f))
            continue;
        std::vector<Index> corners = mesh.faceCorners(f);
        if (Index(corners.size()) <= maxSides)
        {
            out.push_back(f);
            continue;
        }
        Chord c = bestChord(mesh, f, corners, opts.minAngle);
        if (c.score <= -1e300)
        {
            out.push_back(f);
            continue;
        }
        Index g = mesh.splitFace(f, corners[c.i], corners[c.j]);
        work.push_back(f);
        if (g != kInvalid)
            work.push_back(g);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}
} // namespace lila::geo
