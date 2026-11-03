#include "groom/Attrs.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace lila::groom::attr
{
using curve::Spline;
using D3 = glm::dvec3;

double get(const Spline& s, std::string_view name, double def)
{
    auto it = s.attrs.find(std::string(name));
    return it == s.attrs.end() ? def : it->second;
}

void set(Spline& s, std::string_view name, double v) { s.attrs[std::string(name)] = v; }

double point(const Spline& s, std::string_view name, size_t i, double def)
{
    auto it = s.pointAttrs.find(std::string(name));
    return it == s.pointAttrs.end() || i >= it->second.size() ? def : it->second[i];
}

std::vector<double>& points(Spline& s, std::string_view name, double def)
{
    std::vector<double>& v = s.pointAttrs[std::string(name)];
    if (v.size() != s.size())
        v.assign(s.size(), def);
    return v;
}

int64_t nextId(const curve::Splines& s)
{
    int64_t next = 1;
    for (const Spline& sp : s)
        next = std::max(next, int64_t(get(sp, kId, 0.0)) + 1);
    return next;
}

int ensureIds(curve::Splines& s)
{
    int64_t next = nextId(s);
    int named = 0;
    for (Spline& sp : s)
        if (!sp.attrs.contains(std::string(kId)))
        {
            set(sp, kId, double(next++));
            ++named;
        }
    return named;
}

uint32_t seed(const Spline& s, size_t index, uint32_t salt)
{
    const uint64_t id = uint64_t(get(s, kId, double(index + 1)));
    uint64_t h = id * 0x9E3779B97F4A7C15ull ^ (uint64_t(salt) + 0x632BE59BD9B4E019ull);
    h ^= h >> 31;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 29;
    return uint32_t(h);
}

double length(const Spline& s)
{
    double l = 0.0;
    for (size_t i = 1; i < s.size(); ++i)
        l += glm::distance(s.points[i - 1], s.points[i]);
    return l;
}

Spline resampled(const Spline& in, int n)
{
    n = std::max(2, n);
    const Spline s = in.kind == curve::Kind::Poly ? in : curve::convert(in, curve::Kind::Poly, 0.01);
    Spline out;
    out.kind = curve::Kind::Poly;
    out.attrs = s.attrs;
    if (s.size() == 0)
        return out;
    std::vector<double> at(s.size(), 0.0);
    for (size_t i = 1; i < s.size(); ++i)
        at[i] = at[i - 1] + glm::distance(s.points[i - 1], s.points[i]);
    const double total = at.back();
    // Where each new point falls: segment index and fraction.
    std::vector<std::pair<size_t, double>> where;
    for (int k = 0; k < n; ++k)
    {
        if (s.size() < 2 || total < 1e-12)
        {
            where.push_back({0, 0.0});
            continue;
        }
        const double target = total * k / (n - 1);
        size_t seg = size_t(std::upper_bound(at.begin(), at.end(), target) - at.begin());
        seg = std::clamp<size_t>(seg, 1, s.size() - 1) - 1;
        const double l = at[seg + 1] - at[seg];
        where.push_back({seg, l > 1e-12 ? std::clamp((target - at[seg]) / l, 0.0, 1.0) : 0.0});
    }
    auto sample = [&](const std::vector<double>& v) {
        std::vector<double> r;
        r.reserve(size_t(n));
        for (auto [seg, t] : where)
            r.push_back(seg + 1 < v.size() ? v[seg] + (v[seg + 1] - v[seg]) * t : v[seg]);
        return r;
    };
    for (auto [seg, t] : where)
        out.points.push_back(seg + 1 < s.size() ? glm::mix(s.points[seg], s.points[seg + 1], t) : s.points[seg]);
    if (s.radius.size() == s.size())
        out.radius = sample(s.radius);
    if (s.tilt.size() == s.size())
        out.tilt = sample(s.tilt);
    for (const auto& [name, v] : s.pointAttrs)
        if (v.size() == s.size())
            out.pointAttrs[name] = sample(v);
    return out;
}
} // namespace lila::groom::attr
