#include "groom/Locks.h"

#include "core/Profile.h"
#include "geo/Attribute.h"
#include "groom/Attrs.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace lila::groom
{
namespace
{
constexpr std::array<const char*, size_t(LockProfile::Count)> kProfileNames = {"tube", "ribbon", "crescent", "teardrop", "custom"};

D3 anyPerpendicular(const D3& t)
{
    const D3 a = std::abs(t.y) < 0.9 ? D3(0, 1, 0) : D3(1, 0, 0);
    return glm::normalize(glm::cross(a, t));
}

/** v turned about the unit axis to stay perpendicular to it (parallel transport from one tangent to the next). */
D3 transport(const D3& v, const D3& from, const D3& to)
{
    const double c = glm::dot(from, to);
    if (c > 1.0 - 1e-12)
        return v;
    if (c < -1.0 + 1e-12)
        return -v;
    return glm::rotation(from, to) * v;
}

/** Closed ring of points from an open list of samples around the profile, resampled to sides points by arc length. */
std::vector<glm::dvec2> evenRing(const std::vector<glm::dvec2>& in, int sides)
{
    std::vector<double> at(in.size() + 1, 0.0);
    for (size_t i = 0; i < in.size(); ++i)
        at[i + 1] = at[i] + glm::distance(in[i], in[(i + 1) % in.size()]);
    std::vector<glm::dvec2> out;
    const double total = at.back();
    size_t seg = 0;
    for (int k = 0; k < sides; ++k)
    {
        const double target = total * k / sides;
        while (seg + 1 < in.size() && at[seg + 1] < target)
            ++seg;
        const double l = at[seg + 1] - at[seg];
        const double f = l > 1e-12 ? (target - at[seg]) / l : 0.0;
        out.push_back(glm::mix(in[seg], in[(seg + 1) % in.size()], f));
    }
    return out;
}

double ramp(const Ramp& r, double t, double def = 1.0) { return r ? r(t) : def; }
} // namespace

const char* profileName(LockProfile p) { return size_t(p) < kProfileNames.size() ? kProfileNames[size_t(p)] : "?"; }

bool parseProfile(std::string_view name, LockProfile* out)
{
    for (size_t k = 0; k < kProfileNames.size(); ++k)
        if (name == kProfileNames[k])
        {
            *out = LockProfile(k);
            return true;
        }
    return false;
}

std::vector<glm::dvec2> profilePoints(LockProfile p, int sides, const std::vector<glm::dvec2>& custom)
{
    sides = std::clamp(sides, 3, 128);
    const double tau = glm::two_pi<double>();
    std::vector<glm::dvec2> dense;
    const int n = 96;
    switch (p)
    {
    case LockProfile::Tube:
    case LockProfile::Ribbon:
        for (int k = 0; k < sides; ++k)
            dense.push_back({std::cos(tau * k / sides), std::sin(tau * k / sides)});
        return dense;
    case LockProfile::Crescent:
    {
        // Top edge bowed up, bottom edge bowed less, so the band is thick in the middle and thin at its edges.
        for (int k = 0; k <= n / 2; ++k)
        {
            const double x = 1.0 - 2.0 * k / (n / 2);
            const double c = std::sqrt(std::max(0.0, 1.0 - x * x));
            dense.push_back({x, 0.5 * (1.0 - x * x) + 0.5 * c});
        }
        for (int k = 1; k < n / 2; ++k)
        {
            const double x = -1.0 + 2.0 * k / (n / 2);
            const double c = std::sqrt(std::max(0.0, 1.0 - x * x));
            dense.push_back({x, 0.5 * (1.0 - x * x) - 0.5 * c});
        }
        for (glm::dvec2& q : dense)
            q.y = q.y * 2.0 - 1.0;
        break;
    }
    case LockProfile::Teardrop:
        for (int k = 0; k < n; ++k)
        {
            const double a = tau * k / n;
            dense.push_back({std::cos(a), std::sin(a) * std::pow(std::abs(std::sin(a * 0.5)), 1.2)});
        }
        break;
    case LockProfile::Custom:
        if (custom.size() >= 3)
            dense = custom;
        else
            return profilePoints(LockProfile::Ribbon, sides);
        break;
    default: return profilePoints(LockProfile::Ribbon, sides);
    }
    // Into -1..1 on both axes, then even spacing.
    glm::dvec2 lo(1e300), hi(-1e300);
    for (const glm::dvec2& q : dense)
    {
        lo = glm::min(lo, q);
        hi = glm::max(hi, q);
    }
    const glm::dvec2 mid = (lo + hi) * 0.5, half = glm::max((hi - lo) * 0.5, glm::dvec2(1e-9));
    for (glm::dvec2& q : dense)
        q = (q - mid) / half;
    double area = 0.0;
    for (size_t i = 0; i < dense.size(); ++i)
    {
        const glm::dvec2 &a = dense[i], &b = dense[(i + 1) % dense.size()];
        area += a.x * b.y - b.x * a.y;
    }
    if (area < 0.0)
        std::ranges::reverse(dense);
    return evenRing(dense, sides);
}

Frames lockFrames(const Spline& s, const Surface* scalp, double flatten, double twist, const Ramp& twistRamp)
{
    Frames f;
    const size_t n = s.size();
    if (n == 0)
        return f;
    f.tangent.resize(n);
    for (size_t i = 0; i < n; ++i)
    {
        const D3 d = s.points[std::min(i + 1, n - 1)] - s.points[i ? i - 1 : 0];
        f.tangent[i] = glm::length(d) > 1e-12 ? glm::normalize(d) : (i ? f.tangent[i - 1] : D3(0, 1, 0));
    }
    // Where the root has no scalp under it, start from a side perpendicular to the tangent.
    D3 rootNormal = anyPerpendicular(f.tangent[0]);
    if (scalp && !scalp->empty())
        if (auto h = scalp->closest(s.points[0]))
            rootNormal = h->normal;
    const double reach = std::max(1e-3, attr::length(s) * 0.25);
    f.side.resize(n);
    f.thick.resize(n);
    for (size_t i = 0; i < n; ++i)
    {
        const D3& t = f.tangent[i];
        D3 side = i ? transport(f.side[i - 1], f.tangent[i - 1], t) : glm::cross(t, rootNormal);
        if (scalp && flatten > 0.0 && !scalp->empty())
            if (auto h = scalp->closest(s.points[i], reach * 4.0))
            {
                // Flat to the scalp close to it, fading out with distance.
                const double d = glm::distance(h->point, s.points[i]);
                const double w = std::clamp(flatten, 0.0, 1.0) * std::exp(-d / reach);
                D3 want = glm::cross(t, h->normal);
                if (glm::length(want) > 1e-6)
                {
                    want = glm::normalize(want);
                    if (glm::dot(want, side) < 0.0)
                        want = -want;
                    side = glm::mix(side, want, w);
                }
            }
        side -= t * glm::dot(side, t);
        side = glm::length(side) > 1e-9 ? glm::normalize(side) : anyPerpendicular(t);
        f.side[i] = side;
    }
    const double len = std::max(1e-12, attr::length(s));
    double walked = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        if (i)
            walked += glm::distance(s.points[i - 1], s.points[i]);
        const double t = walked / len;
        const double turn = twist * (twistRamp ? twistRamp(t) : t) + (i < s.tilt.size() ? s.tilt[i] : 0.0);
        if (turn != 0.0)
            f.side[i] = glm::angleAxis(turn, f.tangent[i]) * f.side[i];
        f.thick[i] = glm::cross(f.tangent[i], f.side[i]);
    }
    return f;
}

Splines resolveLocks(const Splines& curves, const Surface* scalp, const LockOptions& o, std::vector<std::string>* warnings)
{
    LILA_ZONE();

    Splines out;
    out.reserve(curves.size());
    int skipped = 0;
    for (size_t ci = 0; ci < curves.size(); ++ci)
    {
        const Spline& c = curves[ci];
        if (c.size() < 2 || attr::length(c) < 1e-6)
        {
            ++skipped;
            continue;
        }
        const double len = attr::length(c);
        const int rings = o.segments > 0 ? o.segments + 1 : std::clamp(int(std::ceil(len / std::max(0.05, o.segmentLength))) + 1, 4, 256);
        Spline s = attr::resampled(c, rings);
        const double sLen = std::max(1e-12, attr::length(s));
        const bool stray = attr::get(c, attr::kStray, 0.0) > 0.5;
        const double widthScale = attr::get(c, attr::kWidth, 1.0), thickScale = attr::get(c, attr::kThickness, 1.0);
        const LockProfile prof = LockProfile(std::clamp(int(attr::get(c, attr::kProfile, double(int(o.profile)))), 0, int(LockProfile::Count) - 1));
        std::vector<double>& hw = attr::points(s, attr::kHalfWidth, 0.0);
        std::vector<double>& ht = attr::points(s, attr::kHalfThick, 0.0);
        std::vector<double>& v = attr::points(s, attr::kAlong, 0.0);
        double walked = 0.0;
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (i)
                walked += glm::distance(s.points[i - 1], s.points[i]);
            const double t = std::min(1.0, walked / sLen);
            v[i] = t;
            const double r = i < s.radius.size() ? s.radius[i] : 1.0;
            if (stray)
            {
                hw[i] = ht[i] = 0.5 * o.strayWidth * std::max(0.05, 1.0 - t) * widthScale;
                continue;
            }
            hw[i] = std::max(1e-3, 0.5 * o.width * widthScale * r * ramp(o.widthRamp, t));
            const double ratio = prof == LockProfile::Tube ? 1.0 : std::clamp(o.thickness * thickScale * ramp(o.thicknessRamp, t), 0.02, 4.0);
            ht[i] = std::max(5e-4, hw[i] * ratio);
        }
        if (o.rest && scalp && !scalp->empty())
            for (size_t i = 1; i < s.size(); ++i)
                if (auto h = scalp->closest(s.points[i], ht[i] * 4.0 + o.lift + 1.0))
                {
                    // Out to rest on the scalp, never pulled in.
                    const double d = glm::dot(s.points[i] - h->point, h->normal);
                    const double want = ht[i] + o.lift;
                    if (d < want)
                        s.points[i] += h->normal * (want - d);
                }
        const Frames f = lockFrames(s, scalp, o.flatten, o.twist, o.twistRamp);
        std::vector<double>& sx = attr::points(s, attr::kSideX, 0.0);
        std::vector<double>& sy = attr::points(s, attr::kSideY, 0.0);
        std::vector<double>& sz = attr::points(s, attr::kSideZ, 0.0);
        for (size_t i = 0; i < s.size(); ++i)
        {
            sx[i] = f.side[i].x;
            sy[i] = f.side[i].y;
            sz[i] = f.side[i].z;
        }
        attr::set(s, attr::kProfile, double(int(prof)));
        if (!s.attrs.contains(std::string(attr::kId)))
            attr::set(s, attr::kId, double(ci + 1));
        out.push_back(std::move(s));
    }
    if (skipped && warnings)
        warnings->push_back(std::to_string(skipped) + " curves too short for a lock");
    return out;
}

std::vector<std::vector<double>> collisionRadii(const Splines& curves, const LockOptions& o)
{
    std::vector<std::vector<double>> out;
    out.reserve(curves.size());
    for (const Spline& c : curves)
    {
        std::vector<double> r(c.size(), 0.0);
        const double len = std::max(1e-12, attr::length(c));
        const bool stray = attr::get(c, attr::kStray, 0.0) > 0.5;
        const double widthScale = attr::get(c, attr::kWidth, 1.0), thickScale = attr::get(c, attr::kThickness, 1.0);
        const bool tube = int(attr::get(c, attr::kProfile, double(int(o.profile)))) == int(LockProfile::Tube);
        double walked = 0.0;
        for (size_t i = 0; i < c.size(); ++i)
        {
            if (i)
                walked += glm::distance(c.points[i - 1], c.points[i]);
            const double t = walked / len;
            if (stray)
            {
                r[i] = 0.5 * o.strayWidth;
                continue;
            }
            const double hw = 0.5 * o.width * widthScale * (i < c.radius.size() ? c.radius[i] : 1.0) * ramp(o.widthRamp, t);
            r[i] = std::max(1e-3, tube ? hw : hw * std::clamp(o.thickness * thickScale * ramp(o.thicknessRamp, t), 0.02, 4.0));
        }
        out.push_back(std::move(r));
    }
    return out;
}

LockMesh lockMesh(const Splines& resolved, const LockOptions& o)
{
    LILA_ZONE();

    LockMesh out;
    geo::Mesh& m = out.mesh;
    const int sides = std::clamp(o.sides, 3, 128);
    const int cols = std::max(1, o.columns), rows = std::max(1, o.rows);
    const double du = 1.0 / cols, dv = 1.0 / rows;
    std::array<std::vector<glm::dvec2>, size_t(LockProfile::Count)> profiles;
    for (int p = 0; p < int(LockProfile::Count); ++p)
        profiles[size_t(p)] = profilePoints(LockProfile(p), sides, o.customProfile);
    for (size_t li = 0; li < resolved.size(); ++li)
    {
        const Spline& s = resolved[li];
        const size_t n = s.size();
        if (n < 2)
            continue;
        const auto& prof = profiles[size_t(std::clamp(int(attr::get(s, attr::kProfile, 1.0)), 0, int(LockProfile::Count) - 1))];
        const int32_t lockId = int32_t(attr::get(s, attr::kId, double(li + 1)));
        const uint32_t pick = attr::seed(s, li, o.seed);
        const int cell = int(pick % uint32_t(cols * rows));
        const double u0 = (cell % cols) * du, v0 = (cell / cols) * dv;
        std::vector<geo::Index> pts(n * size_t(sides));
        for (size_t i = 0; i < n; ++i)
        {
            const D3 t = glm::normalize(s.points[std::min(i + 1, n - 1)] - s.points[i ? i - 1 : 0]);
            D3 side(attr::point(s, attr::kSideX, i, 1.0), attr::point(s, attr::kSideY, i, 0.0), attr::point(s, attr::kSideZ, i, 0.0));
            side -= t * glm::dot(side, t);
            side = glm::length(side) > 1e-9 ? glm::normalize(side) : anyPerpendicular(t);
            const D3 thick = glm::cross(t, side);
            const double hw = attr::point(s, attr::kHalfWidth, i, 0.5), ht = attr::point(s, attr::kHalfThick, i, 0.2);
            for (int j = 0; j < sides; ++j)
                pts[i * size_t(sides) + size_t(j)] = m.addPoint(s.points[i] + side * (prof[size_t(j)].x * hw) + thick * (prof[size_t(j)].y * ht));
        }
        // Fetched per face: adding faces grows the attribute arrays.
        auto setUv = [&](geo::Index f, const std::vector<glm::vec2>& uv) {
            auto uvs = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
            auto ids = m.faceAttrs().getOrAdd<int32_t>("lockId");
            size_t k = 0;
            m.forEachFaceCorner(f, [&](geo::Index cn) { uvs[cn] = uv[k++]; });
            ids[f] = lockId;
        };
        for (size_t i = 0; i + 1 < n; ++i)
        {
            const double va = attr::point(s, attr::kAlong, i, double(i) / (n - 1)), vb = attr::point(s, attr::kAlong, i + 1, double(i + 1) / (n - 1));
            for (int j = 0; j < sides; ++j)
            {
                const int k = (j + 1) % sides;
                const geo::Index q[4] = {pts[i * size_t(sides) + size_t(j)], pts[i * size_t(sides) + size_t(k)], pts[(i + 1) * size_t(sides) + size_t(k)],
                                         pts[(i + 1) * size_t(sides) + size_t(j)]};
                const geo::Index f = m.addFace(std::span<const geo::Index>(q, 4));
                if (f == geo::kInvalid)
                    continue;
                const float ua = float(u0 + du * double(j) / sides), ub = float(u0 + du * double(j + 1) / sides);
                // v runs root at the top of the cell to tip at the bottom, as hair textures are painted.
                const float ta = float(v0 + dv * (1.0 - va)), tb = float(v0 + dv * (1.0 - vb));
                setUv(f, {{ua, ta}, {ub, ta}, {ub, tb}, {ua, tb}});
            }
        }
        if (o.caps)
        {
            std::vector<geo::Index> root, tip;
            std::vector<glm::vec2> ruv, tuv;
            for (int j = sides - 1; j >= 0; --j)
            {
                root.push_back(pts[size_t(j)]);
                ruv.push_back({float(u0 + du * (0.5 + 0.5 * prof[size_t(j)].x)), float(v0 + dv)});
            }
            for (int j = 0; j < sides; ++j)
            {
                tip.push_back(pts[(n - 1) * size_t(sides) + size_t(j)]);
                tuv.push_back({float(u0 + du * (0.5 + 0.5 * prof[size_t(j)].x)), float(v0)});
            }
            if (geo::Index f = m.addFace(root); f != geo::kInvalid)
                setUv(f, ruv);
            if (geo::Index f = m.addFace(tip); f != geo::kInvalid)
                setUv(f, tuv);
        }
        ++out.locks;
    }
    return out;
}
} // namespace lila::groom
