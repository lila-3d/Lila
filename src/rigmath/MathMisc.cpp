#include "rigmath/Builder.h"

#include "gn/Field.h"
#include "gn/Texture.h"

namespace lila::rigmath
{
namespace miscfns
{
using I = int64_t;

template <class T> void selectOverload(Function& f)
{
    over(f, "a b pickA=true", "result", [](T a, T b, bool pickA) { return pickA ? a : b; });
}

template <class T> void switchOverload(Function& f, std::string suffix)
{
    over(f, "selection options defaultValue", "result",
         [](I sel, std::vector<T> options, T def) {
             T out = def;
             if (sel >= 0 && sel < I(options.size()))
                 out = options[size_t(sel)];
             return out;
         }, std::move(suffix));
}

bool compare(int64_t op, double a, double b)
{
    switch (op)
    {
    case 0: return a == b;
    case 1: return a != b;
    case 2: return a > b;
    case 3: return a >= b;
    case 4: return a < b;
    default: return a <= b;
    }
}
const char* kOps = "operation=equal|notEqual|greater|greaterEqual|less|lessEqual";

/** A seeded uniform value in [0, 1), stable per (seed, salt). */
double unit(I seed, uint32_t salt = 0) { return gn::hashToUnit(gn::hash32(uint32_t(seed), uint32_t(seed >> 32), salt)); }

D3 unitVector(I seed)
{
    const double z = unit(seed, 1) * 2.0 - 1.0, a = unit(seed, 2) * 2.0 * math::kPi;
    const double r = std::sqrt(std::max(0.0, 1.0 - z * z));
    return {r * std::cos(a), r * std::sin(a), z};
}

/** Gradient noise in -1..1 with the seed choosing an unrelated slice. */
double noise(const D3& p, I seed) { return gn::tex::gradientNoise4(glm::dvec4(p, double(seed) * 1.6180339887 + 0.37)); }
} // namespace miscfns

void addMisc(Library& lib)
{
    using namespace miscfns;

    // Select
    {
        auto& f = lib.fn("select", "select", "A when pickA is on, else B.");
        selectOverload<double>(f);
        selectOverload<I>(f);
        selectOverload<bool>(f);
        selectOverload<D3>(f);
        selectOverload<Rot>(f);
        selectOverload<M4>(f);
        selectOverload<Col>(f);
    }
    {
        auto& f = lib.fn("switchOnInt", "select", "The option whose index is selection; defaultValue when there is none.");
        switchOverload<double>(f, "Float");
        switchOverload<I>(f, "Int");
        switchOverload<bool>(f, "Bool");
        switchOverload<D3>(f, "Vector");
        switchOverload<Rot>(f, "Rotator");
        switchOverload<M4>(f, "Matrix");
        switchOverload<Col>(f, "Color");
    }
    {
        auto& f = lib.fn("condition", "select", "Compares firstTerm with secondTerm: ifTrue when the comparison holds, else ifFalse.");
        over(f, std::string("firstTerm secondTerm ") + kOps + " ifTrue=1 ifFalse",
             "result", [](double a, double b, Choice op, double t, double e) { return compare(op.index, a, b) ? t : e; }, "Float");
        over(f, std::string("firstTerm secondTerm ") + kOps + " ifTrue=1,1,1 ifFalse",
             "result", [](double a, double b, Choice op, D3 t, D3 e) { return compare(op.index, a, b) ? t : e; }, "Vector");
        over(f, std::string("firstTerm secondTerm ") + kOps + " ifTrue ifFalse",
             "result", [](double a, double b, Choice op, M4 t, M4 e) { return compare(op.index, a, b) ? t : e; }, "Matrix");
    }

    // Boolean and bitwise
    {
        auto& f = lib.fn("and", "bitwise", "A AND B: logical for bools, bitwise for ints.");
        over(f, "a b", "result", [](bool a, bool b) { return a && b; });
        over(f, "a b", "result", [](I a, I b) { return a & b; });
    }
    {
        auto& f = lib.fn("or", "bitwise", "A OR B: logical for bools, bitwise for ints.");
        over(f, "a b", "result", [](bool a, bool b) { return a || b; });
        over(f, "a b", "result", [](I a, I b) { return a | b; });
    }
    {
        auto& f = lib.fn("xor", "bitwise", "A XOR B: logical for bools, bitwise for ints.");
        over(f, "a b", "result", [](bool a, bool b) { return a != b; });
        over(f, "a b", "result", [](I a, I b) { return a ^ b; });
    }
    {
        auto& f = lib.fn("not", "bitwise", "NOT A: logical for bools, bitwise complement for ints.");
        over(f, "a", "result", [](bool a) { return !a; });
        over(f, "a", "result", [](I a) { return ~a; });
    }
    over(lib.fn("nand", "bitwise", "NOT (A AND B)."), "a b", "result", [](bool a, bool b) { return !(a && b); });
    over(lib.fn("nor", "bitwise", "NOT (A OR B)."), "a b", "result", [](bool a, bool b) { return !(a || b); });
    over(lib.fn("shiftLeft", "bitwise", "A's bits moved left by shift (0..63)."), "a shift=1", "result",
         [](I a, I s) { return s < 0 || s > 63 ? I(0) : I(uint64_t(a) << s); });
    over(lib.fn("shiftRight", "bitwise", "A's bits moved right by shift (0..63), the sign kept."), "a shift=1", "result",
         [](I a, I s) { return s < 0 || s > 63 ? (a < 0 ? I(-1) : I(0)) : a >> s; });

    // Seeded random and noise
    over(lib.fn("randomFloatFromStream", "noise", "A random value in [0, 1), fixed by the stream (seed)."), "stream", "result",
         [](I s) { return unit(s); });
    over(lib.fn("randomFloatInRangeFromStream", "noise", "A random value in min..max, fixed by the stream (seed)."), "min max=1 stream", "result",
         [](double lo, double hi, I s) { return math::lerp(lo, hi, unit(s)); });
    over(lib.fn("randomIntegerInRangeFromStream", "noise", "A random whole number in min..max (both included), fixed by the stream (seed)."),
         "min max=10 stream", "result", [](I lo, I hi, I s) {
             if (hi < lo)
                 std::swap(lo, hi);
             return lo + std::min(I(unit(s) * double(hi - lo + 1)), hi - lo);
         });
    over(lib.fn("randomBoolFromStream", "noise", "A random bool, fixed by the stream (seed)."), "stream", "result", [](I s) { return unit(s) < 0.5; });
    over(lib.fn("randomUnitVectorFromStream", "noise", "A random direction, uniform over the sphere, fixed by the stream (seed)."), "stream", "result",
         [](I s) { return unitVector(s); });
    over(lib.fn("randomRotatorFromStream", "noise", "A random rotation, fixed by the stream (seed); with roll off, X is random and Y stays level."),
         "roll=true stream", "result", [](bool roll, I s) {
             const Quat q = Quat::findBetweenNormals(math::Vector(1, 0, 0), math::Vector(unitVector(s)));
             if (!roll)
             {
                 const D3 x = q.rotateVector(math::Vector(1, 0, 0)).toGlm(), up = math::axisBasis().up;
                 D3 y = up - x * glm::dot(up, x);
                 y = glm::length(y) > 1e-9 ? glm::normalize(y) : D3(0, 0, 1);
                 return Rot::of(M3(x, y, glm::cross(x, y)));
             }
             return Rot::of(Quat(math::Vector(unitVector(s)), unit(s, 3) * 2.0 * math::kPi) * q);
         });
    over(lib.fn("perlinNoise1D", "noise", "Smooth gradient noise in -1..1 along value; seed picks an unrelated pattern."), "value seed", "result",
         [](double v, I s) { return noise(D3(v, 0.5, 0.25), s); });
    over(lib.fn("perlinNoise3D", "noise", "Smooth gradient noise in -1..1 over space; seed picks an unrelated pattern."), "value seed", "result",
         [](D3 v, I s) { return noise(v, s); });
    over(lib.fn("noiseFloat", "noise",
                "Control Rig's noise: gradient noise at value * frequency, drifting by speed * time, mapped onto minimum..maximum. Wire the "
                "scene time into time to animate it."),
         "value speed=0.1 frequency=1 minimum maximum=1 time seed", "result", [](double v, double sp, double fr, double lo, double hi, double t, I s) {
             return math::lerp(lo, hi, noise(D3(v * fr + sp * t, 0.5, 0.25), s) * 0.5 + 0.5);
         });
    over(lib.fn("noiseVector", "noise",
                "Control Rig's noise per component: gradient noise at position * frequency, drifting by speed * time, mapped onto "
                "minimum..maximum."),
         "position speed=0.1,0.1,0.1 frequency=1 minimum maximum=1 time seed", "result",
         [](D3 p, D3 sp, double fr, double lo, double hi, double t, I s) {
             D3 out;
             for (int k = 0; k < 3; ++k)
                 out[k] = math::lerp(lo, hi, noise(p * fr + sp * t + D3(17.3 * k, 31.1 * k, 7.7 * k), s) * 0.5 + 0.5);
             return out;
         });
}
} // namespace lila::rigmath
