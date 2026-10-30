#include "scene/PostProcess.h"

#include "ops/Command.h"
#include "scene/Graph.h"

#include <algorithm>
#include <cmath>

namespace lila::scene::postProcess
{
namespace
{
constexpr uint32_t kStored = AttrInput | AttrStorable;

AttrDef number(std::string name, std::string shortName, double def, std::string doc, std::optional<double> lo = {},
               std::optional<double> hi = {})
{
    return {.name = std::move(name), .shortName = std::move(shortName), .type = PlugType::Float, .defaultValue = def,
            .flags = kStored | AttrKeyable, .doc = std::move(doc), .minValue = lo, .maxValue = hi};
}

AttrDef color(std::string name, std::string shortName, double def, std::string doc)
{
    AttrDef a{.name = name, .shortName = shortName, .type = PlugType::Vec3, .defaultValue = glm::dvec3(def),
              .flags = kStored | AttrKeyable | AttrColor, .doc = std::move(doc), .minValue = 0.0};
    for (int i = 0; i < 3; ++i)
    {
        a.childNames[i] = name + "RGB"[i];
        a.childShort[i] = shortName + "rgb"[i];
    }
    return a;
}

std::vector<AttrDef> attrs()
{
    std::vector<AttrDef> a(kCount);
    a[kEnabled] = {.name = "enabled", .shortName = "en", .type = PlugType::Bool, .defaultValue = true, .flags = kStored,
                   .doc = "Use this post process. The first enabled one in the scene applies."};
    a[kTonemapper] = {.name = "tonemapper", .shortName = "tm", .type = PlugType::Enum, .defaultValue = int64_t(0), .flags = kStored,
                      .enumNames = {"neutral", "agx", "aces", "linear"},
                      .doc = "Neutral keeps base colours true (the viewport's own); AgX rolls bright colours off to white like film; "
                             "ACES matches Unreal's filmic look; linear clips."};
    a[kAgxLook] = {.name = "agxLook", .shortName = "agl", .type = PlugType::Enum, .defaultValue = int64_t(0), .flags = kStored,
                   .enumNames = {"none", "punchy", "golden"}, .doc = "AgX only: a look on top, more contrast and saturation, or warm."};
    a[kExposure] = number("exposure", "ex", 0.0, "Stops on top of the viewport's exposure.", -16.0, 16.0);
    a[kAutoExposure] = {.name = "autoExposure", .shortName = "ae", .type = PlugType::Bool, .defaultValue = false, .flags = kStored,
                        .doc = "Meter the picture each frame and expose its average to middle grey, easing toward it like an eye."};
    a[kExposureCompensation] = number("exposureCompensation", "ec", 0.0, "Auto exposure: stops brighter or darker than metered.", -10.0, 10.0);
    a[kExposureMin] = number("exposureMin", "emn", -6.0, "Auto exposure: the most it may darken, stops.", -16.0, 16.0);
    a[kExposureMax] = number("exposureMax", "emx", 6.0, "Auto exposure: the most it may brighten, stops.", -16.0, 16.0);
    a[kExposureSpeed] = number("exposureSpeed", "es", 3.0, "Auto exposure: how fast it adapts, per second; 0 jumps at once.", 0.0, 100.0);
    a[kTemperature] = number("temperature", "wt", 6500.0, "White balance: the light colour that shows as white, kelvin.", 1500.0, 15000.0);
    a[kTint] = number("tint", "wtn", 0.0, "White balance: green (negative) against magenta (positive).", -1.0, 1.0);
    a[kContrast] = number("contrast", "ct", 1.0, "Around middle grey; 1 leaves it.", 0.0, 4.0);
    a[kSaturation] = number("saturation", "sat", 1.0, "0 is grey, 1 leaves it.", 0.0, 4.0);
    a[kLift] = color("lift", "lf", 0.0, "Raises the shadows of the graded picture, per channel.");
    a[kLift].minValue = -1.0;
    a[kGamma] = color("gamma", "gm", 1.0, "Brightens (above 1) or darkens the mid-tones, per channel.");
    a[kGain] = color("gain", "gn", 1.0, "Scales the highlights, per channel.");
    a[kBloom] = number("bloom", "bl", 0.0, "Glow around bright light, 0 to 1. A few hundredths is subtle.", 0.0, 1.0);
    a[kBloomRadius] = number("bloomRadius", "blr", 1.0, "Spread of the glow.", 0.1, 4.0);
    a[kVignette] = number("vignette", "vg", 0.0, "Darkens toward the corners, 0 to 1.", 0.0, 1.0);
    a[kGrain] = number("grain", "gr", 0.0, "Film grain, 0 to 1.", 0.0, 1.0);
    a[kChromaticAberration] = number("chromaticAberration", "ca", 0.0, "Colour fringes toward the corners, pixels.", 0.0, 40.0);
    a[kDistortion] = number("distortion", "ds", 0.0, "Lens distortion: negative bulges (barrel), positive pinches.", -1.0, 1.0);
    a[kSharpen] = number("sharpen", "sh", 0.0, "Crisper detail, 0 to 2.", 0.0, 2.0);
    a[kLut] = {.name = "lut", .shortName = "lut", .type = PlugType::String, .defaultValue = std::string(), .flags = kStored | AttrFile,
               .doc = "A colour lookup table (.cube) applied to the finished picture."};
    a[kLutIntensity] = number("lutIntensity", "li", 1.0, "How much of the LUT, 0 to 1.", 0.0, 1.0);
    a[kFog] = {.name = "fog", .shortName = "fg", .type = PlugType::Bool, .defaultValue = false, .flags = kStored,
               .doc = "Height fog: thicker low down, lit by the sun (the first directional light)."};
    a[kFogColor] = color("fogColor", "fgc", 0.0, "Fog: linear colour of its own glow, scaled by the environment intensity.");
    a[kFogColor].defaultValue = glm::dvec3(0.6, 0.7, 0.8);
    a[kFogDensity] = number("fogDensity", "fgd", 0.02, "Fog: thickness at its height, per metre.", 0.0, 10.0);
    a[kFogHeight] = number("fogHeight", "fgh", 0.0, "Fog: height of the given density, cm; it thins above.");
    a[kFogFalloff] = number("fogFalloff", "fgf", 0.2, "Fog: how fast it thins going up, per metre.", 0.0, 100.0);
    a[kFogStart] = number("fogStart", "fgs", 0.0, "Fog: none nearer the camera than this, cm.", 0.0);
    a[kFogSunScatter] = number("fogSunScatter", "fgss", 0.5, "Fog: how much the sun lights it.", 0.0, 10.0);
    a[kFogAnisotropy] = number("fogAnisotropy", "fga", 0.6, "Fog: how much brighter it glows toward the sun, 0 to 0.95.", 0.0, 0.95);
    return a;
}

/** Linear sRGB of a blackbody at `kelvin`, scaled to a peak of 1 (Tanner Helland's fit). */
glm::dvec3 blackbody(double kelvin)
{
    const double t = std::clamp(kelvin, 1000.0, 40000.0) / 100.0;
    double r = t <= 66.0 ? 255.0 : 329.698727446 * std::pow(t - 60.0, -0.1332047592);
    double g = t <= 66.0 ? 99.4708025861 * std::log(t) - 161.1195681661 : 288.1221695283 * std::pow(t - 60.0, -0.0755148492);
    double b = t >= 66.0 ? 255.0 : t <= 19.0 ? 0.0 : 138.5177312231 * std::log(t - 10.0) - 305.0447927307;
    auto lin = [](double c) {
        c = std::clamp(c / 255.0, 0.0, 1.0);
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    glm::dvec3 rgb(lin(r), lin(g), lin(b));
    return rgb / std::max({rgb.r, rgb.g, rgb.b, 1e-6});
}
} // namespace

int firstAttr()
{
    const NodeType* t = types::find("postProcess");
    return t ? int(t->attrs.size()) - kCount : 0;
}

NodeId active(const Graph& g)
{
    const int first = firstAttr();
    for (NodeId id : g.nodes())
        if (const Node* n = g.node(id); n && n->type().isA("postProcess") && n->as<bool>(first + kEnabled))
            return id;
    return kNoNode;
}

glm::dvec3 whiteBalance(double kelvin, double tint)
{
    // Gains that map the chosen light to the white of 6500 K, keeping luminance.
    const glm::dvec3 light = blackbody(kelvin), white = blackbody(6500.0);
    glm::dvec3 gains = white / glm::max(light, glm::dvec3(1e-6));
    gains.g *= 1.0 - 0.25 * std::clamp(tint, -1.0, 1.0);
    const double luma = glm::dot(gains * light, glm::dvec3(0.2126, 0.7152, 0.0722)) / glm::dot(white, glm::dvec3(0.2126, 0.7152, 0.0722));
    return gains / std::max(luma, 1e-6);
}

void registerTypes()
{
    if (types::find("postProcess"))
        return;
    NodeType t;
    t.name = "postProcess";
    t.icon = "adjustments-horizontal";
    t.doc = "How the scene's light becomes the picture: tonemapper, exposure, white balance, grading, bloom, lens effects and a LUT. "
            "The viewport and renders use the first enabled one.";
    t.ownAttrs = attrs();
    t.create = [] { return std::make_unique<Node>(); };
    types::registerType(std::move(t));
}

void registerCommands()
{
    using T = ArgType;
    if (ops::find("postProcess"))
        return;
    ops::registerCommand({
        .name = "postProcess",
        .doc = "Create a post process node, or with no new one needed, return the active one. Returns its name.",
        .flags = {
            {"name", "n", T::String, nullptr, "Name; made unique."},
            {"existing", "e", T::Bool, false, "Return the active post process if there is one instead of creating another."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (a.get<bool>("existing", false))
                if (NodeId id = active(g); id != kNoNode)
                    return g.node(id)->name();
            return ops::execute("createNode", {{"args", {"postProcess"}}, {"name", a.get<std::string>("name", "postProcess")}});
        },
    });
}
} // namespace lila::scene::postProcess
