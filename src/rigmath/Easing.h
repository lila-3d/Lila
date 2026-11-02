#pragma once

#include "math/MathCore.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

/** Easing curves on 0..1: Unreal's EEasingFunc (Kismet Ease) and Control Rig's ERigVMAnimEasingType. */
namespace lila::rigmath::easing
{
inline std::string kismetNames()
{
    return "linear|step|sinusoidalIn|sinusoidalOut|sinusoidalInOut|easeIn|easeOut|easeInOut|expoIn|expoOut|expoInOut|circularIn|circularOut|"
           "circularInOut";
}

inline double easeIn(double t, double e) { return std::pow(t, e); }
inline double easeOut(double t, double e) { return 1.0 - std::pow(1.0 - t, e); }
inline double sinIn(double t) { return 1.0 - std::cos(t * math::kPi * 0.5); }
inline double sinOut(double t) { return std::sin(t * math::kPi * 0.5); }
inline double expoIn(double t) { return t == 0.0 ? 0.0 : std::pow(2.0, 10.0 * (t - 1.0)); }
inline double expoOut(double t) { return t == 1.0 ? 1.0 : 1.0 - std::pow(2.0, -10.0 * t); }
inline double circIn(double t) { return 1.0 - std::sqrt(std::max(0.0, 1.0 - t * t)); }
inline double circOut(double t) { return std::sqrt(std::max(0.0, 1.0 - (t - 1.0) * (t - 1.0))); }
template <class In, class Out> double inOut(double t, In in, Out out) { return t < 0.5 ? in(t * 2.0) * 0.5 : out(t * 2.0 - 1.0) * 0.5 + 0.5; }

/** The eased alpha of Kismet's Ease. Alpha is not clamped, as in Unreal, except by step. */
inline double kismet(int64_t fn, double t, double blendExp, int64_t steps)
{
    switch (fn)
    {
    case 1:
        if (t <= 0.0)
            return 0.0;
        if (t >= 1.0 || steps < 2)
            return t >= 1.0 ? 1.0 : 0.0;
        return std::floor(t * double(steps)) / double(steps - 1);
    case 2: return sinIn(t);
    case 3: return sinOut(t);
    case 4: return inOut(t, sinIn, sinOut);
    case 5: return easeIn(t, blendExp);
    case 6: return easeOut(t, blendExp);
    case 7: return inOut(t, [&](double x) { return easeIn(x, blendExp); }, [&](double x) { return easeOut(x, blendExp); });
    case 8: return expoIn(t);
    case 9: return expoOut(t);
    case 10: return inOut(t, expoIn, expoOut);
    case 11: return circIn(t);
    case 12: return circOut(t);
    case 13: return inOut(t, circIn, circOut);
    default: return t;
    }
}

inline std::string rigVmNames()
{
    return "linear|quadraticEaseIn|quadraticEaseOut|quadraticEaseInOut|cubicEaseIn|cubicEaseOut|cubicEaseInOut|quarticEaseIn|quarticEaseOut|"
           "quarticEaseInOut|quinticEaseIn|quinticEaseOut|quinticEaseInOut|sineEaseIn|sineEaseOut|sineEaseInOut|circularEaseIn|circularEaseOut|"
           "circularEaseInOut|exponentialEaseIn|exponentialEaseOut|exponentialEaseInOut|elasticEaseIn|elasticEaseOut|elasticEaseInOut|backEaseIn|"
           "backEaseOut|backEaseInOut|bounceEaseIn|bounceEaseOut|bounceEaseInOut";
}

inline double powIn(double t, int n) { return std::pow(t, n); }
inline double powOut(double t, int n) { return 1.0 - std::pow(1.0 - t, n); }
inline double elasticIn(double t) { return std::sin(13.0 * math::kPi * 0.5 * t) * std::pow(2.0, 10.0 * (t - 1.0)); }
inline double elasticOut(double t) { return std::sin(-13.0 * math::kPi * 0.5 * (t + 1.0)) * std::pow(2.0, -10.0 * t) + 1.0; }
inline double backIn(double t) { return t * t * t - t * std::sin(t * math::kPi); }
inline double backOut(double t) { return 1.0 - backIn(1.0 - t); }
inline double bounceOut(double t)
{
    if (t < 4.0 / 11.0)
        return 121.0 * t * t / 16.0;
    if (t < 8.0 / 11.0)
        return 363.0 / 40.0 * t * t - 99.0 / 10.0 * t + 17.0 / 5.0;
    if (t < 9.0 / 10.0)
        return 4356.0 / 361.0 * t * t - 35442.0 / 1805.0 * t + 16061.0 / 1805.0;
    return 54.0 / 5.0 * t * t - 513.0 / 25.0 * t + 268.0 / 25.0;
}
inline double bounceIn(double t) { return 1.0 - bounceOut(1.0 - t); }

/** Control Rig's easing of t in 0..1 (Penner's curves). */
inline double rigVm(int64_t type, double t)
{
    if (type >= 1 && type <= 12)
    {
        const int n = int((type - 1) / 3) + 2;
        auto in = [n](double x) { return powIn(x, n); };
        auto out = [n](double x) { return powOut(x, n); };
        switch ((type - 1) % 3)
        {
        case 0: return in(t);
        case 1: return out(t);
        default: return inOut(t, in, out);
        }
    }
    switch (type)
    {
    case 13: return sinIn(t);
    case 14: return sinOut(t);
    case 15: return 0.5 * (1.0 - std::cos(t * math::kPi));
    case 16: return circIn(t);
    case 17: return circOut(t);
    case 18: return inOut(t, circIn, circOut);
    case 19: return expoIn(t);
    case 20: return expoOut(t);
    case 21: return inOut(t, expoIn, expoOut);
    case 22: return elasticIn(t);
    case 23: return elasticOut(t);
    case 24: return inOut(t, elasticIn, elasticOut);
    case 25: return backIn(t);
    case 26: return backOut(t);
    case 27: return inOut(t, backIn, backOut);
    case 28: return bounceIn(t);
    case 29: return bounceOut(t);
    case 30: return inOut(t, bounceIn, bounceOut);
    default: return t;
    }
}
} // namespace lila::rigmath::easing
