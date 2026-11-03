#include "UsdMaterials.h"

#include <lila/Mesh.h>
#include <lila/Node.h>

#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/usd/ar/asset.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usdShade/connectableAPI.h>
#include <pxr/usd/usdShade/shader.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>

PXR_NAMESPACE_USING_DIRECTIVE

namespace lila::usd
{
namespace mat = scene::material;

namespace
{
const TfToken kPreviewSurface("UsdPreviewSurface");
const TfToken kUvTexture("UsdUVTexture");
const TfToken kPrimvarReader("UsdPrimvarReader_float2");
const TfToken kTransform2d("UsdTransform2d");
} // namespace

std::string materialSettings(const scene::Node& n)
{
    json out = json::object();
    const auto& attrs = n.type().attrs;
    for (size_t a = 0; a < attrs.size(); ++a)
    {
        const scene::AttrDef& def = attrs[a];
        if (!(def.flags & scene::AttrStorable) || def.type == scene::PlugType::Mesh || def.type == scene::PlugType::Data)
            continue;
        if (n.value(int(a)) == def.defaultValue)
            continue;
        out[def.name] = scene::toJson(n.value(int(a)));
    }
    return out.dump();
}

double iorOfSpecular(double s)
{
    const double f0 = std::clamp(0.08 * s, 0.0, 0.999);
    const double r = std::sqrt(f0);
    return (1.0 + r) / (1.0 - r);
}

double specularOfIor(double ior)
{
    const double r = (ior - 1.0) / (ior + 1.0);
    return std::clamp(r * r / 0.08, 0.0, 1.0);
}

std::string uvPrimvarName(int set) { return set <= 0 ? "st" : "st" + std::to_string(set); }

int uvSetOfPrimvar(std::string_view name)
{
    if (name == "st" || name == "st0")
        return 0;
    if (name.size() > 2 && name.substr(0, 2) == "st" && std::all_of(name.begin() + 2, name.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return std::stoi(std::string(name.substr(2)));
    if (name.size() > 3 && name.substr(0, 3) == "st_" && std::all_of(name.begin() + 3, name.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return std::stoi(std::string(name.substr(3)));
    return -1;
}

// Export

MaterialWriter::MaterialWriter(scene::Graph& graph, UsdStagePtr stage, SdfPath scope, const fs::path& file, TextureMode mode)
    : mGraph(graph), mStage(std::move(stage)), mScope(std::move(scope)), mFile(file), mMode(mode)
{
}

bool MaterialWriter::twoSided(const std::string& slot) const
{
    const scene::Node* n = scene::materials::find(mGraph, slot);
    return n && n->as<bool>(mat::kTwoSided);
}

std::string MaterialWriter::assetPath(const std::string& lilaPath)
{
    if (lilaPath.empty())
        return {};
    const std::string resolved = scene::materials::resolvePath(lilaPath);
    if (auto it = mAssets.find(resolved); it != mAssets.end())
        return it->second;
    const fs::path src = utf8Path(resolved);
    std::error_code ec;
    if (!fs::exists(src, ec))
        warnings.push_back(std::format("texture {} not found; written as given", resolved));
    const fs::path dir = mFile.parent_path();
    std::string out;
    auto uniqueIn = [&](const std::string& folder) {
        std::string stem = utf8(src.stem()), ext = utf8(src.extension());
        std::string name = stem + ext;
        for (int k = 2; mPackagedNames.count(folder + name); ++k)
            name = stem + "_" + std::to_string(k) + ext;
        mPackagedNames.insert(folder + name);
        return folder + name;
    };
    switch (mMode)
    {
    case TextureMode::Absolute: out = utf8(fs::absolute(src, ec)); break;
    case TextureMode::Relative:
    {
        fs::path rel = fs::relative(src, dir, ec);
        out = ec || rel.empty() || src.root_name() != fs::absolute(dir).root_name() ? utf8(src) : "./" + utf8(rel);
        break;
    }
    case TextureMode::Copy:
    {
        const std::string folder = utf8(mFile.stem()) + "_textures/";
        out = uniqueIn(folder);
        fs::create_directories(dir / utf8Path(folder), ec);
        fs::copy_file(src, dir / utf8Path(out), fs::copy_options::overwrite_existing, ec);
        if (ec)
            warnings.push_back(std::format("couldn't copy texture {}: {}", resolved, ec.message()));
        out = "./" + out;
        break;
    }
    case TextureMode::Package:
    {
        std::string ext = utf8(src.extension());
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".exr" && ext != ".avif")
            warnings.push_back(std::format("texture {} is {}; usdz viewers read only png, jpeg, exr and avif", resolved, ext));
        out = uniqueIn("textures/");
        mPackaged.push_back({src, out});
        break;
    }
    }
    mAssets[resolved] = out;
    return out;
}

void MaterialWriter::writeUnlit(const UsdShadeMaterial& m, UsdShadeShader surface, const scene::Node& n, const std::string& slot,
                                const std::function<UsdShadeShader(const std::string&, GfVec4f)>& texture)
{
    // UsdPreviewSurface has no unlit model: the base color goes out as emission over a black surface that reflects
    // nothing, which other tools draw unlit; the material's lila customData marks it for Lila's own import.
    auto input = [&](const char* name, const SdfValueTypeName& type) { return surface.CreateInput(TfToken(name), type); };
    input("diffuseColor", SdfValueTypeNames->Color3f).Set(GfVec3f(0.0f));
    input("roughness", SdfValueTypeNames->Float).Set(1.0f);
    input("metallic", SdfValueTypeNames->Float).Set(0.0f);
    input("ior", SdfValueTypeNames->Float).Set(1.0f);
    const auto blend = mat::BlendMode(n.as<int64_t>(mat::kBlendMode));
    const double opacity = n.as<double>(mat::kOpacity);
    UsdShadeInput emissive = input("emissiveColor", SdfValueTypeNames->Color3f);
    if (const std::string& baseMap = n.as<std::string>(mat::kBaseColorMap); !baseMap.empty())
    {
        UsdShadeShader t = texture(baseMap, GfVec4f(1.0f, 1.0f, 1.0f, blend == mat::BlendMode::Opaque ? 1.0f : float(opacity)));
        emissive.ConnectToSource(t.ConnectableAPI(), TfToken("rgb"));
        if (blend != mat::BlendMode::Opaque)
            input("opacity", SdfValueTypeNames->Float).ConnectToSource(t.ConnectableAPI(), TfToken("a"));
    }
    else
    {
        const glm::dvec3 c = n.as<glm::dvec3>(mat::kBaseColor);
        emissive.Set(GfVec3f(float(c.x), float(c.y), float(c.z)));
        if (blend != mat::BlendMode::Opaque)
            input("opacity", SdfValueTypeNames->Float).Set(float(opacity));
    }
    if (blend == mat::BlendMode::Masked)
        input("opacityThreshold", SdfValueTypeNames->Float).Set(float(n.as<double>(mat::kOpacityMaskClip)));
    setLilaData(m.GetPrim(), "unlit", VtValue(true));
    if (!n.as<std::string>(mat::kEmissiveMap).empty())
        warnings.push_back(std::format("material {}: UsdPreviewSurface has no room for an unlit material's emissive map; left out", slot));
}

UsdShadeMaterial MaterialWriter::material(const std::string& slot)
{
    if (auto it = mDone.find(slot); it != mDone.end())
        return it->second;
    const SdfPath path = mScope.AppendChild(TfToken(identifier(slot, "Material")));
    UsdShadeMaterial m = UsdShadeMaterial::Define(mStage, path);
    mDone[slot] = m;
    setLilaData(m.GetPrim(), "slot", VtValue(slot));
    const scene::Node* n = scene::materials::find(mGraph, slot);
    if (n)
        setLilaData(m.GetPrim(), "attrs", VtValue(materialSettings(*n)));
    else
        setLilaData(m.GetPrim(), "fallback", VtValue(true));
    if (mPreview || mMaterialX == mtlx::Surface::None)
        writePreview(m, n, slot);
    if (mMaterialX != mtlx::Surface::None)
        mtlx::writeUsd(network(slot, mMaterialX, true), m);
    return m;
}

void MaterialWriter::writePreview(const UsdShadeMaterial& m, const scene::Node* n, const std::string& slot)
{
    const SdfPath path = m.GetPath();
    UsdShadeShader surface = UsdShadeShader::Define(mStage, path.AppendChild(TfToken("PreviewSurface")));
    surface.CreateIdAttr(VtValue(kPreviewSurface));
    m.CreateSurfaceOutput().ConnectToSource(surface.ConnectableAPI(), TfToken("surface"));
    auto input = [&](const char* name, const SdfValueTypeName& type) { return surface.CreateInput(TfToken(name), type); };

    if (!n)
    {
        // Lila's neutral default, so other tools draw what the viewport does; importing it back makes no material.
        input("diffuseColor", SdfValueTypeNames->Color3f).Set(GfVec3f(0.5f));
        input("roughness", SdfValueTypeNames->Float).Set(0.55f);
        return;
    }

    const int uvSet = int(std::max<int64_t>(0, n->as<int64_t>(mat::kUvSet)));
    UsdShadeShader reader;
    auto stReader = [&]() -> UsdShadeShader {
        if (!reader)
        {
            reader = UsdShadeShader::Define(mStage, path.AppendChild(TfToken("PrimvarReader_st")));
            reader.CreateIdAttr(VtValue(kPrimvarReader));
            reader.CreateInput(TfToken("varname"), SdfValueTypeNames->Token).Set(TfToken(uvPrimvarName(uvSet)));
            reader.CreateOutput(TfToken("result"), SdfValueTypeNames->Float2);
        }
        return reader;
    };
    std::map<std::string, UsdShadeShader> textures; // asset path and sampling -> texture shader
    auto texture = [&](const std::string& lilaPath, const char* role, bool srgb, int mapAttr, GfVec4f scale = GfVec4f(1.0f),
                       GfVec4f bias = GfVec4f(0.0f)) -> UsdShadeShader {
        const std::string asset = assetPath(lilaPath);
        const mat::TextureSampling s = scene::materials::sampling(*n, mapAttr);
        const std::string key = std::format("{}|{}|{}|{}", asset, int(s.wrapU), int(s.wrapV), int(s.filter));
        if (auto it = textures.find(key); it != textures.end())
            return it->second;
        UsdShadeShader t = UsdShadeShader::Define(mStage, path.AppendChild(TfToken(std::string("Tex_") + role)));
        t.CreateIdAttr(VtValue(kUvTexture));
        t.CreateInput(TfToken("file"), SdfValueTypeNames->Asset).Set(SdfAssetPath(asset));
        t.CreateInput(TfToken("st"), SdfValueTypeNames->Float2).ConnectToSource(stReader().ConnectableAPI(), TfToken("result"));
        t.CreateInput(TfToken("sourceColorSpace"), SdfValueTypeNames->Token).Set(TfToken(srgb ? "sRGB" : "raw"));
        t.CreateInput(TfToken("wrapS"), SdfValueTypeNames->Token).Set(TfToken(scene::materials::wrapName(s.wrapU)));
        t.CreateInput(TfToken("wrapT"), SdfValueTypeNames->Token).Set(TfToken(scene::materials::wrapName(s.wrapV)));
        if (s.filter == mat::TextureFilter::Nearest)
        {
            // Not UsdPreviewSurface inputs, but the sampler settings Hydra reads from a texture node.
            t.CreateInput(TfToken("magFilter"), SdfValueTypeNames->Token).Set(TfToken("nearest"));
            t.CreateInput(TfToken("minFilter"), SdfValueTypeNames->Token).Set(TfToken("nearestMipmapLinear"));
        }
        if (scale != GfVec4f(1.0f))
            t.CreateInput(TfToken("scale"), SdfValueTypeNames->Float4).Set(scale);
        if (bias != GfVec4f(0.0f))
            t.CreateInput(TfToken("bias"), SdfValueTypeNames->Float4).Set(bias);
        for (const char* o : {"rgb", "r", "g", "b", "a"})
            t.CreateOutput(TfToken(o), std::string(o) == "rgb" ? SdfValueTypeNames->Float3 : SdfValueTypeNames->Float);
        textures[key] = t;
        return t;
    };
    auto connect = [&](UsdShadeInput in, const UsdShadeShader& tex, const char* channel) { in.ConnectToSource(tex.ConnectableAPI(), TfToken(channel)); };
    auto vec3f = [](const glm::dvec3& v) { return GfVec3f(float(v.x), float(v.y), float(v.z)); };

    const auto blend = mat::BlendMode(n->as<int64_t>(mat::kBlendMode));
    const double opacity = n->as<double>(mat::kOpacity);
    const std::string& baseMap = n->as<std::string>(mat::kBaseColorMap);
    if (mat::ShadingModel(n->as<int64_t>(mat::kShadingModel)) == mat::ShadingModel::Unlit)
    {
        writeUnlit(m, surface, *n, slot,
                   [&](const std::string& p, GfVec4f scale) { return texture(p, "baseColor", true, mat::kBaseColorMap, scale); });
        return;
    }
    UsdShadeInput diffuse = input("diffuseColor", SdfValueTypeNames->Color3f);
    if (!baseMap.empty())
    {
        const GfVec4f scale(1.0f, 1.0f, 1.0f, blend == mat::BlendMode::Opaque ? 1.0f : float(opacity));
        UsdShadeShader t = texture(baseMap, "baseColor", true, mat::kBaseColorMap, scale);
        connect(diffuse, t, "rgb");
        if (blend != mat::BlendMode::Opaque)
            connect(input("opacity", SdfValueTypeNames->Float), t, "a");
    }
    else
    {
        diffuse.Set(vec3f(n->as<glm::dvec3>(mat::kBaseColor)));
        if (blend != mat::BlendMode::Opaque)
            input("opacity", SdfValueTypeNames->Float).Set(float(opacity));
    }
    if (blend == mat::BlendMode::Masked)
        input("opacityThreshold", SdfValueTypeNames->Float).Set(float(n->as<double>(mat::kOpacityMaskClip)));

    const std::string& orm = n->as<std::string>(mat::kOrmMap);
    UsdShadeInput metallic = input("metallic", SdfValueTypeNames->Float);
    UsdShadeInput roughness = input("roughness", SdfValueTypeNames->Float);
    if (!orm.empty())
    {
        static const char* kChannel[3] = {"r", "g", "b"};
        const std::array<int, 3> ch = scene::materials::packedChannels(mat::PackedLayout(n->as<int64_t>(mat::kPackedLayout)));
        UsdShadeShader t = texture(orm, "orm", false, mat::kOrmMap);
        connect(input("occlusion", SdfValueTypeNames->Float), t, kChannel[ch[0]]);
        connect(roughness, t, kChannel[ch[1]]);
        connect(metallic, t, kChannel[ch[2]]);
    }
    else
    {
        if (const std::string& p = n->as<std::string>(mat::kMetallicMap); !p.empty())
            connect(metallic, texture(p, "metallic", false, mat::kMetallicMap), "r");
        else
            metallic.Set(float(n->as<double>(mat::kMetallic)));
        if (const std::string& p = n->as<std::string>(mat::kRoughnessMap); !p.empty())
            connect(roughness, texture(p, "roughness", false, mat::kRoughnessMap), "r");
        else
            roughness.Set(float(n->as<double>(mat::kRoughness)));
        if (const std::string& p = n->as<std::string>(mat::kAmbientOcclusionMap); !p.empty())
            connect(input("occlusion", SdfValueTypeNames->Float), texture(p, "occlusion", false, mat::kAmbientOcclusionMap), "r");
    }
    if (const double s = n->as<double>(mat::kSpecular); std::abs(s - 0.5) > 1e-6)
        input("ior", SdfValueTypeNames->Float).Set(float(iorOfSpecular(s)));

    if (const std::string& p = n->as<std::string>(mat::kNormalMap); !p.empty())
    {
        // USD's normal input is OpenGL style; a DirectX map's green is flipped by the texture's scale and bias.
        const bool directX = mat::NormalFormat(n->as<int64_t>(mat::kNormalFormat)) == mat::NormalFormat::DirectX;
        UsdShadeShader t = texture(p, "normal", false, mat::kNormalMap, GfVec4f(2.0f, directX ? -2.0f : 2.0f, 2.0f, 1.0f),
                                   GfVec4f(-1.0f, directX ? 1.0f : -1.0f, -1.0f, 0.0f));
        connect(input("normal", SdfValueTypeNames->Normal3f), t, "rgb");
    }

    const double intensity = n->as<double>(mat::kEmissiveIntensity);
    if (const std::string& p = n->as<std::string>(mat::kEmissiveMap); !p.empty())
    {
        const float k = float(intensity);
        connect(input("emissiveColor", SdfValueTypeNames->Color3f), texture(p, "emissive", true, mat::kEmissiveMap, GfVec4f(k, k, k, 1.0f)),
                "rgb");
    }
    else if (const glm::dvec3 e = n->as<glm::dvec3>(mat::kEmissiveColor) * intensity; e != glm::dvec3(0.0))
    {
        input("emissiveColor", SdfValueTypeNames->Color3f).Set(vec3f(e));
    }
    if (mat::ShadingModel(n->as<int64_t>(mat::kShadingModel)) != mat::ShadingModel::DefaultLit)
        warnings.push_back(std::format("material {}: its {} shading is kept for Lila only; other tools see the default lit inputs", slot,
                                       n->type().attrs[mat::kShadingModel].enumNames[size_t(n->as<int64_t>(mat::kShadingModel))]));
}

// Import

namespace
{
struct Source
{
    UsdShadeShader shader;
    TfToken output;
    bool connected = false;
};

Source sourceOf(const UsdShadeInput& in)
{
    Source s;
    if (!in)
        return s;
    UsdShadeConnectableAPI src;
    TfToken name;
    UsdShadeAttributeType type;
    if (in.GetConnectedSource(&src, &name, &type))
    {
        s.shader = UsdShadeShader(src.GetPrim());
        s.output = name;
        s.connected = true;
    }
    return s;
}

TfToken shaderId(const UsdShadeShader& s)
{
    TfToken id;
    if (s)
        s.GetShaderId(&id);
    return id;
}

template <class T> bool inputValue(const UsdShadeShader& s, const char* name, T* out)
{
    UsdShadeInput in = s.GetInput(TfToken(name));
    if (!in)
        return false;
    // A value can come through a connection to a material interface input.
    UsdShadeAttributeVector attrs = in.GetValueProducingAttributes();
    for (const UsdAttribute& a : attrs)
        if (UsdShadeInput::IsInput(a) && a.Get(out))
            return true;
    return in.Get(out);
}

/** The texture feeding an input, with the channel it reads; null shader when the input is a value. */
struct TextureUse
{
    UsdShadeShader tex;
    std::string channel;
    GfVec4f scale{1.0f}, bias{0.0f};
    std::string colorSpace;
};

/** A UsdUVTexture's wrapS and wrapT (useMetadata reads as repeat) and Hydra's magFilter. */
mat::TextureSampling samplingOf(const UsdShadeShader& tex)
{
    mat::TextureSampling s;
    auto wrap = [&](const char* name) {
        TfToken t;
        if (!inputValue(tex, name, &t))
            return mat::TextureWrap::Repeat;
        if (t == "clamp")
            return mat::TextureWrap::Clamp;
        if (t == "mirror")
            return mat::TextureWrap::Mirror;
        if (t == "black")
            return mat::TextureWrap::Black;
        return mat::TextureWrap::Repeat;
    };
    s.wrapU = wrap("wrapS");
    s.wrapV = wrap("wrapT");
    TfToken mag;
    if (inputValue(tex, "magFilter", &mag) && mag == "nearest")
        s.filter = mat::TextureFilter::Nearest;
    return s;
}

TextureUse textureOf(const UsdShadeShader& surface, const char* inputName)
{
    TextureUse t;
    Source s = sourceOf(surface.GetInput(TfToken(inputName)));
    if (!s.connected || shaderId(s.shader) != kUvTexture)
        return t;
    t.tex = s.shader;
    t.channel = s.output.GetString();
    inputValue(s.shader, "scale", &t.scale);
    inputValue(s.shader, "bias", &t.bias);
    TfToken cs;
    if (inputValue(s.shader, "sourceColorSpace", &cs))
        t.colorSpace = cs.GetString();
    return t;
}
} // namespace

std::string MaterialReader::texturePath(const UsdShadeShader& tex, const std::string& material)
{
    SdfAssetPath asset;
    if (!inputValue(tex, "file", &asset))
        return {};
    std::string path = asset.GetResolvedPath();
    if (path.empty())
    {
        warnings.push_back(std::format("material {}: texture {} not found", material, asset.GetAssetPath()));
        return asset.GetAssetPath();
    }
    // Textures inside a usdz resolve as package[inner]; Lila reads loose files, so those are unpacked beside it.
    if (const size_t open = path.find('['); open != std::string::npos && path.back() == ']')
    {
        const std::string inner = path.substr(open + 1, path.size() - open - 2);
        const fs::path dir = mFile.parent_path() / utf8Path(utf8(mFile.stem()) + "_usdz");
        const fs::path out = dir / utf8Path(inner);
        std::error_code ec;
        if (!fs::exists(out, ec))
        {
            std::shared_ptr<ArAsset> a = ArGetResolver().OpenAsset(ArResolvedPath(path));
            if (!a)
            {
                warnings.push_back(std::format("material {}: can't read {} from the package", material, inner));
                return {};
            }
            fs::create_directories(out.parent_path(), ec);
            std::shared_ptr<const char> buffer = a->GetBuffer();
            std::ofstream(out, std::ios::binary).write(buffer.get(), std::streamsize(a->GetSize()));
            ++mExtracted;
            if (mExtracted == 1)
                warnings.push_back(std::format("textures packaged in the usdz were unpacked to {}", utf8(dir)));
        }
        return utf8(out);
    }
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

void MaterialReader::setTwoSided(const std::string& slot)
{
    // Materials Lila wrote carry their own twoSided; a mesh's doubleSided only speaks for other tools' materials.
    if (!mMade.count(slot) || mOwnSettings.count(slot))
        return;
    if (scene::Node* n = scene::materials::find(mGraph, slot))
        mGraph.setValue({n->id(), mat::kTwoSided, -1}, true);
}

std::string MaterialReader::slot(const UsdShadeMaterial& m)
{
    using namespace scene;
    if (!m)
        return {};
    if (auto it = mSlots.find(m.GetPath()); it != mSlots.end())
        return it->second;
    std::string name = identifier(m.GetPrim().GetName().GetString(), "Material");
    const VtDictionary data = lilaData(m.GetPrim());
    if (auto it = data.find("slot"); it != data.end() && it->second.IsHolding<std::string>() && Graph::validName(it->second.UncheckedGet<std::string>()))
        name = it->second.UncheckedGet<std::string>();
    else if (auto ext = external.find(m.GetPrim().GetName().GetString()); ext != external.end() && Graph::validName(ext->second.first))
        name = ext->second.first;
    if (!types::find("material") || materials::find(mGraph, name) || lilaFlag(m.GetPrim(), "fallback"))
    {
        mSlots[m.GetPath()] = name;
        return name;
    }
    Node* n = materials::create(mGraph, name);
    name = n->name();
    mSlots[m.GetPath()] = name;
    mMade.insert(name);
    created.push_back(name);
    auto set = [&](int attr, PlugValue v) { mGraph.setValue({n->id(), attr, -1}, std::move(v)); };

    std::string own;
    if (auto it = data.find("attrs"); it != data.end() && it->second.IsHolding<std::string>())
        own = it->second.UncheckedGet<std::string>();
    else if (auto ext = external.find(m.GetPrim().GetName().GetString()); ext != external.end())
        own = ext->second.second;

    // MaterialX wins over a UsdPreviewSurface beside it, which other tools write as a fallback. Lila's own materials
    // hold everything in their preview surface (occlusion included), so that is read instead.
    UsdShadeShader surface = m.ComputeSurfaceSource();
    UsdShadeShader mx;
    if (const UsdShadeOutput o = m.GetSurfaceOutput(TfToken("mtlx")); o && o.HasConnectedSource())
        mx = m.ComputeSurfaceSource({TfToken("mtlx")});
    if (!mx && surface && shaderId(surface).GetString().starts_with("ND_"))
        mx = surface;
    const bool ownPreview = !own.empty() && surface && shaderId(surface) == kPreviewSurface;
    if (mx && shaderId(mx).GetString().starts_with("ND_") && !ownPreview)
    {
        readMtlx(mx, n->id(), name, !own.empty());
        if (!own.empty())
            applyOwnSettings(n->id(), own);
        return name;
    }
    if (!surface)
        for (const TfToken& ctx : {TfToken("arnold"), TfToken("ri")})
            if ((surface = m.ComputeSurfaceSource({ctx})))
                break;
    if (!surface || shaderId(surface) != kPreviewSurface)
    {
        skipped.push_back(std::format("material {}: its surface {} has no Lila mapping; imported with default settings", name,
                                      surface ? shaderId(surface).GetString() : std::string("(none)")));
        return name;
    }

    auto colorSpaceCheck = [&](const TextureUse& t, bool color, const char* role) {
        if (t.colorSpace.empty() || t.colorSpace == "auto")
            return;
        if (color && t.colorSpace == "raw")
            warnings.push_back(std::format("material {}: {} texture is marked raw; Lila reads color maps as sRGB", name, role));
        if (!color && t.colorSpace == "sRGB")
            warnings.push_back(std::format("material {}: {} texture is marked sRGB; Lila reads data maps as linear", name, role));
    };
    auto plainScale = [&](const TextureUse& t, const char* role) {
        if (GfVec3f(t.scale[0], t.scale[1], t.scale[2]) != GfVec3f(1.0f) || GfVec3f(t.bias[0], t.bias[1], t.bias[2]) != GfVec3f(0.0f))
            warnings.push_back(std::format("material {}: the {} texture's scale and bias are not kept", name, role));
    };
    int uvSet = -1;
    auto noteUv = [&](const TextureUse& t) {
        Source st = sourceOf(t.tex.GetInput(TfToken("st")));
        if (st.connected && shaderId(st.shader) == kTransform2d)
        {
            GfVec2f scale(1.0f), translation(0.0f);
            float rotation = 0.0f;
            inputValue(st.shader, "scale", &scale);
            inputValue(st.shader, "translation", &translation);
            inputValue(st.shader, "rotation", &rotation);
            if (scale != GfVec2f(1.0f) || translation != GfVec2f(0.0f) || rotation != 0.0f)
                warnings.push_back(std::format("material {}: UsdTransform2d (scale {} {}, rotation {}, translation {} {}) is not kept; Lila "
                                               "materials have no UV transform",
                                               name, scale[0], scale[1], rotation, translation[0], translation[1]));
            st = sourceOf(st.shader.GetInput(TfToken("in")));
        }
        TfToken var;
        std::string varname;
        if (st.connected && inputValue(st.shader, "varname", &var))
            varname = var.GetString();
        else if (std::string s; st.connected && inputValue(st.shader, "varname", &s))
            varname = s;
        if (varname.empty())
            return;
        int set = uvSetOfPrimvar(varname);
        if (set < 0)
            set = 0;
        if (uvSet >= 0 && set != uvSet)
            warnings.push_back(std::format("material {}: textures read different UV sets; Lila uses one per material (uv{})", name, uvSet));
        else
            uvSet = set;
    };

    // An unlit material Lila wrote shows its base color through the emissive input.
    const bool unlit = lilaFlag(m.GetPrim(), "unlit");
    const char* baseInput = unlit ? "emissiveColor" : "diffuseColor";
    if (unlit)
        set(material::kShadingModel, int64_t(material::ShadingModel::Unlit));
    GfVec3f c(0.18f);
    const TextureUse base = textureOf(surface, baseInput);
    if (base.tex)
    {
        noteUv(base);
        colorSpaceCheck(base, true, "base color");
        if (unlit)
        {
            if (GfVec3f(base.scale[0], base.scale[1], base.scale[2]) != GfVec3f(1.0f))
                warnings.push_back(std::format("material {}: the base color texture's scale is not kept", name));
        }
        else
            plainScale(base, "base color");
        set(material::kBaseColorMap, texturePath(base.tex, name));
        materials::setSampling(mGraph, n->id(), material::kBaseColorMap, samplingOf(base.tex));
    }
    else
    {
        inputValue(surface, baseInput, &c);
        set(material::kBaseColor, glm::dvec3(c[0], c[1], c[2]));
    }

    float useSpec = 0.0f;
    if (inputValue(surface, "useSpecularWorkflow", &useSpec) && useSpec != 0.0f)
        warnings.push_back(std::format("material {}: the specular workflow is approximated with metallic and roughness", name));

    // Occlusion, roughness and metallic sharing one texture are a packed map when their channels match a layout.
    const TextureUse occ = textureOf(surface, "occlusion"), rough = textureOf(surface, "roughness"), metal = textureOf(surface, "metallic");
    auto fileOf = [&](const TextureUse& t) {
        SdfAssetPath a;
        return t.tex && inputValue(t.tex, "file", &a) ? a.GetAssetPath() : std::string();
    };
    const std::string occFile = fileOf(occ), roughFile = fileOf(rough), metalFile = fileOf(metal);
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
            static const char* kChannel[3] = {"r", "g", "b"};
            const std::array<int, 3> ch = materials::packedChannels(layout);
            bool fits = true;
            const std::pair<const TextureUse*, const std::string*> uses[3] = {{&occ, &occFile}, {&rough, &roughFile}, {&metal, &metalFile}};
            for (int k = 0; k < 3; ++k)
                if (*uses[k].second == packedFile)
                    fits = fits && uses[k].first->channel == kChannel[ch[size_t(k)]];
            if (fits)
            {
                const TextureUse& any = occFile == packedFile ? occ : roughFile == packedFile ? rough : metal;
                noteUv(any);
                colorSpaceCheck(any, false, "packed");
                set(material::kOrmMap, texturePath(any.tex, name));
                materials::setSampling(mGraph, n->id(), material::kOrmMap, samplingOf(any.tex));
                set(material::kPackedLayout, int64_t(layout));
                packed = true;
                if (shared < 3)
                    warnings.push_back(std::format("material {}: a packed map feeds two of occlusion, roughness and metallic; Lila reads all "
                                                   "three from it",
                                                   name));
                break;
            }
        }
        if (!packed)
            warnings.push_back(std::format("material {}: occlusion, roughness and metallic share a texture in an order Lila has no "
                                           "layout for; each is read as its own map's red channel",
                                           name));
    }
    auto single = [&](const TextureUse& t, const char* input, int mapAttr, int valueAttr, double fallback, const char* role) {
        if (t.tex && !packed)
        {
            noteUv(t);
            colorSpaceCheck(t, false, role);
            plainScale(t, role);
            if (t.channel != "r")
                warnings.push_back(std::format("material {}: the {} texture is read from its {} channel; Lila reads red", name, role, t.channel));
            set(mapAttr, texturePath(t.tex, name));
            materials::setSampling(mGraph, n->id(), mapAttr, samplingOf(t.tex));
        }
        if (valueAttr >= 0 && !t.tex)
        {
            float v = float(fallback);
            inputValue(surface, input, &v);
            set(valueAttr, std::clamp(double(v), 0.0, 1.0));
        }
    };
    single(metal, "metallic", material::kMetallicMap, material::kMetallic, 0.0, "metallic");
    single(rough, "roughness", material::kRoughnessMap, material::kRoughness, 0.5, "roughness");
    single(occ, "occlusion", material::kAmbientOcclusionMap, -1, 1.0, "occlusion");

    float ior = 1.5f;
    if (inputValue(surface, "ior", &ior))
        set(material::kSpecular, specularOfIor(double(ior)));

    if (const TextureUse nrm = textureOf(surface, "normal"); nrm.tex)
    {
        noteUv(nrm);
        colorSpaceCheck(nrm, false, "normal");
        set(material::kNormalMap, texturePath(nrm.tex, name));
        materials::setSampling(mGraph, n->id(), material::kNormalMap, samplingOf(nrm.tex));
        set(material::kNormalFormat, int64_t(nrm.scale[1] < 0.0f ? material::NormalFormat::DirectX : material::NormalFormat::OpenGL));
    }

    TextureUse em;
    if (!unlit)
        em = textureOf(surface, "emissiveColor");
    if (em.tex)
    {
        noteUv(em);
        colorSpaceCheck(em, true, "emissive");
        set(material::kEmissiveMap, texturePath(em.tex, name));
        materials::setSampling(mGraph, n->id(), material::kEmissiveMap, samplingOf(em.tex));
        set(material::kEmissiveIntensity, double(em.scale[0] + em.scale[1] + em.scale[2]) / 3.0);
    }
    else if (GfVec3f e(0.0f); !unlit && inputValue(surface, "emissiveColor", &e) && e != GfVec3f(0.0f))
    {
        set(material::kEmissiveColor, glm::dvec3(e[0], e[1], e[2]));
    }

    float threshold = 0.0f;
    inputValue(surface, "opacityThreshold", &threshold);
    if (const TextureUse op = textureOf(surface, "opacity"); op.tex)
    {
        if (op.channel != "a" || fileOf(op) != fileOf(base))
            warnings.push_back(std::format("material {}: opacity comes from its own texture; Lila takes it from the base color map's alpha", name));
        set(material::kBlendMode, int64_t(threshold > 0.0f ? material::BlendMode::Masked : material::BlendMode::Translucent));
        set(material::kOpacity, double(op.scale[3]));
    }
    else if (float o = 1.0f; inputValue(surface, "opacity", &o) && o < 1.0f)
    {
        set(material::kBlendMode, int64_t(threshold > 0.0f ? material::BlendMode::Masked : material::BlendMode::Translucent));
        set(material::kOpacity, double(o));
    }
    if (threshold > 0.0f)
        set(material::kOpacityMaskClip, double(threshold));
    if (uvSet > 0)
        set(material::kUvSet, int64_t(uvSet));

    for (const char* unsupported : {"clearcoat", "displacement"})
    {
        float v = 0.0f;
        if (sourceOf(surface.GetInput(TfToken(unsupported))).connected || (inputValue(surface, unsupported, &v) && v != 0.0f))
            warnings.push_back(std::format("material {}: {} is not supported and was left out", name, unsupported));
    }

    if (!own.empty())
        applyOwnSettings(n->id(), own);
    return name;
}

void MaterialReader::applyOwnSettings(scene::NodeId id, const std::string& attrs)
{
    using namespace scene;
    Node* n = mGraph.node(id);
    if (!n)
        return;
    mOwnSettings.insert(n->name());
    json extra = json::parse(attrs, nullptr, false);
    if (!extra.is_object())
        return;
    // The settings hold every value that isn't its default, so what they leave out is back at its default, whatever
    // the network implied (an unlit preview surface's roughness 1, say).
    const auto& defs = n->type().attrs;
    for (size_t a = 0; a < defs.size(); ++a)
        if ((defs[a].flags & AttrStorable) && !(defs[a].flags & AttrFile) && defs[a].type != PlugType::Mesh && defs[a].type != PlugType::Data &&
            !extra.contains(defs[a].name) && n->value(int(a)) != defs[a].defaultValue)
            mGraph.setValue({id, int(a), -1}, defs[a].defaultValue);
    for (auto& [key, value] : extra.items())
    {
        const int a = n->type().attrIndex(key);
        if (a < 0)
            continue;
        try
        {
            // Texture paths come from the network, resolved against this file, rather than as they were.
            if (n->type().attrs[size_t(a)].flags & AttrFile)
                continue;
            mGraph.setValue({id, a, -1}, fromJson(n->type().attrs[size_t(a)], value));
        }
        catch (const std::exception&)
        {
        }
    }
}
} // namespace lila::usd
