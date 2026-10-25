#include "curve/Cable.h"

#include "core/Profile.h"
#include "curve/Fit.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <format>

namespace lila::curve
{
namespace
{
using D3 = glm::dvec3;

D3 downOf(const D3& gravity)
{
    double l = glm::length(gravity);
    return l > 1e-12 ? gravity / l : D3(0, -1, 0);
}

std::vector<D3> straight(const D3& a, const D3& b, int samples)
{
    std::vector<D3> out;
    for (int i = 0; i <= samples; ++i)
        out.push_back(glm::mix(a, b, double(i) / samples));
    return out;
}

/** Each segment split into parts pieces. */
std::vector<D3> densify(const std::vector<D3>& pts, int parts)
{
    std::vector<D3> out;
    for (size_t i = 0; i + 1 < pts.size(); ++i)
        for (int k = 0; k < parts; ++k)
            out.push_back(glm::mix(pts[i], pts[i + 1], double(k) / parts));
    if (!pts.empty())
        out.push_back(pts.back());
    return out;
}

/** z with sinh(z) / z = k, for k > 1 (by bisection, so it never runs away). */
double solveShape(double k)
{
    double lo = 0.0, hi = 1.0;
    while (hi < 700.0 && std::sinh(hi) / hi < k)
        hi *= 2.0;
    hi = std::min(hi, 700.0);
    for (int it = 0; it < 200; ++it)
    {
        double mid = 0.5 * (lo + hi);
        if (std::sinh(mid) / mid < k)
            lo = mid;
        else
            hi = mid;
    }
    return 0.5 * (lo + hi);
}

/** A deterministic stream of numbers in [0, 1), the same on every platform. */
struct Random
{
    uint64_t state;

    double next()
    {
        uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        return double(z >> 11) * (1.0 / 9007199254740992.0);
    }
};

/** Whether p is inside a surface or nearer to it than the clearance, and where it should go instead. */
std::optional<D3> pushOut(const D3& p, const CableOptions& o)
{
    std::optional<SurfacePoint> c = o.closest(p);
    if (!c)
        return std::nullopt;
    if (glm::dot(p - c->position, c->normal) >= o.clearance - 1e-9)
        return std::nullopt;
    return c->position + c->normal * o.clearance;
}

/**
 * Lets a hanging cable settle onto the surfaces it passes into: position-based dynamics on the sampled points, its ends
 * pinned, its segments kept at their rest length and each point held out of the surfaces. Untouched when nothing collides.
 */
std::vector<D3> drape(std::vector<D3> pts, double length, const CableOptions& o)
{
    LILA_ZONE();

    const size_t n = pts.size();
    if (n < 3 || !o.closest)
        return pts;
    bool hits = false;
    for (size_t i = 1; i + 1 < n && !hits; ++i)
        hits = pushOut(pts[i], o).has_value();
    if (!hits)
        return pts;
    const double rest = length / double(n - 1);
    const D3 down = downOf(o.gravity);
    for (int it = 0; it < std::max(1, o.iterations); ++it)
    {
        for (size_t i = 1; i + 1 < n; ++i)
            pts[i] += down * rest * 0.1;
        for (int pass = 0; pass < 6; ++pass)
            for (size_t j = 0; j + 1 < n; ++j)
            {
                D3 d = pts[j + 1] - pts[j];
                double len = glm::length(d);
                if (len < 1e-12)
                    continue;
                D3 fix = d * ((len - rest) / len);
                const bool pinA = j == 0, pinB = j + 2 == n;
                if (pinA && pinB)
                    continue;
                if (pinA)
                    pts[j + 1] -= fix;
                else if (pinB)
                    pts[j] += fix;
                else
                {
                    pts[j] += fix * 0.5;
                    pts[j + 1] -= fix * 0.5;
                }
            }
        for (size_t i = 1; i + 1 < n; ++i)
            if (auto q = pushOut(pts[i], o))
                pts[i] = *q;
    }
    return pts;
}
} // namespace

std::vector<D3> catenary(const D3& a, const D3& b, const D3& gravity, double length, int samples)
{
    samples = std::clamp(samples, 1, 4096);
    const D3 up = -downOf(gravity);
    const D3 span = b - a;
    const double chord = glm::length(span);
    const double h = glm::dot(span, up);
    const D3 across = span - up * h;
    const double d = glm::length(across);
    if (!(length > chord * (1.0 + 1e-9)) || d < 1e-9 * std::max(chord, 1.0))
        return straight(a, b, samples);
    const D3 e = across / d;
    // L^2 - h^2 = (2c sinh(d / 2c))^2 for the catenary y = c cosh((x - x1) / c) + k through both ends.
    const double z = solveShape(std::sqrt(length * length - h * h) / d);
    const double c = d / (2.0 * z);
    const double x1 = d / 2.0 - c * std::atanh(std::clamp(h / length, -1.0 + 1e-15, 1.0 - 1e-15));
    const double s0 = std::sinh(-x1 / c);
    const double y0 = std::cosh(-x1 / c);
    std::vector<D3> out;
    out.reserve(size_t(samples) + 1);
    for (int i = 0; i <= samples; ++i)
    {
        // Evenly spaced along the cable, so steep sides of a deep sag get as many points as the bottom.
        const double s = length * double(i) / samples;
        const double x = x1 + c * std::asinh(s / c + s0);
        const double y = c * (std::cosh((x - x1) / c) - y0);
        out.push_back(a + e * x + up * y);
    }
    out.front() = a;
    out.back() = b;
    return out;
}

double sagOf(const std::vector<D3>& pts, const D3& gravity)
{
    if (pts.size() < 3)
        return 0.0;
    const D3 up = -downOf(gravity);
    const D3 a = pts.front(), span = pts.back() - a;
    const double h = glm::dot(span, up);
    const D3 across = span - up * h;
    const double d = glm::length(across);
    if (d < 1e-12)
        return 0.0;
    const D3 e = across / d;
    double sag = 0.0;
    for (const D3& p : pts)
    {
        const double x = glm::dot(p - a, e), y = glm::dot(p - a, up);
        sag = std::max(sag, h * x / d - y);
    }
    return sag;
}

double lengthForSag(const D3& a, const D3& b, const D3& gravity, double depth)
{
    const double chord = glm::distance(a, b);
    if (!(depth > 0.0) || chord < 1e-12)
        return chord;
    auto sagAt = [&](double len) { return sagOf(catenary(a, b, gravity, len, 64), gravity); };
    double lo = chord, hi = chord * 1.5 + depth;
    for (int k = 0; k < 60 && sagAt(hi) < depth; ++k)
        hi *= 2.0;
    for (int it = 0; it < 60; ++it)
    {
        const double mid = 0.5 * (lo + hi);
        (sagAt(mid) < depth ? lo : hi) = mid;
    }
    return 0.5 * (lo + hi);
}

CableResult makeCables(const std::vector<D3>& anchors, bool closed, const CableOptions& o)
{
    LILA_ZONE();

    CableResult r;
    if (anchors.size() < 2)
    {
        r.warnings.push_back("a cable needs two anchor points");
        return r;
    }
    const int count = std::clamp(o.count, 1, 256);
    const int samples = std::clamp(o.samples, 4, 1024);
    const D3 up = -downOf(o.gravity);
    const size_t spans = closed && anchors.size() > 2 ? anchors.size() : anchors.size() - 1;
    int vertical = 0, coincident = 0;
    for (size_t si = 0; si < spans; ++si)
    {
        const D3 a = anchors[si], b = anchors[(si + 1) % anchors.size()];
        const double chord = glm::distance(a, b);
        if (chord < 1e-6)
        {
            ++coincident;
            continue;
        }
        const D3 dir = (b - a) / chord;
        if (glm::length(glm::cross(dir, up)) < 1e-6)
            ++vertical;
        D3 side = glm::cross(dir, up);
        if (glm::length(side) < 1e-6)
            side = glm::cross(dir, std::abs(dir.x) < 0.9 ? D3(1, 0, 0) : D3(0, 0, 1));
        side = glm::normalize(side);
        const D3 lift = glm::cross(side, dir);
        for (int ci = 0; ci < count; ++ci)
        {
            Random rng{uint64_t(o.seed) * 0x100000001B3ull + si * 7919u + uint64_t(ci) * 104729u};
            D3 ea = a, eb = b;
            double vary = 1.0;
            if (count > 1)
            {
                for (D3* end : {&ea, &eb})
                {
                    const double rad = std::max(0.0, o.spread) * std::sqrt(rng.next()), ang = glm::two_pi<double>() * rng.next();
                    *end += (side * std::cos(ang) + lift * std::sin(ang)) * rad;
                }
                vary = std::max(0.0, 1.0 + std::max(0.0, o.sagVariation) * (2.0 * rng.next() - 1.0));
            }
            const double len = o.mode == SagMode::Slack ? glm::distance(ea, eb) * (1.0 + std::max(0.0, o.slack) * vary)
                                                        : lengthForSag(ea, eb, o.gravity, std::max(0.0, o.sag) * vary);
            std::vector<D3> pts = catenary(ea, eb, o.gravity, len, samples);
            double tol = o.tolerance;
            if (o.drape && o.closest)
            {
                pts = drape(std::move(pts), len, o);
                // Where it meets a surface it bends sharply: fitted tighter and checked between samples too, so it stays out.
                pts = densify(pts, 4);
                tol = std::min(tol, std::max(o.clearance * 0.05, 1e-3));
            }
            Spline s = fitBezier(pts, false, std::max(tol, 1e-4));
            if (isValid(s))
                r.cables.push_back(std::move(s));
        }
    }
    if (coincident)
        r.warnings.push_back(std::format("{} span(s) between coincident anchors skipped", coincident));
    if (vertical)
        r.warnings.push_back(std::format("{} span(s) run straight along gravity, so they cannot sag", vertical));
    return r;
}
} // namespace lila::curve
