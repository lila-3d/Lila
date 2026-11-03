#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <concepts>
#include <string>

/** Number formatting and colour conversion the mesh file readers and writers share. Internal to src/geo. */
namespace lila::geo::detail
{
/** Appends v in the shortest form that reads back exactly, or with precision significant digits when above 0. */
template <std::floating_point T> void appendNumber(std::string& s, T v, int precision)
{
    char buf[64];
    auto r = precision > 0 ? std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::general, precision)
                           : std::to_chars(buf, buf + sizeof(buf), v);
    s.append(buf, r.ptr);
}

inline float srgbToLinear(float c)
{
    c = std::max(c, 0.0f);
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

inline float linearToSrgb(float c)
{
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}
} // namespace lila::geo::detail
