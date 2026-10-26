#include "TreeSchema.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>

namespace treegen
{
namespace
{
Field make(const char* section, const char* name, const char* label, Kind kind, json def)
{
    Field f;
    f.section = section;
    f.name = name;
    f.label = label;
    f.kind = kind;
    f.defaultValue = std::move(def);
    return f;
}

Field choice(const char* section, const char* name, const char* label, const char* def, std::vector<std::string> options)
{
    Field f = make(section, name, label, Kind::Enum, def);
    f.options = std::move(options);
    return f;
}

Field slider(const char* section, const char* name, const char* label, double def, double lo, double hi, double fieldLo, double fieldHi)
{
    Field f = make(section, name, label, Kind::Slider, def);
    f.sliderMin = lo;
    f.sliderMax = hi;
    f.fieldMin = fieldLo;
    f.fieldMax = fieldHi;
    return f;
}

Field slider(const char* section, const char* name, const char* label, double def, double lo, double hi)
{
    return slider(section, name, label, def, lo, hi, lo, hi);
}

json pair(double a, double b) { return json::array({a, b}); }

const std::vector<std::string> kUpAxes{"auto", "Y", "Z"};
const std::vector<std::string> kAxes{"auto", "X", "-X", "Y", "-Y", "Z", "-Z"};

std::vector<Field> buildFields()
{
    using K = Kind;
    std::vector<Field> f;
    f.push_back(choice("global", "up_axis", "World Up Axis", "auto", kUpAxes));
    f.push_back(slider("global", "style", "Stylization", 0.0, 0.0, 3.0));

    f.push_back(make("trunk", "height", "Height", K::Length, kTrunkHeightMetres));
    f.push_back(make("trunk", "lean", "Lean", K::Float, 6.0));
    f.push_back(make("trunk", "radius", "Base Radius", K::Length, kTrunkRadiusMetres));
    f.push_back(make("trunk", "taper", "Taper", K::Float, 0.35));
    f.push_back(make("trunk", "sides", "Sides", K::Int, 8));
    f.push_back(make("trunk", "sections", "Sections", K::Int, 8));
    f.push_back(make("trunk", "levels", "Branch Levels", K::Int, 2));
    f.push_back(make("trunk", "branches", "Branches Per Level", K::Int, 3));
    f.push_back(make("trunk", "branch_start", "Branch Start", K::Float, 0.45));
    f.push_back(make("trunk", "branch_angle", "Branch Angle", K::Float, 45.0));
    f.push_back(make("trunk", "angle_variance", "Angle Variance", K::Float, 12.0));
    f.push_back(make("trunk", "length_ratio", "Length Ratio", K::Float, 0.62));
    f.push_back(make("trunk", "length_falloff", "Length Falloff", K::Float, 0.0));
    f.push_back(make("trunk", "radius_ratio", "Radius Ratio", K::Float, 0.6));
    f.push_back(make("trunk", "gnarl", "Gnarl", K::Float, 7.0));
    f.push_back(make("trunk", "up_pull", "Branch Up Pull", K::Float, 0.45));
    f.push_back(choice("trunk", "uv_mode", "UV Mode", "packed", {"packed", "tiled"}));
    f.push_back(make("trunk", "uv_tile", "UV Tile Size (tiled, 0 = auto)", K::Length, 0.0));
    f.push_back(make("trunk", "texel_density", "Texel Density px/m (packed, 0 = fit)", K::Float, 0.0));
    f.push_back(make("trunk", "uv_map_size", "Map Size (packed)", K::Int, 2048));
    f.push_back(make("trunk", "seed", "Seed", K::Int, 0));

    f.push_back(choice("blob", "blob_kind", "Blob Type", "icosphere", {"icosphere", "quadsphere", "uvsphere"}));
    f.push_back(make("blob", "blob_radius", "Blob Radius (0 = auto)", K::Length, 0.0));
    f.push_back(make("blob", "blob_variation", "Blob Variation", K::Float, 0.25));
    f.push_back(make("blob", "blob_taper", "Blob Taper", K::Float, 0.0));
    f.push_back(slider("blob", "blob_clip", "Blob Clip Bottom", 0.0, 0.0, 0.95));
    f.push_back(slider("blob", "blob_offset", "Blob Offset (x radius)", 0.35, -0.5, 0.5, -100.0, 100.0));
    f.push_back(slider("blob", "blob_align", "Blob Align To Branch", 0.0, 0.0, 1.0));
    f.push_back(make("blob", "blob_tilt", "Blob Tilt", K::Float, 0.0));
    f.push_back(make("blob", "blob_detail", "Blob Detail", K::Int, 2));
    f.push_back(make("blob", "keep_canopy", "Keep Blobs (hidden) In The Full Tree", K::Bool, false));

    f.push_back(make("leaf", "count", "Count", K::Int, 400));
    f.push_back(make("leaf", "seed", "Seed", K::Int, 0));
    f.push_back(make("leaf", "size", "Leaf Length (0 = as modelled)", K::Length, 0.0));
    f.push_back(make("leaf", "scale", "Scale", K::Range, pair(0.8, 1.3)));
    f.push_back(make("leaf", "push", "Normal Offset", K::LengthRange, pair(0.0, 0.0)));
    f.push_back(make("leaf", "spin", "Spin", K::Range, pair(0.0, 360.0)));
    f.push_back(make("leaf", "tilt", "Tilt", K::Range, pair(0.0, 25.0)));
    f.push_back(make("leaf", "curl", "Curl", K::Range, pair(-5.0, 20.0)));
    f.push_back(make("leaf", "wrap", "Wrap", K::Range, pair(0.5, 1.0)));
    f.push_back(make("leaf", "wrap_radius", "Wrap Radius (0 = auto)", K::Length, 0.0));
    f.push_back(make("leaf", "gather", "Gather To Branch", K::Range, pair(0.0, 0.0)));
    f.push_back(make("leaf", "align", "Align Along Branch", K::Range, pair(0.0, 0.0)));
    f.push_back(make("leaf", "clumps", "Clumps (0 = even)", K::Int, 0));
    f.push_back(make("leaf", "clump_radius", "Clump Radius (0 = auto)", K::Length, 0.0));
    f.push_back(make("leaf", "clump_falloff", "Clump Falloff", K::Float, 1.0));
    f.push_back(make("leaf", "color_set", "Vertex Colour Set", K::String, "leafShade"));
    f.push_back(make("leaf", "shade_bias", "Shade Bias", K::Float, 0.0));
    f.push_back(make("leaf", "min_distance", "Min Distance", K::Length, 0.0));
    f.push_back(make("leaf", "min_normal_up", "Min Normal Up", K::Float, -1.0));
    f.push_back(make("leaf", "normal_blend", "Canopy Normals", K::Float, 1.0));
    f.push_back(choice("leaf", "pivot", "Leaf Pivot", "base", {"base", "center", "object"}));
    f.push_back(choice("leaf", "leaf_up", "Leaf Up Axis", "auto", kAxes));
    f.push_back(choice("leaf", "leaf_forward", "Leaf Forward Axis", "auto", kAxes));
    f.push_back(make("leaf", "combine", "Combine Into One Mesh", K::Bool, true));

    f.push_back(make("plane", "length", "Plane Length", K::Length, 0.12));
    f.push_back(make("plane", "width", "Plane Width", K::Length, 0.05));
    f.push_back(make("plane", "segments", "Length Segments", K::Int, 6));
    f.push_back(make("plane", "cross", "Width Segments", K::Int, 1));
    f.push_back(slider("plane", "taper", "Tip Taper", 0.6, 0.0, 1.0));
    f.push_back(make("plane", "double", "Double Sided", K::Bool, false));
    f.push_back(make("plane", "name", "Plane Name", K::String, "leafPlane"));

    f.push_back(make("export", "path", "Export Path", K::Folder, ""));
    f.push_back(make("export", "name", "File Name", K::String, ""));
    f.push_back(make("export", "trunk_material", "Trunk Material", K::String, "Trunk"));
    f.push_back(make("export", "canopy_material", "Canopy Material", K::String, "Canopy"));
    f.push_back(make("export", "leaf_material", "Leaves Material", K::String, "Leaves"));
    f.push_back(make("export", "canopy_high", "Canopy In High Poly", K::Bool, false));
    f.push_back(make("export", "canopy_low", "Canopy In Low Poly", K::Bool, false));
    f.push_back(make("export", "trunk_divisions", "High Poly Trunk Divisions", K::Int, 1));
    f.push_back(make("export", "canopy_divisions", "High Poly Canopy Divisions", K::Int, 1));
    f.push_back(make("export", "leaf_divisions", "High Poly Leaf Divisions", K::Int, 0));
    f.push_back(make("export", "opacity_size", "Opacity Map Size", K::Int, 1024));
    return f;
}

json preset(const char* upAxis, const json& trunk, const json& blob, const json& leaf)
{
    json values = json::object();
    if (upAxis)
        values["global.up_axis"] = upAxis;
    for (const auto& [section, block] : {std::pair{"trunk", &trunk}, std::pair{"blob", &blob}, std::pair{"leaf", &leaf}})
        for (const auto& [k, v] : block->items())
            values[std::string(section) + "." + k] = v;
    return values;
}

const std::vector<std::pair<std::string, json>>& presetTable()
{
    static const std::vector<std::pair<std::string, json>> table = [] {
        std::vector<std::pair<std::string, json>> t;
        t.emplace_back("Default", json::object());

        // Sugi, Japanese cedar: tall narrow conifer, near horizontal branches shortening sharply toward the top.
        t.emplace_back("Sugi Young", preset(nullptr,
            {{"height", 7.0}, {"lean", 1.5}, {"radius", 0.16}, {"taper", 0.12}, {"sides", 8}, {"sections", 12}, {"levels", 2}, {"branches", 6},
             {"branch_start", 0.08}, {"branch_angle", 74.0}, {"angle_variance", 8.0}, {"length_ratio", 0.22}, {"length_falloff", 0.85},
             {"radius_ratio", 0.45}, {"gnarl", 2.0}, {"up_pull", 0.15}},
            {{"blob_radius", 0.32}, {"blob_variation", 0.28}, {"blob_taper", 0.85}, {"blob_detail", 1}, {"blob_align", 0.35}},
            {{"count", 1400}, {"size", 0.20}, {"scale", pair(0.8, 1.2)}, {"push", pair(0.0, 0.01)}, {"tilt", pair(10.0, 45.0)},
             {"curl", pair(5.0, 25.0)}, {"wrap", pair(0.2, 0.6)}, {"min_distance", 0.02}, {"normal_blend", 0.85}, {"align", pair(0.25, 0.7)},
             {"gather", pair(0.0, 0.45)}, {"clumps", 45}, {"clump_falloff", 0.8}}));
        t.emplace_back("Sugi Mature", preset(nullptr,
            {{"height", 14.0}, {"lean", 2.0}, {"radius", 0.42}, {"taper", 0.12}, {"sides", 10}, {"sections", 14}, {"levels", 2}, {"branches", 5},
             {"branch_start", 0.18}, {"branch_angle", 78.0}, {"angle_variance", 9.0}, {"length_ratio", 0.20}, {"length_falloff", 0.8},
             {"radius_ratio", 0.45}, {"gnarl", 3.0}, {"up_pull", 0.12}},
            {{"blob_radius", 0.55}, {"blob_variation", 0.3}, {"blob_taper", 0.85}, {"blob_detail", 1}, {"blob_align", 0.35}},
            {{"count", 3000}, {"size", 0.22}, {"scale", pair(0.8, 1.25)}, {"push", pair(0.0, 0.015)}, {"tilt", pair(10.0, 50.0)},
             {"curl", pair(5.0, 25.0)}, {"wrap", pair(0.2, 0.6)}, {"min_distance", 0.03}, {"normal_blend", 0.85}, {"align", pair(0.3, 0.75)},
             {"gather", pair(0.0, 0.5)}, {"clumps", 70}, {"clump_falloff", 0.8}}));
        t.emplace_back("Sugi Ancient", preset(nullptr,
            {{"height", 18.0}, {"lean", 4.0}, {"radius", 0.75}, {"taper", 0.16}, {"sides", 12}, {"sections", 16}, {"levels", 2}, {"branches", 6},
             {"branch_start", 0.42}, {"branch_angle", 72.0}, {"angle_variance", 14.0}, {"length_ratio", 0.26}, {"length_falloff", 0.4},
             {"radius_ratio", 0.5}, {"gnarl", 6.0}, {"up_pull", 0.2}},
            {{"blob_radius", 0.8}, {"blob_variation", 0.32}, {"blob_taper", 0.6}, {"blob_detail", 1}, {"blob_align", 0.3}},
            {{"count", 3600}, {"size", 0.24}, {"scale", pair(0.8, 1.3)}, {"push", pair(0.0, 0.02)}, {"tilt", pair(10.0, 50.0)},
             {"curl", pair(5.0, 30.0)}, {"wrap", pair(0.25, 0.65)}, {"min_distance", 0.035}, {"normal_blend", 0.8}, {"align", pair(0.3, 0.75)},
             {"gather", pair(0.0, 0.5)}, {"clumps", 85}, {"clump_falloff", 0.9}}));

        // Akamatsu, Japanese red pine: sparse and sculptural, heavy gnarl, needle tufts standing off the surface.
        t.emplace_back("Akamatsu Young", preset(nullptr,
            {{"height", 4.5}, {"lean", 14.0}, {"radius", 0.16}, {"taper", 0.3}, {"sides", 8}, {"sections", 10}, {"levels", 2}, {"branches", 3},
             {"branch_start", 0.3}, {"branch_angle", 62.0}, {"angle_variance", 26.0}, {"length_ratio", 0.62}, {"length_falloff", 0.3},
             {"radius_ratio", 0.55}, {"gnarl", 20.0}, {"up_pull", 0.25}},
            {{"blob_radius", 0.45}, {"blob_variation", 0.35}, {"blob_taper", 0.4}, {"blob_detail", 1}, {"blob_align", 0.75}, {"blob_clip", 0.25},
             {"blob_offset", 0.1}},
            {{"count", 550}, {"size", 0.20}, {"scale", pair(0.85, 1.25)}, {"push", pair(0.0, 0.02)}, {"tilt", pair(15.0, 60.0)},
             {"curl", pair(-10.0, 10.0)}, {"wrap", pair(0.0, 0.25)}, {"min_distance", 0.06}, {"min_normal_up", -0.4}, {"normal_blend", 0.5},
             {"align", pair(0.5, 0.95)}, {"gather", pair(0.2, 0.6)}, {"clumps", 26}, {"clump_falloff", 0.5}, {"shade_bias", 0.1}}));
        t.emplace_back("Akamatsu Mature", preset(nullptr,
            {{"height", 7.0}, {"lean", 12.0}, {"radius", 0.3}, {"taper", 0.28}, {"sides", 10}, {"sections", 12}, {"levels", 3}, {"branches", 2},
             {"branch_start", 0.35}, {"branch_angle", 58.0}, {"angle_variance", 28.0}, {"length_ratio", 0.66}, {"length_falloff", 0.25},
             {"radius_ratio", 0.55}, {"gnarl", 18.0}, {"up_pull", 0.3}},
            {{"blob_radius", 0.55}, {"blob_variation", 0.35}, {"blob_taper", 0.45}, {"blob_detail", 1}, {"blob_align", 0.75}, {"blob_clip", 0.25},
             {"blob_offset", 0.1}},
            {{"count", 900}, {"size", 0.20}, {"scale", pair(0.85, 1.25)}, {"push", pair(0.0, 0.025)}, {"tilt", pair(15.0, 60.0)},
             {"curl", pair(-10.0, 12.0)}, {"wrap", pair(0.0, 0.25)}, {"min_distance", 0.06}, {"min_normal_up", -0.4}, {"normal_blend", 0.5},
             {"align", pair(0.5, 0.95)}, {"gather", pair(0.2, 0.65)}, {"clumps", 40}, {"clump_falloff", 0.5}, {"shade_bias", 0.1}}));
        t.emplace_back("Akamatsu Ancient", preset(nullptr,
            {{"height", 8.5}, {"lean", 16.0}, {"radius", 0.55}, {"taper", 0.3}, {"sides", 12}, {"sections", 14}, {"levels", 3}, {"branches", 3},
             {"branch_start", 0.28}, {"branch_angle", 66.0}, {"angle_variance", 30.0}, {"length_ratio", 0.6}, {"length_falloff", 0.25},
             {"radius_ratio", 0.58}, {"gnarl", 22.0}, {"up_pull", 0.35}},
            {{"blob_radius", 0.6}, {"blob_variation", 0.38}, {"blob_taper", 0.5}, {"blob_detail", 1}, {"blob_align", 0.8}, {"blob_clip", 0.3},
             {"blob_offset", 0.05}},
            {{"count", 1500}, {"size", 0.20}, {"scale", pair(0.85, 1.3)}, {"push", pair(0.0, 0.025)}, {"tilt", pair(15.0, 65.0)},
             {"curl", pair(-12.0, 12.0)}, {"wrap", pair(0.0, 0.3)}, {"min_distance", 0.06}, {"min_normal_up", -0.4}, {"normal_blend", 0.5},
             {"align", pair(0.55, 1.0)}, {"gather", pair(0.25, 0.7)}, {"clumps", 55}, {"clump_falloff", 0.5}, {"shade_bias", 0.1}}));

        // Kaede, Japanese maple: broadleaf, rounded spreading crown, clusters wrapped hard onto the blobs.
        t.emplace_back("Kaede Young", preset(nullptr,
            {{"height", 2.8}, {"lean", 8.0}, {"radius", 0.07}, {"taper", 0.4}, {"sides", 8}, {"sections", 8}, {"levels", 2}, {"branches", 3},
             {"branch_start", 0.35}, {"branch_angle", 46.0}, {"angle_variance", 16.0}, {"length_ratio", 0.66}, {"length_falloff", 0.15},
             {"radius_ratio", 0.6}, {"gnarl", 9.0}, {"up_pull", 0.5}},
            {{"blob_radius", 0.42}, {"blob_variation", 0.3}, {"blob_taper", 0.3}, {"blob_detail", 2}, {"blob_align", 0.2}},
            {{"count", 900}, {"size", 0.24}, {"scale", pair(0.75, 1.25)}, {"push", pair(0.0, 0.005)}, {"tilt", pair(5.0, 35.0)},
             {"curl", pair(5.0, 25.0)}, {"wrap", pair(0.55, 1.0)}, {"min_distance", 0.025}, {"min_normal_up", -0.6}, {"normal_blend", 1.0},
             {"align", pair(0.0, 0.35)}, {"gather", pair(0.1, 0.5)}, {"clumps", 22}, {"clump_falloff", 1.2}}));
        t.emplace_back("Kaede Mature", preset(nullptr,
            {{"height", 5.0}, {"lean", 8.0}, {"radius", 0.2}, {"taper", 0.38}, {"sides", 10}, {"sections", 10}, {"levels", 3}, {"branches", 3},
             {"branch_start", 0.32}, {"branch_angle", 50.0}, {"angle_variance", 18.0}, {"length_ratio", 0.62}, {"length_falloff", 0.15},
             {"radius_ratio", 0.6}, {"gnarl", 11.0}, {"up_pull", 0.55}},
            {{"blob_radius", 0.55}, {"blob_variation", 0.32}, {"blob_taper", 0.35}, {"blob_detail", 2}, {"blob_align", 0.2}},
            {{"count", 2600}, {"size", 0.26}, {"scale", pair(0.75, 1.25)}, {"push", pair(0.0, 0.008)}, {"tilt", pair(5.0, 35.0)},
             {"curl", pair(5.0, 25.0)}, {"wrap", pair(0.55, 1.0)}, {"min_distance", 0.03}, {"min_normal_up", -0.6}, {"normal_blend", 1.0},
             {"align", pair(0.0, 0.4)}, {"gather", pair(0.15, 0.55)}, {"clumps", 45}, {"clump_falloff", 1.2}}));
        t.emplace_back("Kaede Ancient", preset(nullptr,
            {{"height", 6.5}, {"lean", 10.0}, {"radius", 0.42}, {"taper", 0.4}, {"sides", 12}, {"sections", 12}, {"levels", 3}, {"branches", 3},
             {"branch_start", 0.22}, {"branch_angle", 56.0}, {"angle_variance", 22.0}, {"length_ratio", 0.6}, {"length_falloff", 0.12},
             {"radius_ratio", 0.62}, {"gnarl", 14.0}, {"up_pull", 0.45}},
            {{"blob_radius", 0.7}, {"blob_variation", 0.35}, {"blob_taper", 0.45}, {"blob_detail", 2}, {"blob_align", 0.25}},
            {{"count", 3400}, {"size", 0.28}, {"scale", pair(0.75, 1.3)}, {"push", pair(0.0, 0.01)}, {"tilt", pair(5.0, 40.0)},
             {"curl", pair(5.0, 30.0)}, {"wrap", pair(0.5, 1.0)}, {"min_distance", 0.035}, {"min_normal_up", -0.6}, {"normal_blend", 1.0},
             {"align", pair(0.0, 0.4)}, {"gather", pair(0.15, 0.55)}, {"clumps", 60}, {"clump_falloff", 1.3}}));
        return t;
    }();
    return table;
}

json tones(std::initializer_list<std::pair<const char*, const char*>> colours, double roughness)
{
    json j = json::object();
    for (const auto& [k, v] : colours)
        j[k] = v;
    j["roughness"] = roughness;
    return j;
}

json leafPalette(const std::string& species)
{
    if (species == "sugi")
        return {{"spring", tones({{"fresh", "#A8C15A"}, {"light", "#94B14E"}, {"base", "#7A9A3C"}, {"bunch", "#6A8A35"}, {"dark", "#5B7A2E"}, {"deep", "#3F5A22"}}, 0.42)},
                {"summer", tones({{"fresh", "#7FA04A"}, {"light", "#6B8F3E"}, {"base", "#547A33"}, {"bunch", "#486A2C"}, {"dark", "#3C5B26"}, {"deep", "#28401B"}}, 0.45)},
                {"autumn", tones({{"fresh", "#7A8F3A"}, {"light", "#698030"}, {"base", "#526A28"}, {"bunch", "#465C23"}, {"dark", "#3B4E1E"}, {"deep", "#283715"}}, 0.5)},
                {"lateAutumn", tones({{"fresh", "#6D8035"}, {"light", "#5D702C"}, {"base", "#485C24"}, {"bunch", "#3E5020"}, {"dark", "#33431B"}, {"deep", "#223012"}}, 0.52)}};
    if (species == "akamatsu")
        return {{"spring", tones({{"fresh", "#96BC6C"}, {"light", "#82A85E"}, {"base", "#63904E"}, {"bunch", "#557F44"}, {"dark", "#476E3A"}, {"deep", "#305028"}}, 0.38)},
                {"summer", tones({{"fresh", "#7FA85E"}, {"light", "#6B9450"}, {"base", "#4F7A42"}, {"bunch", "#436A39"}, {"dark", "#375A30"}, {"deep", "#244022"}}, 0.4)},
                {"autumn", tones({{"fresh", "#77A055"}, {"light", "#648C49"}, {"base", "#4A733D"}, {"bunch", "#3F6434"}, {"dark", "#34552C"}, {"deep", "#223B1F"}}, 0.44)},
                {"lateAutumn", tones({{"fresh", "#6B9450"}, {"light", "#5A8044"}, {"base", "#426838"}, {"bunch", "#385A30"}, {"dark", "#2E4C28"}, {"deep", "#1E351C"}}, 0.46)}};
    return {{"spring", tones({{"fresh", "#C8D47A"}, {"light", "#B4C468"}, {"base", "#9AB055"}, {"bunch", "#8AA24D"}, {"dark", "#7A9445"}, {"deep", "#5A7338"}}, 0.35)},
            {"summer", tones({{"fresh", "#A5B866"}, {"light", "#94AA59"}, {"base", "#789447"}, {"bunch", "#688548"}, {"dark", "#58743B"}, {"deep", "#3D5933"}}, 0.38)},
            {"autumn", tones({{"fresh", "#F0B24A"}, {"light", "#E08C33"}, {"base", "#C86A22"}, {"bunch", "#B3591D"}, {"dark", "#9E4818"}, {"deep", "#6E2E12"}}, 0.42)},
            {"lateAutumn", tones({{"fresh", "#E0713A"}, {"light", "#CF4A28"}, {"base", "#B02A1E"}, {"bunch", "#97211A"}, {"dark", "#7E1815"}, {"deep", "#520F10"}}, 0.46)}};
}

json barkPalette(const std::string& species)
{
    if (species == "sugi")
        return tones({{"light", "#8A6448"}, {"base", "#6B4A33"}, {"dark", "#4A3324"}, {"deep", "#33241A"}}, 0.72);
    if (species == "akamatsu")
        return tones({{"light", "#A87450"}, {"base", "#8A5236"}, {"dark", "#5F3823"}, {"deep", "#402518"}}, 0.68);
    return tones({{"light", "#756047"}, {"base", "#594A39"}, {"dark", "#40372E"}, {"deep", "#302B27"}}, 0.70);
}

double asNumber(const json& v)
{
    if (v.is_number())
        return v.get<double>();
    if (v.is_boolean())
        return v.get<bool>() ? 1.0 : 0.0;
    if (v.is_string())
        return std::stod(v.get<std::string>());
    throw std::invalid_argument("not a number");
}
} // namespace

const std::vector<std::string>& sections()
{
    static const std::vector<std::string> s{"global", "trunk", "blob", "leaf", "plane", "export"};
    return s;
}

const std::vector<Field>& fields()
{
    static const std::vector<Field> f = buildFields();
    return f;
}

const Field* field(const std::string& key)
{
    static const std::map<std::string, const Field*> index = [] {
        std::map<std::string, const Field*> m;
        for (const Field& f : fields())
            m[f.key()] = &f;
        return m;
    }();
    auto it = index.find(key);
    return it == index.end() ? nullptr : it->second;
}

json defaults()
{
    json j = json::object();
    for (const Field& f : fields())
        j[f.key()] = f.defaultValue;
    return j;
}

json coerce(const std::string& key, const json& value)
{
    const Field* f = field(key);
    if (!f)
        return value;
    try
    {
        switch (f->kind)
        {
        case Kind::Range:
        case Kind::LengthRange:
            if (value.is_array())
            {
                if (value.size() != 2)
                    return f->defaultValue;
                return json::array({asNumber(value[0]), asNumber(value[1])});
            }
            return json::array({asNumber(value), asNumber(value)});
        case Kind::Float:
        case Kind::Length:
            return asNumber(value);
        case Kind::Slider:
            return std::clamp(asNumber(value), f->fieldMin, f->fieldMax);
        case Kind::Int:
            return int64_t(std::llround(asNumber(value)));
        case Kind::Bool:
            if (value.is_boolean())
                return value.get<bool>();
            if (value.is_string())
                return !value.get<std::string>().empty();
            return asNumber(value) != 0.0;
        case Kind::String:
        case Kind::Folder:
            if (value.is_null())
                return "";
            return value.is_string() ? value.get<std::string>() : value.dump();
        case Kind::Enum:
        {
            std::string text = value.is_string() ? value.get<std::string>() : value.dump();
            return std::find(f->options.begin(), f->options.end(), text) != f->options.end() ? json(text) : f->defaultValue;
        }
        }
    }
    catch (const std::exception&)
    {
        return f->defaultValue;
    }
    return value;
}

json sanitize(const json& values)
{
    json result = defaults();
    if (values.is_object())
        for (const auto& [k, v] : values.items())
            if (field(k))
                result[k] = coerce(k, v);
    return result;
}

const std::vector<std::string>& builtInPresets()
{
    static const std::vector<std::string> names = [] {
        std::vector<std::string> n;
        for (const auto& [name, _] : presetTable())
            n.push_back(name);
        return n;
    }();
    return names;
}

bool isBuiltIn(const std::string& name)
{
    const auto& n = builtInPresets();
    return std::find(n.begin(), n.end(), name) != n.end();
}

const json& presetOverrides(const std::string& name)
{
    static const json empty = json::object();
    for (const auto& [n, v] : presetTable())
        if (n == name)
            return v;
    return empty;
}

json presetSettings(const std::string& name, const json& userPresets)
{
    if (userPresets.is_object() && userPresets.contains(name))
        return sanitize(userPresets[name]);
    return sanitize(presetOverrides(name));
}

std::vector<std::string> presetNames(const json& userPresets)
{
    std::vector<std::string> names = builtInPresets();
    std::vector<std::string> extra;
    if (userPresets.is_object())
        for (const auto& [k, _] : userPresets.items())
            if (!isBuiltIn(k))
                extra.push_back(k);
    std::sort(extra.begin(), extra.end());
    names.insert(names.end(), extra.begin(), extra.end());
    return names;
}

json stylize(const json& values, double amount)
{
    const double weight = std::max(0.0, amount);
    json styled = values;
    if (weight < 1e-9)
        return styled;
    const double bulk = 1.0 + 0.7 * weight;
    auto num = [&](const char* k) { return styled[k].get<double>(); };
    auto toward = [&](const char* k, double target, double rate) { styled[k] = num(k) + (target - num(k)) * std::min(1.0, rate * weight); };
    auto scalePair = [&](const char* k, double s) { styled[k] = json::array({styled[k][0].get<double>() * s, styled[k][1].get<double>() * s}); };

    styled["trunk.radius"] = num("trunk.radius") * (1.0 + 1.6 * weight);
    styled["trunk.length_ratio"] = num("trunk.length_ratio") * std::max(0.4, 1.0 - 0.15 * weight);
    styled["trunk.gnarl"] = num("trunk.gnarl") * std::max(0.0, 1.0 - 0.35 * weight);
    toward("trunk.radius_ratio", 0.85, 0.5);
    toward("trunk.taper", 0.55, 0.4);

    if (num("blob.blob_radius") > 1e-9)
        styled["blob.blob_radius"] = num("blob.blob_radius") * (1.0 + 0.4 * weight);

    if (num("leaf.size") > 1e-9)
        styled["leaf.size"] = num("leaf.size") * bulk;
    else
        scalePair("leaf.scale", bulk);
    scalePair("leaf.push", bulk);
    styled["leaf.min_distance"] = num("leaf.min_distance") * bulk;
    styled["leaf.count"] = std::max<int64_t>(1, std::llround(styled["leaf.count"].get<double>() / std::pow(bulk, 1.7)));
    json wrap = json::array();
    for (const json& v : styled["leaf.wrap"])
        wrap.push_back(v.get<double>() + (1.0 - v.get<double>()) * std::min(1.0, 0.35 * weight));
    styled["leaf.wrap"] = wrap;
    return styled;
}

json sectionValues(const json& values, const std::string& section)
{
    json out = json::object();
    for (const Field& f : fields())
    {
        if (f.section != section)
            continue;
        json v = values.contains(f.key()) ? values[f.key()] : f.defaultValue;
        if (f.kind == Kind::Length)
            v = v.get<double>() * kUnitsPerMetre;
        else if (f.kind == Kind::LengthRange)
            v = json::array({v[0].get<double>() * kUnitsPerMetre, v[1].get<double>() * kUnitsPerMetre});
        out[f.name] = v;
    }
    return out;
}

json paletteFor(const std::string& species)
{
    std::string s = species;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (s != "kaede" && s != "sugi" && s != "akamatsu")
        s = "kaede";
    return {{"leaves", leafPalette(s)}, {"bark", barkPalette(s)}};
}

std::string speciesOf(const std::string& preset)
{
    std::string first = preset.substr(0, preset.find(' '));
    std::transform(first.begin(), first.end(), first.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return first;
}
} // namespace treegen
