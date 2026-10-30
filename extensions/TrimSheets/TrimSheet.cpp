// The .lilatrim model, its presets, and the formats other tools keep trim layouts in (Valve .rect, SVG, ID maps).

#include "TrimSheet.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <regex>
#include <stdexcept>

namespace trims
{
namespace
{
constexpr std::array<std::pair<Style, const char*>, 13> kStyles = {{
    {Style::Flat, "flat"},
    {Style::Raised, "raised"},
    {Style::Recessed, "recessed"},
    {Style::DoubleBevel, "doubleBevel"},
    {Style::Step, "step"},
    {Style::HalfPipe, "halfPipe"},
    {Style::Groove, "groove"},
    {Style::Ribs, "ribs"},
    {Style::Rivets, "rivets"},
    {Style::Bolts, "bolts"},
    {Style::Vents, "vents"},
    {Style::Tread, "tread"},
    {Style::Planks, "planks"},
}};

constexpr std::array<std::pair<uv::TrimKind, const char*>, 6> kKinds = {{
    {uv::TrimKind::StripU, "stripU"},
    {uv::TrimKind::StripV, "stripV"},
    {uv::TrimKind::Rect, "rect"},
    {uv::TrimKind::Radial, "radial"},
    {uv::TrimKind::Unique, "unique"},
    {uv::TrimKind::Flat, "flat"},
}};

constexpr std::array<std::pair<uv::TrimFit, const char*>, 3> kFits = {{
    {uv::TrimFit::Hotspot, "hotspot"},
    {uv::TrimFit::Seamless, "seamless"},
    {uv::TrimFit::Fill, "fill"},
}};

template <class E, size_t N> const char* nameOf(const std::array<std::pair<E, const char*>, N>& table, E v)
{
    for (const auto& [e, n] : table)
        if (e == v)
            return n;
    return table[0].second;
}

template <class E, size_t N> E fromName(const std::array<std::pair<E, const char*>, N>& table, const std::string& n, E fallback)
{
    for (const auto& [e, name] : table)
        if (n == name)
            return e;
    return fallback;
}

json ivec(const glm::ivec2& v) { return json::array({v.x, v.y}); }
glm::ivec2 ivecFrom(const json& j, glm::ivec2 d = glm::ivec2(0))
{
    return j.is_array() && j.size() == 2 ? glm::ivec2(j[0].get<int>(), j[1].get<int>()) : d;
}

/** Scales a pixel value laid out on a 2048 sheet to the sheet's width. */
struct Scale
{
    double k = 1.0;
    int operator()(int px) const { return std::max(1, int(std::lround(px * k))); }
};

Trim strip(std::string name, int y0, int y1, int width, Style style)
{
    Trim t;
    t.name = std::move(name);
    t.min = {0, y0};
    t.max = {width, y1};
    t.kind = uv::TrimKind::StripU;
    t.fit = uv::TrimFit::Seamless;
    t.profile.style = style;
    if (y1 - y0 <= 32)
        t.tags.push_back("edge");
    return t;
}

Trim rect(std::string name, glm::ivec2 lo, glm::ivec2 hi, Style style, uv::TrimKind kind = uv::TrimKind::Rect)
{
    Trim t;
    t.name = std::move(name);
    t.min = lo;
    t.max = hi;
    t.kind = kind;
    t.fit = kind == uv::TrimKind::Flat ? uv::TrimFit::Fill : uv::TrimFit::Hotspot;
    t.profile.style = style;
    if (kind == uv::TrimKind::Radial)
        t.tags.push_back("radial");
    if (kind == uv::TrimKind::Flat)
        t.tags.push_back("flat");
    return t;
}

/** Lays strips top to bottom; each row is either one full-width strip or a run of rectangles. */
struct Layout
{
    Sheet& sheet;
    Scale s;
    int y = 0;

    void addStrip(const char* name, int h, Style style, double spacing = 0.0)
    {
        const int y1 = std::min(sheet.height, y + s(h));
        Trim t = strip(name, y, y1, sheet.width, style);
        if (spacing > 0.0)
            t.profile.spacingPx = spacing * s.k;
        t.profile.bevelPx = 8.0 * s.k;
        t.profile.detailPx = std::max(2.0, t.profile.detailPx * s.k);
        sheet.trims.push_back(std::move(t));
        y = y1;
    }

    void addRow(const char* name, int h, std::initializer_list<std::pair<int, Style>> cells, uv::TrimKind kind = uv::TrimKind::Rect, int group = -1)
    {
        const int y1 = std::min(sheet.height, y + s(h));
        int x = 0, k = 0;
        for (const auto& [w, style] : cells)
        {
            const int x1 = std::min(sheet.width, x + s(w));
            const uv::TrimKind kk = style == Style::HalfPipe && kind == uv::TrimKind::Rect && x1 - x == y1 - y ? uv::TrimKind::Radial : kind;
            Trim t = rect(std::format("{}{}", name, k++), {x, y}, {x1, y1}, style, kk);
            t.profile.bevelPx = 8.0 * s.k;
            t.profile.detailPx = std::max(2.0, t.profile.detailPx * s.k);
            t.altGroup = group;
            sheet.trims.push_back(std::move(t));
            x = x1;
        }
        y = y1;
    }
};

const std::vector<PresetInfo> kPresets = {
    {"industrialMetal", "Industrial Metal", "Insomniac-style ladder of bevelled strips, rivet and bolt rows, vents, and a band of round caps and plates."},
    {"sciFiHull", "Sci-Fi Hull", "Hull panels: framed rectangles at several sizes and aspects, ribbed and recessed strips, round hatches."},
    {"woodTrims", "Wood Trims", "Planks of varied length with bevelled seams, rounded mouldings and battens."},
    {"concrete", "Concrete Architecture", "Steps, double bevels, recessed reveals and grooves for walls, kerbs and floor edges."},
    {"stoneMouldings", "Stone Mouldings", "Rounded and stepped mouldings, string courses and plinths."},
    {"hotspotGrid", "Hotspot Grid", "Valve-style hotspot atlas: framed panels at 1:1, 2:1, 4:1 and 8:1 in several sizes, for fast blockout texturing."},
    {"ladder", "Power-of-Two Ladder", "Plain strips of 512, 256, 128, 64, 32 and 16 px on a 1024 sheet: a clean start for your own."},
};
} // namespace

std::string_view styleName(Style s) { return nameOf(kStyles, s); }

std::optional<Style> styleFromName(std::string_view name)
{
    for (const auto& [e, n] : kStyles)
        if (name == n)
            return e;
    return std::nullopt;
}

std::vector<std::string> styleNames()
{
    std::vector<std::string> out;
    for (const auto& [e, n] : kStyles)
        out.emplace_back(n);
    return out;
}

json Profile::toJson() const
{
    return {{"style", std::string(styleName(style))}, {"bevelPx", bevelPx}, {"angle", angle},       {"segments", segments},
            {"spacingPx", spacingPx},                {"detailPx", detailPx}, {"seed", seed}};
}

Profile Profile::fromJson(const json& j)
{
    Profile p;
    if (!j.is_object())
        return p;
    p.style = styleFromName(j.value("style", std::string("raised"))).value_or(Style::Raised);
    p.bevelPx = j.value("bevelPx", p.bevelPx);
    p.angle = std::clamp(j.value("angle", p.angle), 1.0, 89.0);
    p.segments = std::clamp(j.value("segments", p.segments), 1, 16);
    p.spacingPx = std::max(1.0, j.value("spacingPx", p.spacingPx));
    p.detailPx = std::max(0.5, j.value("detailPx", p.detailPx));
    p.seed = j.value("seed", p.seed);
    return p;
}

json Trim::toJson() const
{
    json j = {{"name", name},
              {"min", ivec(min)},
              {"max", ivec(max)},
              {"kind", nameOf(kKinds, kind)},
              {"fit", nameOf(kFits, fit)},
              {"insetPx", insetPx},
              {"tags", tags},
              {"altGroup", altGroup},
              {"allowRotate", allowRotate},
              {"reserved", reserved},
              {"profile", profile.toJson()}};
    if (worldSize.x > 0.0 && worldSize.y > 0.0)
        j["worldSize"] = {worldSize.x, worldSize.y};
    return j;
}

Trim Trim::fromJson(const json& j)
{
    Trim t;
    t.name = j.value("name", std::string());
    t.min = ivecFrom(j.value("min", json()));
    t.max = ivecFrom(j.value("max", json()));
    t.kind = fromName(kKinds, j.value("kind", std::string("stripU")), uv::TrimKind::StripU);
    t.fit = fromName(kFits, j.value("fit", std::string(t.kind == uv::TrimKind::StripU || t.kind == uv::TrimKind::StripV ? "seamless" : "hotspot")),
                     uv::TrimFit::Hotspot);
    t.insetPx = std::max(0.0, j.value("insetPx", t.insetPx));
    t.tags = j.value("tags", std::vector<std::string>());
    t.altGroup = j.value("altGroup", -1);
    t.allowRotate = j.value("allowRotate", true);
    t.reserved = j.value("reserved", false);
    if (const json ws = j.value("worldSize", json()); ws.is_array() && ws.size() == 2)
        t.worldSize = {ws[0].get<double>(), ws[1].get<double>()};
    t.profile = Profile::fromJson(j.value("profile", json::object()));
    return t;
}

glm::dvec2 Sheet::worldSize(const Trim& t) const
{
    if (t.worldSize.x > 0.0 && t.worldSize.y > 0.0)
        return t.worldSize;
    return glm::dvec2(t.sizePx()) / pxPerM * 100.0;
}

glm::dvec2 Sheet::uvMin(const Trim& t) const { return {double(t.min.x) / width, 1.0 - double(t.max.y) / height}; }
glm::dvec2 Sheet::uvMax(const Trim& t) const { return {double(t.max.x) / width, 1.0 - double(t.min.y) / height}; }

int Sheet::trimAt(const glm::dvec2& uv) const
{
    const glm::dvec2 px(uv.x * width, (1.0 - uv.y) * height);
    // Tiling strips repeat across U, so only V decides for them.
    const double u = px.x - std::floor(px.x / width) * width;
    for (size_t i = 0; i < trims.size(); ++i)
    {
        const Trim& t = trims[i];
        const bool tilesU = t.kind == uv::TrimKind::StripU;
        const double x = tilesU ? u : px.x;
        if (x >= t.min.x && x < t.max.x && px.y >= t.min.y && px.y < t.max.y)
            return int(i);
    }
    return -1;
}

json Sheet::toJson() const
{
    json t = json::array();
    for (const Trim& tr : trims)
        t.push_back(tr.toJson());
    return {{"lilatrim", 1},
            {"name", name},
            {"width", width},
            {"height", height},
            {"pxPerM", pxPerM},
            {"padding", padding},
            {"mipLevel", mipLevel},
            {"preset", preset},
            {"material", material},
            {"maps", maps},
            {"folder", folder},
            {"supersampling", supersampling},
            {"trims", t}};
}

Sheet Sheet::fromJson(const json& j)
{
    Sheet s;
    s.name = j.value("name", s.name);
    s.width = std::clamp(j.value("width", s.width), 16, 16384);
    s.height = std::clamp(j.value("height", s.height), 16, 16384);
    s.pxPerM = std::max(1.0, j.value("pxPerM", s.pxPerM));
    s.padding = std::clamp(j.value("padding", s.padding), 0, 256);
    s.mipLevel = std::clamp(j.value("mipLevel", s.mipLevel), 0, 12);
    s.preset = j.value("preset", std::string());
    s.material = j.value("material", std::string());
    s.maps = j.value("maps", s.maps);
    s.folder = j.value("folder", std::string());
    s.supersampling = j.value("supersampling", s.supersampling);
    for (const json& t : j.value("trims", json::array()))
        s.trims.push_back(Trim::fromJson(t));
    return s;
}

Sheet load(const std::filesystem::path& file)
{
    std::ifstream in(file);
    if (!in)
        throw std::runtime_error("can't read " + file.string());
    json j;
    try
    {
        in >> j;
    }
    catch (const std::exception& e)
    {
        throw std::runtime_error(file.string() + " is not a trim sheet: " + e.what());
    }
    return Sheet::fromJson(j);
}

void save(const Sheet& sheet, const std::filesystem::path& file)
{
    if (file.has_parent_path())
        std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file);
    if (!out)
        throw std::runtime_error("can't write " + file.string());
    out << sheet.toJson().dump(2) << "\n";
}

std::filesystem::path textureFolder(const Sheet& sheet, const std::filesystem::path& file)
{
    if (!sheet.folder.empty())
    {
        const std::filesystem::path f(sheet.folder);
        return f.is_absolute() ? f : file.parent_path() / f;
    }
    return file.parent_path() / (file.stem().string() + "_Textures");
}

std::vector<std::string> validate(const Sheet& s)
{
    std::vector<std::string> out;
    const int grid = 1 << std::clamp(s.mipLevel, 0, 12);
    std::optional<double> bevel;
    for (size_t i = 0; i < s.trims.size(); ++i)
    {
        const Trim& t = s.trims[i];
        const std::string label = t.name.empty() ? std::format("trim {}", i) : t.name;
        if (t.max.x <= t.min.x || t.max.y <= t.min.y)
            out.push_back(label + " is empty");
        if (t.min.x < 0 || t.min.y < 0 || t.max.x > s.width || t.max.y > s.height)
            out.push_back(label + " runs off the sheet");
        for (int v : {t.min.x, t.min.y, t.max.x, t.max.y})
            if (v % grid != 0 && v != s.width && v != s.height)
            {
                out.push_back(std::format("{} has a border off the {} px grid; mip {} will bleed into its neighbour", label, grid, s.mipLevel));
                break;
            }
        if (t.profile.style != Style::Flat && t.kind != uv::TrimKind::Flat)
        {
            if (bevel && std::abs(*bevel - t.profile.bevelPx) > 0.5)
            {
                out.push_back(std::format("{} bevels {} px where others bevel {} px; edges will read at different widths", label, t.profile.bevelPx, *bevel));
            }
            bevel = bevel.value_or(t.profile.bevelPx);
        }
        for (size_t k = i + 1; k < s.trims.size(); ++k)
        {
            const Trim& o = s.trims[k];
            if (t.min.x < o.max.x && o.min.x < t.max.x && t.min.y < o.max.y && o.min.y < t.max.y)
                out.push_back(std::format("{} overlaps {}", label, o.name.empty() ? std::format("trim {}", k) : o.name));
        }
    }
    return out;
}

std::vector<uv::HotspotTrim> hotspotTrims(const Sheet& s)
{
    std::vector<uv::HotspotTrim> out;
    const double px = 1.0 / double(std::max(s.width, s.height));
    for (const Trim& t : s.trims)
    {
        uv::HotspotTrim h;
        h.min = s.uvMin(t);
        h.max = s.uvMax(t);
        h.worldSize = s.worldSize(t);
        h.kind = t.kind;
        h.fit = t.fit;
        h.inset = t.insetPx * px;
        h.tags = t.tags;
        h.altGroup = t.altGroup;
        h.allowRotate = t.allowRotate;
        h.reserved = t.reserved;
        out.push_back(std::move(h));
    }
    return out;
}

const std::vector<PresetInfo>& presets() { return kPresets; }

Sheet stripLadder(std::string name, int width, int height, std::span<const int> heights)
{
    Sheet s;
    s.name = std::move(name);
    s.width = width;
    s.height = height;
    int y = 0;
    for (size_t i = 0; i < heights.size() && y < height; ++i)
    {
        const int y1 = std::min(height, y + heights[i]);
        s.trims.push_back(strip(std::format("Strip{}", i), y, y1, width, Style::Raised));
        y = y1;
    }
    return s;
}

Sheet preset(std::string_view id, int width)
{
    using S = Style;
    using K = uv::TrimKind;
    const bool ladder = id == "ladder";
    const int base = ladder ? 1024 : 2048;
    Sheet sheet;
    sheet.width = width > 0 ? width : base;
    sheet.height = sheet.width;
    sheet.preset = std::string(id);
    sheet.padding = std::max(2, sheet.width / 256);
    Layout l{sheet, Scale{double(sheet.width) / base}};

    if (id == "industrialMetal")
    {
        sheet.name = "T_IndustrialMetal";
        sheet.material = "Worn Painted Metal";
        l.addStrip("Edge16", 16, S::Raised);
        l.addStrip("Edge16b", 16, S::HalfPipe);
        l.addStrip("Lip32", 32, S::Raised);
        l.addStrip("Ribs32", 32, S::Ribs, 16);
        l.addStrip("Groove64", 64, S::Groove);
        l.addStrip("Rivets64", 64, S::Rivets, 64);
        l.addStrip("Plate128", 128, S::Raised);
        l.addStrip("Rivets128", 128, S::Rivets, 96);
        l.addStrip("Double256", 256, S::DoubleBevel);
        l.addStrip("Bolts256", 256, S::Bolts, 256);
        l.addStrip("Panel512", 512, S::Raised);
        l.addRow("Cap", 256, {{256, S::HalfPipe}, {256, S::HalfPipe}, {256, S::Bolts}, {256, S::Bolts},
                              {256, S::Raised}, {256, S::Recessed}, {256, S::Vents}, {256, S::Flat}});
        sheet.trims.back().kind = K::Flat;
        sheet.trims.back().fit = uv::TrimFit::Fill;
        sheet.trims.back().tags = {"flat"};
        for (Trim& t : sheet.trims)
            if (t.name.starts_with("Cap") && t.profile.style == S::HalfPipe)
                t.tags.push_back("cap");
        l.addStrip("Vents256", 256, S::Vents, 32);
        l.addStrip("Tread32", 32, S::Tread, 32);
    }
    else if (id == "sciFiHull")
    {
        sheet.name = "T_SciFiHull";
        sheet.material = "Trim Hull Paint";
        l.addStrip("Edge32", 32, S::Raised);
        l.addStrip("Edge32b", 32, S::Step);
        l.addStrip("Groove64", 64, S::Groove);
        l.addStrip("Ribs128", 128, S::Ribs, 32);
        l.addRow("Panel512_", 512, {{512, S::Raised}, {512, S::DoubleBevel}, {1024, S::Raised}}, K::Rect, 0);
        l.addRow("Panel256_", 256, {{256, S::Raised}, {256, S::Recessed}, {512, S::DoubleBevel}, {1024, S::Raised}}, K::Rect, 1);
        l.addRow("Long256_", 256, {{1024, S::Recessed}, {1024, S::Raised}}, K::Rect, 2);
        l.addRow("Panel128_", 128, {{512, S::Raised}, {512, S::Bolts}, {1024, S::DoubleBevel}}, K::Rect, 3);
        l.addStrip("Ribs128b", 128, S::Ribs, 24);
        l.addRow("Hatch", 256, {{256, S::HalfPipe}, {256, S::HalfPipe}, {256, S::Vents}, {256, S::Vents},
                                {256, S::Bolts}, {256, S::Raised}, {256, S::Recessed}, {256, S::Flat}});
        sheet.trims.back().kind = K::Flat;
        sheet.trims.back().fit = uv::TrimFit::Fill;
        sheet.trims.back().tags = {"flat"};
        l.addStrip("Recess256", 256, S::Recessed);
    }
    else if (id == "woodTrims")
    {
        sheet.name = "T_WoodTrims";
        sheet.material = "Dusty Wood";
        sheet.pxPerM = 1024.0;
        l.addStrip("Dowel64", 64, S::HalfPipe);
        l.addStrip("Bead64", 64, S::DoubleBevel);
        l.addStrip("Batten128", 128, S::Raised);
        l.addStrip("Board128", 128, S::Planks, 512);
        l.addStrip("Board256", 256, S::Planks, 768);
        l.addStrip("Board256b", 256, S::Planks, 640);
        l.addStrip("Beam256", 256, S::Raised);
        l.addStrip("Floor512", 512, S::Planks, 1024);
        l.addStrip("Skirting256", 256, S::Step);
        l.addStrip("Moulding128", 128, S::HalfPipe);
    }
    else if (id == "concrete")
    {
        sheet.name = "T_Concrete";
        sheet.material = "Trim Concrete";
        sheet.pxPerM = 256.0;
        l.addStrip("Arris32", 32, S::Raised);
        l.addStrip("Reveal64", 64, S::Recessed);
        l.addStrip("Groove64", 64, S::Groove);
        l.addStrip("Kerb128", 128, S::DoubleBevel);
        l.addStrip("Step128", 128, S::Step);
        l.addStrip("Coping256", 256, S::Raised);
        l.addStrip("Plinth256", 256, S::DoubleBevel);
        l.addStrip("Panel512", 512, S::Raised);
        l.addStrip("Recess512", 512, S::Recessed);
        l.addStrip("Joint64", 64, S::Groove);
        l.addStrip("Edge32", 32, S::Step);
    }
    else if (id == "stoneMouldings")
    {
        sheet.name = "T_StoneMouldings";
        sheet.material = "Mossy Stone";
        sheet.pxPerM = 256.0;
        l.addStrip("Torus64", 64, S::HalfPipe);
        l.addStrip("Fillet64", 64, S::Step);
        l.addStrip("Ovolo128", 128, S::HalfPipe);
        l.addStrip("Fascia128", 128, S::DoubleBevel);
        l.addStrip("String256", 256, S::DoubleBevel);
        l.addStrip("Cornice256", 256, S::Step);
        l.addStrip("Ashlar512", 512, S::Raised);
        l.addStrip("Plinth512", 512, S::DoubleBevel);
        l.addStrip("Groove128", 128, S::Groove);
    }
    else if (id == "hotspotGrid")
    {
        sheet.name = "T_HotspotGrid";
        sheet.material = "Trim Painted Concrete";
        sheet.pxPerM = 256.0;
        l.addRow("Sq512_", 512, {{512, S::Raised}, {512, S::Raised}, {1024, S::Raised}}, K::Rect, 0);
        l.addRow("R256_", 256, {{256, S::Raised}, {256, S::Raised}, {512, S::Raised}, {1024, S::Raised}}, K::Rect, 1);
        l.addRow("W256_", 256, {{2048, S::Raised}}, K::Rect, 2);
        l.addRow("R128_", 128, {{128, S::Raised}, {128, S::Raised}, {128, S::Raised}, {128, S::Raised}, {256, S::Raised}, {256, S::Raised},
                                {1024, S::Raised}}, K::Rect, 3);
        l.addRow("W128_", 128, {{1024, S::Raised}, {1024, S::Raised}}, K::Rect, 4);
        l.addRow("W64_", 64, {{512, S::Raised}, {512, S::Raised}, {512, S::Raised}, {512, S::Raised}}, K::Rect, 5);
        l.addRow("Q64_", 64, {{256, S::Raised}, {256, S::Raised}, {256, S::Raised}, {256, S::Raised}, {256, S::Raised}, {256, S::Raised},
                              {256, S::Raised}, {256, S::Raised}}, K::Rect, 6);
        l.addRow("Sq256_", 256, {{256, S::Raised}, {256, S::Raised}, {256, S::Raised}, {256, S::Raised}, {256, S::Raised}, {256, S::Raised},
                                 {256, S::Raised}, {256, S::Raised}}, K::Rect, 7);
        l.addRow("W256b_", 256, {{1024, S::Raised}, {1024, S::Raised}}, K::Rect, 8);
        l.addRow("Sq128_", 128, {{128, S::Raised}, {128, S::Raised}, {128, S::Raised}, {128, S::Raised}, {128, S::Raised}, {128, S::Raised},
                                 {128, S::Raised}, {128, S::Raised}, {128, S::Raised}, {128, S::Raised}, {128, S::Raised}, {128, S::Raised},
                                 {128, S::Raised}, {128, S::Raised}, {128, S::Raised}, {128, S::Raised}}, K::Rect, 9);
    }
    else if (ladder)
    {
        sheet.name = "T_TrimLadder";
        for (int h : {512, 256, 128, 64, 32, 16, 16})
            l.addStrip(std::format("Strip{}", h).c_str(), h, S::Raised);
    }
    else
        throw std::invalid_argument("unknown trim sheet preset '" + std::string(id) + "'");
    return sheet;
}

std::string toRect(const Sheet& s)
{
    std::string out = "Rectangles\n{\n";
    for (const Trim& t : s.trims)
    {
        out += std::format("\trectangle\n\t{{\n\t\tmin \"{} {}\"\n\t\tmax \"{} {}\"\n", t.min.x, t.min.y, t.max.x, t.max.y);
        if (t.allowRotate)
            out += "\t\trotate 1\n";
        if (t.kind == uv::TrimKind::StripU || t.kind == uv::TrimKind::StripV)
            out += "\t\ttiling 1\n";
        if (t.reserved)
            out += "\t\tnohotspot 1\n";
        out += "\t}\n";
    }
    out += "}\n";
    return out;
}

Sheet fromRect(std::string_view text, int width, int height)
{
    Sheet s;
    s.width = width;
    s.height = height;
    const std::string src(text);
    const std::regex block(R"(rectangle\s*\{([^}]*)\})", std::regex::icase);
    const std::regex corner(R"re((min|max)\s+"\s*(-?\d+)\s+(-?\d+)\s*")re", std::regex::icase);
    const std::regex flag(R"re((\w+)\s+"?(\d)"?)re");
    for (auto it = std::sregex_iterator(src.begin(), src.end(), block); it != std::sregex_iterator(); ++it)
    {
        const std::string body = (*it)[1];
        Trim t;
        t.kind = uv::TrimKind::Rect;
        t.fit = uv::TrimFit::Hotspot;
        t.allowRotate = false;
        for (auto c = std::sregex_iterator(body.begin(), body.end(), corner); c != std::sregex_iterator(); ++c)
        {
            const glm::ivec2 v(std::stoi((*c)[2]), std::stoi((*c)[3]));
            ((*c)[1] == "min" || (*c)[1] == "MIN" ? t.min : t.max) = v;
        }
        for (auto f = std::sregex_iterator(body.begin(), body.end(), flag); f != std::sregex_iterator(); ++f)
        {
            const std::string name = (*f)[1];
            const bool on = (*f)[2] == "1";
            if (name == "rotate")
                t.allowRotate = on;
            else if (name == "tiling" && on)
            {
                t.kind = (t.max.x - t.min.x) >= (t.max.y - t.min.y) ? uv::TrimKind::StripU : uv::TrimKind::StripV;
                t.fit = uv::TrimFit::Seamless;
            }
            else if (name == "nohotspot")
                t.reserved = on;
        }
        t.name = std::format("Rect{}", s.trims.size());
        if (t.max.x > t.min.x && t.max.y > t.min.y)
            s.trims.push_back(std::move(t));
    }
    return s;
}

std::string toSvg(const Sheet& s)
{
    std::string out = std::format(R"(<svg xmlns="http://www.w3.org/2000/svg" width="{}" height="{}" viewBox="0 0 {} {}">)", s.width, s.height,
                                  s.width, s.height);
    out += "\n";
    for (size_t i = 0; i < s.trims.size(); ++i)
    {
        const Trim& t = s.trims[i];
        const glm::vec3 c = trimColor(i) * 255.0f;
        out += std::format(R"x(  <rect id="{}" x="{}" y="{}" width="{}" height="{}" fill="rgb({},{},{})" data-kind="{}"/>)x", t.name, t.min.x, t.min.y,
                           t.max.x - t.min.x, t.max.y - t.min.y, int(c.r), int(c.g), int(c.b), nameOf(kKinds, t.kind));
        out += "\n";
    }
    out += "</svg>\n";
    return out;
}

Sheet fromSvg(std::string_view text)
{
    Sheet s;
    const std::string src(text);
    auto number = [](const std::string& tag, const char* attr, double fallback) {
        const std::regex re(std::string("\\b") + attr + R"re(\s*=\s*"([-0-9.eE+]+))re");
        std::smatch m;
        return std::regex_search(tag, m, re) ? std::stod(m[1]) : fallback;
    };
    auto text_ = [](const std::string& tag, const char* attr) {
        const std::regex re(std::string("\\b") + attr + R"re(\s*=\s*"([^"]*)")re");
        std::smatch m;
        return std::regex_search(tag, m, re) ? std::string(m[1]) : std::string();
    };
    std::smatch root;
    if (std::regex_search(src, root, std::regex(R"(<svg\b[^>]*>)")))
    {
        const std::string tag = root[0];
        s.width = int(std::lround(number(tag, "width", 2048)));
        s.height = int(std::lround(number(tag, "height", s.width)));
    }
    const std::regex rectTag(R"(<rect\b[^>]*>)");
    for (auto it = std::sregex_iterator(src.begin(), src.end(), rectTag); it != std::sregex_iterator(); ++it)
    {
        const std::string tag = (*it)[0];
        Trim t;
        t.min = {int(std::lround(number(tag, "x", 0))), int(std::lround(number(tag, "y", 0)))};
        t.max = t.min + glm::ivec2(int(std::lround(number(tag, "width", 0))), int(std::lround(number(tag, "height", 0))));
        t.name = text_(tag, "id");
        if (t.name.empty())
            t.name = std::format("Rect{}", s.trims.size());
        const std::string kind = text_(tag, "data-kind");
        const bool full = t.min.x == 0 && t.max.x == s.width;
        t.kind = kind.empty() ? (full ? uv::TrimKind::StripU : uv::TrimKind::Rect) : fromName(kKinds, kind, uv::TrimKind::Rect);
        t.fit = t.kind == uv::TrimKind::StripU || t.kind == uv::TrimKind::StripV ? uv::TrimFit::Seamless : uv::TrimFit::Hotspot;
        if (t.max.x > t.min.x && t.max.y > t.min.y)
            s.trims.push_back(std::move(t));
    }
    return s;
}

Sheet fromIdMap(const lila::image::Image& img, double tolerance, int minPx)
{
    Sheet s;
    s.width = img.width;
    s.height = img.height;
    const double q = std::max(1.0 / 255.0, tolerance);
    std::map<uint32_t, std::pair<glm::ivec2, glm::ivec2>> boxes;
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x)
        {
            const float* p = &img.rgba[(size_t(y) * img.width + x) * 4];
            if (p[3] < 0.5f)
                continue;
            uint32_t key = 0;
            for (int c = 0; c < 3; ++c)
                key = key * 1024 + uint32_t(std::lround(std::clamp(double(p[c]), 0.0, 1.0) / q));
            auto [it, fresh] = boxes.try_emplace(key, glm::ivec2(x, y), glm::ivec2(x + 1, y + 1));
            if (!fresh)
            {
                it->second.first = glm::min(it->second.first, glm::ivec2(x, y));
                it->second.second = glm::max(it->second.second, glm::ivec2(x + 1, y + 1));
            }
        }
    std::vector<std::pair<glm::ivec2, glm::ivec2>> rects;
    for (const auto& [key, box] : boxes)
        if (box.second.x - box.first.x >= minPx && box.second.y - box.first.y >= minPx)
            rects.push_back(box);
    // Top to bottom, then left to right, so trim order reads like the texture.
    std::ranges::sort(rects, [](const auto& a, const auto& b) { return a.first.y != b.first.y ? a.first.y < b.first.y : a.first.x < b.first.x; });
    for (const auto& [lo, hi] : rects)
    {
        const bool full = lo.x == 0 && hi.x == s.width;
        Trim t = full ? strip(std::format("Strip{}", s.trims.size()), lo.y, hi.y, s.width, Style::Raised)
                      : rect(std::format("Rect{}", s.trims.size()), lo, hi, Style::Raised);
        s.trims.push_back(std::move(t));
    }
    return s;
}

Sheet fromUvs(const geo::Mesh& mesh, std::string_view uvSet, int width, int height)
{
    Sheet s;
    s.width = width;
    s.height = height;
    uv::ChartOptions co;
    co.source = uv::IslandSource::Uvs;
    co.uvSet = uvSet;
    for (const uv::Chart& ch : uv::buildCharts(mesh, {}, co))
    {
        const glm::dvec2 lo = glm::clamp(ch.uvMin(), glm::dvec2(0.0), glm::dvec2(1.0));
        const glm::dvec2 hi = glm::clamp(ch.uvMax(), glm::dvec2(0.0), glm::dvec2(1.0));
        const glm::ivec2 pmin(int(std::lround(lo.x * width)), int(std::lround((1.0 - hi.y) * height)));
        const glm::ivec2 pmax(int(std::lround(hi.x * width)), int(std::lround((1.0 - lo.y) * height)));
        if (pmax.x <= pmin.x || pmax.y <= pmin.y)
            continue;
        const bool full = pmin.x == 0 && pmax.x == width;
        s.trims.push_back(full ? strip(std::format("Strip{}", s.trims.size()), pmin.y, pmax.y, width, Style::Raised)
                               : rect(std::format("Rect{}", s.trims.size()), pmin, pmax, Style::Raised));
    }
    std::ranges::sort(s.trims, [](const Trim& a, const Trim& b) { return a.min.y != b.min.y ? a.min.y < b.min.y : a.min.x < b.min.x; });
    return s;
}

glm::vec3 trimColor(size_t i)
{
    // Golden-ratio hue steps keep neighbouring ids far apart; value alternates so hues that repeat still differ.
    const double h = std::fmod(0.13 + double(i) * 0.6180339887498949, 1.0) * 6.0;
    const double v = (i / 7) % 2 ? 0.7 : 0.95, sat = 0.75;
    const double c = v * sat, x = c * (1.0 - std::abs(std::fmod(h, 2.0) - 1.0)), m = v - c;
    glm::dvec3 rgb;
    switch (int(h))
    {
    case 0: rgb = {c, x, 0}; break;
    case 1: rgb = {x, c, 0}; break;
    case 2: rgb = {0, c, x}; break;
    case 3: rgb = {0, x, c}; break;
    case 4: rgb = {x, 0, c}; break;
    default: rgb = {c, 0, x}; break;
    }
    // Quantised to 8 bit so the written map reads back exactly.
    return glm::vec3(glm::round((rgb + m) * 255.0) / 255.0);
}

namespace
{
/** An image of the sheet's size with each trim's texels set to texel(i). */
lila::image::Image fillTrims(const Sheet& s, const std::function<glm::vec4(size_t)>& texel)
{
    lila::image::Image img;
    img.width = s.width;
    img.height = s.height;
    img.rgba.assign(size_t(s.width) * s.height * 4, 0.0f);
    for (size_t i = 0; i < s.trims.size(); ++i)
    {
        const Trim& t = s.trims[i];
        const glm::vec4 c = texel(i);
        for (int y = std::max(0, t.min.y); y < std::min(s.height, t.max.y); ++y)
            for (int x = std::max(0, t.min.x); x < std::min(s.width, t.max.x); ++x)
                std::copy_n(&c.x, 4, &img.rgba[(size_t(y) * s.width + x) * 4]);
    }
    return img;
}
} // namespace

std::vector<size_t> moveBorder(Sheet& s, int axis, int at, int along, int to)
{
    const int a = axis == 0 ? 0 : 1, b = 1 - a;
    struct Side
    {
        size_t trim;
        bool low; // the border is the trim's min on this axis
    };
    std::vector<Side> sides;
    for (size_t i = 0; i < s.trims.size(); ++i)
    {
        const Trim& t = s.trims[i];
        if (t.max[a] == at)
            sides.push_back({i, false});
        else if (t.min[a] == at)
            sides.push_back({i, true});
    }
    // Grow the run from the trims under `along` through every trim on the line its span touches.
    std::vector<char> in(sides.size(), 0);
    int lo = along, hi = along;
    for (bool grew = true; grew;)
    {
        grew = false;
        for (size_t k = 0; k < sides.size(); ++k)
        {
            const Trim& t = s.trims[sides[k].trim];
            if (in[k] || t.max[b] < lo || t.min[b] > hi)
                continue;
            in[k] = 1;
            lo = std::min(lo, t.min[b]);
            hi = std::max(hi, t.max[b]);
            grew = true;
        }
    }
    int least = std::numeric_limits<int>::min(), most = std::numeric_limits<int>::max();
    for (size_t k = 0; k < sides.size(); ++k)
        if (in[k])
        {
            const Trim& t = s.trims[sides[k].trim];
            if (sides[k].low)
                most = std::min(most, t.max[a] - 1);
            else
                least = std::max(least, t.min[a] + 1);
        }
    std::vector<size_t> out;
    if (least > most)
        return out;
    to = std::clamp(to, least, most);
    for (size_t k = 0; k < sides.size(); ++k)
        if (in[k])
        {
            Trim& t = s.trims[sides[k].trim];
            (sides[k].low ? t.min : t.max)[a] = to;
            out.push_back(sides[k].trim);
        }
    std::ranges::sort(out);
    return out;
}

bool splitTrim(Sheet& s, size_t i, int axis, int at)
{
    if (i >= s.trims.size())
        return false;
    const int a = axis == 0 ? 0 : 1;
    Trim first = s.trims[i];
    if (at <= first.min[a] || at >= first.max[a])
        return false;
    Trim second = first;
    first.max[a] = at;
    second.min[a] = at;
    second.name = first.name + "_b";
    s.trims[i] = first;
    s.trims.insert(s.trims.begin() + std::ptrdiff_t(i + 1), second);
    return true;
}

int snapToGrid(const Sheet& s, double at)
{
    const int step = 1 << std::clamp(s.mipLevel, 0, 12);
    return int(std::lround(at / step)) * step;
}

lila::image::Image idMap(const Sheet& s)
{
    return fillTrims(s, [](size_t i) { return glm::vec4(trimColor(i), 1.0f); });
}

lila::image::Image trimIndexMap(const Sheet& s)
{
    return fillTrims(s, [](size_t i) {
        const float v = float(i + 1);
        return glm::vec4(v, v, v, 1.0f);
    });
}

lila::image::Image trimRectMap(const Sheet& s)
{
    return fillTrims(s, [&](size_t i) {
        const glm::dvec2 lo = s.uvMin(s.trims[i]), hi = s.uvMax(s.trims[i]);
        return glm::vec4(float(lo.x), float(lo.y), float(hi.x), float(hi.y));
    });
}

std::vector<size_t> trimsMatching(const Sheet& s, const json& which)
{
    std::vector<size_t> out;
    auto add = [&](size_t i) {
        if (std::ranges::find(out, i) == out.end())
            out.push_back(i);
    };
    for (const json& w : which.is_array() ? which : json::array({which}))
    {
        if (w.is_number_integer())
        {
            const int64_t i = w.get<int64_t>();
            if (i < 0 || size_t(i) >= s.trims.size())
                throw std::invalid_argument("no trim " + std::to_string(i));
            add(size_t(i));
            continue;
        }
        if (!w.is_string())
            throw std::invalid_argument("trims are indices, names or tags");
        const std::string key = w.get<std::string>();
        bool found = false;
        for (size_t i = 0; i < s.trims.size(); ++i)
            if (s.trims[i].name == key || std::ranges::find(s.trims[i].tags, key) != s.trims[i].tags.end())
            {
                add(i);
                found = true;
            }
        if (!found)
            throw std::invalid_argument("no trim named or tagged " + key);
    }
    std::ranges::sort(out);
    return out;
}
} // namespace trims
