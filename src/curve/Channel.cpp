#include "curve/Channel.h"

#include "core/Profile.h"
#include "curve/Ops.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace lila::curve
{
namespace
{
using D2 = glm::dvec2;
using D3 = glm::dvec3;

/** Quarter arc from a to b about c, points after a up to and including b. */
void arcTo(std::vector<D2>& out, const D2& c, double r, double a0, double a1, int segments)
{
    for (int k = 1; k <= segments; ++k)
    {
        double a = a0 + (a1 - a0) * k / segments;
        out.push_back(c + r * D2(std::cos(a), std::sin(a)));
    }
}
} // namespace

const char* channelShapeName(ChannelShape s)
{
    switch (s)
    {
    case ChannelShape::UChannel: return "uChannel";
    case ChannelShape::VGroove: return "vGroove";
    case ChannelShape::HalfRound: return "halfRound";
    case ChannelShape::Stepped: return "stepped";
    }
    return "uChannel";
}

std::vector<D2> channelSection(const ChannelOptions& o)
{
    const double hw = std::max(o.width, 1e-3) * 0.5, d = std::max(o.depth, 1e-3);
    const int seg = std::clamp(o.segments, 1, 64);
    const double pi = glm::pi<double>();
    std::vector<D2> s;
    switch (o.shape)
    {
    case ChannelShape::UChannel:
    {
        const double r = std::clamp(o.cornerRound, 0.0, 1.0) * std::min(hw, d);
        s.push_back({-hw, 0.0});
        if (r < 1e-6)
        {
            s.push_back({-hw, -d});
            s.push_back({hw, -d});
        }
        else
        {
            s.push_back({-hw, -d + r});
            arcTo(s, {-hw + r, -d + r}, r, pi, 1.5 * pi, seg);
            if (hw - r > 1e-6)
                s.push_back({hw - r, -d});
            arcTo(s, {hw - r, -d + r}, r, 1.5 * pi, 2.0 * pi, seg);
            if (std::abs(s.back().y) < 1e-9)
                s.pop_back();
        }
        s.push_back({hw, 0.0});
        break;
    }
    case ChannelShape::VGroove: s = {{-hw, 0.0}, {0.0, -d}, {hw, 0.0}}; break;
    case ChannelShape::HalfRound:
        s.push_back({-hw, 0.0});
        for (int k = 1; k <= 2 * seg; ++k)
        {
            double a = pi + pi * k / (2 * seg);
            s.push_back({hw * std::cos(a), d * std::sin(a)});
        }
        s.back() = {hw, 0.0};
        break;
    case ChannelShape::Stepped:
    {
        const int n = std::clamp(o.steps, 1, 64);
        const double fw = std::clamp(o.floorWidth, 0.0, 1.0) * hw, dx = (hw - fw) / n, dy = d / n;
        s.push_back({-hw, 0.0});
        for (int k = 1; k <= n; ++k)
        {
            s.push_back({-hw + dx * (k - 1), -dy * k});
            if (k < n || fw > 1e-6)
                s.push_back({-hw + dx * k, -dy * k});
        }
        std::vector<D2> right(s.rbegin(), s.rend());
        for (D2& p : right)
            p.x = -p.x;
        if (fw <= 1e-6)
            right.erase(right.begin());
        s.insert(s.end(), right.begin(), right.end());
        break;
    }
    }
    return s;
}

geo::Mesh channelCutter(const Spline& input, const ChannelOptions& o)
{
    LILA_ZONE();

    if (!isValid(input))
        return {};
    Spline path = input;
    if (o.bendRadius > 0.0)
    {
        FilletOptions fo;
        fo.radius = o.bendRadius;
        path = fillet(input, fo);
    }
    FrameOptions fr;
    fr.mode = FrameMode::FixedUp;
    fr.up = glm::length(o.up) > 1e-9 ? glm::normalize(o.up) : D3(0, 1, 0);
    Polyline pl = tessellate(path, o.tess);
    if (pl.size() < 2)
        return {};
    Path P = makePath(pl, frames(pl, fr));

    const std::vector<D2> sec = channelSection(o);
    const double hw = std::max(o.width, 1e-3) * 0.5;
    const double lift = o.lift > 0.0 ? o.lift : std::max(std::max(o.depth, 1e-3), hw);
    // Section length for UVs: the rim-to-rim part fills the strip, the part above the surface sits on its edges.
    std::vector<double> along(sec.size(), 0.0);
    for (size_t k = 1; k < sec.size(); ++k)
        along[k] = along[k - 1] + glm::distance(sec[k - 1], sec[k]);
    const double len = std::max(along.back(), 1e-9);

    // Profile x is up (the frame normal), y across (the binormal).
    Profile prof;
    std::vector<double> u;
    for (size_t k = 0; k < sec.size(); ++k)
    {
        prof.points.push_back(D3(sec[k].y, sec[k].x, 0.0));
        u.push_back(along[k] / len);
    }
    prof.points.push_back(D3(lift, hw, 0.0));
    u.push_back(1.0);
    prof.points.push_back(D3(lift, -hw, 0.0));
    u.push_back(0.0);
    u.push_back(0.0);
    prof.hard.assign(prof.points.size(), 1);
    // Counter-clockwise with U falling along it: the walls are left behind facing the other way, and the strip still reads
    // the right way round on them.
    if (prof.signedArea() < 0.0)
    {
        std::reverse(prof.points.begin() + 1, prof.points.end());
        std::reverse(u.begin(), u.end());
    }
    else
        for (double& x : u)
            x = 1.0 - x;
    prof.around = std::move(u);

    SweepOptions so;
    so.startCap = so.endCap = CapMode::Ngon;
    so.uv = o.uv;
    so.miter = true;
    return sweep(prof, P, so).mesh;
}
} // namespace lila::curve
