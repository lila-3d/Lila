#include "scene/AnimKeyOps.h"

#include "ops/Command.h"
#include "core/Profile.h"
#include "scene/Anim.h"
#include "scene/CurveModifiers.h"
#include "scene/Graph.h"
#include "scene/Transform.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>

namespace lila::scene::anim
{
namespace
{
constexpr double kKeyOpsEps = 1e-6;
constexpr double kKeyOpsPi = 3.14159265358979323846;
constexpr const char* kBlendNames[] = {"neighbour", "ease", "pushPull", "smooth", "default"};
constexpr const char* kFilterNames[] = {"butterworth", "gaussian", "simplify", "resample"};

double keyOpsDegrees(double slope) { return std::atan(slope) * 180.0 / kKeyOpsPi; }
double keyOpsSlope(double degrees) { return std::tan(std::clamp(degrees, -89.999, 89.999) * kKeyOpsPi / 180.0); }

bool picked(const std::vector<double>& times, double t)
{
    return std::any_of(times.begin(), times.end(), [&](double s) { return std::abs(s - t) <= kKeyOpsEps; });
}

double nearestPicked(const std::vector<double>& times, double t)
{
    double best = 1e300;
    for (double s : times)
        best = std::min(best, std::abs(s - t));
    return best;
}

/** Fixed slope after a value scale of sv and a time scale of st. */
double scaledAngle(double degrees, double sv, double st) { return keyOpsDegrees(keyOpsSlope(degrees) * sv / (st == 0.0 ? 1.0 : st)); }

/** Frame times from t0 to t1 every `step` frames, with t0 and t1 themselves. */
std::vector<double> frameTimes(double t0, double t1, double fps, double step)
{
    std::vector<double> out{t0};
    step = std::max(step, 1e-3);
    for (double f = std::ceil(t0 * fps / step - 1e-9) * step; f / fps < t1 - kKeyOpsEps; f += step)
        if (f / fps > t0 + kKeyOpsEps)
            out.push_back(f / fps);
    if (t1 > t0 + kKeyOpsEps)
        out.push_back(t1);
    return out;
}

/** RBJ second-order low-pass applied forwards then backwards, ends pinned to the input. */
std::vector<double> butterworth(const std::vector<double>& x, double cutoff, double fps)
{
    const size_t n = x.size();
    if (n < 4)
        return x;
    const double fc = std::clamp(cutoff, 1e-3, 0.45 * fps);
    const double w = 2.0 * kKeyOpsPi * fc / fps, cw = std::cos(w), alpha = std::sin(w) / std::sqrt(2.0);
    const double a0 = 1.0 + alpha;
    const double b0 = (1.0 - cw) / 2.0 / a0, b1 = (1.0 - cw) / a0, b2 = b0, a1 = -2.0 * cw / a0, a2 = (1.0 - alpha) / a0;
    // Odd reflection at both ends keeps the filter from pulling the ends towards zero.
    const size_t pad = std::min(n - 1, size_t(3 * std::ceil(fps / fc)));
    std::vector<double> y;
    y.reserve(n + 2 * pad);
    for (size_t i = pad; i > 0; --i)
        y.push_back(2.0 * x[0] - x[i]);
    y.insert(y.end(), x.begin(), x.end());
    for (size_t i = 1; i <= pad; ++i)
        y.push_back(2.0 * x[n - 1] - x[n - 1 - i]);
    auto pass = [&](std::vector<double>& v) {
        double x1 = v[0], x2 = v[0], y1 = v[0], y2 = v[0];
        for (double& s : v)
        {
            const double in = s;
            const double out = b0 * in + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1;
            x1 = in;
            y2 = y1;
            y1 = out;
            s = out;
        }
    };
    pass(y);
    std::reverse(y.begin(), y.end());
    pass(y);
    std::reverse(y.begin(), y.end());
    std::vector<double> out(y.begin() + std::ptrdiff_t(pad), y.begin() + std::ptrdiff_t(pad + n));
    const double d0 = x[0] - out[0], d1 = x[n - 1] - out[n - 1];
    for (size_t i = 0; i < n; ++i)
        out[i] += d0 + (d1 - d0) * double(i) / double(n - 1);
    return out;
}

std::vector<double> gaussian(const std::vector<double>& times, const std::vector<double>& values, double sigma)
{
    std::vector<double> out(values.size());
    sigma = std::max(sigma, 1e-6);
    for (size_t i = 0; i < values.size(); ++i)
    {
        double sum = 0.0, wsum = 0.0;
        for (size_t j = 0; j < values.size(); ++j)
        {
            const double d = (times[j] - times[i]) / sigma;
            if (std::abs(d) > 3.0)
                continue;
            const double w = std::exp(-0.5 * d * d);
            sum += w * values[j];
            wsum += w;
        }
        out[i] = wsum > 0.0 ? sum / wsum : values[i];
    }
    return out;
}

/** Keys of c within [t0, t1] replaced by `fresh`. */
void replaceRange(Curve& c, double t0, double t1, const std::vector<Key>& fresh)
{
    std::erase_if(c.keys, [&](const Key& k) { return k.time >= t0 - kKeyOpsEps && k.time <= t1 + kKeyOpsEps; });
    for (const Key& k : fresh)
        c.keys.push_back(k);
    c.sort();
}

void replaceRange(RotationCurve& rc, double t0, double t1, const std::vector<RotKey>& fresh)
{
    std::erase_if(rc.keys, [&](const RotKey& k) { return k.time >= t0 - kKeyOpsEps && k.time <= t1 + kKeyOpsEps; });
    for (const RotKey& k : fresh)
        rc.keys.push_back(k);
    rc.sort();
}

/** The fewest Hermite keys (fixed to the source's real slopes) within tol of `eval` at every one of `times`. */
std::vector<Key> simplified(const std::function<double(double)>& eval, const std::vector<double>& times, double tol, bool weighted)
{
    constexpr double h = 1e-5;
    auto keyAt = [&](double t) {
        Key k{.time = t, .value = eval(t), .in = Tangent::Fixed, .out = Tangent::Fixed};
        k.inAngle = keyOpsDegrees((k.value - eval(t - h)) / h);
        k.outAngle = keyOpsDegrees((eval(t + h) - k.value) / h);
        k.broken = std::abs(k.inAngle - k.outAngle) > 0.01;
        return k;
    };
    std::vector<double> want(times.size());
    for (size_t i = 0; i < times.size(); ++i)
        want[i] = eval(times[i]);
    Curve trial;
    trial.weighted = weighted;
    trial.keys = {keyAt(times.front())};
    if (times.size() > 1)
        trial.keys.push_back(keyAt(times.back()));
    for (;;)
    {
        double worst = tol;
        size_t at = times.size();
        for (size_t i = 0; i < times.size(); ++i)
            if (double e = std::abs(trial.evaluate(times[i]) - want[i]); e > worst)
            {
                worst = e;
                at = i;
            }
        if (at == times.size() || trial.find(times[at]) >= 0)
            break;
        trial.insert(keyAt(times[at]));
    }
    return trial.keys;
}
} // namespace

double KeyTransform::time(double t, double fps) const
{
    double out = timePivot + (t - timePivot) * timeScale + timeOffset;
    return snap && fps > 0.0 ? std::round(out * fps) / fps : out;
}

double KeyTransform::value(double v, double weight) const
{
    return valuePivot + (v - valuePivot) * (1.0 + (valueScale - 1.0) * weight) + valueOffset * weight;
}

double falloffWeight(double distance, double radius)
{
    if (radius <= 0.0 || distance >= radius)
        return distance <= kKeyOpsEps ? 1.0 : 0.0;
    const double x = 1.0 - distance / radius;
    return x * x * (3.0 - 2.0 * x);
}

int transformKeys(Curve& c, const std::vector<double>& times, const KeyTransform& x, double fps)
{
    std::vector<Key> moved, kept;
    const double st = x.timeScale;
    for (const Key& k : c.keys)
    {
        if (!picked(times, k.time))
        {
            Key n = k;
            if (x.falloff > 0.0)
                if (double w = falloffWeight(nearestPicked(times, k.time), x.falloff); w > 0.0)
                {
                    n.value = x.value(k.value, w);
                    const double sv = 1.0 + (x.valueScale - 1.0) * w;
                    if (n.in == Tangent::Fixed)
                        n.inAngle = scaledAngle(n.inAngle, sv, 1.0);
                    if (n.out == Tangent::Fixed)
                        n.outAngle = scaledAngle(n.outAngle, sv, 1.0);
                }
            kept.push_back(n);
            continue;
        }
        Key m = k;
        m.time = x.time(k.time, fps);
        m.value = x.value(k.value);
        if (m.in == Tangent::Fixed)
            m.inAngle = scaledAngle(m.inAngle, x.valueScale, st);
        if (m.out == Tangent::Fixed)
            m.outAngle = scaledAngle(m.outAngle, x.valueScale, st);
        // Time flipped: what came in now goes out.
        if (st < 0.0)
        {
            std::swap(m.in, m.out);
            std::swap(m.inAngle, m.outAngle);
            std::swap(m.inWeight, m.outWeight);
        }
        moved.push_back(m);
    }
    if (moved.empty())
        return 0;
    c.keys = std::move(kept);
    for (const Key& m : moved)
    {
        if (int i = c.find(m.time); i >= 0)
            c.keys[size_t(i)] = m;
        else
            c.insert(m);
    }
    return int(moved.size());
}

int transformKeys(RotationCurve& rc, const std::array<std::vector<double>, 3>& times, const std::array<KeyTransform, 3>& x, double fps)
{
    std::vector<RotKey> moved, kept;
    for (size_t i = 0; i < rc.keys.size(); ++i)
    {
        RotKey k = rc.keys[i];
        bool any = false;
        glm::dvec3 e = k.euler;
        for (int a = 0; a < 3; ++a)
        {
            if (picked(times[size_t(a)], k.time))
            {
                any = true;
                e[a] = x[size_t(a)].value(e[a]);
            }
            else if (x[size_t(a)].falloff > 0.0 && !times[size_t(a)].empty())
                if (double w = falloffWeight(nearestPicked(times[size_t(a)], k.time), x[size_t(a)].falloff); w > 0.0)
                    e[a] = x[size_t(a)].value(e[a], w);
        }
        if (e != k.euler)
        {
            rc.setEuler(i, e);
            k = rc.keys[i];
        }
        if (!any)
        {
            kept.push_back(k);
            continue;
        }
        const KeyTransform& tx = x[0];
        k.time = tx.time(k.time, fps);
        const double st = tx.timeScale == 0.0 ? 1.0 : tx.timeScale;
        k.inVelocity /= st;
        k.outVelocity /= st;
        if (st < 0.0)
        {
            std::swap(k.in, k.out);
            std::swap(k.inVelocity, k.outVelocity);
            std::swap(k.inWeight, k.outWeight);
        }
        moved.push_back(k);
    }
    if (moved.empty())
        return 0;
    rc.keys = std::move(kept);
    for (const RotKey& m : moved)
    {
        if (int j = rc.find(m.time); j >= 0)
            rc.keys[size_t(j)] = m;
        else
        {
            rc.keys.push_back(m);
            rc.sort();
        }
    }
    return int(moved.size());
}

const char* blendModeName(BlendMode m)
{
    return kBlendNames[size_t(m)];
}

BlendMode blendModeFromName(std::string_view name)
{
    for (size_t i = 0; i < std::size(kBlendNames); ++i)
        if (name == kBlendNames[i])
            return BlendMode(i);
    throw std::invalid_argument("unknown blend mode '" + std::string(name) + "' (neighbour, ease, pushPull, smooth, default)");
}

std::vector<double> blendValues(const std::vector<double>& times, const std::vector<double>& values, const std::vector<char>& selected,
                                BlendMode mode, double amount, double defaultValue)
{
    const size_t n = values.size();
    std::vector<double> out = values;
    for (size_t i = 0; i < n; ++i)
    {
        if (!selected[i])
            continue;
        // The picked run's neighbours: the nearest unpicked key each side, else the run's own end.
        size_t p = i, q = i;
        while (p > 0 && selected[p])
            --p;
        if (selected[p])
            for (p = i; p > 0 && selected[p - 1]; --p)
                ;
        while (q + 1 < n && selected[q])
            ++q;
        if (selected[q])
            for (q = i; q + 1 < n && selected[q + 1]; ++q)
                ;
        const double v = values[i], vp = values[p], vq = values[q];
        const double span = times[q] - times[p];
        const double x = span > 0.0 ? std::clamp((times[i] - times[p]) / span, 0.0, 1.0) : 0.5;
        switch (mode)
        {
        case BlendMode::Neighbour: out[i] = amount < 0.0 ? v + (vp - v) * -amount : v + (vq - v) * amount; break;
        case BlendMode::Ease:
        {
            const double shape = 1.0 + 3.0 * std::abs(amount);
            const double f = amount < 0.0 ? std::pow(x, shape) : amount > 0.0 ? 1.0 - std::pow(1.0 - x, shape) : x;
            out[i] = vp + (vq - vp) * f;
            break;
        }
        case BlendMode::PushPull:
        {
            const double line = vp + (vq - vp) * x;
            out[i] = line + (v - line) * (1.0 + amount);
            break;
        }
        case BlendMode::Smooth:
        {
            const double avg = 0.5 * (values[i > 0 ? i - 1 : i] + values[i + 1 < n ? i + 1 : i]);
            out[i] = v + (avg - v) * amount;
            break;
        }
        case BlendMode::Default: out[i] = v + (defaultValue - v) * amount; break;
        }
    }
    return out;
}

int blendKeys(Curve& c, const std::vector<double>& times, BlendMode mode, double amount, double defaultValue)
{
    std::vector<double> t, v;
    std::vector<char> sel;
    int count = 0;
    for (const Key& k : c.keys)
    {
        t.push_back(k.time);
        v.push_back(k.value);
        sel.push_back(picked(times, k.time) ? 1 : 0);
        count += sel.back();
    }
    std::vector<double> out = blendValues(t, v, sel, mode, amount, defaultValue);
    for (size_t i = 0; i < c.keys.size(); ++i)
        c.keys[i].value = out[i];
    return count;
}

int blendKeys(RotationCurve& rc, int axis, const std::vector<double>& times, BlendMode mode, double amount, double defaultValue)
{
    axis = std::clamp(axis, 0, 2);
    std::vector<double> t, v;
    std::vector<char> sel;
    int count = 0;
    for (const RotKey& k : rc.keys)
    {
        t.push_back(k.time);
        v.push_back(k.euler[axis]);
        sel.push_back(picked(times, k.time) ? 1 : 0);
        count += sel.back();
    }
    std::vector<double> out = blendValues(t, v, sel, mode, amount, defaultValue);
    for (size_t i = 0; i < rc.keys.size(); ++i)
        if (sel[i])
        {
            glm::dvec3 e = rc.keys[i].euler;
            e[axis] = out[i];
            rc.setEuler(i, e);
        }
    return count;
}

const char* filterName(Filter f)
{
    return kFilterNames[size_t(f)];
}

Filter filterFromName(std::string_view name)
{
    for (size_t i = 0; i < std::size(kFilterNames); ++i)
        if (name == kFilterNames[i])
            return Filter(i);
    throw std::invalid_argument("unknown filter '" + std::string(name) + "' (butterworth, gaussian, simplify, resample)");
}

int filterKeys(Curve& c, double t0, double t1, const FilterSettings& s, double fps)
{
    LILA_ZONE();
    if (c.keys.empty() || t1 < t0)
        return int(c.keys.size());
    const Curve src = c.looped();
    auto eval = [&](double t) { return src.evaluate(t); };
    switch (s.filter)
    {
    case Filter::Gaussian:
    {
        std::vector<double> t, v;
        std::vector<size_t> idx;
        for (size_t i = 0; i < c.keys.size(); ++i)
            if (c.keys[i].time >= t0 - kKeyOpsEps && c.keys[i].time <= t1 + kKeyOpsEps)
            {
                idx.push_back(i);
                t.push_back(c.keys[i].time);
                v.push_back(c.keys[i].value);
            }
        std::vector<double> out = gaussian(t, v, s.width / fps);
        for (size_t j = 0; j < idx.size(); ++j)
            c.keys[idx[j]].value = out[j];
        break;
    }
    case Filter::Resample:
    case Filter::Butterworth:
    {
        std::vector<double> times = frameTimes(t0, t1, fps, s.filter == Filter::Resample ? s.step : 1.0);
        std::vector<double> values(times.size());
        for (size_t i = 0; i < times.size(); ++i)
            values[i] = eval(times[i]);
        if (s.filter == Filter::Butterworth)
            values = butterworth(values, s.cutoff, fps);
        std::vector<Key> fresh;
        for (size_t i = 0; i < times.size(); ++i)
            fresh.push_back({.time = times[i], .value = values[i]});
        replaceRange(c, t0, t1, fresh);
        break;
    }
    case Filter::Simplify:
    {
        std::vector<double> times = frameTimes(t0, t1, fps, 1.0);
        replaceRange(c, t0, t1, simplified(eval, times, std::max(s.tolerance, 1e-9), false));
        break;
    }
    }
    return int(c.keys.size());
}

int filterKeys(RotationCurve& rc, int axis, double t0, double t1, const FilterSettings& s, double fps)
{
    LILA_ZONE();
    if (rc.keys.empty() || t1 < t0)
        return int(rc.keys.size());
    const RotationCurve src = rc.looped();
    auto axes = [&](int a) { return axis < 0 || axis == a; };
    if (s.filter == Filter::Gaussian)
    {
        std::vector<double> t;
        std::vector<size_t> idx;
        for (size_t i = 0; i < rc.keys.size(); ++i)
            if (rc.keys[i].time >= t0 - kKeyOpsEps && rc.keys[i].time <= t1 + kKeyOpsEps)
            {
                idx.push_back(i);
                t.push_back(rc.keys[i].time);
            }
        std::array<std::vector<double>, 3> out;
        for (int a = 0; a < 3; ++a)
        {
            std::vector<double> v;
            for (size_t i : idx)
                v.push_back(rc.keys[i].euler[a]);
            out[size_t(a)] = axes(a) ? gaussian(t, v, s.width / fps) : v;
        }
        for (size_t j = 0; j < idx.size(); ++j)
            rc.setEuler(idx[j], {out[0][j], out[1][j], out[2][j]});
        return int(rc.keys.size());
    }
    std::vector<double> times = frameTimes(t0, t1, fps, s.filter == Filter::Resample ? s.step : 1.0);
    std::array<std::vector<double>, 3> v;
    for (double t : times)
    {
        glm::dvec3 e = src.evaluate(t);
        for (int a = 0; a < 3; ++a)
            v[size_t(a)].push_back(e[a]);
    }
    if (s.filter == Filter::Butterworth)
        for (int a = 0; a < 3; ++a)
            if (axes(a))
                v[size_t(a)] = butterworth(v[size_t(a)], s.cutoff, fps);
    auto keysAt = [&](const std::vector<size_t>& pick) {
        std::vector<RotKey> fresh;
        for (size_t i : pick)
            fresh.push_back(rc.makeKey(times[i], {v[0][i], v[1][i], v[2][i]}));
        return fresh;
    };
    std::vector<size_t> all(times.size());
    for (size_t i = 0; i < all.size(); ++i)
        all[i] = i;
    if (s.filter != Filter::Simplify)
    {
        replaceRange(rc, t0, t1, keysAt(all));
        return int(rc.keys.size());
    }
    // Greedy: add the frame with the largest error on any axis until every frame is within tolerance.
    std::vector<size_t> pick = {0};
    if (times.size() > 1)
        pick.push_back(times.size() - 1);
    const double tol = std::max(s.tolerance, 1e-9);
    for (;;)
    {
        RotationCurve trial = rc;
        replaceRange(trial, t0, t1, keysAt(pick));
        double worst = tol;
        size_t at = times.size();
        for (size_t i = 0; i < times.size(); ++i)
        {
            glm::dvec3 e = trial.evaluate(times[i]);
            for (int a = 0; a < 3; ++a)
                if (double err = std::abs(e[a] - v[size_t(a)][i]); err > worst)
                {
                    worst = err;
                    at = i;
                }
        }
        if (at == times.size() || std::find(pick.begin(), pick.end(), at) != pick.end())
        {
            rc = std::move(trial);
            break;
        }
        pick.insert(std::upper_bound(pick.begin(), pick.end(), at), at);
    }
    return int(rc.keys.size());
}

namespace
{
/** One channel's picked keys: a scalar curve (axis -1) or an Euler axis of a rotation curve (-1 for all three). */
struct KeySet
{
    NodeId curve = kNoNode;
    int axis = -1;
    PlugRef plug;
    /** Empty with all set: every key in the call's time range. */
    std::vector<double> times;
    bool all = false;
    std::optional<double> valuePivot, valueScale, valueOffset;
};

std::pair<double, double> rangeOf(const Args& a)
{
    if (!a.has("time"))
        return {-1e300, 1e300};
    const json& t = a.flags["time"];
    if (t.is_number())
        return {t.get<double>() - kKeyOpsEps, t.get<double>() + kKeyOpsEps};
    if (t.is_array() && t.size() == 2)
        return {t[0].is_number() ? t[0].get<double>() : -1e300, t[1].is_number() ? t[1].get<double>() : 1e300};
    throw CommandError("time takes seconds or (start, end)");
}

void addPlug(Graph& g, std::vector<KeySet>& out, const PlugRef& p, KeySet base)
{
    NodeId c = curveFor(g, p);
    if (!c)
        return;
    base.curve = c;
    base.plug = p;
    base.axis = isRotationCurve(*g.node(c)) ? p.component : -1;
    out.push_back(std::move(base));
}

void addName(Graph& g, std::vector<KeySet>& out, const std::string& name, const KeySet& base)
{
    if (name.find('.') != std::string::npos)
    {
        PlugRef p;
        try
        {
            p = g.plug(name);
        }
        catch (const std::exception& e)
        {
            throw CommandError(e.what());
        }
        if (p.component < 0 && g.attr(p).type == PlugType::Vec3)
        {
            NodeId whole = curveFor(g, {p.node, p.attr, 0});
            if (whole && isRotationCurve(*g.node(whole)))
            {
                KeySet s = base;
                s.curve = whole;
                s.plug = p;
                out.push_back(std::move(s));
                return;
            }
            for (int c = 0; c < 3; ++c)
                addPlug(g, out, {p.node, p.attr, c}, base);
            return;
        }
        addPlug(g, out, p, base);
        return;
    }
    Node* n = g.find(name);
    if (!n)
        throw CommandError("no node named '" + name + "'");
    if (isAnyCurve(*n))
    {
        KeySet s = base;
        s.curve = n->id();
        out.push_back(std::move(s));
        return;
    }
    std::set<NodeId> rotations;
    for (const AnimatedPlug& ap : animatedPlugs(g, n->id()))
    {
        if (isRotationCurve(*g.node(ap.curve)) && !rotations.insert(ap.curve).second)
            continue;
        KeySet s = base;
        s.curve = ap.curve;
        s.plug = ap.plug;
        s.axis = -1;
        out.push_back(std::move(s));
    }
}

/**
 * keys=[{curve: name, times: [...], valuePivot, valueScale, valueOffset}] picks keys exactly; otherwise the objects
 * (curves, plugs or nodes, default the selection) with every key in time.
 */
std::vector<KeySet> keySetsFrom(Graph& g, const Args& a)
{
    std::vector<KeySet> out;
    if (a.has("keys"))
    {
        const json& keys = a.flags["keys"];
        if (!keys.is_array())
            throw CommandError("keys takes a list of {curve, times}");
        for (const json& e : keys)
        {
            if (!e.is_object() || !e.contains("curve") || !e["curve"].is_string() || !e.contains("times") || !e["times"].is_array())
                throw CommandError("each keys entry needs curve and times: " + e.dump());
            KeySet base;
            for (const json& t : e["times"])
                if (t.is_number())
                    base.times.push_back(t.get<double>());
            for (auto [field, slot] : {std::pair{"valuePivot", &base.valuePivot}, {"valueScale", &base.valueScale}, {"valueOffset", &base.valueOffset}})
                if (e.contains(field) && e[field].is_number())
                    *slot = e[field].get<double>();
            addName(g, out, e["curve"].get<std::string>(), base);
        }
        return out;
    }
    KeySet base;
    base.all = true;
    for (const std::string& obj : a.objects())
        addName(g, out, obj, base);
    return out;
}

std::vector<double> timesOf(const Node& curve, const KeySet& s, std::pair<double, double> range)
{
    if (!s.all)
        return s.times;
    std::vector<double> out;
    for (double t : curveKeyTimes(curve))
        if (t >= range.first && t <= range.second)
            out.push_back(t);
    return out;
}

/** The value the channel rests at: the driven attribute's default. */
double defaultOf(Graph& g, const KeySet& s, int axis)
{
    Node* c = g.node(s.curve);
    std::vector<PlugRef> dsts;
    if (c)
    {
        const int out = isRotationCurve(*c) ? int(RotationCurveNode::kOutput) : int(CurveNode::kOutput);
        dsts = g.destinations({s.curve, out, -1});
        for (int k = 0; k < 3 && dsts.empty(); ++k)
            dsts = g.destinations({s.curve, out, k});
    }
    PlugRef p = s.plug.node ? s.plug : dsts.empty() ? PlugRef{} : dsts.front();
    if (!p.node || !g.node(p.node))
        return 0.0;
    const PlugValue& d = g.attr(p).defaultValue;
    if (const auto* v = std::get_if<double>(&d))
        return *v;
    if (const auto* v = std::get_if<glm::dvec3>(&d))
        return (*v)[std::clamp(p.component >= 0 ? p.component : axis, 0, 2)];
    return 0.0;
}

double fpsNow() { return playback().fps > 0.0 ? playback().fps : 30.0; }

/** Scalar curves and rotation curves each written once after every set picking keys on them has applied. */
struct Batch
{
    Graph& g;
    std::map<NodeId, Curve> scalars;
    std::map<NodeId, RotationCurve> rotations;

    Curve& scalar(NodeId id)
    {
        auto it = scalars.find(id);
        return it != scalars.end() ? it->second : scalars.emplace(id, curveOf(*g.node(id))).first->second;
    }
    RotationCurve& rotation(NodeId id)
    {
        auto it = rotations.find(id);
        return it != rotations.end() ? it->second : rotations.emplace(id, rotationCurveOf(*g.node(id))).first->second;
    }
    void write()
    {
        for (auto& [id, c] : scalars)
            setCurve(g, id, c);
        for (auto& [id, c] : rotations)
            setRotationCurve(g, id, c);
    }
};

std::vector<FlagDef> keyFlags()
{
    using T = ArgType;
    return {
        {"keys", "k", T::Any, nullptr,
         "Exact keys: [{curve: curve or plug name, times: [seconds], valuePivot, valueScale, valueOffset}], the last three "
         "overriding the call's for that channel. Without it, every key of the objects in time."},
        {"time", "t", T::Any, nullptr, "Seconds or (start, end): the keys of the objects to use when keys is not given."},
    };
}
} // namespace

void registerKeyOpCommands()
{
    using T = ArgType;
    std::vector<FlagDef> transformFlags = keyFlags();
    for (FlagDef f : std::vector<FlagDef>{
             {"timeOffset", "to", T::Float, 0.0, "Seconds added to key times."},
             {"timeScale", "ts", T::Float, 1.0, "Key times scale about timePivot (negative reverses)."},
             {"timePivot", "tp", T::Float, 0.0, "Seconds."},
             {"valueOffset", "vo", T::Float, 0.0, "Added to key values."},
             {"valueScale", "vs", T::Float, 1.0, "Key values scale about valuePivot."},
             {"valuePivot", "vp", T::Float, 0.0, "Value the scale is about."},
             {"falloff", "fo", T::Float, 0.0, "Seconds: unpicked keys this close to a picked one follow the value change, less with distance.", {}, {}, 0.0},
             {"snap", "sn", T::Bool, false, "Moved keys land on whole frames."},
         })
        transformFlags.push_back(f);
    ops::registerCommand({
        .name = "transformKeys",
        .doc = "Move and scale keys in time and value about a pivot, optionally with neighbouring keys following the value "
               "change (falloff). Moved keys replace keys already at their new time. Returns the number of keys moved.",
        .positionalDoc = "objects",
        .flags = transformFlags,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const auto range = rangeOf(a);
            KeyTransform base{.timeOffset = a.get<double>("timeOffset"), .timeScale = a.get<double>("timeScale", 1.0),
                              .timePivot = a.get<double>("timePivot"), .valueOffset = a.get<double>("valueOffset"),
                              .valueScale = a.get<double>("valueScale", 1.0), .valuePivot = a.get<double>("valuePivot"),
                              .falloff = a.get<double>("falloff"), .snap = a.get<bool>("snap")};
            Batch batch{g};
            std::map<NodeId, std::array<std::vector<double>, 3>> rotTimes;
            std::map<NodeId, std::array<KeyTransform, 3>> rotX;
            int count = 0;
            for (const KeySet& s : keySetsFrom(g, a))
            {
                KeyTransform x = base;
                if (s.valuePivot)
                    x.valuePivot = *s.valuePivot;
                if (s.valueScale)
                    x.valueScale = *s.valueScale;
                if (s.valueOffset)
                    x.valueOffset = *s.valueOffset;
                std::vector<double> times = timesOf(*g.node(s.curve), s, range);
                if (!isRotationCurve(*g.node(s.curve)))
                {
                    count += transformKeys(batch.scalar(s.curve), times, x, fpsNow());
                    continue;
                }
                auto& at = rotTimes[s.curve];
                auto& ax = rotX.try_emplace(s.curve, std::array<KeyTransform, 3>{base, base, base}).first->second;
                for (int k = 0; k < 3; ++k)
                    if (s.axis < 0 || s.axis == k)
                    {
                        at[size_t(k)].insert(at[size_t(k)].end(), times.begin(), times.end());
                        ax[size_t(k)] = x;
                    }
            }
            for (auto& [id, times] : rotTimes)
                count += transformKeys(batch.rotation(id), times, rotX[id], fpsNow());
            batch.write();
            return count;
        },
    });

    std::vector<FlagDef> blendFlags = keyFlags();
    blendFlags.push_back({"mode", "m", T::String, "neighbour",
                          "neighbour: towards the key before (< 0) or after (> 0). ease: onto an ease-in (< 0) or ease-out (> 0) "
                          "curve between the neighbours. pushPull: away from (> 0) or onto (< 0) the line between them. smooth: "
                          "towards the average of each key's neighbours. default: towards the attribute's default value.",
                          {"neighbour", "ease", "pushPull", "smooth", "default"}});
    blendFlags.push_back({"amount", "a", T::Float, 0.5, "-1 to 1.", {}, {}, -2.0, 2.0});
    ops::registerCommand({
        .name = "blendKeys",
        .doc = "Blend picked keys' values towards their neighbours, an ease, the line between their neighbours, a smoothed "
               "value or the default. Each run of picked keys blends between the unpicked keys either side. Returns the "
               "number of keys changed.",
        .positionalDoc = "objects",
        .flags = blendFlags,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            BlendMode mode;
            try
            {
                mode = blendModeFromName(a.get<std::string>("mode", "neighbour"));
            }
            catch (const std::exception& e)
            {
                throw CommandError(e.what());
            }
            const double amount = a.get<double>("amount", 0.5);
            const auto range = rangeOf(a);
            Batch batch{g};
            int count = 0;
            for (const KeySet& s : keySetsFrom(g, a))
            {
                std::vector<double> times = timesOf(*g.node(s.curve), s, range);
                if (!isRotationCurve(*g.node(s.curve)))
                {
                    count += blendKeys(batch.scalar(s.curve), times, mode, amount, defaultOf(g, s, 0));
                    continue;
                }
                for (int k = 0; k < 3; ++k)
                    if (s.axis < 0 || s.axis == k)
                        count += blendKeys(batch.rotation(s.curve), k, times, mode, amount, defaultOf(g, s, k));
            }
            batch.write();
            return count;
        },
    });

    std::vector<FlagDef> filterFlags = keyFlags();
    for (FlagDef f : std::vector<FlagDef>{
             {"filter", "f", T::String, "butterworth",
              "butterworth: low-pass every frame (a key per frame). gaussian: smooth the keys' values, same keys. simplify: "
              "the fewest keys within tolerance. resample: a key every step frames.",
              {"butterworth", "gaussian", "simplify", "resample"}},
             {"cutoff", "co", T::Float, 7.0, "Hz: Butterworth keeps motion slower than this.", {}, {}, 0.1, 60.0},
             {"width", "w", T::Float, 1.0, "Frames: Gaussian smoothing width.", {}, {}, 0.1, 50.0},
             {"tolerance", "tol", T::Float, 0.05, "Simplify: largest change allowed, in the curve's units (degrees for rotations).", {}, {}, 0.0},
             {"step", "st", T::Float, 1.0, "Resample: frames between keys.", {}, {}, 0.1, 1000.0},
         })
        filterFlags.push_back(f);
    ops::registerCommand({
        .name = "filterCurve",
        .doc = "Filter curves between their first and last picked key (every key when one or none is picked): Butterworth "
               "low-pass, Gaussian smoothing, Simplify to the fewest keys within a tolerance, or Resample. Returns the "
               "number of keys after.",
        .positionalDoc = "objects",
        .flags = filterFlags,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            FilterSettings fs;
            try
            {
                fs.filter = filterFromName(a.get<std::string>("filter", "butterworth"));
            }
            catch (const std::exception& e)
            {
                throw CommandError(e.what());
            }
            fs.cutoff = a.get<double>("cutoff", 7.0);
            fs.width = a.get<double>("width", 1.0);
            fs.tolerance = a.get<double>("tolerance", 0.05);
            fs.step = a.get<double>("step", 1.0);
            const auto range = rangeOf(a);
            Batch batch{g};
            int count = 0;
            for (const KeySet& s : keySetsFrom(g, a))
            {
                std::vector<double> times = timesOf(*g.node(s.curve), s, range);
                if (s.all && !a.has("time"))
                    times.clear();
                std::vector<double> keys = curveKeyTimes(*g.node(s.curve));
                if (keys.empty())
                    continue;
                double t0 = keys.front(), t1 = keys.back();
                if (times.size() > 1)
                {
                    t0 = *std::min_element(times.begin(), times.end());
                    t1 = *std::max_element(times.begin(), times.end());
                }
                if (!isRotationCurve(*g.node(s.curve)))
                    count += filterKeys(batch.scalar(s.curve), t0, t1, fs, fpsNow());
                else
                    count += filterKeys(batch.rotation(s.curve), s.axis, t0, t1, fs, fpsNow());
            }
            batch.write();
            return count;
        },
    });

    ops::registerCommand({
        .name = "curveModifier",
        .doc = "Add or edit a non-destructive modifier on curves (curves, plugs or nodes, default the selection's animated "
               "channels): noise, stepped or limits. A curve holds one of each type; giving a type again edits it. remove=True "
               "removes that type, or every modifier with no type. query=True returns [{curve, modifiers}].",
        .positionalDoc = "objects",
        .flags = {
            {"type", "typ", T::String, nullptr, "noise | stepped | limits", {"noise", "stepped", "limits"}},
            {"remove", "rm", T::Bool, false, "Remove this type, or every modifier when no type is given."},
            {"query", "q", T::Bool, false, ""},
            {"amplitude", "amp", T::Float, nullptr, "Noise: size of the wobble in the curve's units (degrees on rotations)."},
            {"period", "per", T::Float, nullptr, "Noise: frames per wave.", {}, {}, 0.1},
            {"phase", "ph", T::Float, nullptr, "Noise: frames the noise is shifted by."},
            {"seed", "sd", T::Int, nullptr, "Noise: a different seed gives different noise."},
            {"octaves", "oct", T::Int, nullptr, "Noise: layers of finer detail.", {}, {}, 1, 8},
            {"step", "st", T::Float, nullptr, "Stepped: frames each value holds for.", {}, {}, 0.1},
            {"offset", "of", T::Float, nullptr, "Stepped: frame the steps start from."},
            {"min", "mn", T::Float, nullptr, "Limits: lowest value."},
            {"max", "mx", T::Float, nullptr, "Limits: highest value."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const double fps = fpsNow();
            std::vector<NodeId> curves;
            for (const KeySet& s : keySetsFrom(g, Args{a.positional, json::object()}))
                if (std::find(curves.begin(), curves.end(), s.curve) == curves.end())
                    curves.push_back(s.curve);
            if (a.get<bool>("query"))
            {
                json out = json::array();
                for (NodeId id : curves)
                {
                    json mods = json::array();
                    for (const Modifier& m : modifiersOf(*g.node(id)))
                        mods.push_back(modifierToJson(m));
                    out.push_back({{"curve", g.node(id)->path()}, {"modifiers", mods}});
                }
                return out;
            }
            std::optional<ModifierType> type;
            if (a.has("type"))
            {
                try
                {
                    type = modifierTypeFromName(a.get<std::string>("type"));
                }
                catch (const std::exception& e)
                {
                    throw CommandError(e.what());
                }
            }
            const bool remove = a.get<bool>("remove");
            if (!type && !remove)
                throw CommandError("curveModifier: give a type, or remove=True");
            int count = 0;
            for (NodeId id : curves)
            {
                Node* n = g.node(id);
                std::vector<Modifier> mods = modifiersOf(*n);
                auto it = type ? std::find_if(mods.begin(), mods.end(), [&](const Modifier& m) { return m.type == *type; }) : mods.end();
                if (remove)
                {
                    if (!type)
                        mods.clear();
                    else if (it != mods.end())
                        mods.erase(it);
                }
                else
                {
                    if (it == mods.end())
                    {
                        mods.push_back(Modifier{.type = *type});
                        it = mods.end() - 1;
                    }
                    Modifier& m = *it;
                    if (a.has("amplitude"))
                        m.amplitude = a.get<double>("amplitude");
                    if (a.has("period"))
                        m.period = std::max(1e-4, a.get<double>("period") / fps);
                    if (a.has("phase"))
                        m.phase = a.get<double>("phase") / fps;
                    if (a.has("seed"))
                        m.seed = a.get<int>("seed");
                    if (a.has("octaves"))
                        m.octaves = std::clamp(a.get<int>("octaves"), 1, 8);
                    if (a.has("step"))
                        m.step = std::max(1e-4, a.get<double>("step") / fps);
                    if (a.has("offset"))
                        m.offset = a.get<double>("offset") / fps;
                    if (a.has("min"))
                        m.min = a.get<double>("min");
                    if (a.has("max"))
                        m.max = a.get<double>("max");
                }
                const int attr = isRotationCurve(*n) ? int(RotationCurveNode::kModifiers) : int(CurveNode::kModifiers);
                std::string text = modifiersToText(mods);
                if (text != n->as<std::string>(attr))
                {
                    g.setValue({id, attr}, text);
                    ++count;
                }
            }
            return count;
        },
    });
}
} // namespace lila::scene::anim
