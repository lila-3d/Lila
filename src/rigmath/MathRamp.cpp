#include "rigmath/Builder.h"

#include "scene/AnimCurve.h"

#include <array>
#include <unordered_map>

namespace lila::rigmath
{
namespace rampfns
{
using json = nlohmann::json;

/** N anim curves read from a Curve pin: one for a float ramp, three for a vector or colour ramp. */
template <size_t N> struct Ramp
{
    std::array<scene::anim::Curve, N> curves;

    /** Linear from (0, 0) to (1, 1) on every channel. */
    Ramp()
    {
        for (scene::anim::Curve& c : curves)
            for (double t : {0.0, 1.0})
                c.insert({.time = t, .value = t, .in = scene::anim::Tangent::Linear, .out = scene::anim::Tangent::Linear});
    }
};

/** Keys as keysToJson writes them, or compact [in, out] / [in, out, "tangent"] pairs. */
scene::anim::Curve curveOf(const json& keys)
{
    if (!keys.is_array())
        throw std::invalid_argument("a ramp's keys are a list");
    scene::anim::Curve c;
    for (const json& k : keys)
    {
        if (!k.is_array())
        {
            for (const scene::anim::Key& key : scene::anim::Curve::keysFromJson(json::array({k})))
                c.insert(key);
            continue;
        }
        if (k.size() < 2 || !k[0].is_number() || !k[1].is_number())
            throw std::invalid_argument("a ramp key is [in, out] or [in, out, \"tangent\"]");
        scene::anim::Key key{.time = k[0].get<double>(), .value = k[1].get<double>()};
        if (k.size() > 2)
            key.in = key.out = scene::anim::tangentFromName(k[2].get<std::string>());
        c.insert(key);
    }
    return c;
}

/** Whether j lists channels (lists of keys) rather than keys. */
bool isChannels(const json& j)
{
    return j.is_array() && !j.empty() && j[0].is_array() && (j[0].empty() || j[0][0].is_array() || j[0][0].is_object());
}

template <size_t N> Ramp<N> parse(const std::string& text)
{
    const json j = json::parse(text);
    Ramp<N> r;
    bool channels = false;
    if constexpr (N > 1)
        channels = isChannels(j);
    if (channels)
    {
        if (j.size() != N)
            throw std::invalid_argument("a vector or colour ramp has three channels");
        for (size_t k = 0; k < N; ++k)
            r.curves[k] = curveOf(j[k]);
    }
    else
        r.curves.fill(curveOf(j));
    return r;
}

/** Parsed ramps by their text, so evaluation doesn't reparse JSON every time. */
template <size_t N> Ramp<N> cached(const std::string& text)
{
    thread_local std::unordered_map<std::string, Ramp<N>> cache;
    if (auto it = cache.find(text); it != cache.end())
        return it->second;
    Ramp<N> r = parse<N>(text);
    if (cache.size() > 256)
        cache.clear();
    cache.emplace(text, r);
    return r;
}

double unitIn(double v, double lo, double hi) { return std::abs(hi - lo) > 1e-12 ? (v - lo) / (hi - lo) : 0.0; }

template <size_t N> D3 sample(const Ramp<N>& r, double u)
{
    D3 out(0.0);
    for (size_t k = 0; k < 3; ++k)
        out[int(k)] = r.curves[k % N].evaluate(u);
    return out;
}
} // namespace rampfns

template <size_t N> struct Traits<rampfns::Ramp<N>>
{
    using R = rampfns::Ramp<N>;
    static constexpr Kind kind = Kind::Curve;
    static R from(const PlugValue& v)
    {
        auto s = std::get_if<std::string>(&v);
        return s && !s->empty() ? rampfns::cached<N>(*s) : R{};
    }
    static PlugValue to(const R& r)
    {
        if constexpr (N == 1)
            return r.curves[0].keysToJson().dump();
        nlohmann::json j = nlohmann::json::array();
        for (const scene::anim::Curve& c : r.curves)
            j.push_back(c.keysToJson());
        return j.dump();
    }
    static PlugValue parse(std::string_view s) { return std::string(s); }
};

void addRamps(Library& lib)
{
    using namespace rampfns;
    auto& f = lib.fn("evaluateCurve", "range",
                     "Control Rig's Evaluate Curve: value's place in sourceMinimum..sourceMaximum (0..1) looked up on the curve's keys, "
                     "then mapped onto targetMinimum..targetMaximum. The vector and colour ramps look up one curve per channel and "
                     "return it directly.");
    over(f, "value curve sourceMinimum sourceMaximum=1 targetMinimum targetMaximum=1", "result",
         [](double v, Ramp<1> r, double s0, double s1, double t0, double t1) { return math::lerp(t0, t1, r.curves[0].evaluate(unitIn(v, s0, s1))); },
         "Float");
    over(f, "value curve sourceMinimum sourceMaximum=1", "result",
         [](double v, Ramp<3> r, double s0, double s1) { return sample(r, unitIn(v, s0, s1)); }, "Vector");
    over(f, "value curve sourceMinimum sourceMaximum=1", "result",
         [](double v, Ramp<3> r, double s0, double s1) { return Col{sample(r, unitIn(v, s0, s1))}; }, "Color");
}
} // namespace lila::rigmath
