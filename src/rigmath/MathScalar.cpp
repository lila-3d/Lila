#include "rigmath/Builder.h"
#include "rigmath/Easing.h"

#include <algorithm>
#include <limits>

namespace lila::rigmath
{
namespace scalarfns
{
using I = int64_t;
using math::kPi;

double safeSqrt(double a) { return a >= 0.0 ? std::sqrt(a) : 0.0; }
double powSafe(double b, double e) { return b < 0.0 && e != std::floor(e) ? 0.0 : std::pow(b, e); }
double signOf(double a) { return a > 0.0 ? 1.0 : (a < 0.0 ? -1.0 : 0.0); }
I wrapInt(I v, I lo, I hi)
{
    const I size = hi - lo + 1;
    if (size <= 0)
        return hi;
    return lo + (((v - lo) % size) + size) % size;
}
double degAsin(double a) { return math::radiansToDegrees(std::asin(std::clamp(a, -1.0, 1.0))); }
double degAcos(double a) { return math::radiansToDegrees(std::acos(std::clamp(a, -1.0, 1.0))); }

/** floor, ceil, round or truncate: the float result and the same as an integer. */
template <class F> void rounding(Library& lib, const char* name, const char* doc, F op)
{
    auto& f = lib.fn(name, "range", doc);
    over(f, "a", "result integer", [op](double a) { double r = op(a); return std::tuple{r, I(r)}; });
    over(f, "a", "result", [op](D3 a) { return D3(op(a.x), op(a.y), op(a.z)); });
}
} // namespace scalarfns

void addScalar(Library& lib)
{
    using namespace scalarfns;

    // Arithmetic
    {
        auto& f = lib.fn("add", "arithmetic", "A + B.");
        over(f, "a b", "result", [](double a, double b) { return a + b; });
        over(f, "a b", "result", [](I a, I b) { return a + b; });
        over(f, "a b", "result", [](D3 a, D3 b) { return a + b; });
        over(f, "a b", "result", [](Col a, Col b) { return Col{a.rgb + b.rgb}; });
    }
    {
        auto& f = lib.fn("subtract", "arithmetic", "A - B.");
        over(f, "a b", "result", [](double a, double b) { return a - b; });
        over(f, "a b", "result", [](I a, I b) { return a - b; });
        over(f, "a b", "result", [](D3 a, D3 b) { return a - b; });
        over(f, "a b", "result", [](Col a, Col b) { return Col{a.rgb - b.rgb}; });
    }
    {
        auto& f = lib.fn("multiply", "arithmetic", "A * B. Vectors and colours multiply per component; quats apply B first, then A; matrices "
                                                   "apply A first, then B, as in Unreal.");
        over(f, "a b=1", "result", [](double a, double b) { return a * b; });
        over(f, "a b=1", "result", [](I a, I b) { return a * b; });
        over(f, "a b=1,1,1", "result", [](D3 a, D3 b) { return a * b; });
        over(f, "a b=1", "result", [](D3 a, double b) { return a * b; }, "VectorFloat");
        over(f, "a b=1,1,1", "result", [](Col a, Col b) { return Col{a.rgb * b.rgb}; });
        over(f, "a b=1", "result", [](Col a, double b) { return Col{a.rgb * b}; }, "ColorFloat");
        over(f, "a b", "result", [](Quat a, Quat b) { return a * b; });
        over(f, "a b", "result", [](M4 a, M4 b) { return M4(b * a); });
    }
    {
        auto& f = lib.fn("divide", "arithmetic", "A / B; 0 where B is 0.");
        over(f, "a b=1", "result", [](double a, double b) { return b == 0.0 ? 0.0 : a / b; });
        over(f, "a b=1", "result", [](I a, I b) { return b == 0 ? I(0) : a / b; });
        over(f, "a b=1,1,1", "result", [](D3 a, D3 b) {
            return D3(b.x == 0.0 ? 0.0 : a.x / b.x, b.y == 0.0 ? 0.0 : a.y / b.y, b.z == 0.0 ? 0.0 : a.z / b.z);
        });
        over(f, "a b=1", "result", [](D3 a, double b) { return b == 0.0 ? D3(0.0) : a / b; }, "VectorFloat");
    }
    over(lib.fn("safeDivide", "arithmetic", "A / B, or 0 when B is 0."), "a b=1", "result", [](double a, double b) { return b == 0.0 ? 0.0 : a / b; });
    {
        auto& f = lib.fn("percent", "arithmetic", "The remainder of A / B (A % B), with A's sign; 0 where B is 0.");
        over(f, "a b=1", "result", [](double a, double b) { return b == 0.0 ? 0.0 : std::fmod(a, b); });
        over(f, "a b=1", "result", [](I a, I b) { return b == 0 ? I(0) : a % b; });
    }
    over(lib.fn("fmod", "arithmetic", "Divides dividend by divisor: the whole quotient and the remainder."), "dividend divisor=1", "quotient remainder",
         [](double a, double b) {
             if (b == 0.0)
                 return std::tuple{I(0), 0.0};
             const double r = std::fmod(a, b);
             return std::tuple{I(std::trunc(a / b)), r};
         });
    {
        auto& f = lib.fn("negate", "arithmetic", "-A.");
        over(f, "a", "result", [](double a) { return -a; });
        over(f, "a", "result", [](I a) { return -a; });
        over(f, "a", "result", [](D3 a) { return -a; });
    }
    over(lib.fn("multiplyByPi", "arithmetic", "A * pi."), "a", "result", [](double a) { return a * kPi; });
    over(lib.fn("getPI", "arithmetic", "pi."), "", "result", [] { return kPi; });
    over(lib.fn("getTAU", "arithmetic", "2 pi."), "", "result", [] { return 2.0 * kPi; });

    // Comparison
    {
        auto& f = lib.fn("equal", "comparison", "A == B; vectors, rotators and matrices compare within the tolerance, rotator angles modulo 360.");
        over(f, "a b", "result", [](double a, double b) { return a == b; });
        over(f, "a b", "result", [](I a, I b) { return a == b; });
        over(f, "a b", "result", [](bool a, bool b) { return a == b; });
        over(f, "a b errorTolerance=0.0001", "result", [](D3 a, D3 b, double t) { return math::Vector(a).equals(b, t); });
        over(f, "a b errorTolerance=0.0001", "result", [](Rot a, Rot b, double t) {
            for (int k = 0; k < 3; ++k)
                if (std::abs(math::normalizeAxis(a.deg[k] - b.deg[k])) > t)
                    return false;
            return true;
        });
        over(f, "a b errorTolerance=0.0001", "result", [](M4 a, M4 b, double t) { return math::Matrix(a).equals(math::Matrix(b), t); });
        over(f, "a b errorTolerance=0.0001", "result", [](Col a, Col b, double t) { return math::Vector(a.rgb).equals(b.rgb, t); });
    }
    {
        auto& f = lib.fn("notEqual", "comparison", "A != B; vectors and matrices compare within the tolerance.");
        over(f, "a b", "result", [](double a, double b) { return a != b; });
        over(f, "a b", "result", [](I a, I b) { return a != b; });
        over(f, "a b", "result", [](bool a, bool b) { return a != b; });
        over(f, "a b errorTolerance=0.0001", "result", [](D3 a, D3 b, double t) { return !math::Vector(a).equals(b, t); });
        over(f, "a b errorTolerance=0.0001", "result", [](M4 a, M4 b, double t) { return !math::Matrix(a).equals(math::Matrix(b), t); });
    }
    over(lib.fn("nearlyEqual", "comparison", "|A - B| <= errorTolerance."), "a b errorTolerance=0.000001", "result",
         [](double a, double b, double t) { return std::abs(a - b) <= t; });
    for (auto [name, doc, op] : std::initializer_list<std::tuple<const char*, const char*, int>>{
             {"greater", "A > B.", 0}, {"greaterEqual", "A >= B.", 1}, {"less", "A < B.", 2}, {"lessEqual", "A <= B.", 3}})
    {
        auto& f = lib.fn(name, "comparison", doc);
        auto test = [op](double a, double b) { return op == 0 ? a > b : op == 1 ? a >= b : op == 2 ? a < b : a <= b; };
        over(f, "a b", "result", [test](double a, double b) { return test(a, b); });
        over(f, "a b", "result", [test](I a, I b) { return test(double(a), double(b)); });
    }
    {
        auto& f = lib.fn("inRange", "comparison", "Whether value lies between min and max, each end included or not.");
        over(f, "value min max=1 inclusiveMin=true inclusiveMax=true", "result", [](double v, double lo, double hi, bool inLo, bool inHi) {
            return (inLo ? v >= lo : v > lo) && (inHi ? v <= hi : v < hi);
        });
        over(f, "value min max=1 inclusiveMin=true inclusiveMax=true", "result", [](I v, I lo, I hi, bool inLo, bool inHi) {
            return (inLo ? v >= lo : v > lo) && (inHi ? v <= hi : v < hi);
        });
    }

    // Range
    {
        auto& f = lib.fn("min", "range", "The smaller of A and B; vectors per component.");
        over(f, "a b", "result", [](double a, double b) { return std::min(a, b); });
        over(f, "a b", "result", [](I a, I b) { return std::min(a, b); });
        over(f, "a b", "result", [](D3 a, D3 b) { return glm::min(a, b); });
    }
    {
        auto& f = lib.fn("max", "range", "The larger of A and B; vectors per component.");
        over(f, "a b", "result", [](double a, double b) { return std::max(a, b); });
        over(f, "a b", "result", [](I a, I b) { return std::max(a, b); });
        over(f, "a b", "result", [](D3 a, D3 b) { return glm::max(a, b); });
    }
    {
        auto& f = lib.fn("clamp", "range", "Value limited to min..max; vectors per component.");
        over(f, "value min max=1", "result", [](double v, double lo, double hi) { return std::max(lo, std::min(v, hi)); });
        over(f, "value min max=1", "result", [](I v, I lo, I hi) { return std::max(lo, std::min(v, hi)); });
        over(f, "value min max=1,1,1", "result", [](D3 v, D3 lo, D3 hi) { return glm::max(lo, glm::min(v, hi)); });
    }
    {
        auto& f = lib.fn("abs", "range", "|A|; vectors per component.");
        over(f, "a", "result", [](double a) { return std::abs(a); });
        over(f, "a", "result", [](I a) { return a < 0 ? -a : a; });
        over(f, "a", "result", [](D3 a) { return glm::abs(a); });
    }
    {
        auto& f = lib.fn("sign", "range", "-1, 0 or 1 by A's sign; vectors per component.");
        over(f, "a", "result", [](double a) { return signOf(a); });
        over(f, "a", "result", [](I a) { return I(a > 0) - I(a < 0); });
        over(f, "a", "result", [](D3 a) { return D3(signOf(a.x), signOf(a.y), signOf(a.z)); });
    }
    rounding(lib, "floor", "The largest whole number not above A.", [](double a) { return std::floor(a); });
    rounding(lib, "ceil", "The smallest whole number not below A.", [](double a) { return std::ceil(a); });
    rounding(lib, "round", "A rounded to the nearest whole number, halves away from zero.", [](double a) { return std::round(a); });
    rounding(lib, "truncate", "A with its fraction dropped (toward zero).", [](double a) { return std::trunc(a); });
    over(lib.fn("fraction", "range", "The fractional part of A: A - floor(A)."), "a", "result", [](double a) { return a - std::floor(a); });
    {
        auto& f = lib.fn("wrap", "range", "Value wrapped into min..max as a cycle; ints treat both ends as part of the cycle.");
        over(f, "value min max=1", "result", [](double v, double lo, double hi) { return math::wrap(v, lo, hi); });
        over(f, "value min max=1", "result", [](I v, I lo, I hi) { return wrapInt(v, lo, hi); });
    }
    {
        auto& f = lib.fn("gridSnap", "range", "Value snapped to the nearest multiple of grid; vectors per component.");
        over(f, "value grid=1", "result", [](double v, double g) { return math::gridSnap(v, g); });
        over(f, "value grid=1", "result", [](D3 v, double g) { return D3(math::gridSnap(v.x, g), math::gridSnap(v.y, g), math::gridSnap(v.z, g)); });
    }
    over(lib.fn("mapRangeClamped", "range", "Value mapped from inRangeA..inRangeB onto outRangeA..outRangeB, held within the out range."),
         "value inRangeA inRangeB=1 outRangeA outRangeB=1", "result", [](double v, double a, double b, double c, double d) { return math::mapRangeClamped(v, a, b, c, d); });
    over(lib.fn("mapRangeUnclamped", "range", "Value mapped from inRangeA..inRangeB onto outRangeA..outRangeB, extrapolating past either end."),
         "value inRangeA inRangeB=1 outRangeA outRangeB=1", "result", [](double v, double a, double b, double c, double d) { return math::mapRangeUnclamped(v, a, b, c, d); });
    over(lib.fn("normalizeToRange", "range", "Where value lies in rangeMin..rangeMax as 0..1, unclamped."), "value rangeMin rangeMax=1", "result",
         [](double v, double lo, double hi) { return lo == hi ? (v < lo ? 0.0 : 1.0) : (v - lo) / (hi - lo); });
    over(lib.fn("normalizeAxis", "range", "An angle in degrees wrapped into (-180, 180]."), "angle", "result", [](double a) { return math::normalizeAxis(a); });
    over(lib.fn("clampAxis", "range", "An angle in degrees wrapped into [0, 360)."), "angle", "result", [](double a) { return math::clampAxis(a); });
    over(lib.fn("clampAngle", "range", "An angle in degrees limited to the arc min..max, going the shorter way round."),
         "angleDegrees minAngleDegrees=-90 maxAngleDegrees=90", "result", [](double a, double lo, double hi) { return math::clampAngle(a, lo, hi); });
    over(lib.fn("maxOfFloatArray", "range", "The largest value and its index; -1 and 0 for an empty array."), "floatArray", "indexOfMaxValue maxValue",
         [](std::vector<double> v) {
             if (v.empty())
                 return std::tuple{I(-1), 0.0};
             auto it = std::max_element(v.begin(), v.end());
             return std::tuple{I(it - v.begin()), *it};
         });
    over(lib.fn("minOfFloatArray", "range", "The smallest value and its index; -1 and 0 for an empty array."), "floatArray", "indexOfMinValue minValue",
         [](std::vector<double> v) {
             if (v.empty())
                 return std::tuple{I(-1), 0.0};
             auto it = std::min_element(v.begin(), v.end());
             return std::tuple{I(it - v.begin()), *it};
         });
    over(lib.fn("maxOfIntArray", "range", "The largest value and its index; -1 and 0 for an empty array."), "intArray", "indexOfMaxValue maxValue",
         [](std::vector<I> v) {
             if (v.empty())
                 return std::tuple{I(-1), I(0)};
             auto it = std::max_element(v.begin(), v.end());
             return std::tuple{I(it - v.begin()), *it};
         });
    over(lib.fn("minOfIntArray", "range", "The smallest value and its index; -1 and 0 for an empty array."), "intArray", "indexOfMinValue minValue",
         [](std::vector<I> v) {
             if (v.empty())
                 return std::tuple{I(-1), I(0)};
             auto it = std::min_element(v.begin(), v.end());
             return std::tuple{I(it - v.begin()), *it};
         });
    over(lib.fn("averageOfFloatArray", "range", "The mean of the values; 0 for an empty array."), "floatArray", "result", [](std::vector<double> v) {
        double s = 0.0;
        for (double x : v)
            s += x;
        return v.empty() ? 0.0 : s / double(v.size());
    });

    // Interpolation
    over(lib.fn("lerp", "interpolation", "A + alpha * (B - A)."), "a b=1 alpha=0.5", "result", [](double a, double b, double t) { return math::lerp(a, b, t); });
    over(lib.fn("finterpTo", "interpolation",
                "One step of easing current toward target: closes deltaTime * interpSpeed of the gap; a speed of 0 or less snaps."),
         "current target deltaTime=0.0333333 interpSpeed=1", "result", [](double c, double t, double dt, double s) { return math::finterpTo(c, t, dt, s); });
    over(lib.fn("finterpToConstant", "interpolation", "One step of moving current toward target at interpSpeed units a second."),
         "current target deltaTime=0.0333333 interpSpeed=1", "result",
         [](double c, double t, double dt, double s) { return math::finterpToConstant(c, t, dt, s); });
    over(lib.fn("finterpEaseInOut", "interpolation", "A to B by alpha, easing in and out by exponent."), "a b=1 alpha=0.5 exponent=2", "result",
         [](double a, double b, double t, double e) { return math::lerp(a, b, easing::kismet(7, t, e, 2)); });

    // Easing
    const std::string kismetEases = "easingFunc=" + easing::kismetNames();
    over(lib.fn("ease", "easing", "A to B by alpha through an easing function. blendExp shapes easeIn/easeOut/easeInOut, steps the step function."),
         "a b=1 alpha=0.5 " + kismetEases + " blendExp=2 steps=2", "result",
         [](double a, double b, double t, Choice fn, double e, I steps) { return math::lerp(a, b, easing::kismet(fn.index, t, e, steps)); });
    over(lib.fn("vease", "easing", "Vector A to B by alpha through an easing function."), "a b=1,1,1 alpha=0.5 " + kismetEases + " blendExp=2 steps=2",
         "result", [](D3 a, D3 b, double t, Choice fn, double e, I steps) { return glm::mix(a, b, easing::kismet(fn.index, t, e, steps)); });
    over(lib.fn("easing", "easing",
                "Control Rig's easing: value's place in sourceMinimum..sourceMaximum (clamped) eased, then mapped onto "
                "targetMinimum..targetMaximum."),
         "value type=" + easing::rigVmNames() + " sourceMinimum sourceMaximum=1 targetMinimum targetMaximum=1", "result",
         [](double v, Choice type, double s0, double s1, double t0, double t1) {
             const double t = s1 == s0 ? 0.0 : std::clamp((v - s0) / (s1 - s0), 0.0, 1.0);
             return t0 + easing::rigVm(type.index, t) * (t1 - t0);
         });

    // Trig
    over(lib.fn("sin", "trig", "Sine of an angle in radians."), "a", "result", [](double a) { return std::sin(a); });
    over(lib.fn("cos", "trig", "Cosine of an angle in radians."), "a", "result", [](double a) { return std::cos(a); });
    over(lib.fn("tan", "trig", "Tangent of an angle in radians."), "a", "result", [](double a) { return std::tan(a); });
    over(lib.fn("asin", "trig", "Arcsine in radians; A is clamped to -1..1."), "a", "result", [](double a) { return std::asin(std::clamp(a, -1.0, 1.0)); });
    over(lib.fn("acos", "trig", "Arccosine in radians; A is clamped to -1..1."), "a", "result", [](double a) { return std::acos(std::clamp(a, -1.0, 1.0)); });
    over(lib.fn("atan", "trig", "Arctangent in radians."), "a", "result", [](double a) { return std::atan(a); });
    over(lib.fn("atan2", "trig", "The angle in radians of the point (x, y), in -pi..pi."), "y x=1", "result", [](double y, double x) { return std::atan2(y, x); });
    over(lib.fn("degSin", "trig", "Sine of an angle in degrees."), "a", "result", [](double a) { return std::sin(math::degreesToRadians(a)); });
    over(lib.fn("degCos", "trig", "Cosine of an angle in degrees."), "a", "result", [](double a) { return std::cos(math::degreesToRadians(a)); });
    over(lib.fn("degTan", "trig", "Tangent of an angle in degrees."), "a", "result", [](double a) { return std::tan(math::degreesToRadians(a)); });
    over(lib.fn("degAsin", "trig", "Arcsine in degrees; A is clamped to -1..1."), "a", "result", [](double a) { return degAsin(a); });
    over(lib.fn("degAcos", "trig", "Arccosine in degrees; A is clamped to -1..1."), "a", "result", [](double a) { return degAcos(a); });
    over(lib.fn("degAtan", "trig", "Arctangent in degrees."), "a", "result", [](double a) { return math::radiansToDegrees(std::atan(a)); });
    over(lib.fn("degAtan2", "trig", "The angle in degrees of the point (x, y), in -180..180."), "y x=1", "result",
         [](double y, double x) { return math::radiansToDegrees(std::atan2(y, x)); });
    over(lib.fn("degreesToRadians", "trig", "Degrees to radians."), "a", "result", [](double a) { return math::degreesToRadians(a); });
    over(lib.fn("radiansToDegrees", "trig", "Radians to degrees."), "a", "result", [](double a) { return math::radiansToDegrees(a); });

    // Exponential
    over(lib.fn("pow", "exponential", "base raised to exp; 0 for a negative base with a fractional exponent."), "base=2 exp=2", "result",
         [](double b, double e) { return powSafe(b, e); });
    over(lib.fn("sqrt", "exponential", "Square root; 0 for a negative A."), "a=1", "result", [](double a) { return safeSqrt(a); });
    over(lib.fn("square", "exponential", "A * A."), "a", "result", [](double a) { return a * a; });
    over(lib.fn("exp", "exponential", "e raised to A."), "a", "result", [](double a) { return std::exp(a); });
    over(lib.fn("loge", "exponential", "Natural logarithm; 0 for A <= 0."), "a=1", "result", [](double a) { return a > 0.0 ? std::log(a) : 0.0; });
    over(lib.fn("log", "exponential", "Logarithm of A to base; 0 when either is out of range."), "a=1 base=10", "result",
         [](double a, double b) { return a > 0.0 && b > 0.0 && b != 1.0 ? std::log(a) / std::log(b) : 0.0; });
    over(lib.fn("hypotenuse", "exponential", "sqrt(width^2 + height^2)."), "width height", "result", [](double w, double h) { return std::hypot(w, h); });
}
} // namespace lila::rigmath
