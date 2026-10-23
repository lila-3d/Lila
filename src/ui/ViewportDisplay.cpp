#include "ui/ViewportDisplay.h"

#include "core/PrefStore.h"
#include "render/DisplayOptions.h"

#include <algorithm>
#include <cmath>

namespace lila::ui::viewportDisplay
{
void apply(render::FrameState& s)
{
    const PrefStore& p = PrefStore::get();
    s.msaaSamples = render::msaaSamples(p.value<bool>("general", kAaEnabled, true), p.value<int>("general", kAaSamples, kDefaultSamples), 0xFFu);
    s.grid = p.value<bool>("general", kGrid, true);
    s.backfaceCulling = p.value<bool>("general", kBackfaceCulling, false);
    s.xray = p.value<bool>("general", kXray, false);
    s.lighting = render::lightingFromName(p.value<std::string>("general", kLighting, "environment"));
    if (!p.value<bool>("general", kShadows, true))
        s.groundShadow = 0.0f;
    s.exposure = render::exposureScale(p.value<double>("general", kExposure, 0.0));
    s.gamma = p.value<float>("general", kGamma, 1.0f);
}

bool defaultMaterial()
{
    return PrefStore::get().value<bool>("general", kDefaultMaterial, false);
}

ComponentThickness componentThickness()
{
    const PrefStore& p = PrefStore::get();
    const ComponentThickness d;
    auto get = [&](const char* key, double fallback) { return std::max(0.0, p.value<double>("general", key, fallback)); };
    return {get(kEdgeThickness, d.edge),     get(kEdgeThicknessHover, d.edgeHover),   get(kEdgeThicknessSelected, d.edgeSelected),
            get(kVertexThickness, d.vertex), get(kVertexThicknessHover, d.vertexHover), get(kVertexThicknessSelected, d.vertexSelected)};
}

bool accentColors(const char* key)
{
    const PrefStore& p = PrefStore::get();
    return accentColors(p.value<bool>("general", kMonotone, false), p.value<bool>("general", key, true));
}

glm::vec3 axisColor(int axis)
{
    static constexpr glm::vec3 kAxes[3] = {{0.85f, 0.40f, 0.40f}, {0.50f, 0.80f, 0.42f}, {0.42f, 0.56f, 0.90f}};
    return kAxes[std::clamp(axis, 0, 2)];
}

render::VertexColorMode vertexColorMode()
{
    std::string m = PrefStore::get().value<std::string>("general", kVertexColors, "off");
    return m == "shaded" ? render::VertexColorMode::Shaded : m == "unlit" ? render::VertexColorMode::Unlit : render::VertexColorMode::Off;
}

render::ColorChannel colorChannel()
{
    std::string c = PrefStore::get().value<std::string>("general", kColorChannel, "rgb");
    return c == "r" ? render::ColorChannel::R
           : c == "g" ? render::ColorChannel::G
           : c == "b" ? render::ColorChannel::B
           : c == "a" ? render::ColorChannel::A
                      : render::ColorChannel::Rgb;
}

namespace
{
float toLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

/** Piecewise-linear ramp through sRGB stops, returned linear. */
glm::vec4 ramp(std::initializer_list<glm::vec3> stops, float w)
{
    const glm::vec3* s = stops.begin();
    float x = std::clamp(w, 0.0f, 1.0f) * float(stops.size() - 1);
    size_t i = std::min(size_t(x), stops.size() - 2);
    glm::vec3 c = glm::mix(s[i], s[i + 1], x - float(i));
    return {toLinear(c.r), toLinear(c.g), toLinear(c.b), 1.0f};
}
} // namespace

bool grayscaleWeights()
{
    return PrefStore::get().value<bool>("general", kGrayscaleWeights, false);
}

glm::vec4 weightColor(float w, bool grayscale)
{
    if (grayscale)
    {
        float v = toLinear(std::clamp(w, 0.0f, 1.0f));
        return {v, v, v, 1.0f};
    }
    return ramp({{0.0f, 0.0f, 0.0f}, {0.1f, 0.15f, 0.85f}, {0.0f, 0.75f, 0.9f}, {0.1f, 0.85f, 0.2f}, {0.95f, 0.9f, 0.1f}, {0.95f, 0.2f, 0.1f},
                 {1.0f, 1.0f, 1.0f}},
                w);
}

glm::vec4 falloffColor(float w, bool grayscale)
{
    if (grayscale)
    {
        float v = toLinear(std::clamp(w, 0.0f, 1.0f));
        return {v, v, v, 1.0f};
    }
    return ramp({{0.25f, 0.0f, 0.0f}, {0.8f, 0.1f, 0.0f}, {1.0f, 0.5f, 0.0f}, {1.0f, 1.0f, 0.0f}}, w);
}
} // namespace lila::ui::viewportDisplay
