#include "curve/StripUvAttrs.h"

#include "scene/Node.h"
#include "scene/Plug.h"

#include <algorithm>
#include <cctype>

namespace lila::curve
{
using namespace scene;

namespace
{
constexpr uint32_t kUvKey = AttrInput | AttrStorable | AttrKeyable;

AttrDef uvRangeAttr(std::string name, double def, std::string doc, const std::string& category)
{
    AttrDef a{.name = std::move(name), .type = PlugType::Float, .defaultValue = def, .flags = kUvKey, .doc = std::move(doc)};
    a.category = category;
    return a;
}

double uvNumberOr(const Node& n, const std::string& name, double def)
{
    int a = n.type().attrIndex(name);
    return a >= 0 ? n.as<double>(a) : def;
}

constexpr const char* kUvSuffixes[4] = {"UMin", "VMin", "UMax", "VMax"};
} // namespace

std::vector<AttrDef> uvRectAttrs(const std::string& prefix, const UvRect& def, const std::string& what, const std::string& category)
{
    const double v[4] = {def.lo.x, def.lo.y, def.hi.x, def.hi.y};
    const char* sides[4] = {"Low U", "Low V", "High U", "High V"};
    std::vector<AttrDef> out;
    for (int k = 0; k < 4; ++k)
    {
        std::string name = prefix.empty() ? std::string(1, char(std::tolower(kUvSuffixes[k][0]))) + (kUvSuffixes[k] + 1) : prefix + kUvSuffixes[k];
        out.push_back(uvRangeAttr(std::move(name), v[k], std::string(sides[k]) + " edge of the UV rectangle " + what + " land in.", category));
    }
    return out;
}

std::vector<AttrDef> stripUvAttrs(const StripUv& d, bool caps, const std::string& category)
{
    AttrDef layout{.name = "uvLayout", .type = PlugType::Enum, .defaultValue = int64_t(d.layout == UvLayout::Along), .flags = AttrInput | AttrStorable,
                   .enumNames = {"around", "along"},
                   .doc = "around: U around the profile, V along the path. along: U along the path, V around it (a trim strip)."};
    layout.category = category;
    std::vector<AttrDef> out = {layout, uvRangeAttr("texelDensity", d.perCm, "UV units per cm along the path; 0 fits each part to the rectangle.", category)};
    out.back().minValue = 0.0;
    for (AttrDef& a : uvRectAttrs("", d.rect, "the swept surface's UVs", category))
        out.push_back(std::move(a));
    if (caps)
        for (AttrDef& a : uvRectAttrs("cap", d.caps, "caps", category))
            out.push_back(std::move(a));
    return out;
}

UvRect uvRectOf(const Node& n, const std::string& prefix)
{
    double v[4];
    const UvRect def;
    const double fallback[4] = {def.lo.x, def.lo.y, def.hi.x, def.hi.y};
    for (int k = 0; k < 4; ++k)
    {
        std::string name = prefix.empty() ? std::string(1, char(std::tolower(kUvSuffixes[k][0]))) + (kUvSuffixes[k] + 1) : prefix + kUvSuffixes[k];
        v[k] = uvNumberOr(n, name, fallback[k]);
    }
    return {{v[0], v[1]}, {v[2], v[3]}};
}

StripUv stripUvOf(const Node& n, const StripUv& defaults)
{
    StripUv o = defaults;
    if (int a = n.type().attrIndex("uvLayout"); a >= 0)
        o.layout = n.as<int64_t>(a) == 1 ? UvLayout::Along : UvLayout::Around;
    o.perCm = std::max(0.0, uvNumberOr(n, "texelDensity", defaults.perCm));
    o.rect = uvRectOf(n, "");
    if (n.type().attrIndex("capUMin") >= 0)
        o.caps = uvRectOf(n, "cap");
    return o;
}

StripUv stripUvFrom(const nlohmann::json& values, const StripUv& defaults)
{
    StripUv o = defaults;
    auto num = [&](const char* k, double def) { return values.contains(k) && values[k].is_number() ? values[k].get<double>() : def; };
    if (values.contains("uvLayout") && values["uvLayout"].is_string())
        o.layout = values["uvLayout"].get<std::string>() == "along" ? UvLayout::Along : UvLayout::Around;
    o.perCm = std::max(0.0, num("texelDensity", o.perCm));
    o.rect = {{num("uMin", o.rect.lo.x), num("vMin", o.rect.lo.y)}, {num("uMax", o.rect.hi.x), num("vMax", o.rect.hi.y)}};
    o.caps = {{num("capUMin", o.caps.lo.x), num("capVMin", o.caps.lo.y)}, {num("capUMax", o.caps.hi.x), num("capVMax", o.caps.hi.y)}};
    return o;
}

bool isUvAttr(std::string_view name)
{
    if (name == "uvLayout" || name == "texelDensity")
        return true;
    for (const char* s : kUvSuffixes)
    {
        std::string_view suffix = s;
        if (name.size() >= suffix.size() && name.substr(name.size() - suffix.size()) == suffix)
            return true;
        if (name.size() == suffix.size() && char(std::tolower(suffix[0])) == name[0] && name.substr(1) == suffix.substr(1))
            return true;
    }
    return false;
}
} // namespace lila::curve
