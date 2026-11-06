// SPDX-License-Identifier: Apache-2.0
#pragma once

// The core's transcendental math: pm::sin, pm::exp, pm::pow... in place of std::'s, so one switch
// decides how a groom's float math is evaluated on every platform.
//
// Every other float operation the core does is IEEE 754 and compiled as written (CMakeLists.txt,
// "Determinism": no contraction, no fast math), so it gives the same bits on every compiler. The C
// runtimes' float transcendental functions do not: MSVC's sinf, expf, powf... and glibc's round
// differently in the last bit now and then, and those bits grow into other strands (a clump picked,
// a k-means cluster, a collision). Measured on the 39 presets of test_determinism.cpp
// (PresetDigests.json): with the C runtime's float functions 9 make the same bits on Linux (GCC,
// glibc) as on Windows (MSVC), 12 more the same geometry with other hair colors; evaluated in double
// precision and rounded to float, all 39 match bit for bit.
//
// GROOMLAB_PORTABLE_MATH (CMake option, ON): each float function is the C runtime's
// double function, rounded to float: the same bits on Windows, Linux and macOS (the double functions
// agree to well under a float's last bit; one that rounds to another float is vanishingly rare), at
// the cost of a double evaluation. Every golden is recorded with it on (test::kRecordedMath). OFF:
// the C runtime's float function, as std:: is (for comparisons only).
//
// Double arguments go to the double function either way; integers are taken as doubles, as std::
// does; mixed argument types fall back to std:: itself.

#include <cmath>
#include <type_traits>

#ifndef GROOMLAB_PORTABLE_MATH
#define GROOMLAB_PORTABLE_MATH 1
#endif

namespace groomlab::pm {

#if GROOMLAB_PORTABLE_MATH
#define GROOMLAB_PM_FLOAT1(name) \
    inline float name(float x) { return static_cast<float>(std::name(static_cast<double>(x))); }
#define GROOMLAB_PM_FLOAT2(name)                                                         \
    inline float name(float a, float b) {                                                \
        return static_cast<float>(std::name(static_cast<double>(a), static_cast<double>(b))); \
    }
#else
#define GROOMLAB_PM_FLOAT1(name) \
    inline float name(float x) { return std::name(x); }
#define GROOMLAB_PM_FLOAT2(name) \
    inline float name(float a, float b) { return std::name(a, b); }
#endif

#define GROOMLAB_PM1(name)                                                       \
    GROOMLAB_PM_FLOAT1(name)                                                     \
    inline double name(double x) { return std::name(x); }                        \
    template <class T, std::enable_if_t<std::is_integral<T>::value, int> = 0>   \
    inline double name(T x) {                                                    \
        return std::name(static_cast<double>(x));                                \
    }
#define GROOMLAB_PM2(name)                                                       \
    GROOMLAB_PM_FLOAT2(name)                                                     \
    inline double name(double a, double b) { return std::name(a, b); }           \
    template <class A, class B>                                                  \
    inline auto name(A a, B b) {                                                 \
        return std::name(a, b);                                                  \
    }

GROOMLAB_PM1(sin)
GROOMLAB_PM1(cos)
GROOMLAB_PM1(tan)
GROOMLAB_PM1(asin)
GROOMLAB_PM1(acos)
GROOMLAB_PM1(atan)
GROOMLAB_PM1(sinh)
GROOMLAB_PM1(cosh)
GROOMLAB_PM1(tanh)
GROOMLAB_PM1(exp)
GROOMLAB_PM1(exp2)
GROOMLAB_PM1(log)
GROOMLAB_PM1(log2)
GROOMLAB_PM1(log10)
GROOMLAB_PM1(cbrt)
GROOMLAB_PM2(atan2)
GROOMLAB_PM2(pow)
GROOMLAB_PM2(hypot)

#undef GROOMLAB_PM1
#undef GROOMLAB_PM2
#undef GROOMLAB_PM_FLOAT1
#undef GROOMLAB_PM_FLOAT2

}  // namespace groomlab::pm
