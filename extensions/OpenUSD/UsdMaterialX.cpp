// MaterialX shading in USD stages: Lila materials written as OpenPBR or Standard Surface networks, and MaterialX
// networks (OpenPBR, Standard Surface, glTF PBR, UsdPreviewSurface, unlit) read back into Lila materials.

#include "UsdMaterials.h"

#include <lila/Mesh.h>
#include <lila/Node.h>

#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/usdShade/connectableAPI.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/shader.h>

#include <algorithm>
#include <cmath>
#include <format>

PXR_NAMESPACE_USING_DIRECTIVE

namespace lila::usd
{
namespace mat = scene::material;

namespace mtlx
{
const char* surfaceName(Surface s)
{
    switch (s)
    {
    case Surface::OpenPbr: return "openPBR";
    case Surface::StandardSurface: return "standardSurface";
    default: return "none";
    }
}

bool parseSurface(std::string_view name, Surface* out)
{
    if (name == "openPBR" || name == "openpbr" || name == "open_pbr_surface")
        *out = Surface::OpenPbr;
    else if (name == "standardSurface" || name == "standard_surface")
        *out = Surface::StandardSurface;
    else if (name == "none" || name == "off")
        *out = Surface::None;
    else
        return false;
    return true;
}
} // namespace mtlx

namespace
{
/** The inputs of a MaterialX surface shader Lila maps, with the defaults its nodedef gives them. */
struct Model
{
    const char* id;
    const char* category;
    const char* baseColor;
    GfVec3f baseColorDefault;
    const char* baseWeight; // null: none
    float baseWeightDefault;
    const char* metalness;
    float metalnessDefault;
    const char* roughness;
    float roughnessDefault;
    const char* ior;
    const char* specularWeight;
    const char* normal;
    const char* occlusion;
    const char* emissionColor;
    GfVec3f emissionColorDefault;
    const char* emission; // the emission weight; null: none
    float emissionDefault;
    const char* opacity;
    /** Inputs Lila has no equivalent for, reported when connected or not zero. */
    std::vector<const char*> unsupported;
};

const Model kOpenPbr{"ND_open_pbr_surface_surfaceshader", "open_pbr_surface", "base_color", GfVec3f(0.8f), "base_weight", 1.0f,
                     "base_metalness", 0.0f, "specular_roughness", 0.3f, "specular_ior", "specular_weight", "geometry_normal",
                     nullptr, "emission_color", GfVec3f(1.0f), "emission_luminance", 0.0f, "geometry_opacity",
                     {"coat_weight", "transmission_weight", "subsurface_weight", "fuzz_weight", "thin_film_weight",
                      "specular_roughness_anisotropy", "base_diffuse_roughness", "geometry_thin_walled", "geometry_coat_normal"}};
const Model kStandard{"ND_standard_surface_surfaceshader", "standard_surface", "base_color", GfVec3f(1.0f), "base", 0.8f, "metalness",
                      0.0f, "specular_roughness", 0.2f, "specular_IOR", "specular", "normal", nullptr, "emission_color", GfVec3f(1.0f),
                      "emission", 0.0f, "opacity",
                      {"coat", "transmission", "subsurface", "sheen", "thin_film_thickness", "specular_anisotropy", "diffuse_roughness",
                       "thin_walled", "coat_normal"}};
const Model kGltf{"ND_gltf_pbr_surfaceshader", "gltf_pbr", "base_color", GfVec3f(1.0f), nullptr, 1.0f, "metallic", 1.0f, "roughness",
                  1.0f, "ior", "specular", "normal", "occlusion", "emissive", GfVec3f(0.0f), "emissive_strength", 1.0f, "alpha",
                  {"clearcoat", "transmission", "iridescence", "sheen_color", "anisotropy_strength", "dispersion", "clearcoat_normal"}};
const Model kPreview{"ND_UsdPreviewSurface_surfaceshader", "UsdPreviewSurface", "diffuseColor", GfVec3f(0.18f), nullptr, 1.0f, "metallic",
                     0.0f, "roughness", 0.5f, "ior", nullptr, "normal", "occlusion", "emissiveColor", GfVec3f(0.0f), nullptr, 1.0f, "opacity",
                     {"clearcoat", "displacement", "useSpecularWorkflow"}};

const Model* modelOf(std::string_view id)
{
    for (const Model* m : {&kOpenPbr, &kStandard, &kGltf, &kPreview})
        if (id == m->id)
            return m;
    // Versioned nodedefs (ND_standard_surface_surfaceshader_100) read as their family.
    for (const Model* m : {&kOpenPbr, &kStandard, &kGltf, &kPreview})
        if (id.starts_with(m->id))
            return m;
    return nullptr;
}

using mtlx::Input;
using mtlx::Network;

mtlx::Node& add(Network& net, std::string name, std::string category, std::string type, std::string nodedef)
{
    mtlx::Node& n = net.nodes.emplace_back();
    n.name = std::move(name);
    n.category = std::move(category);
    n.nodedef = std::move(nodedef);
    if (type != "multioutput")
        n.outputs.push_back({"out", type});
    n.type = std::move(type);
    return n;
}

Input& input(Network& net, const std::string& node, std::string name, std::string type)
{
    auto it = std::find_if(net.nodes.begin(), net.nodes.end(), [&](const mtlx::Node& n) { return n.name == node; });
    Input& in = it->inputs.emplace_back();
    in.name = std::move(name);
    in.type = std::move(type);
    return in;
}

void link(Network& net, const std::string& node, std::string name, std::string type, const std::string& from, const std::string& output = {})
{
    Input& in = input(net, node, std::move(name), std::move(type));
    in.node = from;
    in.output = output;
}

const char* addressMode(mat::TextureWrap w)
{
    switch (w)
    {
    case mat::TextureWrap::Clamp: return "clamp";
    case mat::TextureWrap::Mirror: return "mirror";
    case mat::TextureWrap::Black: return "constant";
    default: return "periodic";
    }
}

SdfValueTypeName sdfType(const std::string& t)
{
    if (t == "float")
        return SdfValueTypeNames->Float;
    if (t == "integer")
        return SdfValueTypeNames->Int;
    if (t == "boolean")
        return SdfValueTypeNames->Bool;
    if (t == "vector2")
        return SdfValueTypeNames->Float2;
    if (t == "vector3")
        return SdfValueTypeNames->Float3;
    if (t == "vector4")
        return SdfValueTypeNames->Float4;
    if (t == "color3")
        return SdfValueTypeNames->Color3f;
    if (t == "color4")
        return SdfValueTypeNames->Color4f;
    if (t == "string")
        return SdfValueTypeNames->String;
    if (t == "filename")
        return SdfValueTypeNames->Asset;
    return SdfValueTypeNames->Token; // surfaceshader and the other closures
}

VtValue usdValue(const Input& in)
{
    auto at = [&](size_t k) { return k < in.numbers.size() ? in.numbers[k] : 0.0f; };
    const std::string& t = in.type;
    if (t == "float")
        return VtValue(at(0));
    if (t == "integer")
        return VtValue(int(std::lround(at(0))));
    if (t == "boolean")
        return VtValue(at(0) != 0.0f);
    if (t == "vector2")
        return VtValue(GfVec2f(at(0), at(1)));
    if (t == "vector3" || t == "color3")
        return VtValue(GfVec3f(at(0), at(1), at(2)));
    if (t == "vector4" || t == "color4")
        return VtValue(GfVec4f(at(0), at(1), at(2), at(3)));
    if (t == "filename")
        return VtValue(SdfAssetPath(in.text));
    return VtValue(in.text);
}
} // namespace

namespace mtlx
{
void writeUsd(const Network& net, const UsdShadeMaterial& m)
{
    const UsdStagePtr stage = m.GetPrim().GetStage();
    std::map<std::string, UsdShadeShader> shaders;
    for (const Node& node : net.nodes)
    {
        UsdShadeShader s = UsdShadeShader::Define(stage, m.GetPath().AppendChild(TfToken(node.name)));
        s.CreateIdAttr(VtValue(TfToken(node.nodedef)));
        for (const auto& [name, type] : node.outputs)
            s.CreateOutput(TfToken(name), sdfType(type));
        shaders[node.name] = s;
    }
    for (const Node& node : net.nodes)
        for (const Input& in : node.inputs)
        {
            UsdShadeInput i = shaders[node.name].CreateInput(TfToken(in.name), sdfType(in.type));
            if (!in.node.empty())
                i.ConnectToSource(shaders[in.node].ConnectableAPI(), TfToken(in.output.empty() ? "out" : in.output));
            else
                i.Set(usdValue(in));
            if (!in.colorSpace.empty())
                i.GetAttr().SetColorSpace(TfToken(in.colorSpace));
        }
    m.CreateSurfaceOutput(TfToken("mtlx")).ConnectToSource(shaders[net.surface].ConnectableAPI(), TfToken("out"));
}
} // namespace mtlx

// Export

mtlx::Network MaterialWriter::network(const std::string& slot, mtlx::Surface surface, bool usd)
{
    Network net;
    net.surface = "MtlxSurface";
    const Model& model = surface == mtlx::Surface::StandardSurface ? kStandard : kOpenPbr;
    const bool standard = surface == mtlx::Surface::StandardSurface;
    const scene::Node* n = scene::materials::find(mGraph, slot);
    const bool unlit = n && mat::ShadingModel(n->as<int64_t>(mat::kShadingModel)) == mat::ShadingModel::Unlit;
    if (unlit)
        add(net, net.surface, "surface_unlit", "surfaceshader", "ND_surface_unlit");
    else
        add(net, net.surface, model.category, "surfaceshader", model.id);
    auto value = [&](const char* name, const char* type, std::vector<float> v) { input(net, net.surface, name, type).numbers = std::move(v); };
    auto connect = [&](const char* name, const char* type, const std::string& from, const std::string& output = {}) {
        link(net, net.surface, name, type, from, output);
    };
    if (standard && !unlit)
        value("base", "float", {1.0f});
    if (!n)
    {
        value(model.baseColor, "color3", {0.5f, 0.5f, 0.5f});
        value(model.roughness, "float", {0.55f});
        return net;
    }
    // Warnings the UsdPreviewSurface written beside this network already gives are left to it.
    const bool alone = !mPreview || !mStage;

    const int uvSet = int(std::max<int64_t>(0, n->as<int64_t>(mat::kUvSet)));
    std::string texcoord;
    auto uv = [&]() -> std::string {
        if (uvSet == 0 || !texcoord.empty())
            return texcoord;
        texcoord = "MtlxTexcoord";
        if (usd)
        {
            add(net, texcoord, "geompropvalue", "vector2", "ND_geompropvalue_vector2");
            input(net, texcoord, "geomprop", "string").text = uvPrimvarName(uvSet);
        }
        else
        {
            add(net, texcoord, "texcoord", "vector2", "ND_texcoord_vector2");
            input(net, texcoord, "index", "integer").numbers = {float(uvSet)};
        }
        return texcoord;
    };
    std::map<std::string, std::string> images; // file, type and sampling -> image node
    auto image = [&](const std::string& lilaPath, int mapAttr, const char* role, const std::string& type) -> std::string {
        const std::string asset = assetPath(lilaPath);
        const mat::TextureSampling s = scene::materials::sampling(*n, mapAttr);
        const bool srgb = scene::materials::srgb(*n, mapAttr);
        const std::string key = std::format("{}|{}|{}|{}|{}|{}", asset, type, int(s.wrapU), int(s.wrapV), int(s.filter), srgb);
        if (auto it = images.find(key); it != images.end())
            return it->second;
        const std::string name = std::string("MtlxTex_") + role;
        add(net, name, "image", type, "ND_image_" + type);
        Input& file = input(net, name, "file", "filename");
        file.text = asset;
        if (srgb)
            file.colorSpace = "srgb_texture";
        if (s.wrapU != mat::TextureWrap::Repeat)
            input(net, name, "uaddressmode", "string").text = addressMode(s.wrapU);
        if (s.wrapV != mat::TextureWrap::Repeat)
            input(net, name, "vaddressmode", "string").text = addressMode(s.wrapV);
        if (s.filter == mat::TextureFilter::Nearest)
            input(net, name, "filtertype", "string").text = "closest";
        if (const std::string t = uv(); !t.empty())
            link(net, name, "texcoord", "vector2", t);
        images[key] = name;
        return name;
    };

    const auto blend = mat::BlendMode(n->as<int64_t>(mat::kBlendMode));
    const bool alpha = blend != mat::BlendMode::Opaque;
    const float opacity = float(n->as<double>(mat::kOpacity));
    const glm::dvec3 base = n->as<glm::dvec3>(mat::kBaseColor);
    std::string color, alphaNode;
    if (const std::string& baseMap = n->as<std::string>(mat::kBaseColorMap); !baseMap.empty())
    {
        if (alpha)
        {
            const std::string img = image(baseMap, mat::kBaseColorMap, "baseColor", "color4");
            color = "MtlxBaseColorRgb";
            add(net, color, "convert", "color3", "ND_convert_color4_color3");
            link(net, color, "in", "color4", img);
            alphaNode = "MtlxAlpha";
            add(net, alphaNode, "extract", "float", "ND_extract_color4");
            link(net, alphaNode, "in", "color4", img);
            input(net, alphaNode, "index", "integer").numbers = {3.0f};
            if (opacity != 1.0f)
            {
                add(net, "MtlxAlphaScale", "multiply", "float", "ND_multiply_float");
                link(net, "MtlxAlphaScale", "in1", "float", alphaNode);
                input(net, "MtlxAlphaScale", "in2", "float").numbers = {opacity};
                alphaNode = "MtlxAlphaScale";
            }
        }
        else
            color = image(baseMap, mat::kBaseColorMap, "baseColor", "color3");
    }
    auto writeOpacity = [&](const char* name, bool asColor) {
        if (!alpha)
            return;
        if (alphaNode.empty())
        {
            if (asColor)
                value(name, "color3", {opacity, opacity, opacity});
            else
                value(name, "float", {opacity});
            return;
        }
        if (!asColor)
            return connect(name, "float", alphaNode);
        add(net, "MtlxOpacityRgb", "convert", "color3", "ND_convert_float_color3");
        link(net, "MtlxOpacityRgb", "in", "float", alphaNode);
        connect(name, "color3", "MtlxOpacityRgb");
    };
    const std::vector<float> baseValue{float(base.x), float(base.y), float(base.z)};

    if (unlit)
    {
        if (!color.empty())
            connect("emission_color", "color3", color);
        else
            value("emission_color", "color3", baseValue);
        writeOpacity("opacity", false);
        if (alone && !n->as<std::string>(mat::kEmissiveMap).empty())
            warnings.push_back(std::format("material {}: MaterialX's unlit surface has no room for an emissive map; left out", slot));
        return net;
    }
    if (!color.empty())
        connect(model.baseColor, "color3", color);
    else
        value(model.baseColor, "color3", baseValue);

    if (const std::string& orm = n->as<std::string>(mat::kOrmMap); !orm.empty())
    {
        static const char* kOut[3] = {"outx", "outy", "outz"};
        const std::array<int, 3> ch = scene::materials::packedChannels(mat::PackedLayout(n->as<int64_t>(mat::kPackedLayout)));
        const std::string img = image(orm, mat::kOrmMap, "orm", "vector3");
        mtlx::Node& split = add(net, "MtlxOrmChannels", "separate3", "multioutput", "ND_separate3_vector3");
        for (const char* o : kOut)
            split.outputs.push_back({o, "float"});
        link(net, "MtlxOrmChannels", "in", "vector3", img);
        connect(model.roughness, "float", "MtlxOrmChannels", kOut[ch[1]]);
        connect(model.metalness, "float", "MtlxOrmChannels", kOut[ch[2]]);
    }
    else
    {
        if (const std::string& p = n->as<std::string>(mat::kMetallicMap); !p.empty())
            connect(model.metalness, "float", image(p, mat::kMetallicMap, "metallic", "float"));
        else
            value(model.metalness, "float", {float(n->as<double>(mat::kMetallic))});
        if (const std::string& p = n->as<std::string>(mat::kRoughnessMap); !p.empty())
            connect(model.roughness, "float", image(p, mat::kRoughnessMap, "roughness", "float"));
        else
            value(model.roughness, "float", {float(n->as<double>(mat::kRoughness))});
        if (alone && !n->as<std::string>(mat::kAmbientOcclusionMap).empty())
            warnings.push_back(std::format("material {}: {} has no occlusion input; its occlusion map is left out", slot, model.category));
    }
    if (const double s = n->as<double>(mat::kSpecular); std::abs(s - 0.5) > 1e-6)
        value(model.ior, "float", {float(iorOfSpecular(s))});

    if (const std::string& p = n->as<std::string>(mat::kNormalMap); !p.empty())
    {
        std::string src = image(p, mat::kNormalMap, "normal", "vector3");
        // MaterialX reads normal maps OpenGL style; a DirectX map's green becomes 1 - g on the way in.
        if (mat::NormalFormat(n->as<int64_t>(mat::kNormalFormat)) == mat::NormalFormat::DirectX)
        {
            add(net, "MtlxNormalFlip", "multiply", "vector3", "ND_multiply_vector3");
            link(net, "MtlxNormalFlip", "in1", "vector3", src);
            input(net, "MtlxNormalFlip", "in2", "vector3").numbers = {1.0f, -1.0f, 1.0f};
            add(net, "MtlxNormalGreen", "add", "vector3", "ND_add_vector3");
            link(net, "MtlxNormalGreen", "in1", "vector3", "MtlxNormalFlip");
            input(net, "MtlxNormalGreen", "in2", "vector3").numbers = {0.0f, 1.0f, 0.0f};
            src = "MtlxNormalGreen";
        }
        add(net, "MtlxNormalMap", "normalmap", "vector3", "ND_normalmap_float");
        link(net, "MtlxNormalMap", "in", "vector3", src);
        connect(model.normal, "vector3", "MtlxNormalMap");
    }

    const float intensity = float(n->as<double>(mat::kEmissiveIntensity));
    if (const std::string& p = n->as<std::string>(mat::kEmissiveMap); !p.empty())
    {
        connect(model.emissionColor, "color3", image(p, mat::kEmissiveMap, "emissive", "color3"));
        value(model.emission, "float", {intensity});
    }
    else if (const glm::dvec3 e = n->as<glm::dvec3>(mat::kEmissiveColor); e * double(intensity) != glm::dvec3(0.0))
    {
        value(model.emissionColor, "color3", {float(e.x), float(e.y), float(e.z)});
        value(model.emission, "float", {intensity});
    }
    writeOpacity(model.opacity, standard);
    if (alone && mat::ShadingModel(n->as<int64_t>(mat::kShadingModel)) != mat::ShadingModel::DefaultLit)
        warnings.push_back(std::format("material {}: its {} shading is kept for Lila only; other tools see the default lit inputs", slot,
                                       n->type().attrs[mat::kShadingModel].enumNames[size_t(n->as<int64_t>(mat::kShadingModel))]));
    return net;
}

// Import

namespace
{
TfToken idOf(const UsdShadeShader& s)
{
    TfToken id;
    if (s)
        s.GetShaderId(&id);
    return id;
}

/** A numeric value as up to four floats. */
bool numbers(const UsdAttribute& a, GfVec4f* out, int* width)
{
    VtValue v;
    if (!a.Get(&v) || v.IsEmpty())
        return false;
    if (v.IsHolding<float>())
        *out = GfVec4f(v.UncheckedGet<float>(), 0.0f, 0.0f, 0.0f), *width = 1;
    else if (v.IsHolding<double>())
        *out = GfVec4f(float(v.UncheckedGet<double>()), 0.0f, 0.0f, 0.0f), *width = 1;
    else if (v.IsHolding<int>())
        *out = GfVec4f(float(v.UncheckedGet<int>()), 0.0f, 0.0f, 0.0f), *width = 1;
    else if (v.IsHolding<bool>())
        *out = GfVec4f(v.UncheckedGet<bool>() ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f), *width = 1;
    else if (v.IsHolding<GfVec2f>())
        *out = GfVec4f(v.UncheckedGet<GfVec2f>()[0], v.UncheckedGet<GfVec2f>()[1], 0.0f, 0.0f), *width = 2;
    else if (v.IsHolding<GfVec3f>())
    {
        const GfVec3f c = v.UncheckedGet<GfVec3f>();
        *out = GfVec4f(c[0], c[1], c[2], 0.0f), *width = 3;
    }
    else if (v.IsHolding<GfVec4f>())
        *out = v.UncheckedGet<GfVec4f>(), *width = 4;
    else
        return false;
    return true;
}

/**
 * What a shader input amounts to once the nodes upstream are folded: a constant, or one texture's channels times a
 * scale plus a bias, or something Lila can't map (why says what).
 */
struct Signal
{
    enum Kind
    {
        Unset,
        Value,
        Texture,
        Unmapped,
    } kind = Unset;
    GfVec4f value{0.0f};
    int width = 1;
    UsdShadeShader image;
    std::string channels; // the texture's channels read, in order: "rgb", "a"
    GfVec4f scale{1.0f}, bias{0.0f};
    std::string why;

    static Signal constant(float v)
    {
        Signal s;
        s.kind = Value;
        s.value = GfVec4f(v, 0.0f, 0.0f, 0.0f);
        return s;
    }
    static Signal unmapped(std::string why)
    {
        Signal s;
        s.kind = Unmapped;
        s.why = std::move(why);
        return s;
    }
    Signal orElse(float v) const { return kind == Unset ? constant(v) : *this; }
    Signal orElse(const GfVec3f& c) const
    {
        if (kind != Unset)
            return *this;
        Signal s = constant(c[0]);
        s.value = GfVec4f(c[0], c[1], c[2], 0.0f);
        s.width = 3;
        return s;
    }
    float scalar() const { return value[0]; }
    GfVec3f rgb() const { return width == 1 ? GfVec3f(value[0]) : GfVec3f(value[0], value[1], value[2]); }
};

/** Channels in the type a nodedef name ends with: ND_image_color3 is 3. */
int widthOf(std::string_view id)
{
    if (id.ends_with("color4") || id.ends_with("vector4"))
        return 4;
    if (id.ends_with("color3") || id.ends_with("vector3"))
        return 3;
    if (id.ends_with("vector2"))
        return 2;
    return 1;
}

Signal pick(Signal s, int index)
{
    if (s.kind == Signal::Texture)
    {
        if (index < 0 || index >= int(s.channels.size()))
            return Signal::unmapped(std::format("channel {} of a {}-channel texture", index, s.channels.size()));
        s.channels = s.channels.substr(size_t(index), 1);
        s.scale = GfVec4f(s.scale[index], 1.0f, 1.0f, 1.0f);
        s.bias = GfVec4f(s.bias[index], 0.0f, 0.0f, 0.0f);
    }
    else if (s.kind == Signal::Value)
    {
        s.value = GfVec4f(s.width == 1 ? s.value[0] : s.value[std::clamp(index, 0, 3)], 0.0f, 0.0f, 0.0f);
        s.width = 1;
    }
    return s;
}

/** A convert node: a texture or value narrowed (color4 to color3) or a single channel spread (float to color3). */
Signal convert(Signal s, int width)
{
    if (s.kind == Signal::Texture)
    {
        if (int(s.channels.size()) > width)
            s.channels.resize(size_t(width));
        else if (s.channels.size() == 1 && width > 1)
        {
            s.channels = std::string(size_t(width), s.channels[0]);
            s.scale = GfVec4f(s.scale[0]);
            s.bias = GfVec4f(s.bias[0]);
        }
    }
    else if (s.kind == Signal::Value)
    {
        if (s.width == 1 && width > 1)
            s.value = GfVec4f(s.value[0]);
        s.width = width;
    }
    return s;
}

Signal arith(Signal a, Signal b, bool multiply, const std::string& id)
{
    if (a.kind == Signal::Unmapped)
        return a;
    if (b.kind == Signal::Unmapped)
        return b;
    if (a.kind == Signal::Texture && b.kind == Signal::Texture)
        return Signal::unmapped(std::format("a {} node combining two textures", id));
    if (a.kind == Signal::Value && b.kind == Signal::Value)
    {
        Signal r = a;
        r.width = std::max(a.width, b.width);
        for (int k = 0; k < 4; ++k)
        {
            const float x = a.width == 1 ? a.value[0] : a.value[k], y = b.width == 1 ? b.value[0] : b.value[k];
            r.value[k] = multiply ? x * y : x + y;
        }
        return r;
    }
    Signal t = a.kind == Signal::Texture ? a : b;
    const Signal& k = a.kind == Signal::Texture ? b : a;
    for (size_t c = 0; c < t.channels.size() && c < 4; ++c)
    {
        const float v = k.width == 1 ? k.value[0] : k.value[int(c)];
        if (multiply)
        {
            t.scale[int(c)] *= v;
            t.bias[int(c)] *= v;
        }
        else
            t.bias[int(c)] += v;
    }
    return t;
}

Signal resolve(const UsdShadeInput& in, int depth = 0);

/** What a shader's output amounts to. */
Signal fold(const UsdShadeShader& s, const std::string& output, int depth)
{
    const std::string id = idOf(s).GetString();
    auto in = [&](const char* name) { return resolve(s.GetInput(TfToken(name)), depth + 1); };
    auto starts = [&](std::string_view p) { return id.starts_with(p); };
    if (starts("ND_image_") || starts("ND_tiledimage_") || starts("ND_gltf_image_") || starts("ND_UsdUVTexture") || id == "UsdUVTexture")
    {
        Signal t;
        t.kind = Signal::Texture;
        t.image = s;
        if (id.find("UsdUVTexture") != std::string::npos)
        {
            GfVec4f scale(1.0f), bias(0.0f);
            if (UsdShadeInput i = s.GetInput(TfToken("scale")))
                i.Get(&scale);
            if (UsdShadeInput i = s.GetInput(TfToken("bias")))
                i.Get(&bias);
            static const std::string kAll = "rgba";
            t.channels = output == "rgb" ? "rgb" : output;
            for (size_t c = 0; c < t.channels.size(); ++c)
            {
                const size_t from = kAll.find(t.channels[c]);
                if (from == std::string::npos)
                    return Signal::unmapped("UsdUVTexture output " + output);
                t.scale[int(c)] = scale[int(from)];
                t.bias[int(c)] = bias[int(from)];
            }
            return t;
        }
        static const char* kChannels[5] = {"", "r", "rg", "rgb", "rgba"};
        t.channels = kChannels[widthOf(id)];
        if (starts("ND_gltf_image_"))
        {
            Signal factor = in("factor");
            if (factor.kind == Signal::Value)
                t = arith(t, factor, true, id);
        }
        return t;
    }
    if (starts("ND_constant_"))
        return in("value");
    if (starts("ND_dot_") || starts("ND_srgb_texture_to_") || starts("ND_lin_") || starts("ND_g22_") || starts("ND_g18_"))
        return in("in");
    if (starts("ND_convert_"))
        return convert(in("in"), widthOf(id));
    if (starts("ND_extract_"))
    {
        const Signal index = in("index").orElse(0.0f);
        if (index.kind != Signal::Value)
            return Signal::unmapped("an extract node with a connected index");
        return pick(in("in"), int(std::lround(index.scalar())));
    }
    if (starts("ND_separate"))
    {
        static const std::string kNames = "rgbaxyzw";
        const size_t k = output.empty() ? std::string::npos : kNames.find(output.back());
        if (k == std::string::npos)
            return Signal::unmapped(std::format("{} output {}", id, output));
        return pick(in("in"), int(k % 4));
    }
    if (starts("ND_multiply_"))
        return arith(in("in1").orElse(0.0f), in("in2").orElse(1.0f), true, id);
    if (starts("ND_add_"))
        return arith(in("in1").orElse(0.0f), in("in2").orElse(0.0f), false, id);
    if (starts("ND_subtract_"))
        return arith(in("in1").orElse(0.0f), arith(in("in2").orElse(0.0f), Signal::constant(-1.0f), true, id), false, id);
    if (starts("ND_mix_"))
    {
        const Signal mix = in("mix").orElse(0.0f);
        if (mix.kind != Signal::Value)
            return mix.kind == Signal::Unmapped ? mix : Signal::unmapped("a mix node driven by a texture");
        const float m = mix.scalar();
        const Signal fg = in("fg").orElse(0.0f), bg = in("bg").orElse(0.0f);
        if (m >= 1.0f)
            return fg;
        if (m <= 0.0f)
            return bg;
        return arith(arith(fg, Signal::constant(m), true, id), arith(bg, Signal::constant(1.0f - m), true, id), false, id);
    }
    if (starts("ND_normalmap"))
        return in("in");
    return Signal::unmapped(std::format("a {} node", id.empty() ? s.GetPrim().GetTypeName().GetString() : id));
}

Signal resolve(const UsdShadeInput& in, int depth)
{
    Signal s;
    if (!in)
        return s;
    if (depth > 64)
        return Signal::unmapped("a network too deep to follow");
    for (const UsdAttribute& a : in.GetValueProducingAttributes())
    {
        if (UsdShadeOutput::IsOutput(a))
            return fold(UsdShadeShader(a.GetPrim()), UsdShadeOutput(a).GetBaseName().GetString(), depth);
        if (numbers(a, &s.value, &s.width))
        {
            s.kind = Signal::Value;
            return s;
        }
    }
    return s;
}

template <class T> bool valueOf(const UsdShadeShader& s, const char* name, T* out)
{
    const UsdShadeInput in = s.GetInput(TfToken(name));
    if (!in)
        return false;
    for (const UsdAttribute& a : in.GetValueProducingAttributes())
        if (UsdShadeInput::IsInput(a) && a.Get(out))
            return true;
    return false;
}

/** An image node's file as authored, to tell textures apart. */
std::string fileOf(const UsdShadeShader& image)
{
    SdfAssetPath a;
    return image && valueOf(image, "file", &a) ? a.GetAssetPath() : std::string();
}

/** Wrap and filter of a MaterialX image node or a UsdUVTexture. */
mat::TextureSampling imageSampling(const UsdShadeShader& image)
{
    mat::TextureSampling s;
    const bool preview = idOf(image).GetString().find("UsdUVTexture") != std::string::npos;
    auto wrap = [&](const char* name) {
        std::string mode;
        if (TfToken t; preview && valueOf(image, name, &t))
            mode = t.GetString();
        else if (!preview)
            valueOf(image, name, &mode);
        if (mode == "clamp")
            return mat::TextureWrap::Clamp;
        if (mode == "mirror")
            return mat::TextureWrap::Mirror;
        if (mode == "constant" || mode == "black")
            return mat::TextureWrap::Black;
        return mat::TextureWrap::Repeat;
    };
    s.wrapU = wrap(preview ? "wrapS" : "uaddressmode");
    s.wrapV = wrap(preview ? "wrapT" : "vaddressmode");
    std::string filter;
    if (TfToken t; preview && valueOf(image, "magFilter", &t))
        filter = t.GetString();
    else if (!preview)
        valueOf(image, "filtertype", &filter);
    if (filter == "closest" || filter == "nearest")
        s.filter = mat::TextureFilter::Nearest;
    return s;
}

UsdShadeShader upstream(const UsdShadeInput& in)
{
    if (in)
        for (const UsdAttribute& a : in.GetValueProducingAttributes())
            if (UsdShadeOutput::IsOutput(a))
                return UsdShadeShader(a.GetPrim());
    return UsdShadeShader();
}

std::string describe(const Signal& s)
{
    if (s.width == 1)
        return std::format("{}", s.value[0]);
    std::string out = "(";
    for (int k = 0; k < s.width; ++k)
        out += std::format("{}{}", k ? ", " : "", s.value[k]);
    return out + ")";
}
} // namespace

void MaterialReader::readMtlx(const UsdShadeShader& surface, scene::NodeId id, const std::string& name, bool own)
{
    using namespace scene;
    auto set = [&](int attr, PlugValue v) { mGraph.setValue({id, attr, -1}, std::move(v)); };
    const std::string surfaceId = idOf(surface).GetString();
    auto input = [&](const char* inputName) { return inputName ? resolve(surface.GetInput(TfToken(inputName))) : Signal(); };
    auto skip = [&](const Signal& s, const char* inputName) {
        skipped.push_back(std::format("material {}: {} comes from {}; left at its default", name, inputName, s.why));
    };

    int uvSet = -1;
    auto noteUv = [&](const UsdShadeShader& image) {
        const bool preview = idOf(image).GetString().find("UsdUVTexture") != std::string::npos;
        UsdShadeShader src = upstream(image.GetInput(TfToken(preview ? "st" : "texcoord")));
        int set = 0;
        for (int hops = 0; src && hops < 8; ++hops)
        {
            const std::string kind = idOf(src).GetString();
            if (kind.starts_with("ND_texcoord_"))
            {
                int index = 0;
                valueOf(src, "index", &index);
                set = index;
                break;
            }
            std::string var;
            if (TfToken t; valueOf(src, kind.starts_with("ND_geompropvalue") ? "geomprop" : "varname", &t))
                var = t.GetString();
            else
                valueOf(src, kind.starts_with("ND_geompropvalue") ? "geomprop" : "varname", &var);
            if (kind.starts_with("ND_geompropvalue") || kind.find("PrimvarReader") != std::string::npos)
            {
                set = uvSetOfPrimvar(var);
                if (set < 0)
                {
                    warnings.push_back(std::format("material {}: textures read the primvar {}; Lila reads it as UV set 0", name, var));
                    set = 0;
                }
                break;
            }
            if (kind.starts_with("ND_place2d") || kind.starts_with("ND_multiply_vector2") || kind.starts_with("ND_add_vector2") ||
                kind.starts_with("ND_rotate2d") || kind.find("Transform2d") != std::string::npos)
            {
                warnings.push_back(std::format("material {}: a {} node transforms the UVs; Lila materials have no UV transform", name, kind));
                UsdShadeShader next;
                for (const char* in : {"texcoord", "in", "in1"})
                    if ((next = upstream(src.GetInput(TfToken(in)))))
                        break;
                src = next;
                continue;
            }
            warnings.push_back(std::format("material {}: texture coordinates come from a {} node; read as UV set 0", name, kind));
            break;
        }
        if (uvSet >= 0 && set != uvSet)
            warnings.push_back(std::format("material {}: textures read different UV sets; Lila uses one per material (uv{})", name, uvSet));
        else
            uvSet = set;
    };
    auto useTexture = [&](const Signal& t, int mapAttr, const char* role, bool color) {
        noteUv(t.image);
        const std::string kind = idOf(t.image).GetString();
        if (kind.starts_with("ND_tiledimage_"))
        {
            GfVec2f tiling(1.0f), offset(0.0f);
            valueOf(t.image, "uvtiling", &tiling);
            valueOf(t.image, "uvoffset", &offset);
            if (tiling != GfVec2f(1.0f) || offset != GfVec2f(0.0f))
                warnings.push_back(std::format("material {}: the {} texture's tiling ({} {}) and offset ({} {}) are not kept", name, role,
                                               tiling[0], tiling[1], offset[0], offset[1]));
        }
        std::string space;
        if (const UsdShadeInput file = t.image.GetInput(TfToken("file")))
            space = file.GetAttr().GetColorSpace().GetString();
        if (TfToken cs; space.empty() && valueOf(t.image, "sourceColorSpace", &cs) && cs != "auto")
            space = cs == "sRGB" ? "srgb_texture" : cs.GetString();
        const bool srgb = space.find("srgb") != std::string::npos;
        mat::TextureSampling s = imageSampling(t.image);
        if (color && !space.empty() && !srgb)
        {
            s.colorSpace = mat::TextureColorSpace::Linear;
            if (space != "lin_rec709" && space != "raw")
                warnings.push_back(std::format("material {}: {} texture is in {}; Lila reads it as linear Rec.709", name, role, space));
        }
        else if (!color && srgb)
            s.colorSpace = mat::TextureColorSpace::Srgb;
        set(mapAttr, texturePath(t.image, name));
        materials::setSampling(mGraph, id, mapAttr, s);
    };
    auto plain = [&](const Signal& t, const char* role) {
        const size_t c = std::min<size_t>(t.channels.size(), 3);
        for (size_t k = 0; k < c; ++k)
            if (std::abs(t.scale[int(k)] - 1.0f) > 1e-5f || std::abs(t.bias[int(k)]) > 1e-5f)
            {
                warnings.push_back(std::format("material {}: the {} texture's scale and bias are not kept", name, role));
                return;
            }
    };
    // Opacity from a texture is the base color map's alpha in Lila.
    auto readOpacity = [&](Signal o, const Signal& base, bool* translucent) {
        if (o.kind == Signal::Unmapped)
            return skip(o, "opacity");
        if (o.kind == Signal::Texture)
        {
            o = pick(o, 0);
            if (!(base.kind == Signal::Texture && fileOf(o.image) == fileOf(base.image) && o.channels == "a"))
                warnings.push_back(std::format("material {}: opacity comes from its own texture; Lila takes it from the base color map's alpha", name));
            set(material::kOpacity, double(std::clamp(o.scale[0], 0.0f, 1.0f)));
            *translucent = true;
        }
        else if (o.kind == Signal::Value && o.scalar() < 1.0f)
        {
            set(material::kOpacity, double(std::max(0.0f, o.scalar())));
            *translucent = true;
        }
    };

    if (surfaceId == "ND_surface_unlit")
    {
        set(material::kShadingModel, int64_t(material::ShadingModel::Unlit));
        const Signal weight = input("emission").orElse(1.0f), color = input("emission_color").orElse(GfVec3f(1.0f));
        if (color.kind == Signal::Texture)
        {
            useTexture(color, material::kBaseColorMap, "base color", true);
            if (weight.kind == Signal::Value && weight.scalar() != 1.0f)
                warnings.push_back(std::format("material {}: its emission weight {} is not kept with a texture", name, weight.scalar()));
            plain(color, "base color");
        }
        else if (color.kind == Signal::Value && weight.kind == Signal::Value)
        {
            const GfVec3f c = color.rgb() * weight.scalar();
            set(material::kBaseColor, glm::dvec3(c[0], c[1], c[2]));
        }
        else
            skip(color.kind == Signal::Unmapped ? color : weight, color.kind == Signal::Unmapped ? "emission_color" : "emission");
        bool translucent = false;
        readOpacity(input("opacity"), color, &translucent);
        if (translucent)
            set(material::kBlendMode, int64_t(material::BlendMode::Translucent));
        if (const Signal t = input("transmission"); t.kind == Signal::Texture || (t.kind == Signal::Value && t.scalar() != 0.0f))
            skipped.push_back(std::format("material {}: transmission has no Lila equivalent; left out", name));
        if (uvSet > 0)
            set(material::kUvSet, int64_t(uvSet));
        return;
    }

    const Model* model = modelOf(surfaceId);
    if (!model)
    {
        skipped.push_back(std::format("material {}: its MaterialX surface {} has no Lila mapping; imported with default settings", name, surfaceId));
        return;
    }
    std::set<std::string> handled;
    for (const char* in : {model->baseColor, model->baseWeight, model->metalness, model->roughness, model->ior, model->specularWeight,
                           model->normal, model->occlusion, model->emissionColor, model->emission, model->opacity})
        if (in)
            handled.insert(in);

    // Base color, times its weight.
    const Signal base = input(model->baseColor).orElse(model->baseColorDefault);
    const Signal baseWeight = input(model->baseWeight).orElse(model->baseWeightDefault);
    if (base.kind == Signal::Texture)
    {
        useTexture(base, material::kBaseColorMap, "base color", true);
        if (baseWeight.kind == Signal::Value && baseWeight.scalar() != 1.0f)
            warnings.push_back(std::format("material {}: the base weight {} is not kept with a base color texture", name, baseWeight.scalar()));
        plain(base, "base color");
    }
    else if (base.kind == Signal::Value)
    {
        const GfVec3f c = base.rgb() * (baseWeight.kind == Signal::Value ? baseWeight.scalar() : 1.0f);
        set(material::kBaseColor, glm::dvec3(c[0], c[1], c[2]));
    }
    else
        skip(base, model->baseColor);
    if (baseWeight.kind == Signal::Texture || baseWeight.kind == Signal::Unmapped)
        skipped.push_back(std::format("material {}: {} is driven by a network; not kept", name, model->baseWeight));

    // Metallic, roughness and occlusion: one texture feeding them in a known channel order is a packed map.
    const Signal metal = input(model->metalness).orElse(model->metalnessDefault);
    const Signal rough = input(model->roughness).orElse(model->roughnessDefault);
    const Signal occ = input(model->occlusion);
    auto texFile = [&](const Signal& s) { return s.kind == Signal::Texture ? fileOf(s.image) : std::string(); };
    const std::string occFile = texFile(occ), roughFile = texFile(rough), metalFile = texFile(metal);
    std::string packedFile;
    int shared = 0;
    for (const std::string* f : {&occFile, &roughFile, &metalFile})
        if (!f->empty() && ((*f == occFile) + (*f == roughFile) + (*f == metalFile)) >= 2)
        {
            packedFile = *f;
            shared = (occFile == packedFile) + (roughFile == packedFile) + (metalFile == packedFile);
            break;
        }
    bool packed = false;
    if (shared >= 2)
    {
        for (auto layout : {material::PackedLayout::ORM, material::PackedLayout::MRO})
        {
            static const char kChannel[3] = {'r', 'g', 'b'};
            const std::array<int, 3> ch = materials::packedChannels(layout);
            const std::pair<const Signal*, const std::string*> uses[3] = {{&occ, &occFile}, {&rough, &roughFile}, {&metal, &metalFile}};
            bool fits = true;
            for (int k = 0; k < 3; ++k)
                if (*uses[k].second == packedFile)
                    fits = fits && uses[k].first->channels == std::string(1, kChannel[ch[size_t(k)]]);
            if (!fits)
                continue;
            const Signal& any = occFile == packedFile ? occ : roughFile == packedFile ? rough : metal;
            useTexture(any, material::kOrmMap, "packed", false);
            set(material::kPackedLayout, int64_t(layout));
            packed = true;
            // Surfaces without an occlusion input (OpenPBR, Standard Surface) can only use two of the three.
            if (shared < 3 && model->occlusion && !own)
                warnings.push_back(std::format("material {}: a packed map feeds two of occlusion, roughness and metallic; Lila reads all three "
                                               "from it",
                                               name));
            break;
        }
        if (!packed)
            warnings.push_back(std::format("material {}: occlusion, roughness and metallic share a texture in an order Lila has no layout "
                                           "for; each is read as its own map's red channel",
                                           name));
    }
    auto single = [&](const Signal& s, const char* inputName, int mapAttr, int valueAttr, const char* role) {
        if (s.kind == Signal::Texture && !packed)
        {
            if (s.channels.size() != 1 || s.channels != "r")
                warnings.push_back(std::format("material {}: the {} texture is read from its {} channel; Lila reads red", name, role, s.channels));
            useTexture(s, mapAttr, role, false);
            plain(s, role);
        }
        else if (s.kind == Signal::Value && valueAttr >= 0)
            set(valueAttr, std::clamp(double(s.scalar()), 0.0, 1.0));
        else if (s.kind == Signal::Unmapped)
            skip(s, inputName);
    };
    single(metal, model->metalness, material::kMetallicMap, material::kMetallic, "metallic");
    single(rough, model->roughness, material::kRoughnessMap, material::kRoughness, "roughness");
    if (model->occlusion)
        single(occ, model->occlusion, material::kAmbientOcclusionMap, -1, "occlusion");

    // Specular from the IOR (and the specular weight scaling it).
    const Signal ior = input(model->ior), specWeight = input(model->specularWeight);
    if (ior.kind == Signal::Value || specWeight.kind == Signal::Value)
    {
        double s = specularOfIor(ior.kind == Signal::Value ? double(ior.scalar()) : 1.5);
        if (specWeight.kind == Signal::Value)
            s *= double(specWeight.scalar());
        set(material::kSpecular, std::clamp(s, 0.0, 1.0));
    }
    for (const auto& [s, in] : {std::pair{&ior, model->ior}, std::pair{&specWeight, model->specularWeight}})
        if (s->kind == Signal::Texture || s->kind == Signal::Unmapped)
            skipped.push_back(std::format("material {}: {} is driven by a network; not kept", name, in));

    // Normal map; a negative green scale (a flip on the way in) means DirectX.
    if (const Signal nrm = input(model->normal); nrm.kind == Signal::Texture)
    {
        useTexture(nrm, material::kNormalMap, "normal", false);
        const bool directX = nrm.channels.size() >= 2 && nrm.scale[1] < 0.0f;
        set(material::kNormalFormat, int64_t(directX ? material::NormalFormat::DirectX : material::NormalFormat::OpenGL));
    }
    else if (nrm.kind == Signal::Unmapped)
        skip(nrm, model->normal);

    // Emission: its color (or texture) times its weight.
    const Signal emColor = input(model->emissionColor).orElse(model->emissionColorDefault);
    const Signal emWeight = input(model->emission).orElse(model->emissionDefault);
    if (emWeight.kind == Signal::Value && emWeight.scalar() != 0.0f)
    {
        const float w = emWeight.scalar();
        if (emColor.kind == Signal::Texture)
        {
            useTexture(emColor, material::kEmissiveMap, "emissive", true);
            const float k = (emColor.scale[0] + emColor.scale[1] + emColor.scale[2]) / 3.0f;
            set(material::kEmissiveIntensity, double(w * k));
        }
        else if (emColor.kind == Signal::Value && emColor.rgb() != GfVec3f(0.0f))
        {
            const GfVec3f c = emColor.rgb();
            set(material::kEmissiveColor, glm::dvec3(c[0], c[1], c[2]));
            set(material::kEmissiveIntensity, double(w));
        }
        else if (emColor.kind == Signal::Unmapped)
            skip(emColor, model->emissionColor);
    }
    else if (emWeight.kind == Signal::Texture || emWeight.kind == Signal::Unmapped)
        skipped.push_back(std::format("material {}: {} is driven by a network; not kept", name, model->emission));

    // Opacity, and the blend mode the surface implies.
    bool translucent = false;
    Signal opacity = input(model->opacity);
    if (model == &kStandard)
        opacity = pick(opacity, 0);
    readOpacity(opacity, base, &translucent);
    material::BlendMode blend = translucent ? material::BlendMode::Translucent : material::BlendMode::Opaque;
    float threshold = 0.0f;
    if (model == &kGltf)
    {
        int mode = 0;
        valueOf(surface, "alpha_mode", &mode);
        handled.insert("alpha_mode");
        handled.insert("alpha_cutoff");
        blend = mode == 1 ? material::BlendMode::Masked : mode == 2 ? material::BlendMode::Translucent : material::BlendMode::Opaque;
        if (mode == 1)
        {
            threshold = 0.5f;
            valueOf(surface, "alpha_cutoff", &threshold);
        }
    }
    else if (model == &kPreview && valueOf(surface, "opacityThreshold", &threshold) && threshold > 0.0f)
    {
        handled.insert("opacityThreshold");
        if (translucent)
            blend = material::BlendMode::Masked;
    }
    if (blend != material::BlendMode::Opaque)
        set(material::kBlendMode, int64_t(blend));
    if (blend == material::BlendMode::Masked)
        set(material::kOpacityMaskClip, double(threshold));
    if (uvSet > 0)
        set(material::kUvSet, int64_t(uvSet));

    // What Lila has no equivalent for, and connected inputs nothing above read.
    for (const char* u : model->unsupported)
    {
        handled.insert(u);
        const Signal s = input(u);
        if (s.kind == Signal::Texture || s.kind == Signal::Unmapped)
            skipped.push_back(std::format("material {}: {} is driven by a network; Lila has no equivalent", name, u));
        else if (s.kind == Signal::Value && (s.value != GfVec4f(0.0f)))
            skipped.push_back(std::format("material {}: {} {} has no Lila equivalent; left out", name, u, describe(s)));
    }
    for (const UsdShadeInput& in : surface.GetInputs())
    {
        const std::string inName = in.GetBaseName().GetString();
        if (handled.count(inName) || !upstream(in))
            continue;
        const Signal s = resolve(in);
        skipped.push_back(std::format("material {}: {} is connected to {}; not mapped", name, inName,
                                      s.kind == Signal::Unmapped ? s.why : std::string("a texture")));
    }
}
} // namespace lila::usd
