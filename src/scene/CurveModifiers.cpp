#include "scene/CurveModifiers.h"

#include "scene/AnimCurve.h"
#include "scene/RotationCurve.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace lila::scene::anim
{
namespace
{
constexpr const char* kTypeNames[] = {"noise", "stepped", "limits"};

uint32_t hash(int32_t i, int32_t seed)
{
    uint32_t h = uint32_t(i) * 0x9E3779B1u ^ uint32_t(seed) * 0x85EBCA77u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}

/** Gradient noise in about [-1, 1], one wave per unit of x. */
double gradientNoise(double x, int seed)
{
    const double fl = std::floor(x);
    const auto i = int32_t(fl);
    const double f = x - fl;
    auto grad = [&](int32_t at) { return double(hash(at, seed) & 0xFFFFu) / 32767.5 - 1.0; };
    const double a = grad(i) * f, b = grad(i + 1) * (f - 1.0);
    const double s = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    return 2.0 * (a + (b - a) * s);
}
} // namespace

const char* modifierTypeName(ModifierType t)
{
    return kTypeNames[size_t(t)];
}

ModifierType modifierTypeFromName(std::string_view name)
{
    for (size_t i = 0; i < std::size(kTypeNames); ++i)
        if (name == kTypeNames[i])
            return ModifierType(i);
    throw std::invalid_argument("unknown curve modifier '" + std::string(name) + "' (noise, stepped, limits)");
}

Modifier modifierFromJson(const json& j)
{
    if (!j.is_object() || !j.contains("type") || !j["type"].is_string())
        throw std::invalid_argument("curve modifier needs a type: " + j.dump());
    Modifier m;
    m.type = modifierTypeFromName(j["type"].get<std::string>());
    m.amplitude = j.value("amplitude", m.amplitude);
    m.period = std::max(1e-4, j.value("period", m.period));
    m.phase = j.value("phase", m.phase);
    m.seed = j.value("seed", m.seed);
    m.octaves = std::clamp(j.value("octaves", m.octaves), 1, 8);
    m.step = std::max(1e-4, j.value("step", m.step));
    m.offset = j.value("offset", m.offset);
    if (j.contains("min") && j["min"].is_number())
        m.min = j["min"].get<double>();
    if (j.contains("max") && j["max"].is_number())
        m.max = j["max"].get<double>();
    return m;
}

json modifierToJson(const Modifier& m)
{
    json j = {{"type", modifierTypeName(m.type)}};
    switch (m.type)
    {
    case ModifierType::Noise:
        j["amplitude"] = m.amplitude;
        j["period"] = m.period;
        j["phase"] = m.phase;
        j["seed"] = m.seed;
        j["octaves"] = m.octaves;
        break;
    case ModifierType::Stepped:
        j["step"] = m.step;
        j["offset"] = m.offset;
        break;
    case ModifierType::Limits:
        if (m.min)
            j["min"] = *m.min;
        if (m.max)
            j["max"] = *m.max;
        break;
    }
    return j;
}

std::vector<Modifier> modifiersFromText(const std::string& text)
{
    std::vector<Modifier> out;
    if (text.empty())
        return out;
    try
    {
        json j = json::parse(text);
        if (j.is_array())
            for (const json& e : j)
                out.push_back(modifierFromJson(e));
    }
    catch (const std::exception&)
    {
        out.clear();
    }
    return out;
}

std::string modifiersToText(const std::vector<Modifier>& mods)
{
    if (mods.empty())
        return {};
    json j = json::array();
    for (const Modifier& m : mods)
        j.push_back(modifierToJson(m));
    return j.dump();
}

std::vector<Modifier> modifiersOf(const Node& curve)
{
    if (isRotationCurve(curve))
        return modifiersFromText(curve.as<std::string>(RotationCurveNode::kModifiers));
    if (isCurve(curve))
        return modifiersFromText(curve.as<std::string>(CurveNode::kModifiers));
    return {};
}

double modifiedTime(const std::vector<Modifier>& mods, double t)
{
    for (const Modifier& m : mods)
        if (m.type == ModifierType::Stepped)
            t = m.offset + std::floor((t - m.offset) / m.step + 1e-9) * m.step;
    return t;
}

double modifiedValue(const std::vector<Modifier>& mods, double t, double v, int channel)
{
    for (const Modifier& m : mods)
    {
        if (m.type == ModifierType::Noise)
        {
            double sum = 0.0, amp = 1.0, norm = 0.0, x = (t + m.phase) / m.period;
            for (int o = 0; o < m.octaves; ++o)
            {
                sum += amp * gradientNoise(x, m.seed * 131 + channel * 7919 + o * 104729);
                norm += amp;
                amp *= 0.5;
                x *= 2.0;
            }
            v += m.amplitude * sum / norm;
        }
        else if (m.type == ModifierType::Limits)
        {
            if (m.min)
                v = std::max(v, *m.min);
            if (m.max)
                v = std::min(v, *m.max);
        }
    }
    return v;
}

double evaluateModified(const Curve& c, const std::vector<Modifier>& mods, double t)
{
    return modifiedValue(mods, t, c.evaluate(modifiedTime(mods, t)));
}

glm::dvec3 evaluateModified(const RotationCurve& c, const std::vector<Modifier>& mods, double t)
{
    glm::dvec3 e = c.evaluate(modifiedTime(mods, t));
    for (int a = 0; a < 3; ++a)
        e[a] = modifiedValue(mods, t, e[a], a);
    return e;
}
} // namespace lila::scene::anim
