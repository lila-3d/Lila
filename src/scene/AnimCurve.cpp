#include "scene/AnimCurve.h"

#include "core/Profile.h"
#include "scene/AnimInterpolator.h"
#include "scene/Graph.h"
#include "scene/RotationCurve.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace lila::scene::anim
{
namespace
{
constexpr const char* kTangentNames[] = {"auto", "spline", "linear", "flat", "step", "clamped", "fixed"};
constexpr const char* kInfinityNames[] = {"constant", "linear", "cycle", "cycleRelative", "oscillate"};

double secant(const Key& a, const Key& b)
{
    double dt = b.time - a.time;
    return dt > 0.0 ? (b.value - a.value) / dt : 0.0;
}

double hermite(double v0, double m0, double v1, double m1, double dt, double s)
{
    double s2 = s * s, s3 = s2 * s;
    return (2 * s3 - 3 * s2 + 1) * v0 + (s3 - 2 * s2 + s) * dt * m0 + (-2 * s3 + 3 * s2) * v1 + (s3 - s2) * dt * m1;
}

double bezier(double p0, double p1, double p2, double p3, double u)
{
    double w = 1.0 - u;
    return w * w * w * p0 + 3 * w * w * u * p1 + 3 * w * u * u * p2 + u * u * u * p3;
}
} // namespace

const char* tangentName(Tangent t)
{
    return kTangentNames[size_t(t)];
}

Tangent tangentFromName(std::string_view name)
{
    for (size_t i = 0; i < std::size(kTangentNames); ++i)
        if (name == kTangentNames[i])
            return Tangent(i);
    if (name == "clampedAuto" || name == "autoClamped")
        return Tangent::Auto;
    throw std::invalid_argument("unknown tangent type '" + std::string(name) + "' (auto, spline, linear, flat, step, clamped, fixed)");
}

const char* infinityName(Infinity i)
{
    return kInfinityNames[size_t(i)];
}

Infinity infinityFromName(std::string_view name)
{
    for (size_t i = 0; i < std::size(kInfinityNames); ++i)
        if (name == kInfinityNames[i])
            return Infinity(i);
    if (name == "cycleOffset")
        return Infinity::CycleRelative;
    throw std::invalid_argument("unknown infinity '" + std::string(name) + "' (constant, linear, cycle, cycleRelative, oscillate)");
}

double Curve::autoSlope(size_t i, bool clampOvershoot) const
{
    size_t n = keys.size();
    if (n < 2)
        return 0.0;
    if (i == 0)
        return clampOvershoot ? 0.0 : secant(keys[0], keys[1]);
    if (i == n - 1)
        return clampOvershoot ? 0.0 : secant(keys[n - 2], keys[n - 1]);
    const Key& a = keys[i - 1];
    const Key& b = keys[i + 1];
    double m = b.time > a.time ? (b.value - a.value) / (b.time - a.time) : 0.0;
    if (!clampOvershoot)
        return m;
    double dPrev = secant(a, keys[i]);
    double dNext = secant(keys[i], b);
    if (dPrev * dNext <= 0.0)
        return 0.0;
    // Fritsch-Carlson: a slope within three times both secants keeps each segment monotonic.
    double limit = 3.0 * std::min(std::abs(dPrev), std::abs(dNext));
    return std::clamp(m, -limit, limit);
}

double Curve::inSlope(size_t i) const
{
    const Key& k = keys[i];
    switch (k.in)
    {
    case Tangent::Auto: return autoSlope(i, true);
    case Tangent::Spline: return autoSlope(i, false);
    case Tangent::Linear: return i > 0 ? secant(keys[i - 1], k) : (keys.size() > 1 ? secant(k, keys[1]) : 0.0);
    case Tangent::Flat:
    case Tangent::Step: return 0.0;
    case Tangent::Clamped:
    {
        constexpr double eps = 1e-5;
        if ((i > 0 && std::abs(keys[i - 1].value - k.value) < eps) || (i + 1 < keys.size() && std::abs(keys[i + 1].value - k.value) < eps))
            return 0.0;
        return autoSlope(i, false);
    }
    case Tangent::Fixed: return std::tan(k.inAngle * 3.14159265358979323846 / 180.0);
    }
    return 0.0;
}

double Curve::outSlope(size_t i) const
{
    const Key& k = keys[i];
    switch (k.out)
    {
    case Tangent::Auto: return autoSlope(i, true);
    case Tangent::Spline: return autoSlope(i, false);
    case Tangent::Linear: return i + 1 < keys.size() ? secant(k, keys[i + 1]) : (i > 0 ? secant(keys[i - 1], k) : 0.0);
    case Tangent::Flat:
    case Tangent::Step: return 0.0;
    case Tangent::Clamped:
    {
        constexpr double eps = 1e-5;
        if ((i > 0 && std::abs(keys[i - 1].value - k.value) < eps) || (i + 1 < keys.size() && std::abs(keys[i + 1].value - k.value) < eps))
            return 0.0;
        return autoSlope(i, false);
    }
    case Tangent::Fixed: return std::tan(k.outAngle * 3.14159265358979323846 / 180.0);
    }
    return 0.0;
}

double Curve::evalSegment(size_t i, double t) const
{
    const Key& a = keys[i];
    const Key& b = keys[i + 1];
    if (const Interpolator* custom = findInterpolator(a.interpolator))
        return custom->evaluate(*this, i, t);
    if (a.out == Tangent::Step)
        return a.value;
    double dt = b.time - a.time;
    if (dt <= 0.0)
        return b.value;
    double m0 = outSlope(i), m1 = inSlope(i + 1);
    if (!weighted)
        return hermite(a.value, m0, b.value, m1, dt, (t - a.time) / dt);

    double h0 = std::clamp(a.outWeight, 0.0, 3.0) * dt / 3.0;
    double h1 = std::clamp(b.inWeight, 0.0, 3.0) * dt / 3.0;
    double x1 = a.time + h0, y1 = a.value + m0 * h0;
    double x2 = b.time - h1, y2 = b.value - m1 * h1;
    // x(u) is monotonic for handles inside the segment, so bisection always finds the one u for t.
    double lo = 0.0, hi = 1.0, u = (t - a.time) / dt;
    for (int it = 0; it < 60; ++it)
    {
        double x = bezier(a.time, x1, x2, b.time, u);
        if (std::abs(x - t) < 1e-10)
            break;
        (x < t ? lo : hi) = u;
        u = 0.5 * (lo + hi);
    }
    return bezier(a.value, y1, y2, b.value, u);
}

double Curve::evaluate(double t) const
{
    size_t n = keys.size();
    if (n == 0)
        return 0.0;
    if (n == 1)
        return keys[0].value;
    if (loop)
        return looped().evaluate(t);
    const Key& first = keys.front();
    const Key& last = keys.back();
    double range = last.time - first.time;

    if (t < first.time || t > last.time)
    {
        bool before = t < first.time;
        Infinity mode = before ? pre : post;
        switch (mode)
        {
        case Infinity::Constant: return before ? first.value : last.value;
        case Infinity::Linear: return before ? first.value + (t - first.time) * inSlope(0) : last.value + (t - last.time) * outSlope(n - 1);
        case Infinity::Cycle:
        case Infinity::CycleRelative:
        case Infinity::Oscillate:
        {
            if (range <= 0.0)
                return before ? first.value : last.value;
            double cycles = std::floor((t - first.time) / range);
            double local = t - first.time - cycles * range;
            if (mode == Infinity::Oscillate)
                return evaluate(std::fmod(std::abs(cycles), 2.0) == 1.0 ? last.time - local : first.time + local);
            double v = evaluate(first.time + local);
            if (mode == Infinity::CycleRelative)
                v += cycles * (last.value - first.value);
            return v;
        }
        }
    }

    auto it = std::upper_bound(keys.begin(), keys.end(), t, [](double x, const Key& k) { return x < k.time; });
    size_t i = size_t(it - keys.begin());
    if (i >= n)
        return last.value;
    return evalSegment(i - 1, t);
}

int Curve::find(double t, double eps) const
{
    for (size_t i = 0; i < keys.size(); ++i)
        if (std::abs(keys[i].time - t) <= eps)
            return int(i);
    return -1;
}

size_t Curve::insert(const Key& k)
{
    if (int i = find(k.time); i >= 0)
    {
        Key& slot = keys[size_t(i)];
        slot.value = k.value;
        return size_t(i);
    }
    auto it = std::upper_bound(keys.begin(), keys.end(), k.time, [](double x, const Key& key) { return x < key.time; });
    return size_t(keys.insert(it, k) - keys.begin());
}

void Curve::sort()
{
    std::stable_sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) { return a.time < b.time; });
}

Curve Curve::looped() const
{
    Curve c = *this;
    c.loop = false;
    size_t n = keys.size();
    if (!loop || n < 2)
        return c;
    Key& first = c.keys.front();
    Key& last = c.keys.back();
    double range = last.time - first.time;
    last.value = first.value;
    // The seam's neighbours across the loop: the key before the last one, a cycle early, and the second key.
    Curve seam;
    seam.weighted = weighted;
    Key before = c.keys[n - 2];
    before.time -= range;
    if (n == 2)
        before.value = first.value;
    seam.keys = {before, first, c.keys[1]};
    auto spans = [](Tangent t) { return t == Tangent::Auto || t == Tangent::Spline || t == Tangent::Clamped || t == Tangent::Linear; };
    auto degrees = [](double slope) { return std::atan(slope) * 180.0 / 3.14159265358979323846; };
    if (range > 0.0)
    {
        if (spans(first.in))
        {
            first.inAngle = degrees(seam.inSlope(1));
            first.in = Tangent::Fixed;
        }
        if (spans(first.out))
        {
            first.outAngle = degrees(seam.outSlope(1));
            first.out = Tangent::Fixed;
        }
    }
    last.in = first.in;
    last.out = first.out;
    last.inAngle = first.inAngle;
    last.outAngle = first.outAngle;
    last.inWeight = first.inWeight;
    last.outWeight = first.outWeight;
    c.post = Infinity::Cycle;
    return c;
}

json Curve::keysToJson() const
{
    json out = json::array();
    for (const Key& k : keys)
    {
        json j = {{"t", k.time}, {"v", k.value}};
        if (k.in != Tangent::Auto)
            j["i"] = tangentName(k.in);
        if (k.out != Tangent::Auto)
            j["o"] = tangentName(k.out);
        if (k.in == Tangent::Fixed)
            j["ia"] = k.inAngle;
        if (k.out == Tangent::Fixed)
            j["oa"] = k.outAngle;
        if (k.inWeight != 1.0)
            j["iw"] = k.inWeight;
        if (k.outWeight != 1.0)
            j["ow"] = k.outWeight;
        if (!k.interpolator.empty())
            j["interp"] = k.interpolator;
        if (k.breakdown)
            j["bd"] = true;
        out.push_back(std::move(j));
    }
    return out;
}

std::vector<Key> Curve::keysFromJson(const json& j)
{
    std::vector<Key> out;
    if (j.is_null())
        return out;
    if (!j.is_array())
        throw std::invalid_argument("anim keys must be a list");
    for (const json& e : j)
    {
        if (!e.is_object() || !e.contains("t") || !e["t"].is_number() || !e.contains("v") || !e["v"].is_number())
            throw std::invalid_argument("anim key needs numeric t and v: " + e.dump());
        Key k;
        k.time = e["t"].get<double>();
        k.value = e["v"].get<double>();
        if (e.contains("i"))
            k.in = tangentFromName(e["i"].get<std::string>());
        if (e.contains("o"))
            k.out = tangentFromName(e["o"].get<std::string>());
        k.breakdown = e.value("bd", false);
        k.inAngle = e.value("ia", 0.0);
        k.outAngle = e.value("oa", 0.0);
        k.inWeight = e.value("iw", 1.0);
        k.outWeight = e.value("ow", 1.0);
        k.interpolator = e.value("interp", std::string());
        out.push_back(k);
    }
    std::stable_sort(out.begin(), out.end(), [](const Key& a, const Key& b) { return a.time < b.time; });
    return out;
}

void CurveNode::compute(const EvalContext& ctx)
{
    LILA_ZONE();

    const std::string& text = as<std::string>(kKeys);
    if (text != mCachedText)
    {
        mCachedText = text;
        try
        {
            mCached.keys = text.empty() ? std::vector<Key>{} : Curve::keysFromJson(json::parse(text));
        }
        catch (const std::exception&)
        {
            mCached.keys.clear();
        }
    }
    mCached.pre = Infinity(std::clamp<int64_t>(as<int64_t>(kPreInfinity), 0, 4));
    mCached.post = Infinity(std::clamp<int64_t>(as<int64_t>(kPostInfinity), 0, 4));
    mCached.weighted = as<bool>(kWeighted);
    mCached.loop = as<bool>(kLoop);
    setOutput(kOutput, mCached.evaluate(as<bool>(kMute) ? as<double>(kMuteTime) : ctx.time));
}

bool isCurve(const Node& n)
{
    return n.type().isA("animCurve");
}

Curve curveOf(const Node& n)
{
    Curve c;
    const std::string& text = n.as<std::string>(CurveNode::kKeys);
    // Malformed keys (a hand-edited file) read as none, as compute() sees them.
    try
    {
        if (!text.empty())
            c.keys = Curve::keysFromJson(json::parse(text));
    }
    catch (const std::exception&)
    {
        c.keys.clear();
    }
    c.pre = Infinity(std::clamp<int64_t>(n.as<int64_t>(CurveNode::kPreInfinity), 0, 4));
    c.post = Infinity(std::clamp<int64_t>(n.as<int64_t>(CurveNode::kPostInfinity), 0, 4));
    c.weighted = n.as<bool>(CurveNode::kWeighted);
    c.loop = n.as<bool>(CurveNode::kLoop);
    return c;
}

void setCurve(Graph& g, NodeId id, const Curve& c)
{
    LILA_ZONE();

    Node* n = g.node(id);
    if (!n)
        throw std::invalid_argument("no such curve");
    Curve stored = c;
    // A loop key stores the first key's value and tangents too, so the keys read back as they evaluate.
    if (stored.loop && stored.keys.size() > 1)
    {
        const Key& first = stored.keys.front();
        Key& last = stored.keys.back();
        last.value = first.value;
        last.in = first.in;
        last.out = first.out;
        last.inAngle = first.inAngle;
        last.outAngle = first.outAngle;
        last.inWeight = first.inWeight;
        last.outWeight = first.outWeight;
    }
    std::string text = stored.keysToJson().dump();
    if (text != n->as<std::string>(CurveNode::kKeys))
        g.setValue({id, CurveNode::kKeys}, text);
    if (int64_t(c.pre) != n->as<int64_t>(CurveNode::kPreInfinity))
        g.setValue({id, CurveNode::kPreInfinity}, int64_t(c.pre));
    if (int64_t(c.post) != n->as<int64_t>(CurveNode::kPostInfinity))
        g.setValue({id, CurveNode::kPostInfinity}, int64_t(c.post));
    if (c.weighted != n->as<bool>(CurveNode::kWeighted))
        g.setValue({id, CurveNode::kWeighted}, c.weighted);
    if (c.loop != n->as<bool>(CurveNode::kLoop))
        g.setValue({id, CurveNode::kLoop}, c.loop);
}

void registerTypes()
{
    if (types::find("animCurve"))
        return;
    std::vector<std::string> inf(std::begin(kInfinityNames), std::end(kInfinityNames));
    NodeType base;
    base.name = "animCurve";
    base.timeDependent = true;
    base.ownAttrs = {
        {.name = "keys", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "Keys as JSON: [{t: seconds, v: value, i/o: tangent types, ia/oa: fixed angles, iw/ow: weights}]."},
        {.name = "preInfinity", .shortName = "pri", .type = PlugType::Enum, .defaultValue = int64_t(0), .enumNames = inf,
         .doc = "Before the first key."},
        {.name = "postInfinity", .shortName = "poi", .type = PlugType::Enum, .defaultValue = int64_t(0), .enumNames = inf,
         .doc = "After the last key."},
        {.name = "weightedTangents", .shortName = "wt", .type = PlugType::Bool, .defaultValue = false,
         .doc = "Tangent handles have length as well as angle."},
        {.name = "output", .shortName = "o", .type = PlugType::Float, .defaultValue = 0.0, .flags = AttrOutput,
         .doc = "Value at the current time."},
        {.name = "loop", .shortName = "lp", .type = PlugType::Bool, .defaultValue = false,
         .doc = "The last key mirrors the first key's value and tangents, and the curve cycles after it."},
        {.name = "mute", .shortName = "mu", .type = PlugType::Bool, .defaultValue = false,
         .doc = "Hold the value at muteTime instead of playing the curve (the Channel Box's Mute)."},
        {.name = "muteTime", .shortName = "mut", .type = PlugType::Float, .defaultValue = 0.0, .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "Seconds: the time a muted curve holds."},
    };
    const NodeType& b = types::registerType(std::move(base));
    for (const char* name : {"animCurveTL", "animCurveTA", "animCurveTU"})
    {
        NodeType t;
        t.name = name;
        t.base = &b;
        t.timeDependent = true;
        t.create = [] { return std::make_unique<CurveNode>(); };
        types::registerType(std::move(t));
    }
    registerRotationCurveType();
}
} // namespace lila::scene::anim
