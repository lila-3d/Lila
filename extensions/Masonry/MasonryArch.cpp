// Arch geometry: voussoirs laid radially round one centre (round, segmental, flat) or two (pointed), with the keystone at
// the crown standing a little proud of the extrados.

#include "MasonryArch.h"

#include <algorithm>
#include <cmath>

namespace masonry
{
namespace
{
constexpr double kKeyRaise = 1.15; // keystone depth over the ring's

/** A circle arch: centre height, intrados radius and the springing angle (the arch spans theta0 to pi - theta0). */
struct Circle
{
    double cy = 0.0, radius = 0.0, theta0 = 0.0;
};

Circle circleOf(const ArchSpec& a)
{
    const double half = std::max(a.span * 0.5, 1e-3);
    Circle c;
    if (a.kind == ArchKind::Round)
    {
        c.radius = half;
        return c;
    }
    double r = a.rise > 0.0 ? std::min(a.rise, half) : (a.kind == ArchKind::Flat ? half / 30.0 : half * 0.5);
    r = std::max(r, half * 1e-3);
    c.radius = (half * half + r * r) / (2.0 * r);
    c.cy = r - c.radius;
    c.theta0 = std::atan2(-c.cy, half);
    return c;
}

/** Pointed arch: the right half's centre x (on the left of the middle), radius and apex angle. */
struct Pointed
{
    double cx = 0.0, radius = 0.0, apex = 0.0, height = 0.0;
};

Pointed pointedOf(const ArchSpec& a)
{
    const double s = std::max(a.span, 1e-3);
    Pointed p;
    p.height = a.rise > 0.0 ? std::max(a.rise, s * 0.5 + 1e-3) : s * std::sqrt(3.0) * 0.5;
    p.radius = (s * s * 0.25 + p.height * p.height) / s;
    p.cx = s * 0.5 - p.radius;
    p.apex = std::acos(std::clamp(-p.cx / p.radius, -1.0, 1.0));
    return p;
}

ArchStone wedge(const glm::dvec2& centre, double ri, double re, double ta, double tb, Role role)
{
    const double m = (ta + tb) * 0.5, h = (tb - ta) * 0.5;
    const glm::dvec2 u(std::cos(m), std::sin(m));
    ArchStone s;
    s.axisY = u;
    s.axisX = {u.y, -u.x};
    s.bottom = 2.0 * ri * std::sin(h);
    s.top = 2.0 * re * std::sin(h);
    s.height = (re - ri) * std::cos(h);
    s.center = centre + u * ((ri + re) * 0.5 * std::cos(h));
    s.role = role;
    return s;
}
} // namespace

double archRise(const ArchSpec& a)
{
    if (a.kind == ArchKind::Lintel)
        return 0.0;
    if (a.kind == ArchKind::Pointed)
        return pointedOf(a).height;
    const Circle c = circleOf(a);
    return c.cy + c.radius;
}

std::vector<ArchStone> archStones(const ArchSpec& a)
{
    std::vector<ArchStone> out;
    if (a.kind == ArchKind::Lintel || a.span <= 0.0)
        return out;
    const double ring = std::max(a.ring, 1e-3);
    const double unit = std::max(a.unit, 1e-3);
    if (a.kind == ArchKind::Pointed)
    {
        const Pointed p = pointedOf(a);
        const double re = p.radius + ring;
        const double gap = a.joint / (p.radius + ring * 0.5);
        const int perSide = std::max(2, int(std::lround(p.radius * p.apex / unit)));
        const double step = p.apex / (perSide + (a.keystone ? 0.5 : 0.0));
        for (int i = 0; i < perSide; ++i)
        {
            const double ta = i * step + gap * 0.5, tb = (i + 1) * step - gap * 0.5;
            ArchStone right = wedge({p.cx, 0.0}, p.radius, re, ta, tb, Role::Voussoir);
            out.push_back(right);
            ArchStone left = right;
            left.center.x = -left.center.x;
            left.axisY.x = -left.axisY.x;
            left.axisX = {left.axisY.y, -left.axisY.x};
            out.push_back(left);
        }
        if (a.keystone)
        {
            const double tk = perSide * step + gap * 0.5;
            const glm::dvec2 dir(std::cos(tk), std::sin(tk));
            const glm::dvec2 in = glm::dvec2(p.cx, 0.0) + dir * p.radius, out2 = glm::dvec2(p.cx, 0.0) + dir * re;
            ArchStone k;
            k.bottom = std::max(2.0 * in.x, 1e-3);
            const double top = std::max(2.0 * out2.x, k.bottom);
            k.height = (out2.y - in.y) * kKeyRaise;
            k.top = k.bottom + (top - k.bottom) * kKeyRaise;
            k.center = {0.0, in.y + k.height * 0.5};
            k.role = Role::Keystone;
            out.push_back(k);
        }
        return out;
    }
    const Circle c = circleOf(a);
    const double re = c.radius + ring;
    const double sweep = kPi - 2.0 * c.theta0;
    int n = std::max(3, int(std::lround(c.radius * sweep / unit)));
    if (a.keystone && n % 2 == 0)
        ++n;
    if (!a.keystone && n % 2 == 1)
        ++n;
    const double step = sweep / n;
    const double gap = a.joint / (c.radius + ring * 0.5);
    for (int i = 0; i < n; ++i)
    {
        const bool key = a.keystone && i == n / 2;
        const double ta = c.theta0 + i * step + gap * 0.5, tb = c.theta0 + (i + 1) * step - gap * 0.5;
        out.push_back(wedge({0.0, c.cy}, c.radius, key ? c.radius + ring * kKeyRaise : re, ta, tb, key ? Role::Keystone : Role::Voussoir));
    }
    return out;
}

double extradosHalfWidth(const ArchSpec& a, double v)
{
    if (a.kind == ArchKind::Lintel)
        return -1.0;
    v = std::max(v, 0.0);
    const double ring = std::max(a.ring, 1e-3) * kKeyRaise;
    if (a.kind == ArchKind::Pointed)
    {
        const Pointed p = pointedOf(a);
        const double re = p.radius + ring;
        if (v >= re)
            return -1.0;
        const double x = p.cx + std::sqrt(re * re - v * v);
        return x > 0.0 ? x : -1.0;
    }
    const Circle c = circleOf(a);
    const double re = c.radius + ring, dy = v - c.cy;
    if (dy >= re)
        return -1.0;
    return std::sqrt(re * re - dy * dy);
}
} // namespace masonry
