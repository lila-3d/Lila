#pragma once

#include "math/Vector.h"

#include <optional>
#include <string>
#include <string_view>

namespace lila::math
{
struct Color;

/** Linear-space float RGBA. */
struct LinearColor
{
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;

    constexpr LinearColor() = default;
    constexpr LinearColor(float inR, float inG, float inB, float inA = 1.0f) : r(inR), g(inG), b(inB), a(inA) {}
    /** Decodes sRGB. */
    explicit LinearColor(const Color& c);
    explicit LinearColor(const Vector& v) : r(float(v.x)), g(float(v.y)), b(float(v.z)) {}

    static constexpr LinearColor white() { return {1, 1, 1, 1}; }
    static constexpr LinearColor gray() { return {0.5f, 0.5f, 0.5f, 1}; }
    static constexpr LinearColor black() { return {0, 0, 0, 1}; }
    static constexpr LinearColor transparent() { return {0, 0, 0, 0}; }
    static constexpr LinearColor red() { return {1, 0, 0, 1}; }
    static constexpr LinearColor green() { return {0, 1, 0, 1}; }
    static constexpr LinearColor blue() { return {0, 0, 1, 1}; }
    static constexpr LinearColor yellow() { return {1, 1, 0, 1}; }
    static LinearColor fromSRGBColor(const Color& c) { return LinearColor(c); }
    /** Hue in degrees, saturation and value 0..1, alpha kept. */
    static LinearColor fromHSV(float hue, float saturation, float value, float alpha = 1.0f);
    static LinearColor lerp(const LinearColor& a, const LinearColor& b, float alpha);

    float& operator[](int i) { return i == 0 ? r : (i == 1 ? g : (i == 2 ? b : a)); }
    float operator[](int i) const { return i == 0 ? r : (i == 1 ? g : (i == 2 ? b : a)); }
    constexpr LinearColor operator+(const LinearColor& o) const { return {r + o.r, g + o.g, b + o.b, a + o.a}; }
    constexpr LinearColor operator-(const LinearColor& o) const { return {r - o.r, g - o.g, b - o.b, a - o.a}; }
    constexpr LinearColor operator*(const LinearColor& o) const { return {r * o.r, g * o.g, b * o.b, a * o.a}; }
    constexpr LinearColor operator/(const LinearColor& o) const { return {r / o.r, g / o.g, b / o.b, a / o.a}; }
    constexpr LinearColor operator*(float s) const { return {r * s, g * s, b * s, a * s}; }
    constexpr LinearColor operator/(float s) const { return {r / s, g / s, b / s, a / s}; }
    constexpr bool operator==(const LinearColor&) const = default;
    bool equals(const LinearColor& o, float tolerance = 1.0e-4f) const;

    /** To 8-bit, rounding; sRGB encodes the rgb channels. */
    Color toColor(bool sRGB) const;
    /** To 8-bit without any curve, truncating. */
    Color quantize() const;
    /** To 8-bit without any curve, rounding. */
    Color quantizeRound() const;
    /** (hue degrees, saturation, value, alpha) packed into r, g, b, a. */
    LinearColor linearRGBToHSV() const;
    /** Inverse of linearRGBToHSV. */
    LinearColor hsvToLinearRGB() const;
    /** Rec. 709 luma weights on linear values. */
    float getLuminance() const { return r * 0.2126f + g * 0.7152f + b * 0.0722f; }
    float getMax() const { return std::max({r, g, b, a}); }
    float getMin() const { return std::min({r, g, b, a}); }
    LinearColor desaturate(float desaturation) const;
    LinearColor getClamped(float minValue = 0.0f, float maxValue = 1.0f) const;
    LinearColor copyWithNewOpacity(float alpha) const { return {r, g, b, alpha}; }
    bool isAlmostBlack() const { return r * r < 1.0e-8f && g * g < 1.0e-8f && b * b < 1.0e-8f; }
    Vector toVector() const { return {r, g, b}; }
};

inline constexpr LinearColor operator*(float s, const LinearColor& c) { return c * s; }

/** 8-bit RGBA, sRGB-encoded rgb by convention. */
struct Color
{
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 255;

    constexpr Color() = default;
    constexpr Color(uint8_t inR, uint8_t inG, uint8_t inB, uint8_t inA = 255) : r(inR), g(inG), b(inB), a(inA) {}

    static constexpr Color white() { return {255, 255, 255}; }
    static constexpr Color black() { return {0, 0, 0}; }
    static constexpr Color transparent() { return {0, 0, 0, 0}; }
    static constexpr Color red() { return {255, 0, 0}; }
    static constexpr Color green() { return {0, 255, 0}; }
    static constexpr Color blue() { return {0, 0, 255}; }
    static constexpr Color yellow() { return {255, 255, 0}; }
    static constexpr Color cyan() { return {0, 255, 255}; }
    static constexpr Color magenta() { return {255, 0, 255}; }
    static constexpr Color orange() { return {243, 156, 18}; }
    static constexpr Color purple() { return {169, 7, 228}; }
    /** Accepts RGB, RGBA, RRGGBB or RRGGBBAA with an optional leading '#'. */
    static std::optional<Color> fromHex(std::string_view hex);

    constexpr bool operator==(const Color&) const = default;
    /** "RRGGBBAA". */
    std::string toHex() const;
    uint32_t toPackedARGB() const { return uint32_t(a) << 24 | uint32_t(r) << 16 | uint32_t(g) << 8 | b; }
    uint32_t toPackedRGBA() const { return uint32_t(r) << 24 | uint32_t(g) << 16 | uint32_t(b) << 8 | a; }
    /** sRGB decode to linear. */
    LinearColor toLinear() const { return LinearColor(*this); }
    /** Divides by 255 with no curve. */
    LinearColor reinterpretAsLinear() const { return {r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f}; }
    Color withAlpha(uint8_t alpha) const { return {r, g, b, alpha}; }
};

float srgbToLinear(float c);
float linearToSRGB(float c);
} // namespace lila::math
