#include "math/Color.h"

#include <cstdio>

namespace lila::math
{
float srgbToLinear(float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float linearToSRGB(float c)
{
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

namespace
{
uint8_t roundToByte(float v) { return uint8_t(std::lround(clamp(v, 0.0f, 1.0f) * 255.0f)); }
uint8_t floorToByte(float v) { return uint8_t(std::floor(clamp(v, 0.0f, 1.0f) * 255.999f)); }

int hexDigit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
} // namespace

LinearColor::LinearColor(const Color& c)
    : r(srgbToLinear(c.r / 255.0f)), g(srgbToLinear(c.g / 255.0f)), b(srgbToLinear(c.b / 255.0f)), a(c.a / 255.0f)
{
}

LinearColor LinearColor::fromHSV(float hue, float saturation, float value, float alpha)
{
    return LinearColor(hue, saturation, value, alpha).hsvToLinearRGB();
}

LinearColor LinearColor::lerp(const LinearColor& a, const LinearColor& b, float alpha) { return a + (b - a) * alpha; }

bool LinearColor::equals(const LinearColor& o, float tolerance) const
{
    return std::abs(r - o.r) <= tolerance && std::abs(g - o.g) <= tolerance && std::abs(b - o.b) <= tolerance &&
           std::abs(a - o.a) <= tolerance;
}

Color LinearColor::toColor(bool sRGB) const
{
    if (!sRGB)
        return quantizeRound();
    return {roundToByte(linearToSRGB(clamp(r, 0.0f, 1.0f))), roundToByte(linearToSRGB(clamp(g, 0.0f, 1.0f))),
            roundToByte(linearToSRGB(clamp(b, 0.0f, 1.0f))), roundToByte(a)};
}

Color LinearColor::quantize() const { return {floorToByte(r), floorToByte(g), floorToByte(b), floorToByte(a)}; }

Color LinearColor::quantizeRound() const { return {roundToByte(r), roundToByte(g), roundToByte(b), roundToByte(a)}; }

LinearColor LinearColor::linearRGBToHSV() const
{
    const float maxC = std::max({r, g, b});
    const float minC = std::min({r, g, b});
    const float range = maxC - minC;
    float hue = 0.0f;
    if (range > 0.0f)
    {
        if (maxC == r)
            hue = std::fmod((g - b) / range, 6.0f);
        else if (maxC == g)
            hue = (b - r) / range + 2.0f;
        else
            hue = (r - g) / range + 4.0f;
        hue *= 60.0f;
        if (hue < 0.0f)
            hue += 360.0f;
    }
    const float saturation = maxC > 0.0f ? range / maxC : 0.0f;
    return {hue, saturation, maxC, a};
}

LinearColor LinearColor::hsvToLinearRGB() const
{
    const float hue = std::fmod(std::fmod(r, 360.0f) + 360.0f, 360.0f);
    const float saturation = g;
    const float value = b;
    const float c = value * saturation;
    const float h = hue / 60.0f;
    const float x = c * (1.0f - std::abs(std::fmod(h, 2.0f) - 1.0f));
    const float m = value - c;
    float rr = 0.0f, gg = 0.0f, bb = 0.0f;
    switch (int(h) % 6)
    {
    case 0: rr = c, gg = x; break;
    case 1: rr = x, gg = c; break;
    case 2: gg = c, bb = x; break;
    case 3: gg = x, bb = c; break;
    case 4: rr = x, bb = c; break;
    default: rr = c, bb = x; break;
    }
    return {rr + m, gg + m, bb + m, a};
}

LinearColor LinearColor::desaturate(float desaturation) const
{
    const float lum = getLuminance();
    return lerp(*this, LinearColor(lum, lum, lum, 0.0f), desaturation).copyWithNewOpacity(a);
}

LinearColor LinearColor::getClamped(float minValue, float maxValue) const
{
    return {clamp(r, minValue, maxValue), clamp(g, minValue, maxValue), clamp(b, minValue, maxValue),
            clamp(a, minValue, maxValue)};
}

std::optional<Color> Color::fromHex(std::string_view hex)
{
    if (!hex.empty() && hex.front() == '#')
        hex.remove_prefix(1);
    int digits[8];
    for (size_t i = 0; i < hex.size() && i < 8; ++i)
        if ((digits[i] = hexDigit(hex[i])) < 0)
            return std::nullopt;

    auto byte = [&](int hi, int lo) { return uint8_t(digits[hi] * 16 + digits[lo]); };
    switch (hex.size())
    {
    case 3: return Color(byte(0, 0), byte(1, 1), byte(2, 2));
    case 4: return Color(byte(0, 0), byte(1, 1), byte(2, 2), byte(3, 3));
    case 6: return Color(byte(0, 1), byte(2, 3), byte(4, 5));
    case 8: return Color(byte(0, 1), byte(2, 3), byte(4, 5), byte(6, 7));
    default: return std::nullopt;
    }
}

std::string Color::toHex() const
{
    char buf[9];
    std::snprintf(buf, sizeof(buf), "%02X%02X%02X%02X", r, g, b, a);
    return buf;
}
} // namespace lila::math
