#include "scene/RotationCurve.h"

#include "core/Profile.h"
#include "scene/Graph.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace lila::scene::anim
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;
constexpr const char* kInterpNames[] = {"slerp", "smooth"};
constexpr const char* kOrderNames[] = {"xyz", "yzx", "zxy", "xzy", "yxz", "zyx"};

/** v + 2 pi k along v's axis, for the k that lands closest to target: the same rotation with the turns target asks for. */
glm::dvec3 nearestTurn(const glm::dvec3& v, const glm::dvec3& target)
{
    double angle = glm::length(v);
    glm::dvec3 axis;
    if (angle > 1e-9)
        axis = v / angle;
    else if (glm::length(target) > 1e-9)
        axis = glm::normalize(target);
    else
        return v;
    double k = std::round((glm::dot(target, axis) - angle) / kTwoPi);
    return axis * (angle + kTwoPi * k);
}

glm::dvec3 jsonVec3(const json& j)
{
    if (!j.is_array() || j.size() != 3)
        throw std::invalid_argument("rotation key expects a list of 3 numbers: " + j.dump());
    return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}
} // namespace

const char* rotInterpName(RotInterp i)
{
    return kInterpNames[size_t(i)];
}

RotInterp rotInterpFromName(std::string_view name)
{
    for (size_t i = 0; i < std::size(kInterpNames); ++i)
        if (name == kInterpNames[i])
            return RotInterp(i);
    throw std::invalid_argument("unknown rotation interpolation '" + std::string(name) + "' (slerp, smooth)");
}

glm::dquat eulerToQuat(const glm::dvec3& degrees, RotateOrder order)
{
    return glm::normalize(glm::quat_cast(glm::dmat3(eulerToMatrix(degrees, order))));
}

glm::dvec3 quatLog(glm::dquat q)
{
    if (q.w < 0.0)
        q = -q;
    glm::dvec3 v(q.x, q.y, q.z);
    double s = glm::length(v);
    if (s < 1e-12)
        return v * 2.0;
    return v * (2.0 * std::atan2(s, q.w) / s);
}

glm::dquat quatExp(const glm::dvec3& v)
{
    double angle = glm::length(v);
    if (angle < 1e-12)
        return glm::normalize(glm::dquat(1.0, v.x * 0.5, v.y * 0.5, v.z * 0.5));
    glm::dvec3 a = v * (std::sin(angle * 0.5) / angle);
    return glm::dquat(std::cos(angle * 0.5), a.x, a.y, a.z);
}

RotKey RotationCurve::makeKey(double time, const glm::dvec3& euler) const
{
    RotKey k;
    k.time = time;
    k.euler = euler;
    k.q = eulerToQuat(euler, order);
    return k;
}

glm::dvec3 RotationCurve::segment(size_t i) const
{
    const RotKey& a = keys[i];
    const RotKey& b = keys[i + 1];
    glm::dvec3 r = quatLog(glm::inverse(a.q) * b.q);
    glm::dvec3 de = b.euler - a.euler;
    double widest = std::max({std::abs(de.x), std::abs(de.y), std::abs(de.z)});
    if (widest < 180.0 - 1e-9)
        return r;
    // Typed values half a turn or more apart: follow the Euler path in steps short enough to be unambiguous, and
    // keep the turns it makes.
    int steps = int(std::ceil(widest / 45.0));
    glm::dvec3 path(0.0);
    glm::dquat prev = a.q;
    for (int s = 1; s <= steps; ++s)
    {
        glm::dquat q = eulerToQuat(a.euler + de * (double(s) / steps), order);
        glm::dvec3 step = quatLog(glm::inverse(prev) * q);
        path += glm::inverse(a.q) * (prev * step);
        prev = q;
    }
    return nearestTurn(r, path);
}

bool RotationCurve::neighbour(size_t i, bool next, glm::dvec3& r, double& dt) const
{
    size_t n = keys.size();
    if (n < 2)
        return false;
    size_t s;
    if (next)
    {
        if (i + 1 < n)
            s = i;
        else if (mCyclic)
            s = 0;
        else
            return false;
    }
    else
    {
        if (i > 0)
            s = i - 1;
        else if (mCyclic)
            s = n - 2;
        else
            return false;
    }
    r = segment(s);
    dt = keys[s + 1].time - keys[s].time;
    return dt > 0.0;
}

glm::dvec3 RotationCurve::velocity(size_t i, bool out) const
{
    const RotKey& k = keys[i];
    Tangent type = out ? k.out : k.in;
    glm::dvec3 rp(0.0), rn(0.0);
    double dp = 0.0, dn = 0.0;
    bool hp = neighbour(i, false, rp, dp);
    bool hn = neighbour(i, true, rn, dn);
    auto spline = [&]() -> glm::dvec3 {
        if (hp && hn)
            return (rp + rn) / (dp + dn);
        if (hp)
            return rp / dp;
        if (hn)
            return rn / dn;
        return glm::dvec3(0.0);
    };
    switch (type)
    {
    case Tangent::Flat:
    case Tangent::Step: return glm::dvec3(0.0);
    case Tangent::Fixed: return glm::radians(out ? k.outVelocity : k.inVelocity);
    case Tangent::Linear:
        if (out)
            return hn ? rn / dn : hp ? rp / dp : glm::dvec3(0.0);
        return hp ? rp / dp : hn ? rn / dn : glm::dvec3(0.0);
    case Tangent::Spline: return spline();
    case Tangent::Clamped:
    {
        constexpr double eps = 1e-5;
        if ((hp && glm::length(rp) < eps) || (hn && glm::length(rn) < eps))
            return glm::dvec3(0.0);
        return spline();
    }
    case Tangent::Auto:
    {
        if (!hp || !hn || glm::dot(rp, rn) <= 0.0)
            return glm::dvec3(0.0);
        glm::dvec3 m = (rp + rn) / (dp + dn);
        // As the scalar auto tangent: no faster than three times either neighbouring span, so it never overshoots.
        double limit = 3.0 * std::min(glm::length(rp) / dp, glm::length(rn) / dn);
        double len = glm::length(m);
        return len > limit && len > 0.0 ? m * (limit / len) : m;
    }
    }
    return glm::dvec3(0.0);
}

glm::dvec3 RotationCurve::inVelocity(size_t i) const
{
    if (loop)
        return looped().inVelocity(i);
    return velocity(i, false);
}

glm::dvec3 RotationCurve::outVelocity(size_t i) const
{
    if (loop)
        return looped().outVelocity(i);
    return velocity(i, true);
}

glm::dquat RotationCurve::evalSegment(size_t i, double t) const
{
    const RotKey& a = keys[i];
    const RotKey& b = keys[i + 1];
    double dt = b.time - a.time;
    if (a.out == Tangent::Step)
        return a.q;
    if (dt <= 0.0)
        return b.q;
    double u = std::clamp((t - a.time) / dt, 0.0, 1.0);
    glm::dvec3 r = segment(i);
    if (interp == RotInterp::Slerp)
        return glm::normalize(a.q * quatExp(r * u));

    // Cumulative cubic Bezier on the rotation group: q = p0 exp(w1 b1) exp(w2 b2) exp(w3 b3), the b's being the
    // cumulative Bernstein bases. Linear tangents both sides make it exactly the slerp.
    double h0 = dt / 3.0 * (weighted ? std::clamp(a.outWeight, 0.0, 3.0) : 1.0);
    double h1 = dt / 3.0 * (weighted ? std::clamp(b.inWeight, 0.0, 3.0) : 1.0);
    glm::dvec3 w1 = velocity(i, true) * h0;
    glm::dvec3 w3 = velocity(i + 1, false) * h1;
    glm::dvec3 w2 = nearestTurn(quatLog(quatExp(-w1) * quatExp(r) * quatExp(-w3)), r - w1 - w3);
    double v = 1.0 - u;
    double b1 = 1.0 - v * v * v, b2 = 3.0 * u * u - 2.0 * u * u * u, b3 = u * u * u;
    return glm::normalize(a.q * quatExp(w1 * b1) * quatExp(w2 * b2) * quatExp(w3 * b3));
}

glm::dquat RotationCurve::evalInside(double t) const
{
    size_t n = keys.size();
    auto it = std::upper_bound(keys.begin(), keys.end(), t, [](double x, const RotKey& k) { return x < k.time; });
    size_t i = size_t(it - keys.begin());
    if (i == 0)
        return keys.front().q;
    if (i >= n)
        return keys.back().q;
    return evalSegment(i - 1, t);
}

glm::dquat RotationCurve::evaluateQuat(double t) const
{
    size_t n = keys.size();
    if (n == 0)
        return glm::dquat(1.0, 0.0, 0.0, 0.0);
    if (n == 1)
        return keys[0].q;
    if (loop)
        return looped().evaluateQuat(t);
    const RotKey& first = keys.front();
    const RotKey& last = keys.back();
    double range = last.time - first.time;
    if (t >= first.time && t <= last.time)
        return evalInside(t);

    bool before = t < first.time;
    switch (before ? pre : post)
    {
    case Infinity::Constant: break;
    case Infinity::Linear:
        return before ? glm::normalize(first.q * quatExp(velocity(0, false) * (t - first.time)))
                      : glm::normalize(last.q * quatExp(velocity(n - 1, true) * (t - last.time)));
    case Infinity::Cycle:
    case Infinity::CycleRelative:
    case Infinity::Oscillate:
    {
        if (range <= 0.0)
            break;
        double cycles = std::floor((t - first.time) / range);
        double local = t - first.time - cycles * range;
        Infinity mode = before ? pre : post;
        if (mode == Infinity::Oscillate)
            return evalInside(std::fmod(std::abs(cycles), 2.0) == 1.0 ? last.time - local : first.time + local);
        glm::dquat q = evalInside(first.time + local);
        if (mode == Infinity::CycleRelative)
        {
            // Each cycle adds the turn from the first key to the last, in the parent's frame.
            glm::dvec3 world = first.q * segmentTotal();
            q = quatExp(world * cycles) * q;
        }
        return glm::normalize(q);
    }
    }
    return before ? first.q : last.q;
}

glm::dvec3 RotationCurve::segmentTotal() const
{
    glm::dvec3 total(0.0);
    for (size_t i = 0; i + 1 < keys.size(); ++i)
        total += glm::inverse(keys[0].q) * (keys[i].q * segment(i));
    return nearestTurn(quatLog(glm::inverse(keys.front().q) * keys.back().q), total);
}

glm::dvec3 RotationCurve::referenceEuler(double t) const
{
    size_t n = keys.size();
    const RotKey& first = keys.front();
    const RotKey& last = keys.back();
    if (t <= first.time || t >= last.time)
    {
        bool before = t <= first.time;
        Infinity mode = before ? pre : post;
        double range = last.time - first.time;
        if ((mode == Infinity::Cycle || mode == Infinity::CycleRelative || mode == Infinity::Oscillate) && range > 0.0 &&
            (t < first.time || t > last.time))
        {
            double cycles = std::floor((t - first.time) / range);
            double local = t - first.time - cycles * range;
            if (mode == Infinity::Oscillate)
                return referenceEuler(std::fmod(std::abs(cycles), 2.0) == 1.0 ? last.time - local : first.time + local);
            glm::dvec3 e = referenceEuler(first.time + local);
            return mode == Infinity::CycleRelative ? e + cycles * (last.euler - first.euler) : e;
        }
        if (mode == Infinity::Linear && t != first.time && t != last.time)
        {
            size_t k = before ? 0 : n - 1;
            glm::dvec3 rate(eulerSlope(k, 0, !before), eulerSlope(k, 1, !before), eulerSlope(k, 2, !before));
            return keys[k].euler + rate * (t - keys[k].time);
        }
        return before ? first.euler : last.euler;
    }
    auto it = std::upper_bound(keys.begin(), keys.end(), t, [](double x, const RotKey& k) { return x < k.time; });
    size_t i = size_t(it - keys.begin());
    const RotKey& a = keys[i - 1];
    const RotKey& b = keys[i];
    double dt = b.time - a.time;
    if (a.out == Tangent::Step || dt <= 0.0)
        return a.euler;
    // Walk from the key in steps small enough that each sample's nearest Euler solution is the continuous one.
    glm::dvec3 de = b.euler - a.euler;
    double span = std::max({glm::degrees(glm::length(segment(i - 1))), std::abs(de.x), std::abs(de.y), std::abs(de.z)});
    int steps = std::max(1, int(std::ceil(span * (t - a.time) / dt / 10.0)));
    glm::dvec3 e = a.euler;
    for (int s = 1; s <= steps; ++s)
    {
        double ts = a.time + (t - a.time) * double(s) / steps;
        e = eulerNear(glm::mat3_cast(evalSegment(i - 1, ts)), order, e);
    }
    return e;
}

glm::dvec3 RotationCurve::evaluate(double t) const
{
    if (keys.empty())
        return glm::dvec3(0.0);
    if (loop && keys.size() > 1)
        return looped().evaluate(t);
    // Exactly on a key: its own Euler value, so keys read back as typed.
    if (int i = find(t, 1e-9); i >= 0)
        return keys[size_t(i)].euler;
    glm::dquat q = evaluateQuat(t);
    return eulerNear(glm::mat3_cast(q), order, referenceEuler(t));
}

double RotationCurve::eulerSlope(size_t i, int axis, bool out) const
{
    if (i >= keys.size())
        return 0.0;
    const RotKey& k = keys[i];
    constexpr double h = 1e-6;
    double t = k.time;
    glm::dvec3 at = evaluate(t);
    glm::dquat q = evaluateQuat(out ? t + h : t - h);
    glm::dvec3 e = eulerNear(glm::mat3_cast(q), order, at);
    return out ? (e[axis] - at[axis]) / h : (at[axis] - e[axis]) / h;
}

int RotationCurve::find(double t, double eps) const
{
    for (size_t i = 0; i < keys.size(); ++i)
        if (std::abs(keys[i].time - t) <= eps)
            return int(i);
    return -1;
}

size_t RotationCurve::insert(const RotKey& k)
{
    if (int i = find(k.time); i >= 0)
    {
        RotKey& slot = keys[size_t(i)];
        slot.q = k.q;
        slot.euler = k.euler;
        return size_t(i);
    }
    auto it = std::upper_bound(keys.begin(), keys.end(), k.time, [](double x, const RotKey& key) { return x < key.time; });
    return size_t(keys.insert(it, k) - keys.begin());
}

void RotationCurve::sort()
{
    std::stable_sort(keys.begin(), keys.end(), [](const RotKey& a, const RotKey& b) { return a.time < b.time; });
}

void RotationCurve::setEuler(size_t i, const glm::dvec3& euler)
{
    keys[i].euler = euler;
    keys[i].q = eulerToQuat(euler, order);
}

void RotationCurve::reorder(RotateOrder to)
{
    if (to == order)
        return;
    glm::dvec3 ref(0.0);
    for (size_t i = 0; i < keys.size(); ++i)
    {
        glm::dvec3 near = i == 0 ? matrixToEuler(glm::mat3_cast(keys[i].q), to) : ref;
        keys[i].euler = eulerNear(glm::mat3_cast(keys[i].q), to, near);
        ref = keys[i].euler;
    }
    order = to;
}

RotationCurve RotationCurve::looped() const
{
    RotationCurve c = *this;
    c.loop = false;
    size_t n = keys.size();
    if (!loop || n < 2)
        return c;
    c.mCyclic = true;
    const RotKey& first = keys.front();
    RotKey& last = c.keys.back();
    glm::dvec3 turns = glm::round((last.euler - first.euler) / 360.0) * 360.0;
    last.q = first.q;
    last.euler = first.euler + turns;
    last.in = first.in;
    last.out = first.out;
    last.inVelocity = first.inVelocity;
    last.outVelocity = first.outVelocity;
    last.inWeight = first.inWeight;
    last.outWeight = first.outWeight;
    c.post = turns == glm::dvec3(0.0) ? Infinity::Cycle : Infinity::CycleRelative;
    return c;
}

json RotationCurve::keysToJson() const
{
    json out = json::array();
    for (const RotKey& k : keys)
    {
        json j = {{"t", k.time}, {"q", {k.q.x, k.q.y, k.q.z, k.q.w}}, {"e", {k.euler.x, k.euler.y, k.euler.z}}};
        if (k.in != Tangent::Auto)
            j["i"] = tangentName(k.in);
        if (k.out != Tangent::Auto)
            j["o"] = tangentName(k.out);
        if (k.in == Tangent::Fixed)
            j["iv"] = {k.inVelocity.x, k.inVelocity.y, k.inVelocity.z};
        if (k.out == Tangent::Fixed)
            j["ov"] = {k.outVelocity.x, k.outVelocity.y, k.outVelocity.z};
        if (k.inWeight != 1.0)
            j["iw"] = k.inWeight;
        if (k.outWeight != 1.0)
            j["ow"] = k.outWeight;
        if (k.breakdown)
            j["bd"] = true;
        if (k.broken)
            j["br"] = true;
        out.push_back(std::move(j));
    }
    return out;
}

std::vector<RotKey> RotationCurve::keysFromJson(const json& j, RotateOrder order)
{
    std::vector<RotKey> out;
    if (j.is_null())
        return out;
    if (!j.is_array())
        throw std::invalid_argument("rotation keys must be a list");
    for (const json& e : j)
    {
        if (!e.is_object() || !e.contains("t") || !e["t"].is_number() || (!e.contains("q") && !e.contains("e")))
            throw std::invalid_argument("rotation key needs a numeric t and q or e: " + e.dump());
        RotKey k;
        k.time = e["t"].get<double>();
        try
        {
            if (e.contains("q"))
            {
                const json& q = e["q"];
                if (!q.is_array() || q.size() != 4)
                    throw std::invalid_argument("rotation key q must be [x, y, z, w]: " + e.dump());
                k.q = glm::dquat(q[3].get<double>(), q[0].get<double>(), q[1].get<double>(), q[2].get<double>());
                if (glm::length(k.q) < 1e-9)
                    throw std::invalid_argument("rotation key q is zero: " + e.dump());
                k.q = glm::normalize(k.q);
                k.euler = e.contains("e") ? jsonVec3(e["e"]) : matrixToEuler(glm::mat3_cast(k.q), order);
            }
            else
            {
                k.euler = jsonVec3(e["e"]);
                k.q = eulerToQuat(k.euler, order);
            }
            if (e.contains("iv"))
                k.inVelocity = jsonVec3(e["iv"]);
            if (e.contains("ov"))
                k.outVelocity = jsonVec3(e["ov"]);
        }
        catch (const json::exception&)
        {
            throw std::invalid_argument("rotation key has non-numeric values: " + e.dump());
        }
        if (e.contains("i"))
            k.in = tangentFromName(e["i"].get<std::string>());
        if (e.contains("o"))
            k.out = tangentFromName(e["o"].get<std::string>());
        k.inWeight = e.value("iw", 1.0);
        k.outWeight = e.value("ow", 1.0);
        k.breakdown = e.value("bd", false);
        k.broken = e.value("br", false);
        out.push_back(k);
    }
    std::stable_sort(out.begin(), out.end(), [](const RotKey& a, const RotKey& b) { return a.time < b.time; });
    return out;
}

void RotationCurveNode::compute(const EvalContext& ctx)
{
    LILA_ZONE();

    auto order = RotateOrder(std::clamp<int64_t>(as<int64_t>(kRotateOrder), 0, 5));
    const std::string& text = as<std::string>(kKeys);
    if (text != mCachedText || order != mCached.order)
    {
        mCachedText = text;
        mCached.order = order;
        try
        {
            mCached.keys = text.empty() ? std::vector<RotKey>{} : RotationCurve::keysFromJson(json::parse(text), order);
        }
        catch (const std::exception&)
        {
            mCached.keys.clear();
        }
    }
    mCached.pre = Infinity(std::clamp<int64_t>(as<int64_t>(kPreInfinity), 0, 4));
    mCached.post = Infinity(std::clamp<int64_t>(as<int64_t>(kPostInfinity), 0, 4));
    mCached.weighted = as<bool>(kWeighted);
    mCached.interp = RotInterp(std::clamp<int64_t>(as<int64_t>(kInterpolation), 0, 1));
    mCached.loop = as<bool>(kLoop);
    if (const std::string& mods = as<std::string>(kModifiers); mods != mModifiersText)
    {
        mModifiersText = mods;
        mModifiers = modifiersFromText(mods);
    }
    const double t = as<bool>(kMute) ? as<double>(kMuteTime) : heldTime().value_or(ctx.time);
    const RotationCurve& c = rotationPreviewOf(id()) ? *rotationPreviewOf(id()) : mCached;
    setOutput(kOutput, mModifiers.empty() ? c.evaluate(t) : evaluateModified(c, mModifiers, t));
}

namespace
{
std::unordered_map<NodeId, RotationCurve>& rotationPreviews()
{
    static std::unordered_map<NodeId, RotationCurve> m;
    return m;
}
} // namespace

const RotationCurve* rotationPreviewOf(NodeId curve)
{
    auto& m = rotationPreviews();
    if (m.empty())
        return nullptr;
    auto it = m.find(curve);
    return it == m.end() ? nullptr : &it->second;
}

void setPreview(Graph& g, NodeId curve, const RotationCurve& c)
{
    rotationPreviews()[curve] = c;
    g.touch(curve);
}

void clearRotationPreviews(Graph& g)
{
    for (auto& [id, c] : rotationPreviews())
        if (g.node(id))
            g.touch(id);
    rotationPreviews().clear();
}

bool isRotationCurve(const Node& n)
{
    return n.type().isA("animCurveRQ");
}

bool isAnyCurve(const Node& n)
{
    return isCurve(n) || isRotationCurve(n);
}

RotationCurve rotationCurveOf(const Node& n)
{
    RotationCurve c;
    c.order = RotateOrder(std::clamp<int64_t>(n.as<int64_t>(RotationCurveNode::kRotateOrder), 0, 5));
    const std::string& text = n.as<std::string>(RotationCurveNode::kKeys);
    try
    {
        if (!text.empty())
            c.keys = RotationCurve::keysFromJson(json::parse(text), c.order);
    }
    catch (const std::exception&)
    {
        c.keys.clear();
    }
    c.pre = Infinity(std::clamp<int64_t>(n.as<int64_t>(RotationCurveNode::kPreInfinity), 0, 4));
    c.post = Infinity(std::clamp<int64_t>(n.as<int64_t>(RotationCurveNode::kPostInfinity), 0, 4));
    c.weighted = n.as<bool>(RotationCurveNode::kWeighted);
    c.loop = n.as<bool>(RotationCurveNode::kLoop);
    c.interp = RotInterp(std::clamp<int64_t>(n.as<int64_t>(RotationCurveNode::kInterpolation), 0, 1));
    return c;
}

void setRotationCurve(Graph& g, NodeId id, const RotationCurve& c)
{
    LILA_ZONE();

    Node* n = g.node(id);
    if (!n || !isRotationCurve(*n))
        throw std::invalid_argument("no such rotation curve");
    auto set = [&](int attr, PlugValue v) {
        if (n->value(attr) != v)
            g.setValue({id, attr}, std::move(v));
    };
    set(RotationCurveNode::kRotateOrder, int64_t(c.order));
    if (c.loop && c.keys.size() > 1)
    {
        // A loop key stores the first key's rotation and tangents too, so the keys read back as they evaluate.
        RotationCurve looped = c.looped();
        RotationCurve stored = c;
        stored.keys.back() = looped.keys.back();
        stored.keys.back().time = c.keys.back().time;
        set(RotationCurveNode::kKeys, stored.keysToJson().dump());
    }
    else
        set(RotationCurveNode::kKeys, c.keysToJson().dump());
    set(RotationCurveNode::kPreInfinity, int64_t(c.pre));
    set(RotationCurveNode::kPostInfinity, int64_t(c.post));
    set(RotationCurveNode::kWeighted, c.weighted);
    set(RotationCurveNode::kLoop, c.loop);
    set(RotationCurveNode::kInterpolation, int64_t(c.interp));
}

RotationCurve rotationFromEuler(const std::array<const Curve*, 3>& xyz, const glm::dvec3& rest, RotateOrder order, RotInterp interp)
{
    LILA_ZONE();

    RotationCurve rc;
    rc.order = order;
    rc.interp = interp;
    std::set<double> times;
    const Curve* lead = nullptr;
    for (const Curve* c : xyz)
        if (c)
        {
            if (!lead)
                lead = c;
            for (const Key& k : c->keys)
                times.insert(k.time);
        }
    if (lead)
    {
        rc.pre = lead->pre;
        rc.post = lead->post;
        rc.weighted = lead->weighted;
        rc.loop = lead->loop;
    }
    std::vector<double> sorted;
    for (double t : times)
        if (sorted.empty() || t - sorted.back() > 1e-6)
            sorted.push_back(t);
    for (double t : sorted)
    {
        glm::dvec3 e = rest;
        Tangent in = Tangent::Auto, out = Tangent::Auto;
        bool typed = false;
        for (int a = 0; a < 3; ++a)
        {
            const Curve* c = xyz[size_t(a)];
            if (!c)
                continue;
            e[a] = c->evaluate(t);
            if (int i = c->find(t); i >= 0 && !typed)
            {
                const Key& k = c->keys[size_t(i)];
                in = k.in == Tangent::Fixed ? Tangent::Spline : k.in;
                out = k.out == Tangent::Fixed ? Tangent::Spline : k.out;
                typed = true;
            }
        }
        RotKey k = rc.makeKey(t, e);
        k.in = in;
        k.out = out;
        rc.keys.push_back(k);
    }
    return rc;
}

std::array<Curve, 3> eulerFromRotation(const RotationCurve& rc, std::vector<double> times)
{
    LILA_ZONE();

    std::array<Curve, 3> out;
    for (Curve& c : out)
    {
        c.pre = rc.pre;
        c.post = rc.post;
        c.weighted = rc.weighted;
        c.loop = rc.loop;
    }
    if (times.empty())
        for (const RotKey& k : rc.keys)
            times.push_back(k.time);
    std::sort(times.begin(), times.end());
    glm::dvec3 prev(0.0);
    bool first = true;
    for (double t : times)
    {
        int ki = rc.find(t);
        glm::dvec3 e = rc.evaluate(t);
        // Keys keep their typed value; samples between follow on from the previous one.
        if (!first && ki < 0)
            e = eulerNear(glm::mat3_cast(rc.evaluateQuat(t)), rc.order, prev);
        prev = e;
        first = false;
        for (int a = 0; a < 3; ++a)
        {
            Key k{.time = t, .value = e[a], .in = Tangent::Linear, .out = Tangent::Linear};
            if (ki >= 0)
            {
                const RotKey& rk = rc.keys[size_t(ki)];
                k.in = rk.in == Tangent::Fixed ? Tangent::Spline : rk.in;
                k.out = rk.out == Tangent::Fixed ? Tangent::Spline : rk.out;
                k.inWeight = rk.inWeight;
                k.outWeight = rk.outWeight;
            }
            out[size_t(a)].keys.push_back(k);
        }
    }
    return out;
}

std::array<Curve, 3> eulerKeysFor(const RotationCurve& rc, const std::vector<double>& frames, double toleranceDegrees)
{
    LILA_ZONE();

    constexpr double h = 1e-6;
    std::vector<double> times;
    for (const RotKey& k : rc.keys)
        times.push_back(k.time);
    auto degrees = [](double slope) { return std::atan(slope) * 180.0 / kPi; };
    auto build = [&]() {
        std::array<Curve, 3> out;
        glm::dvec3 prev(0.0);
        for (size_t s = 0; s < times.size(); ++s)
        {
            double t = times[s];
            int ki = rc.find(t);
            glm::dvec3 e = ki >= 0 || s == 0 ? rc.evaluate(t) : eulerNear(glm::mat3_cast(rc.evaluateQuat(t)), rc.order, prev);
            prev = e;
            glm::dvec3 before = eulerNear(glm::mat3_cast(rc.evaluateQuat(t - h)), rc.order, e);
            glm::dvec3 after = eulerNear(glm::mat3_cast(rc.evaluateQuat(t + h)), rc.order, e);
            for (int a = 0; a < 3; ++a)
            {
                Key k{.time = t, .value = e[a], .in = Tangent::Fixed, .out = Tangent::Fixed};
                k.inAngle = degrees((e[a] - before[a]) / h);
                k.outAngle = degrees((after[a] - e[a]) / h);
                if (ki >= 0 && rc.keys[size_t(ki)].out == Tangent::Step)
                    k.out = Tangent::Step;
                out[size_t(a)].keys.push_back(k);
            }
        }
        return out;
    };
    std::array<Curve, 3> out = build();
    for (size_t pass = 0; pass < frames.size(); ++pass)
    {
        // The worst frame in each span between keys that misses, all added at once.
        std::vector<double> add;
        double worst = 0.0, worstT = 0.0;
        size_t span = 0;
        auto flush = [&] {
            if (worst > toleranceDegrees)
                add.push_back(worstT);
            worst = 0.0;
        };
        for (double f : frames)
        {
            while (span < times.size() && times[span] <= f + 1e-9)
            {
                flush();
                ++span;
            }
            glm::dvec3 e(out[0].evaluate(f), out[1].evaluate(f), out[2].evaluate(f));
            double err = glm::degrees(glm::length(quatLog(glm::inverse(eulerToQuat(e, rc.order)) * rc.evaluateQuat(f))));
            if (err > worst)
            {
                worst = err;
                worstT = f;
            }
        }
        flush();
        if (add.empty())
            break;
        times.insert(times.end(), add.begin(), add.end());
        std::sort(times.begin(), times.end());
        times.erase(std::unique(times.begin(), times.end(), [](double a, double b) { return std::abs(a - b) < 1e-9; }), times.end());
        out = build();
    }
    return out;
}

int eulerFilter(std::array<Curve*, 3> xyz, const glm::dvec3& rest, RotateOrder order)
{
    LILA_ZONE();

    std::set<double> times;
    for (Curve* c : xyz)
        if (c)
            for (const Key& k : c->keys)
                times.insert(k.time);
    std::array<Curve, 3> before;
    for (int a = 0; a < 3; ++a)
        if (xyz[size_t(a)])
            before[size_t(a)] = *xyz[size_t(a)];
    int changed = 0;
    glm::dvec3 prev(0.0);
    bool first = true;
    double last = -1e300;
    for (double t : times)
    {
        if (t - last <= 1e-6)
            continue;
        last = t;
        glm::dvec3 e = rest;
        int idx[3] = {-1, -1, -1};
        bool all = true;
        for (int a = 0; a < 3; ++a)
        {
            if (!xyz[size_t(a)])
                continue;
            e[a] = before[size_t(a)].evaluate(t);
            idx[a] = xyz[size_t(a)]->find(t);
            all = all && idx[a] >= 0;
        }
        glm::dvec3 fixed = e;
        if (!first)
        {
            if (all)
                fixed = eulerNear(glm::dmat3(eulerToMatrix(e, order)), order, prev);
            else
                for (int a = 0; a < 3; ++a)
                    fixed[a] = e[a] + 360.0 * std::round((prev[a] - e[a]) / 360.0);
        }
        first = false;
        for (int a = 0; a < 3; ++a)
        {
            if (idx[a] < 0)
                continue;
            Key& k = xyz[size_t(a)]->keys[size_t(idx[a])];
            if (std::abs(k.value - fixed[a]) > 1e-9)
            {
                k.value = fixed[a];
                ++changed;
            }
        }
        prev = fixed;
    }
    return changed;
}

void registerRotationCurveType()
{
    if (types::find("animCurveRQ"))
        return;
    constexpr const char* kInfinity[] = {"constant", "linear", "cycle", "cycleRelative", "oscillate"};
    NodeType t;
    t.name = "animCurveRQ";
    t.timeDependent = true;
    t.icon = "rotate-clockwise";
    t.doc = "A rotation curve: quaternion keys driving a whole rotate plug as Euler angles, interpolated without gimbal lock.";
    AttrDef output{.name = "output", .shortName = "o", .type = PlugType::Vec3, .defaultValue = glm::dvec3(0.0), .flags = AttrOutput,
                   .doc = "Euler rotation at the current time, degrees in rotateOrder."};
    for (int i = 0; i < 3; ++i)
    {
        output.childNames[size_t(i)] = std::string("output") + char('X' + i);
        output.childShort[size_t(i)] = std::string("o") + char('x' + i);
    }
    t.ownAttrs = {
        {.name = "keys", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "Keys as JSON: [{t: seconds, q: [x, y, z, w], e: Euler degrees, i/o: tangent types, iv/ov: fixed angular "
                "velocities in degrees per second, iw/ow: weights}]."},
        {.name = "preInfinity", .shortName = "pri", .type = PlugType::Enum, .defaultValue = int64_t(0),
         .enumNames = {std::begin(kInfinity), std::end(kInfinity)}, .doc = "Before the first key."},
        {.name = "postInfinity", .shortName = "poi", .type = PlugType::Enum, .defaultValue = int64_t(0),
         .enumNames = {std::begin(kInfinity), std::end(kInfinity)}, .doc = "After the last key."},
        {.name = "weightedTangents", .shortName = "wt", .type = PlugType::Bool, .defaultValue = false,
         .doc = "Tangent handles have length as well as direction."},
        {.name = "loop", .shortName = "lp", .type = PlugType::Bool, .defaultValue = false,
         .doc = "The last key mirrors the first key's rotation and tangents, and the curve cycles after it."},
        {.name = "rotateOrder", .shortName = "ro", .type = PlugType::Enum, .defaultValue = int64_t(0),
         .enumNames = {std::begin(kOrderNames), std::end(kOrderNames)}, .doc = "Euler order of the output, kept equal to the driven transform's."},
        {.name = "interpolation", .shortName = "itp", .type = PlugType::Enum, .defaultValue = int64_t(RotInterp::Smooth),
         .enumNames = {std::begin(kInterpNames), std::end(kInterpNames)},
         .doc = "slerp: constant speed along the shortest arc between keys. smooth: cubic through the keys, honouring tangents."},
        output,
        {.name = "mute", .shortName = "mu", .type = PlugType::Bool, .defaultValue = false,
         .doc = "Hold the value at muteTime instead of playing the curve (the Channel Box's Mute)."},
        {.name = "muteTime", .shortName = "mut", .type = PlugType::Float, .defaultValue = 0.0, .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "Seconds: the time a muted curve holds."},
        {.name = "modifiers", .shortName = "mods", .type = PlugType::String, .defaultValue = std::string(),
         .flags = AttrInput | AttrStorable | AttrHidden, .doc = "Non-destructive modifiers as JSON; see the curveModifier command."},
    };
    t.create = [] { return std::make_unique<RotationCurveNode>(); };
    types::registerType(std::move(t));
}
} // namespace lila::scene::anim
