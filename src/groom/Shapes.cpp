#include "groom/Shapes.h"

#include "groom/Attrs.h"
#include "groom/Locks.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <random>

namespace lila::groom
{
namespace
{
constexpr std::array<const char*, size_t(LockShape::Count)> kShapeNames = {"lock", "braid", "rope", "ringlet"};
constexpr std::array<const char*, size_t(Preset::Count)> kPresetNames = {"ponytail", "bun", "bangs"};

Spline polyOf(std::vector<D3> pts)
{
    Spline s;
    s.kind = Kind::Poly;
    s.points = std::move(pts);
    s.normalize();
    return s;
}

D3 onScalp(const Surface& scalp, const D3& p, D3* normal = nullptr)
{
    if (auto h = scalp.closest(p))
    {
        if (normal)
            *normal = h->normal;
        return h->point;
    }
    return p;
}

D3 safeNormalize(const D3& v, const D3& fallback) { return glm::length(v) > 1e-12 ? glm::normalize(v) : fallback; }
} // namespace

const char* lockShapeName(LockShape s) { return size_t(s) < kShapeNames.size() ? kShapeNames[size_t(s)] : "?"; }

bool parseLockShape(std::string_view name, LockShape* out)
{
    for (size_t k = 0; k < kShapeNames.size(); ++k)
        if (name == kShapeNames[k])
        {
            *out = LockShape(k);
            return true;
        }
    return false;
}

const char* presetName(Preset p) { return size_t(p) < kPresetNames.size() ? kPresetNames[size_t(p)] : "?"; }

bool parsePreset(std::string_view name, Preset* out)
{
    for (size_t k = 0; k < kPresetNames.size(); ++k)
        if (name == kPresetNames[k])
        {
            *out = Preset(k);
            return true;
        }
    return false;
}

Splines shapeAlong(const std::vector<D3>& stroke, const std::vector<double>& pressure, const D3& up, const ShapeOptions& o)
{
    Splines out;
    if (stroke.size() < 2)
        return out;
    Spline path = polyOf(stroke);
    if (pressure.size() == stroke.size())
        for (double p : pressure)
            path.radius.push_back(std::clamp(0.25 + 0.75 * p, 0.05, 1.0));
    const double len = attr::length(path);
    if (len < 1e-6)
        return out;
    const int n = std::clamp(int(std::ceil(len * std::max(0.1, o.density))) + 1, 4, 1024);
    path = attr::resampled(path, n);
    if (o.shape == LockShape::Lock)
    {
        attr::set(path, attr::kGuide, 1.0);
        out.push_back(std::move(path));
        return out;
    }
    // Frames along the stroke, the plait's flat side facing up at the root.
    Frames f;
    {
        Spline tmp = path;
        tmp.tilt.clear();
        f = lockFrames(tmp, nullptr, 0.0);
        const D3 want = safeNormalize(glm::cross(f.tangent[0], up), f.side[0]);
        const double a = std::atan2(glm::dot(glm::cross(f.side[0], want), f.tangent[0]), glm::dot(f.side[0], want));
        for (size_t i = 0; i < f.side.size(); ++i)
        {
            const glm::dquat q = glm::angleAxis(a, f.tangent[i]);
            f.side[i] = q * f.side[i];
            f.thick[i] = glm::cross(f.tangent[i], f.side[i]);
        }
    }
    std::vector<double> along(path.size(), 0.0);
    for (size_t i = 1; i < path.size(); ++i)
        along[i] = along[i - 1] + glm::distance(path.points[i - 1], path.points[i]);
    const int strands = o.shape == LockShape::Braid ? 3 : o.shape == LockShape::Rope ? 2 : 1;
    const double tau = glm::two_pi<double>();
    for (int k = 0; k < strands; ++k)
    {
        Spline s = path;
        const double offset = tau * k / strands;
        for (size_t i = 0; i < s.size(); ++i)
        {
            const double phi = tau * along[i] / std::max(1e-3, o.period) + offset;
            // Grows in over the first turn so the strands start together at the root.
            const double grow = glm::smoothstep(0.0, std::max(1e-3, o.period * 0.5), along[i]);
            D3 off(0.0);
            if (o.shape == LockShape::Braid)
                off = f.side[i] * (std::sin(phi) * o.size) + f.thick[i] * (std::sin(2.0 * phi) * 0.5 * o.size);
            else
                off = f.side[i] * (std::cos(phi) * o.size) + f.thick[i] * (std::sin(phi) * o.size);
            if (o.shape == LockShape::Ringlet)
                off -= f.side[i] * o.size;
            s.points[i] += off * grow;
        }
        // Plaited strands are rounder than a lying lock, so the plait reads as a plait.
        if (strands > 1)
        {
            attr::set(s, attr::kWidth, o.strandWidth);
            attr::set(s, attr::kThickness, 2.5);
        }
        attr::set(s, attr::kGuide, 1.0);
        out.push_back(std::move(s));
    }
    return out;
}

Splines preset(const Surface& scalp, const PresetOptions& o)
{
    Splines out;
    if (scalp.empty())
        return out;
    const D3 down = safeNormalize(o.down, D3(0, -1, 0));
    std::mt19937 rng(o.seed);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    const int count = std::clamp(o.count, 1, 4096);
    const int pts = 20;
    switch (o.preset)
    {
    case Preset::Ponytail:
    {
        D3 n;
        const D3 g = onScalp(scalp, o.at, &n);
        const D3 band = g + n * 0.6;
        for (int k = 0; k < count; ++k)
        {
            // Roots on a disc around the gather point, laid onto the scalp.
            const double a = glm::two_pi<double>() * (k + u(rng) * 0.5) / count, r = o.radius * std::sqrt(0.2 + 0.8 * u(rng));
            const D3 t1 = safeNormalize(glm::cross(n, std::abs(n.y) < 0.9 ? D3(0, 1, 0) : D3(1, 0, 0)), D3(1, 0, 0)), t2 = glm::cross(n, t1);
            const D3 root = onScalp(scalp, g + (t1 * std::cos(a) + t2 * std::sin(a)) * r);
            std::vector<D3> p;
            const int toBand = pts / 3;
            for (int i = 0; i < toBand; ++i)
            {
                const double s = double(i) / toBand;
                D3 nn;
                const D3 q = onScalp(scalp, glm::mix(root, g, s), &nn);
                p.push_back(q + nn * (0.4 * s));
            }
            const double spread = 0.15 * o.radius;
            const D3 fan = safeNormalize(root - g, t1);
            const double tail = o.length * (0.85 + 0.15 * u(rng));
            for (int i = 0; i <= pts - toBand; ++i)
            {
                const double s = double(i) / (pts - toBand);
                // Out from the band, then falling, fanning a little toward the tip.
                const D3 out1 = n * (2.0 * std::sin(s * glm::half_pi<double>()) * (1.0 - s));
                p.push_back(band + out1 + down * (tail * s) + fan * (spread * s * s));
            }
            Spline sp = polyOf(std::move(p));
            attr::set(sp, attr::kGuide, 1.0);
            out.push_back(std::move(sp));
        }
        break;
    }
    case Preset::Bun:
    {
        D3 n;
        const D3 g = onScalp(scalp, o.at, &n);
        const D3 t1 = safeNormalize(glm::cross(n, std::abs(n.y) < 0.9 ? D3(0, 1, 0) : D3(1, 0, 0)), D3(1, 0, 0)), t2 = glm::cross(n, t1);
        for (int k = 0; k < count; ++k)
        {
            const double a0 = glm::two_pi<double>() * k / count;
            const D3 root = onScalp(scalp, g + (t1 * std::cos(a0) + t2 * std::sin(a0)) * o.radius * (0.7 + 0.3 * u(rng)));
            std::vector<D3> p{root};
            // Up from the root to the bun, then once and a half round it, rising into a dome.
            for (int i = 1; i < pts; ++i)
            {
                const double s = double(i) / (pts - 1);
                const double a = a0 + s * glm::two_pi<double>() * 1.5;
                const double r = o.radius * (0.55 - 0.35 * s) * (1.0 + 0.1 * (u(rng) - 0.5));
                const double h = o.radius * (0.3 + 0.7 * std::sin(s * glm::half_pi<double>()));
                p.push_back(g + n * h + (t1 * std::cos(a) + t2 * std::sin(a)) * r);
            }
            Spline sp = polyOf(std::move(p));
            attr::set(sp, attr::kGuide, 1.0);
            out.push_back(std::move(sp));
        }
        break;
    }
    case Preset::Bangs:
    {
        for (int k = 0; k < count; ++k)
        {
            const double s0 = count > 1 ? double(k) / (count - 1) : 0.5;
            D3 n;
            const D3 root = onScalp(scalp, glm::mix(o.at, o.to, s0), &n);
            const double len = o.length * (0.8 + 0.2 * u(rng));
            const D3 forward = safeNormalize(n - down * glm::dot(n, down), n);
            std::vector<D3> p;
            for (int i = 0; i < pts; ++i)
            {
                const double s = double(i) / (pts - 1);
                // Forward off the hairline, then down over the forehead.
                p.push_back(root + forward * (0.25 * len * std::sin(s * glm::pi<double>() * 0.6)) + down * (len * s * s * 0.9 + len * 0.1 * s));
            }
            Spline sp = polyOf(std::move(p));
            attr::set(sp, attr::kGuide, 1.0);
            out.push_back(std::move(sp));
        }
        break;
    }
    default: break;
    }
    return out;
}
} // namespace lila::groom
