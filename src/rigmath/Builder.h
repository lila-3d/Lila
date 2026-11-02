#pragma once

#include "rigmath/MathNodes.h"

#include "math/Math.h"
#include "scene/Node.h"
#include "scene/Transform.h"

#include <cmath>
#include <deque>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

/** Declaring functions and their overloads. Internal to lila_rigmath. */
namespace lila::rigmath
{
using D3 = glm::dvec3;
using M3 = glm::dmat3;
using M4 = glm::dmat4;
using Quat = math::Quat;
using scene::PlugValue;

/** The rotate order Rot converts with: the evaluating node's, set around each kernel call. */
extern thread_local scene::RotateOrder tRotateOrder;

/** A rotator: Euler degrees in the evaluating node's rotate order. */
struct Rot
{
    D3 deg{0.0};

    M3 mat() const { return M3(scene::eulerToMatrix(deg, tRotateOrder)); }
    Quat quat() const { return Quat::fromGlmMat3(mat()); }
    static Rot of(const M3& m) { return {scene::matrixToEuler(m, tRotateOrder)}; }
    static Rot of(const Quat& q) { return of(q.getNormalized().toGlmMat3()); }
};

/** Linear RGB. */
struct Col
{
    D3 rgb{0.0};
};

/** An index into the pin's choices. */
struct Choice
{
    int64_t index = 0;
};

inline double num(const PlugValue& v)
{
    if (auto d = std::get_if<double>(&v))
        return *d;
    if (auto i = std::get_if<int64_t>(&v))
        return double(*i);
    if (auto b = std::get_if<bool>(&v))
        return *b ? 1.0 : 0.0;
    return 0.0;
}

inline D3 parseVec(std::string_view text)
{
    D3 out(0.0);
    std::string s(text);
    char* p = s.data();
    for (int k = 0; k < 3 && *p; ++k)
    {
        out[k] = std::strtod(p, &p);
        if (*p == ',')
            ++p;
    }
    return out;
}

/** The rotation in a matrix: normalised axes, a mirrored one folded back. */
inline Quat quatOf(const M4& m) { return math::Matrix(m).toQuat().getNormalized(); }
inline M4 matOf(const Quat& q) { return M4(q.getNormalized().toGlmMat3()); }

template <class T> struct Traits;
template <> struct Traits<bool>
{
    static constexpr Kind kind = Kind::Bool;
    static bool from(const PlugValue& v) { return num(v) != 0.0; }
    static PlugValue to(bool b) { return b; }
    static PlugValue parse(std::string_view s) { return s == "true" || s == "1"; }
};
template <> struct Traits<int64_t>
{
    static constexpr Kind kind = Kind::Int;
    static int64_t from(const PlugValue& v)
    {
        if (auto i = std::get_if<int64_t>(&v))
            return *i;
        return int64_t(std::llround(num(v)));
    }
    static PlugValue to(int64_t i) { return i; }
    static PlugValue parse(std::string_view s) { return int64_t(std::strtoll(std::string(s).c_str(), nullptr, 10)); }
};
template <> struct Traits<double>
{
    static constexpr Kind kind = Kind::Float;
    static double from(const PlugValue& v) { return num(v); }
    static PlugValue to(double d) { return std::isfinite(d) ? d : 0.0; }
    static PlugValue parse(std::string_view s) { return std::strtod(std::string(s).c_str(), nullptr); }
};
template <> struct Traits<D3>
{
    static constexpr Kind kind = Kind::Vector;
    static D3 from(const PlugValue& v)
    {
        auto p = std::get_if<D3>(&v);
        return p ? *p : D3(num(v));
    }
    static PlugValue to(const D3& v)
    {
        D3 o = v;
        for (int k = 0; k < 3; ++k)
            if (!std::isfinite(o[k]))
                o[k] = 0.0;
        return o;
    }
    static PlugValue parse(std::string_view s) { return parseVec(s); }
};
template <> struct Traits<Col>
{
    static constexpr Kind kind = Kind::Color;
    static Col from(const PlugValue& v) { return {Traits<D3>::from(v)}; }
    static PlugValue to(const Col& c) { return Traits<D3>::to(c.rgb); }
    static PlugValue parse(std::string_view s) { return parseVec(s); }
};
template <> struct Traits<Rot>
{
    static constexpr Kind kind = Kind::Rotator;
    static Rot from(const PlugValue& v) { return {Traits<D3>::from(v)}; }
    static PlugValue to(const Rot& r) { return Traits<D3>::to(r.deg); }
    static PlugValue parse(std::string_view s) { return parseVec(s); }
};
template <> struct Traits<M4>
{
    static constexpr Kind kind = Kind::Matrix;
    static M4 from(const PlugValue& v)
    {
        auto p = std::get_if<M4>(&v);
        return p ? *p : M4(1.0);
    }
    static PlugValue to(const M4& m)
    {
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                if (!std::isfinite(m[c][r]))
                    return M4(1.0);
        return m;
    }
    static PlugValue parse(std::string_view) { return M4(1.0); }
};
template <> struct Traits<Quat>
{
    static constexpr Kind kind = Kind::Quat;
    static Quat from(const PlugValue& v) { return quatOf(Traits<M4>::from(v)); }
    static PlugValue to(const Quat& q) { return q.containsNaN() ? PlugValue(M4(1.0)) : Traits<M4>::to(matOf(q)); }
    static PlugValue parse(std::string_view) { return M4(1.0); }
};
template <> struct Traits<Choice>
{
    static constexpr Kind kind = Kind::Choice;
    static Choice from(const PlugValue& v) { return {Traits<int64_t>::from(v)}; }
    static PlugValue to(const Choice& c) { return c.index; }
    static PlugValue parse(std::string_view) { return int64_t(0); }
};

template <class T> struct ArrayOf
{
    static constexpr bool value = false;
    using elem = T;
};
template <class T> struct ArrayOf<std::vector<T>>
{
    static constexpr bool value = true;
    using elem = T;
};

/** "name", "name=default"; a Choice pin lists its choices as "name=a|b|c", the first being the default. */
template <class T> Pin makePin(std::string_view spec)
{
    using E = typename ArrayOf<T>::elem;
    Pin p;
    size_t eq = spec.find('=');
    p.name = std::string(spec.substr(0, eq));
    std::string_view def = eq == std::string_view::npos ? std::string_view() : spec.substr(eq + 1);
    p.kind = Traits<E>::kind;
    p.array = ArrayOf<T>::value;
    if constexpr (std::is_same_v<E, Choice>)
    {
        while (!def.empty())
        {
            size_t bar = def.find('|');
            p.choices.emplace_back(def.substr(0, bar));
            def = bar == std::string_view::npos ? std::string_view() : def.substr(bar + 1);
        }
        p.defaultValue = int64_t(0);
    }
    else if (def.empty())
        p.defaultValue = Traits<E>::to(E{});
    else
        p.defaultValue = Traits<E>::parse(def);
    if (p.kind == Kind::Matrix || p.kind == Kind::Quat)
        p.defaultValue = M4(1.0);
    return p;
}

inline std::vector<std::string_view> splitSpecs(std::string_view s)
{
    std::vector<std::string_view> out;
    while (!s.empty())
    {
        size_t sp = s.find(' ');
        if (sp != 0)
            out.push_back(s.substr(0, sp));
        if (sp == std::string_view::npos)
            break;
        s = s.substr(sp + 1);
    }
    return out;
}

template <class T> T readPin(const scene::Node& n, int attr)
{
    if constexpr (ArrayOf<T>::value)
    {
        T out;
        using E = typename ArrayOf<T>::elem;
        // Element indices can skip; a missing one reads as the default, so options keep their numbers.
        const E gap = Traits<E>::from(n.type().attrs[size_t(attr)].defaultValue);
        for (const auto& e : n.elements(attr))
        {
            if (e.first < 0)
                continue;
            if (out.size() < size_t(e.first))
                out.resize(size_t(e.first), gap);
            out.push_back(Traits<E>::from(e.second));
        }
        return out;
    }
    else
        return Traits<T>::from(n.value(attr));
}

template <class F> struct Sig : Sig<decltype(&F::operator())>
{
};
template <class C, class R, class... A> struct Sig<R (C::*)(A...) const>
{
    using Ret = R;
    using Args = std::tuple<std::decay_t<A>...>;
};

template <class T> struct IsTuple : std::false_type
{
};
template <class... T> struct IsTuple<std::tuple<T...>> : std::true_type
{
};

template <class Tuple, size_t... I> std::vector<Pin> pinsOf(const std::vector<std::string_view>& specs, std::index_sequence<I...>)
{
    if (specs.size() != sizeof...(I))
        throw std::logic_error("rigmath: pin names don't match the kernel's parameters");
    return {makePin<std::tuple_element_t<I, Tuple>>(specs[I])...};
}

template <class F, class Args, size_t... I> auto callKernel(const F& f, const scene::Node& n, std::index_sequence<I...>)
{
    (void)n;
    return f(readPin<std::tuple_element_t<I, Args>>(n, int(I))...);
}

template <class R> std::vector<PlugValue> toValues(const R& r)
{
    if constexpr (IsTuple<R>::value)
        return std::apply([](const auto&... v) { return std::vector<PlugValue>{Traits<std::decay_t<decltype(v)>>::to(v)...}; }, r);
    else
        return {Traits<R>::to(r)};
}

/**
 * Adds an overload whose kernel is fn. ins and outs name its parameters and results, space separated, each optionally
 * "name=default"; a kernel returning a tuple has one output per element. suffix names the overload in its node type.
 */
template <class F> void over(Function& f, std::string_view ins, std::string_view outs, F fn, std::string suffix = {})
{
    using S = Sig<F>;
    using Args = typename S::Args;
    using R = typename S::Ret;
    constexpr size_t nIn = std::tuple_size_v<Args>;
    Overload o;
    o.suffix = std::move(suffix);
    o.inputs = pinsOf<Args>(splitSpecs(ins), std::make_index_sequence<nIn>());
    if constexpr (IsTuple<R>::value)
        o.outputs = pinsOf<R>(splitSpecs(outs), std::make_index_sequence<std::tuple_size_v<R>>());
    else
        o.outputs = pinsOf<std::tuple<R>>(splitSpecs(outs), std::make_index_sequence<1>());
    o.run = [fn](const scene::Node& n) { return toValues(callKernel<F, Args>(fn, n, std::make_index_sequence<nIn>())); };
    f.overloads.push_back(std::move(o));
}

/** The library under construction. */
struct Library
{
    std::deque<Function> fns;

    Function& fn(std::string name, std::string family, std::string doc)
    {
        fns.push_back({std::move(name), std::move(family), std::move(doc), {}});
        return fns.back();
    }
};

void addScalar(Library& lib);
void addVector(Library& lib);
void addRotation(Library& lib);
void addMatrix(Library& lib);
void addMisc(Library& lib);
void addRamps(Library& lib);
} // namespace lila::rigmath
