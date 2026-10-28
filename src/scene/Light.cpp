#include "scene/Light.h"

#include "core/ExtensionContext.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/RigUtil.h"

#include <algorithm>
#include <array>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

namespace lila::scene
{
namespace
{
constexpr uint32_t kStored = AttrInput | AttrStorable;

constexpr std::array<const char*, 6> kKindNames{"point", "spot", "directional", "area", "disc", "dome"};
constexpr std::array<const char*, 6> kUnitNames{"unitless", "candela", "lumens", "lux", "nits", "watts"};

AttrDef floatAttr(std::string name, std::string shortName, double def, std::string doc, std::optional<double> lo = {},
                  std::optional<double> hi = {})
{
    return {.name = std::move(name), .shortName = std::move(shortName), .type = PlugType::Float, .defaultValue = def,
            .flags = kStored | AttrKeyable, .doc = std::move(doc), .minValue = lo, .maxValue = hi};
}

AttrDef stringAttr(std::string name, std::string shortName, std::string doc, uint32_t extraFlags = 0)
{
    return {.name = std::move(name), .shortName = std::move(shortName), .type = PlugType::String, .defaultValue = std::string(),
            .flags = kStored | extraFlags, .doc = std::move(doc)};
}

std::vector<AttrDef> lightAttrs()
{
    std::vector<AttrDef> a(Light::kCount - Light::kLightType);
    auto at = [&](int attr) -> AttrDef& { return a[size_t(attr - Light::kLightType)]; };
    at(Light::kLightType) = {.name = "lightType", .shortName = "lt", .type = PlugType::Enum, .defaultValue = int64_t(0), .flags = kStored,
                             .enumNames = {kKindNames.begin(), kKindNames.end()},
                             .doc = "Point, spot, directional, area (a rectangle), disc or dome."};
    AttrDef color{.name = "color", .shortName = "cl", .type = PlugType::Vec3, .defaultValue = glm::dvec3(1.0),
                  .flags = kStored | AttrKeyable, .doc = "Linear colour.", .minValue = 0.0};
    for (int i = 0; i < 3; ++i)
    {
        color.childNames[i] = std::string("color") + "RGB"[i];
        color.childShort[i] = std::string("cl") + "rgb"[i];
    }
    at(Light::kColor) = color;
    at(Light::kIntensity) = floatAttr("intensity", "in", 1.0, "Brightness, in intensityUnits.", 0.0);
    at(Light::kIntensityUnits) = {.name = "intensityUnits", .shortName = "iu", .type = PlugType::Enum, .defaultValue = int64_t(0),
                                  .flags = kStored, .enumNames = {kUnitNames.begin(), kUnitNames.end()},
                                  .doc = "What intensity is measured in, kept so formats with real units convert exactly."};
    at(Light::kUseTemperature) = {.name = "useTemperature", .shortName = "ut", .type = PlugType::Bool, .defaultValue = false,
                                  .flags = kStored, .doc = "Tint by temperature as well as colour."};
    at(Light::kTemperature) = floatAttr("temperature", "tmp", 6500.0, "Colour temperature, kelvin.", 1000.0, 40000.0);
    at(Light::kRange) = floatAttr("range", "rng", 0.0, "Distance the light stops reaching, cm. 0 has no cutoff.", 0.0);
    at(Light::kInnerConeAngle) = floatAttr("innerConeAngle", "ica", 0.0, "Spot: half angle of full brightness, degrees.", 0.0, 90.0);
    at(Light::kOuterConeAngle) = floatAttr("outerConeAngle", "oca", 45.0, "Spot: half angle where the light ends, degrees.", 0.0, 90.0);
    at(Light::kWidth) = floatAttr("width", "w", 100.0, "Area: width, cm. Disc: diameter.", 0.0);
    at(Light::kHeight) = floatAttr("height", "h", 100.0, "Area: height, cm.", 0.0);
    at(Light::kSourceRadius) = floatAttr("sourceRadius", "sr", 0.0, "Point and spot: emitter radius for soft shadows, cm.", 0.0);
    at(Light::kCastShadows) = {.name = "castShadows", .shortName = "cs", .type = PlugType::Bool, .defaultValue = true, .flags = kStored,
                               .doc = "Casts shadows."};
    at(Light::kTexture) = stringAttr("texture", "tex", "Dome: environment image. Point and spot: IES profile.");
    at(Light::kProvider) = stringAttr("provider", "prv", "Light provider asked first for this light; empty lets any handle it.");
    at(Light::kSourceFormat) = stringAttr("sourceFormat", "sfm", "What the light was read as, empty when made in Lila.", AttrHidden);
    at(Light::kExtra) = stringAttr("extra", "ext", "JSON of the source format's settings beyond these, written back on export.", AttrHidden);
    return a;
}

bool handles(const lights::Provider& p, const Node& light) { return !p.handles || p.handles(light); }
} // namespace

namespace lights
{
const char* kindName(Kind kind)
{
    return kKindNames[std::clamp(size_t(kind), size_t(0), kKindNames.size() - 1)];
}

Settings settings(const Node& n)
{
    Settings s;
    s.kind = Kind(std::clamp<int64_t>(n.as<int64_t>(Light::kLightType), 0, int64_t(kKindNames.size()) - 1));
    s.color = n.as<glm::dvec3>(Light::kColor);
    s.intensity = n.as<double>(Light::kIntensity);
    s.units = Units(std::clamp<int64_t>(n.as<int64_t>(Light::kIntensityUnits), 0, int64_t(kUnitNames.size()) - 1));
    s.useTemperature = n.as<bool>(Light::kUseTemperature);
    s.temperature = n.as<double>(Light::kTemperature);
    s.range = n.as<double>(Light::kRange);
    s.innerConeAngle = n.as<double>(Light::kInnerConeAngle);
    s.outerConeAngle = n.as<double>(Light::kOuterConeAngle);
    s.width = n.as<double>(Light::kWidth);
    s.height = n.as<double>(Light::kHeight);
    s.sourceRadius = n.as<double>(Light::kSourceRadius);
    s.castShadows = n.as<bool>(Light::kCastShadows);
    s.texture = n.as<std::string>(Light::kTexture);
    s.provider = n.as<std::string>(Light::kProvider);
    s.sourceFormat = n.as<std::string>(Light::kSourceFormat);
    s.extra = n.as<std::string>(Light::kExtra);
    return s;
}

void setSettings(Graph& g, NodeId id, const Settings& s)
{
    auto set = [&](int attr, PlugValue v) { g.setValue({id, attr, -1}, std::move(v)); };
    set(Light::kLightType, int64_t(s.kind));
    set(Light::kColor, s.color);
    set(Light::kIntensity, s.intensity);
    set(Light::kIntensityUnits, int64_t(s.units));
    set(Light::kUseTemperature, s.useTemperature);
    set(Light::kTemperature, s.temperature);
    set(Light::kRange, s.range);
    set(Light::kInnerConeAngle, s.innerConeAngle);
    set(Light::kOuterConeAngle, s.outerConeAngle);
    set(Light::kWidth, s.width);
    set(Light::kHeight, s.height);
    set(Light::kSourceRadius, s.sourceRadius);
    set(Light::kCastShadows, s.castShadows);
    set(Light::kTexture, s.texture);
    set(Light::kProvider, s.provider);
    set(Light::kSourceFormat, s.sourceFormat);
    set(Light::kExtra, s.extra);
}

Node& create(Graph& g, const std::string& name, NodeId parent, const Settings& s)
{
    Node* n = g.createNode(*types::find("light"), name.empty() ? std::string(kindName(s.kind)) + "Light" : name, parent);
    setSettings(g, n->id(), s);
    return *n;
}

glm::dvec3 effectiveColor(const Settings& s)
{
    return s.useTemperature ? s.color * kelvinToLinear(s.temperature) : s.color;
}

glm::dvec3 kelvinToLinear(double kelvin)
{
    // Tanner Helland's fit of the Planckian locus, in sRGB, then linearised.
    const double t = std::clamp(kelvin, 1000.0, 40000.0) / 100.0;
    double r = t <= 66.0 ? 255.0 : 329.698727446 * std::pow(t - 60.0, -0.1332047592);
    double gr = t <= 66.0 ? 99.4708025861 * std::log(t) - 161.1195681661 : 288.1221695283 * std::pow(t - 60.0, -0.0755148492);
    double b = t >= 66.0 ? 255.0 : t <= 19.0 ? 0.0 : 138.5177312231 * std::log(t - 10.0) - 305.0447927307;
    auto lin = [](double c) {
        c = std::clamp(c / 255.0, 0.0, 1.0);
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    glm::dvec3 out{lin(r), lin(gr), lin(b)};
    return out / std::max({out.x, out.y, out.z, 1e-6});
}

Registry<Provider>& providers()
{
    static Registry<Provider> r;
    return r;
}

void registerProvider(ext::Context& ctx, Provider provider)
{
    std::string id = provider.id;
    ctx.add(providers(), "lightProvider", std::move(id), std::move(provider));
}

const Provider* providerFor(const Node& light)
{
    if (!light.type().isA("light"))
        return nullptr;
    const std::string& preferred = light.as<std::string>(Light::kProvider);
    if (const Provider* p = preferred.empty() ? nullptr : providers().find(preferred); p && handles(*p, light))
        return p;
    const Provider* fallback = nullptr;
    for (const Provider* p : providers().allInOrder())
    {
        if (!handles(*p, light))
            continue;
        if (!p->placeholder)
            return p;
        if (!fallback)
            fallback = p;
    }
    return fallback;
}

bool isPlaceholder(const Node& light)
{
    if (!light.type().isA("light"))
        return false;
    const Provider* p = providerFor(light);
    return !p || p->placeholder;
}
} // namespace lights

void registerLightTypes()
{
    if (types::find("light"))
        return;
    NodeType t;
    t.name = "light";
    t.icon = "bulb";
    t.base = types::find("locator");
    t.dag = true;
    t.doc = "A light: kind, colour, intensity, falloff, cone and area. Lit and drawn by a light provider extension; a light "
            "only a placeholder provider handles is flagged in the outliner.";
    t.ownAttrs = lightAttrs();
    t.ownAttrs.insert(t.ownAttrs.begin(), Locator::hiddenDrawSize());
    t.create = [] { return std::make_unique<Light>(); };
    types::registerType(std::move(t));
}

void registerLightCommands()
{
    using T = ArgType;
    if (ops::find("light"))
        return;
    ops::registerCommand({
        .name = "light",
        .doc = "Create a light. Returns its path. Flags left out keep the light's defaults.",
        .flags = {
            {"type", "t", T::String, "point", "point, spot, directional, area, disc or dome."},
            {"name", "n", T::String, nullptr, "Name; made unique among its siblings. Defaults to <type>Light."},
            {"parent", "p", T::String, nullptr, "Parent transform."},
            {"position", "pos", T::Vec3, nullptr, "World position, cm."},
            {"color", "c", T::Vec3, nullptr, "Linear colour."},
            {"intensity", "i", T::Float, nullptr, "Brightness, in units.", {}, {}, 0.0},
            {"units", "u", T::String, nullptr, "unitless, candela, lumens, lux, nits or watts."},
            {"temperature", "tmp", T::Float, nullptr, "Colour temperature in kelvin; turns useTemperature on.", {}, {}, 1000.0, 40000.0},
            {"range", "r", T::Float, nullptr, "Cutoff distance, cm. 0 has none.", {}, {}, 0.0},
            {"innerConeAngle", "ica", T::Float, nullptr, "Spot: half angle of full brightness, degrees.", {}, {}, 0.0, 90.0},
            {"outerConeAngle", "oca", T::Float, nullptr, "Spot: half angle where the light ends, degrees.", {}, {}, 0.0, 90.0},
            {"width", "w", T::Float, nullptr, "Area width or disc diameter, cm.", {}, {}, 0.0},
            {"height", "h", T::Float, nullptr, "Area height, cm.", {}, {}, 0.0},
            {"castShadows", "cs", T::Bool, nullptr, "Casts shadows."},
            {"provider", "prv", T::String, nullptr, "Light provider asked first for this light."},
            {"skipSelect", "ss", T::Bool, false, "Don't select the new light."},
        },
        .run = [](const Args& a) -> json {
            auto index = [](const auto& names, const std::string& v, const char* what) {
                auto it = std::find(names.begin(), names.end(), v);
                if (it == names.end())
                    throw CommandError(std::string("light: unknown ") + what + " '" + v + "'");
                return int(it - names.begin());
            };
            auto vec = [&](const char* flag) {
                const json& j = a.flags[flag];
                return glm::dvec3(j.at(0).get<double>(), j.at(1).get<double>(), j.at(2).get<double>());
            };
            lights::Settings s;
            s.kind = lights::Kind(index(kKindNames, a.get<std::string>("type", "point"), "type"));
            if (a.has("color"))
                s.color = vec("color");
            if (a.has("intensity"))
                s.intensity = a.get<double>("intensity");
            if (a.has("units"))
                s.units = lights::Units(index(kUnitNames, a.get<std::string>("units"), "units"));
            if (a.has("temperature"))
            {
                s.useTemperature = true;
                s.temperature = a.get<double>("temperature");
            }
            if (a.has("range"))
                s.range = a.get<double>("range");
            if (a.has("innerConeAngle"))
                s.innerConeAngle = a.get<double>("innerConeAngle");
            if (a.has("outerConeAngle"))
                s.outerConeAngle = a.get<double>("outerConeAngle");
            if (a.has("width"))
                s.width = a.get<double>("width");
            if (a.has("height"))
                s.height = a.get<double>("height");
            if (a.has("castShadows"))
                s.castShadows = a.get<bool>("castShadows");
            if (a.has("provider"))
                s.provider = a.get<std::string>("provider");

            Graph& g = Graph::current();
            NodeId parent = kNoNode;
            if (a.has("parent"))
            {
                const Node* p = g.find(a.get<std::string>("parent"));
                if (!p)
                    throw CommandError("light: no parent '" + a.get<std::string>("parent") + "'");
                parent = p->id();
            }
            Node* n = rigutil::guarded([&] { return &lights::create(g, a.get<std::string>("name", ""), parent, s); });
            if (a.has("position"))
            {
                g.evaluate();
                glm::dmat4 local = glm::inverse(n->as<glm::dmat4>(Transform::kParentMatrix)) * glm::translate(glm::dmat4(1.0), vec("position"));
                g.setValue({n->id(), Transform::kTranslate, -1}, glm::dvec3(local[3]));
            }
            if (!a.get<bool>("skipSelect", false))
                g.select({n->id()});
            return n->path();
        },
    });
}
} // namespace lila::scene
