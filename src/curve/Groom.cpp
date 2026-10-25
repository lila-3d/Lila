#include "curve/Groom.h"

#include "core/Profile.h"
#include "geo/ColorSets.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>

namespace lila::curve::groom
{
namespace
{
const char* const kBrushNames[] = {"comb", "smooth", "length", "puff", "add", "delete", "density", "cut", "pinch", "slide"};
static_assert(std::size(kBrushNames) == size_t(Brush::Count));

double smoothFalloff(double t)
{
    return t >= 1.0 ? 0.0 : (1.0 - t) * (1.0 - t) * (1.0 + 2.0 * t);
}

/** Rotation taking unit a onto unit b (identity when they agree, a half turn about any perpendicular when opposite). */
glm::dquat rotationBetween(const D3& a, const D3& b)
{
    const double c = glm::dot(a, b);
    if (c > 1.0 - 1e-12)
        return glm::dquat(1.0, 0.0, 0.0, 0.0);
    if (c < -1.0 + 1e-12)
    {
        D3 axis = glm::cross(a, D3(1, 0, 0));
        if (glm::length(axis) < 1e-6)
            axis = glm::cross(a, D3(0, 1, 0));
        return glm::angleAxis(glm::pi<double>(), glm::normalize(axis));
    }
    return glm::rotation(a, b);
}

std::vector<double> segmentLengths(const Spline& s)
{
    std::vector<double> out;
    for (size_t i = 1; i < s.size(); ++i)
        out.push_back(glm::distance(s.points[i - 1], s.points[i]));
    return out;
}

/** Follow the leader from the root: each point back at its old distance from the one before, in its new direction. */
void restoreLengths(Spline& s, const std::vector<double>& lengths)
{
    for (size_t i = 1; i < s.size() && i - 1 < lengths.size(); ++i)
    {
        D3 d = s.points[i] - s.points[i - 1];
        const double l = glm::length(d);
        if (l < 1e-12)
            continue;
        s.points[i] = s.points[i - 1] + d / l * lengths[i - 1];
    }
}

/** Bezier handles follow their anchors after the points moved. */
void settleHandles(Spline& s, const std::vector<D3>& before)
{
    if (s.kind != Kind::Bezier || s.handleL.size() != s.size() || before.size() != s.size())
        return;
    for (size_t i = 0; i < s.size(); ++i)
    {
        const D3 d = s.points[i] - before[i];
        s.handleL[i] += d;
        s.handleR[i] += d;
    }
    updateHandles(s);
}

/** Distance the brush measures: across the view (a cylinder) when it has one, else straight (a sphere). */
double brushDistance(const Dab& d, const D3& p)
{
    const D3 rel = p - d.center;
    if (glm::length(d.viewDir) < 1e-12)
        return glm::length(rel);
    return glm::length(rel - d.viewDir * glm::dot(rel, d.viewDir));
}

struct Weigher
{
    const Settings& o;
    const Dab& d;
    D3 view{0.0};

    double operator()(const D3& p) const
    {
        if (o.frontOnly && glm::length(view) > 0.5 && glm::dot(p - d.center, view) > d.radius)
            return 0.0;
        const double t = brushDistance(d, p) / std::max(d.radius, 1e-12);
        if (t >= 1.0)
            return 0.0;
        const double f = o.falloff ? o.falloff(t) : smoothFalloff(t);
        return std::clamp(f, 0.0, 1.0) * std::clamp(o.strength, 0.0, 1.0) * std::clamp(d.strength, 0.0, 1.0);
    }
};

bool editable(const Settings& o, size_t si) { return o.mask.empty() || (si < o.mask.size() && o.mask[si]); }

D3 rootNormal(const Spline& s, const Surface* surface, const D3& fallback)
{
    if (surface && !s.points.empty())
        if (auto h = surface->closest(s.points.front()))
            return h->normal;
    return fallback;
}

double arcLength(const std::vector<D3>& p)
{
    double l = 0.0;
    for (size_t i = 1; i < p.size(); ++i)
        l += glm::distance(p[i - 1], p[i]);
    return l;
}

void eraseSplines(Splines& s, Settings& o, const std::vector<char>& remove, Stats* stats)
{
    Splines keep;
    std::vector<char> mask;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (remove[i])
        {
            if (stats)
                ++stats->removed;
            continue;
        }
        keep.push_back(std::move(s[i]));
        if (!o.mask.empty())
            mask.push_back(i < o.mask.size() ? o.mask[i] : 1);
    }
    s = std::move(keep);
    if (!o.mask.empty())
        o.mask = std::move(mask);
}

/** A uniformly random point in the disk of the dab, on the plane square to its normal. */
D3 diskPoint(const Dab& d, std::mt19937& rng)
{
    std::uniform_real_distribution<double> u(0.0, 1.0);
    const D3 n = glm::length(d.normal) > 1e-12 ? glm::normalize(d.normal) : D3(0, 1, 0);
    D3 a = glm::cross(n, std::abs(n.x) < 0.9 ? D3(1, 0, 0) : D3(0, 1, 0));
    a = glm::normalize(a);
    const D3 b = glm::cross(n, a);
    const double r = d.radius * std::sqrt(u(rng)), t = glm::two_pi<double>() * u(rng);
    return d.center + a * (r * std::cos(t)) + b * (r * std::sin(t));
}

/** A new curve at root with normal n: the shape of the nearest curves turned onto n, or straight out along n. */
Spline grow(const Splines& s, const Settings& o, const Surface* surface, const D3& root, const D3& n)
{
    Spline out;
    out.kind = s.empty() ? Kind::Poly : s.front().kind == Kind::Bezier ? Kind::Poly : s.front().kind;
    const int pts = std::max(2, o.addPoints);
    if (o.interpolate && !s.empty())
    {
        std::vector<std::pair<double, size_t>> near;
        for (size_t i = 0; i < s.size(); ++i)
            if (s[i].size() >= 2)
                near.push_back({glm::distance(s[i].points.front(), root), i});
        const size_t k = std::min(near.size(), size_t(std::max(1, o.neighbours)));
        std::partial_sort(near.begin(), near.begin() + ptrdiff_t(k), near.end());
        std::vector<D3> shape(size_t(pts), D3(0.0));
        double total = 0.0;
        for (size_t j = 0; j < k; ++j)
        {
            const Spline& nb = s[near[j].second];
            const std::vector<D3> r = resample(nb.points, pts);
            const glm::dquat q = rotationBetween(rootNormal(nb, surface, n), n);
            const double w = 1.0 / std::max(near[j].first, 1e-6);
            for (size_t i = 0; i < r.size(); ++i)
                shape[i] += (q * (r[i] - r[0])) * w;
            total += w;
        }
        if (total > 0.0)
        {
            for (D3& p : shape)
                out.points.push_back(root + p / total);
            if (out.kind == Kind::Nurbs)
                out.degree = std::min(3, pts - 1);
            out.normalize();
            if (arcLength(out.points) > 1e-9)
                return out;
            out.points.clear();
        }
    }
    for (int i = 0; i < pts; ++i)
        out.points.push_back(root + n * (o.addLength * i / (pts - 1)));
    if (out.kind == Kind::Nurbs)
        out.degree = std::min(3, pts - 1);
    out.normalize();
    return out;
}

/** Plants up to count curves in the dab on the surface, no closer than minDistance to any root. */
int plant(Splines& s, Settings& o, const Dab& d, const Surface& surface, std::mt19937& rng, int count, double minDistance)
{
    std::vector<D3> roots;
    const double reach = d.radius + minDistance;
    for (const Spline& sp : s)
        if (!sp.points.empty() && glm::distance(sp.points.front(), d.center) < reach)
            roots.push_back(sp.points.front());
    int added = 0;
    for (int tries = 0; tries < count * 8 && added < count; ++tries)
    {
        auto hit = surface.closest(diskPoint(d, rng), d.radius);
        if (!hit || glm::distance(hit->point, d.center) > d.radius || glm::dot(hit->normal, d.normal) < 0.0)
            continue;
        if (minDistance > 0.0 && std::ranges::any_of(roots, [&](const D3& r) { return glm::distance(r, hit->point) < minDistance; }))
            continue;
        s.push_back(grow(s, o, &surface, hit->point, hit->normal));
        if (!o.mask.empty())
            o.mask.push_back(1);
        roots.push_back(hit->point);
        ++added;
    }
    return added;
}
} // namespace

const char* brushName(Brush b) { return size_t(b) < std::size(kBrushNames) ? kBrushNames[size_t(b)] : "?"; }

bool parseBrush(std::string_view name, Brush* out)
{
    for (size_t k = 0; k < std::size(kBrushNames); ++k)
        if (name == kBrushNames[k])
        {
            *out = Brush(k);
            return true;
        }
    return false;
}

Surface::Surface(geo::Mesh mesh, std::string densitySet) : mMesh(std::move(mesh)), mDensitySet(std::move(densitySet))
{
    LILA_ZONE();

    if (mMesh.numFaces() == 0)
        return;
    mBvh.build(mMesh);
    mNormals.assign(mMesh.pointCapacity(), D3(0.0));
    for (geo::Index f = 0; f < mMesh.faceCapacity(); ++f)
        if (mMesh.faceValid(f))
        {
            const D3 a = mMesh.faceAreaVector(f);
            mMesh.forEachFaceCorner(f, [&](geo::Index c) { mNormals[mMesh.cornerPoint(c)] += a; });
        }
    for (D3& n : mNormals)
        n = glm::length(n) > 1e-30 ? glm::normalize(n) : D3(0, 1, 0);
    if (!mDensitySet.empty() && !geo::colors::has(mMesh, mDensitySet))
        mDensitySet.clear();
}

Surface::Hit Surface::hitAt(geo::Index triangle, const D3& bary, const D3& point) const
{
    Hit h;
    h.point = point;
    h.triangle = triangle;
    h.bary = bary;
    const geo::Index* c = &mBvh.triangles().corners[size_t(triangle) * 3];
    D3 n(0.0);
    for (int k = 0; k < 3; ++k)
        n += mNormals[mMesh.cornerPoint(c[k])] * bary[k];
    h.normal = glm::length(n) > 1e-30 ? glm::normalize(n) : D3(0, 1, 0);
    return h;
}

std::optional<Surface::Hit> Surface::closest(const D3& p, double maxDistance) const
{
    if (empty())
        return std::nullopt;
    geo::BvhClosest c;
    if (!mBvh.closestPoint(p, c, maxDistance) || c.triangle == geo::kInvalid)
        return std::nullopt;
    return hitAt(c.triangle, c.bary, c.point);
}

std::optional<Surface::Hit> Surface::raycast(const D3& origin, const D3& dir) const
{
    if (empty())
        return std::nullopt;
    geo::BvhHit h;
    if (!mBvh.raycast(origin, dir, h))
        return std::nullopt;
    return hitAt(h.triangle, h.bary, h.point);
}

double Surface::density(const Hit& h) const
{
    if (mDensitySet.empty() || h.triangle == geo::kInvalid)
        return 1.0;
    std::span<const glm::vec4> v = geo::colors::values(mMesh, mDensitySet);
    if (v.empty())
        return 1.0;
    const glm::vec4 c = mBvh.interpolate<glm::vec4>(v, h.triangle, h.bary);
    return std::clamp(0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b, 0.0, 1.0);
}

std::vector<D3> resample(const std::vector<D3>& pts, int n)
{
    n = std::max(2, n);
    std::vector<D3> out;
    if (pts.empty())
        return out;
    const double total = arcLength(pts);
    if (pts.size() < 2 || total < 1e-12)
        return std::vector<D3>(size_t(n), pts.front());
    size_t seg = 0;
    double walked = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double target = total * i / (n - 1);
        while (seg + 2 < pts.size() && walked + glm::distance(pts[seg], pts[seg + 1]) < target)
        {
            walked += glm::distance(pts[seg], pts[seg + 1]);
            ++seg;
        }
        const double l = glm::distance(pts[seg], pts[seg + 1]);
        const double t = l > 1e-12 ? std::clamp((target - walked) / l, 0.0, 1.0) : 0.0;
        out.push_back(glm::mix(pts[seg], pts[seg + 1], t));
    }
    return out;
}

void applyDab(Splines& s, Settings& o, const Dab& d, const Surface* surface, std::mt19937& rng, Stats* stats)
{
    LILA_ZONE();

    if (!(d.radius > 0.0) || !std::isfinite(d.radius))
        return;
    Weigher weigh{o, d, glm::length(d.viewDir) > 1e-12 ? glm::normalize(d.viewDir) : D3(0.0)};
    auto warn = [&](const char* w) {
        if (stats && std::ranges::find(stats->warnings, w) == stats->warnings.end())
            stats->warnings.push_back(w);
    };
    const bool invert = o.invert;
    switch (o.brush)
    {
    case Brush::Comb:
    case Brush::Smooth:
    case Brush::Puff:
    case Brush::Pinch:
    {
        for (size_t si = 0; si < s.size(); ++si)
        {
            Spline& sp = s[si];
            if (sp.size() < 2 || !editable(o, si))
                continue;
            std::vector<double> w(sp.size(), 0.0);
            bool any = false;
            for (size_t i = 1; i < sp.size(); ++i)
            {
                w[i] = weigh(sp.points[i]);
                any = any || w[i] > 0.0;
            }
            if (!any)
                continue;
            const std::vector<D3> before = sp.points;
            const std::vector<double> lengths = segmentLengths(sp);
            if (o.brush == Brush::Comb)
            {
                for (size_t i = 1; i < sp.size(); ++i)
                    sp.points[i] += d.delta * w[i];
            }
            else if (o.brush == Brush::Smooth)
            {
                for (size_t i = 1; i < sp.size(); ++i)
                {
                    const D3 target = i + 1 < sp.size() ? (before[i - 1] + before[i + 1]) * 0.5
                                                        : (i >= 2 ? before[i - 1] * 2.0 - before[i - 2] : before[i]);
                    // Half steps: a full step toward the neighbours' middle flips a zigzag rather than flattening it.
                    sp.points[i] += (target - before[i]) * (w[i] * 0.5);
                }
            }
            else if (o.brush == Brush::Puff)
            {
                // Toward standing straight out of the root's normal; inverted, lying down flat around it.
                const D3 n = rootNormal(sp, surface, d.normal);
                double along = 0.0;
                for (size_t i = 1; i < sp.size(); ++i)
                {
                    along += lengths[i - 1];
                    D3 target = before[0] + n * along;
                    if (invert)
                    {
                        D3 flat = before[i] - before[0];
                        flat -= n * glm::dot(flat, n);
                        target = glm::length(flat) > 1e-12 ? before[0] + glm::normalize(flat) * along : before[i];
                    }
                    sp.points[i] += (target - before[i]) * w[i] * 0.5;
                }
            }
            else
            {
                // Toward the brush's axis through the view, or away when inverted.
                const D3 axis = glm::length(weigh.view) > 0.5 ? weigh.view : glm::normalize(d.normal);
                for (size_t i = 1; i < sp.size(); ++i)
                {
                    D3 rel = d.center - before[i];
                    rel -= axis * glm::dot(rel, axis);
                    sp.points[i] += rel * (w[i] * 0.3 * (invert ? -1.0 : 1.0));
                }
            }
            if (o.keepLength)
                restoreLengths(sp, lengths);
            settleHandles(sp, before);
            if (stats)
                ++stats->changed;
        }
        break;
    }
    case Brush::Length:
    {
        for (size_t si = 0; si < s.size(); ++si)
        {
            Spline& sp = s[si];
            if (sp.size() < 2 || !editable(o, si))
                continue;
            double w = 0.0;
            for (const D3& p : sp.points)
                w = std::max(w, weigh(p));
            if (w <= 0.0)
                continue;
            // Every segment scales from the root, so the shape stays and only the length changes.
            const double f = std::max(0.05, 1.0 + (invert ? -1.0 : 1.0) * w * 0.1);
            const std::vector<D3> before = sp.points;
            for (size_t i = 1; i < sp.size(); ++i)
                sp.points[i] = sp.points[i - 1] + (before[i] - before[i - 1]) * f;
            settleHandles(sp, before);
            if (stats)
                ++stats->changed;
        }
        break;
    }
    case Brush::Cut:
    {
        std::vector<char> remove(s.size(), 0);
        bool anyRemoved = false;
        for (size_t si = 0; si < s.size(); ++si)
        {
            Spline& sp = s[si];
            if (sp.size() < 2 || !editable(o, si))
                continue;
            // A cut through the root takes the whole curve.
            if (weigh(sp.points[0]) > 0.0)
            {
                remove[si] = 1;
                anyRemoved = true;
                continue;
            }
            size_t k = 0;
            for (size_t i = 1; i < sp.size(); ++i)
                if (weigh(sp.points[i]) > 0.0)
                {
                    k = i;
                    break;
                }
            if (k == 0)
                continue;
            // The part up to where the brush begins, spread back over as many points as the curve had.
            const int n = int(sp.size());
            std::vector<D3> kept(sp.points.begin(), sp.points.begin() + ptrdiff_t(k));
            double lo = 0.0, hi = 1.0;
            for (int it = 0; it < 30; ++it)
            {
                const double mid = (lo + hi) * 0.5;
                (weigh(glm::mix(sp.points[k - 1], sp.points[k], mid)) > 0.0 ? hi : lo) = mid;
            }
            kept.push_back(glm::mix(sp.points[k - 1], sp.points[k], lo));
            Spline cut = sp;
            cut.points = resample(kept, n);
            cut.handleL.clear();
            cut.handleR.clear();
            cut.normalize();
            if (sp.kind == Kind::Bezier)
                updateHandles(cut);
            sp = std::move(cut);
            if (stats)
                ++stats->changed;
        }
        if (anyRemoved)
            eraseSplines(s, o, remove, stats);
        break;
    }
    case Brush::Delete:
    {
        std::uniform_real_distribution<double> u(0.0, 1.0);
        std::vector<char> remove(s.size(), 0);
        bool any = false;
        for (size_t si = 0; si < s.size(); ++si)
        {
            if (!editable(o, si))
                continue;
            double w = 0.0;
            for (const D3& p : s[si].points)
                w = std::max(w, weigh(p));
            // Soft edges thin out rather than cut a hard line.
            if (w > 0.0 && u(rng) < std::min(1.0, w * 2.0))
            {
                remove[si] = 1;
                any = true;
            }
        }
        if (any)
            eraseSplines(s, o, remove, stats);
        break;
    }
    case Brush::Add:
    {
        if (!surface || surface->empty())
        {
            warn("add needs a surface to plant curves on (groomSurface)");
            break;
        }
        const int count = std::max(1, int(std::lround(o.addCount * std::clamp(d.strength, 0.0, 1.0))));
        const int added = plant(s, o, d, *surface, rng, count, o.minDistance);
        if (stats)
            stats->added += added;
        break;
    }
    case Brush::Density:
    {
        if (!surface || surface->empty())
        {
            warn("density needs a surface (groomSurface)");
            break;
        }
        const auto hit = surface->closest(d.center, d.radius);
        const double target = std::max(1e-6, o.density) * (hit ? surface->density(*hit) : 1.0);
        const double area = glm::pi<double>() * d.radius * d.radius;
        const int want = int(std::floor(target * area));
        std::vector<size_t> inside;
        for (size_t si = 0; si < s.size(); ++si)
            if (!s[si].points.empty() && glm::distance(s[si].points.front(), d.center) < d.radius && editable(o, si))
                inside.push_back(si);
        const double spacing = o.minDistance > 0.0 ? o.minDistance : 0.7 / std::sqrt(target);
        if (!invert && int(inside.size()) < want)
        {
            const int need = std::min(want - int(inside.size()), std::max(1, o.addCount));
            const int added = plant(s, o, d, *surface, rng, need, spacing);
            if (stats)
                stats->added += added;
        }
        else if (invert && int(inside.size()) > want)
        {
            // Thin the most crowded roots first.
            std::vector<std::pair<double, size_t>> crowd;
            for (size_t a : inside)
            {
                double nearest = 1e300;
                for (size_t b : inside)
                    if (a != b)
                        nearest = std::min(nearest, glm::distance(s[a].points.front(), s[b].points.front()));
                crowd.push_back({nearest, a});
            }
            std::sort(crowd.begin(), crowd.end());
            std::vector<char> remove(s.size(), 0);
            const size_t drop = std::min(crowd.size(), size_t(std::max(1, std::min(int(inside.size()) - want, std::max(1, o.addCount)))));
            for (size_t k = 0; k < drop; ++k)
                remove[crowd[k].second] = 1;
            eraseSplines(s, o, remove, stats);
        }
        break;
    }
    case Brush::Slide:
    {
        for (size_t si = 0; si < s.size(); ++si)
        {
            Spline& sp = s[si];
            if (sp.points.empty() || !editable(o, si))
                continue;
            const double w = weigh(sp.points.front());
            if (w <= 0.0)
                continue;
            const std::vector<D3> before = sp.points;
            const D3 root = before.front();
            D3 to = root + d.delta * w;
            glm::dquat q(1.0, 0.0, 0.0, 0.0);
            if (surface && !surface->empty())
            {
                // The root slides over the surface and the curve turns with its normal.
                const auto from = surface->closest(root);
                const auto onto = surface->closest(to);
                if (!onto)
                    continue;
                to = onto->point;
                if (from)
                    q = rotationBetween(from->normal, onto->normal);
            }
            for (size_t i = 0; i < sp.size(); ++i)
                sp.points[i] = to + q * (before[i] - root);
            settleHandles(sp, before);
            if (stats)
                ++stats->changed;
        }
        break;
    }
    default: break;
    }
}

int attachRoots(Splines& s, const Surface& surface)
{
    int moved = 0;
    for (Spline& sp : s)
    {
        if (sp.points.empty())
            continue;
        const auto h = surface.closest(sp.points.front());
        if (!h)
            continue;
        const D3 d = h->point - sp.points.front();
        if (glm::length(d) < 1e-9)
            continue;
        const std::vector<D3> before = sp.points;
        for (D3& p : sp.points)
            p += d;
        settleHandles(sp, before);
        ++moved;
    }
    return moved;
}

Splines guides(const Surface& surface, const GuideOptions& o, std::vector<std::string>* warnings)
{
    LILA_ZONE();

    Splines out;
    if (surface.empty() || o.count <= 0)
        return out;
    const geo::Mesh& m = surface.mesh();
    const auto& tri = surface.bvh().triangles();
    const size_t triCount = tri.corners.size() / 3;
    // Triangles by area times density at their middle, for picking roots evenly over the surface.
    std::vector<double> cumulative(triCount, 0.0);
    double total = 0.0, area = 0.0;
    for (size_t t = 0; t < triCount; ++t)
    {
        const D3 a = m.position(m.cornerPoint(tri.corners[t * 3])), b = m.position(m.cornerPoint(tri.corners[t * 3 + 1])),
                 c = m.position(m.cornerPoint(tri.corners[t * 3 + 2]));
        const double ar = 0.5 * glm::length(glm::cross(b - a, c - a));
        Surface::Hit mid;
        mid.triangle = geo::Index(t);
        mid.bary = D3(1.0 / 3.0);
        total += ar * surface.density(mid);
        area += ar;
        cumulative[t] = total;
    }
    if (total <= 0.0)
    {
        if (warnings)
            warnings->push_back("the density map is black everywhere");
        return out;
    }
    const double spacing = o.minDistance > 0.0 ? o.minDistance : 0.5 * std::sqrt(area / o.count);
    std::mt19937 rng(o.seed);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    std::vector<D3> roots;
    for (int tries = 0; tries < o.count * 30 && int(out.size()) < o.count; ++tries)
    {
        const double pick = u(rng) * total;
        const size_t t = size_t(std::lower_bound(cumulative.begin(), cumulative.end(), pick) - cumulative.begin());
        if (t >= triCount)
            continue;
        double b1 = u(rng), b2 = u(rng);
        if (b1 + b2 > 1.0)
        {
            b1 = 1.0 - b1;
            b2 = 1.0 - b2;
        }
        const D3 bary(1.0 - b1 - b2, b1, b2);
        D3 p(0.0);
        for (int k = 0; k < 3; ++k)
            p += m.position(m.cornerPoint(tri.corners[t * 3 + size_t(k)])) * bary[k];
        if (std::ranges::any_of(roots, [&](const D3& r) { return glm::distance(r, p) < spacing; }))
            continue;
        auto hit = surface.closest(p);
        if (!hit)
            continue;
        roots.push_back(hit->point);
        const double len = o.length * (1.0 - o.lengthJitter * u(rng));
        Spline g;
        g.kind = Kind::Poly;
        const int n = std::max(2, o.points);
        for (int i = 0; i < n; ++i)
            g.points.push_back(hit->point + hit->normal * (len * i / (n - 1)));
        g.normalize();
        out.push_back(std::move(g));
    }
    if (int(out.size()) < o.count && warnings)
        warnings->push_back("only " + std::to_string(out.size()) + " guides fit at that spacing");
    return out;
}
} // namespace lila::curve::groom
