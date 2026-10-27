#include "ui/ViewportDisplay.h"

#include "core/PrefStore.h"
#include "render/DisplayOptions.h"
#include "render/Renderer.h"
#include "scene/Relocate.h"
#include "ui/Theme.h"

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
    s.kuwahara = p.value<bool>("general", kKuwahara, false);
    s.kuwaharaRadius = p.value<float>("general", kKuwaharaRadius, 4.0f);
    s.outline = p.value<bool>("general", kOutline, false);
    s.outlineWidth = p.value<float>("general", kOutlineWidth, 1.0f);
    s.gridColor = glm::vec3(themeLinear("grid", glm::vec4(s.gridColor, 1.0f)));
    s.gridAxisX = glm::vec3(themeLinear("gridAxisX", glm::vec4(s.gridAxisX, 1.0f)));
    s.gridAxisZ = glm::vec3(themeLinear("gridAxisZ", glm::vec4(s.gridAxisZ, 1.0f)));
    s.gridOrigin = scene::relocate::displayOrigin();
    s.wireSelected = themeLinear("wireSelected", s.wireSelected);
    s.wirePreselect = themeLinear("wirePreselect", s.wirePreselect);
    s.wireUnderSelected = themeLinear("wireUnderSelected", s.wireUnderSelected);
    s.wireDefault = themeLinear("wireframe", s.wireDefault);
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
            get(kVertexThickness, d.vertex), get(kVertexThicknessHover, d.vertexHover), get(kVertexThicknessSelected, d.vertexSelected),
            get(kUvSeamThickness, d.uvSeam), get(kSymmetryEdgeThickness, d.symmetryEdge)};
}

bool accentColors(const char* key)
{
    const PrefStore& p = PrefStore::get();
    return accentColors(p.value<bool>("general", kMonotone, false), p.value<bool>("general", key, true));
}

glm::vec3 axisColor(int axis)
{
    static constexpr glm::vec3 kAxes[3] = {{0.85f, 0.40f, 0.40f}, {0.50f, 0.80f, 0.42f}, {0.42f, 0.56f, 0.90f}};
    static constexpr const char* kTokens[3] = {"axisX", "axisY", "axisZ"};
    int i = std::clamp(axis, 0, 2);
    return glm::vec3(themeSrgb(kTokens[i], glm::vec4(kAxes[i], 1.0f)));
}

glm::vec3 keyTickColor()
{
    return glm::vec3(themeSrgb("keyTick", glm::vec4(0.86f, 0.36f, 0.36f, 1.0f)));
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

void applyQuality(Quality q)
{
    PrefStore& p = PrefStore::get();
    p.set("general", "/viewport/ao/enabled", q != Quality::Fastest);
    p.set("general", "/viewport/ao/mode", q == Quality::Best ? "accurate" : "fast");
    p.set("general", kAaEnabled, q != Quality::Fastest);
    p.set("general", kAaSamples, q == Quality::Best ? 8 : 4);
    p.set("general", kShadows, q != Quality::Fastest);
}

Quality recommendedQuality()
{
    const render::Renderer::GpuInfo gpu = render::Renderer::get().gpuInfo();
    constexpr uint64_t kGiB = 1ull << 30;
    if (gpu.name.empty())
        return Quality::Best;
    if (!gpu.discrete || gpu.vramBytes < 4 * kGiB)
        return Quality::Fastest;
    return gpu.vramBytes < 8 * kGiB ? Quality::Balanced : Quality::Best;
}
} // namespace lila::ui::viewportDisplay
