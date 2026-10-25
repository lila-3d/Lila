#include "gltf/GltfMaterials.h"

#include "core/Profile.h"
#include "fbx/ImportCommon.h"
#include "scene/Graph.h"
#include "scene/Materials.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace lila::gltf
{
namespace
{
using namespace scene;

float toLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
float toSrgb(float l)
{
    l = std::max(l, 0.0f);
    return l <= 0.0031308f ? 12.92f * l : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
}

std::string str(const char* s) { return s ? std::string(s) : std::string(); }

const cgltf_image* imageOf(const cgltf_texture_view& v)
{
    if (!v.texture)
        return nullptr;
    const cgltf_texture& t = *v.texture;
    if (t.image)
        return t.image;
    if (t.has_webp && t.webp_image)
        return t.webp_image;
    if (t.has_basisu && t.basisu_image)
        return t.basisu_image;
    return nullptr;
}

int texcoordOf(const cgltf_texture_view& v) { return v.has_transform && v.transform.has_texcoord ? v.transform.texcoord : v.texcoord; }

/** KHR_texture_transform as a matrix on glTF UVs: translation * rotation * scale. */
glm::dmat3 transformOf(const cgltf_texture_view& v)
{
    if (!v.has_transform)
        return glm::dmat3(1.0);
    const cgltf_texture_transform& t = v.transform;
    glm::dmat3 tr(1.0), rot(1.0), sc(1.0);
    tr[2][0] = t.offset[0];
    tr[2][1] = t.offset[1];
    const double c = std::cos(double(t.rotation)), s = std::sin(double(t.rotation));
    rot[0][0] = c;
    rot[1][0] = s;
    rot[0][1] = -s;
    rot[1][1] = c;
    sc[0][0] = t.scale[0];
    sc[1][1] = t.scale[1];
    return tr * rot * sc;
}

bool nearMat3(const glm::dmat3& a, const glm::dmat3& b)
{
    for (int c = 0; c < 3; ++c)
        if (glm::length(a[c] - b[c]) > 1e-6)
            return false;
    return true;
}

bool isWhite(const float* f, int n)
{
    for (int i = 0; i < n; ++i)
        if (std::abs(f[i] - 1.0f) > 1e-6f)
            return false;
    return true;
}

const char* wrapName(cgltf_wrap_mode w)
{
    switch (w)
    {
    case cgltf_wrap_mode_clamp_to_edge: return "clamp to edge";
    case cgltf_wrap_mode_mirrored_repeat: return "mirrored repeat";
    default: return "repeat";
    }
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

/** Inverse of the sRGB decode, so a pure factor multiply happens in linear space. */
void multiplySrgb(Pixels& p, const glm::vec3& factor)
{
    for (size_t i = 0; i < p.rgba.size(); i += 4)
        for (int c = 0; c < 3; ++c)
            p.rgba[i + size_t(c)] = toSrgb(toLinear(p.rgba[i + size_t(c)]) * factor[c]);
}
} // namespace

ImageStore::ImageStore(const cgltf_data& data, const fs::path& gltfPath, const fs::path& textureDir, ImportResult& result)
    : mData(data), mDir(gltfPath.parent_path()), mTextureDir(textureDir), mFileStem(utf8(gltfPath.stem())), mResult(result)
{
}

std::string ImageStore::stem(const cgltf_image* image)
{
    const size_t index = cgltf_image_index(&mData, image);
    std::string name = image->name && *image->name ? fbx::safeName(image->name, "image") : std::string();
    if (name.empty() && image->uri && std::string_view(image->uri).substr(0, 5) != "data:")
    {
        std::string uri = image->uri;
        cgltf_decode_uri(uri.data());
        name = fbx::safeName(utf8(utf8Path(uri.c_str()).stem()), "image");
    }
    return name.empty() ? std::format("{}_image{}", fbx::safeName(mFileStem, "gltf"), index) : name;
}

bool ImageStore::bytes(const cgltf_image* image, std::string& out)
{
    if (image->buffer_view)
    {
        const uint8_t* p = cgltf_buffer_view_data(image->buffer_view);
        if (!p)
            return false;
        out.assign(reinterpret_cast<const char*>(p), image->buffer_view->size);
        return true;
    }
    if (!image->uri)
        return false;
    std::string_view uri(image->uri);
    if (uri.substr(0, 5) == "data:")
    {
        const size_t comma = uri.find(',');
        if (comma == std::string_view::npos || uri.substr(0, comma).find(";base64") == std::string_view::npos)
            return false;
        std::string_view b64 = uri.substr(comma + 1);
        size_t size = b64.size() / 4 * 3;
        if (!b64.empty() && b64.back() == '=')
            --size;
        if (b64.size() > 1 && b64[b64.size() - 2] == '=')
            --size;
        cgltf_options opt{};
        void* decoded = nullptr;
        if (cgltf_load_buffer_base64(&opt, size, b64.data(), &decoded) != cgltf_result_success)
            return false;
        out.assign(static_cast<const char*>(decoded), size);
        free(decoded);
        return true;
    }
    return readFile(utf8Path(path(image)), out);
}

std::string ImageStore::path(const cgltf_image* image)
{
    if (!image)
        return {};
    if (auto it = mPaths.find(image); it != mPaths.end())
        return it->second;
    std::string& out = mPaths[image];
    if (image->uri && std::string_view(image->uri).substr(0, 5) != "data:")
    {
        std::string uri = image->uri;
        cgltf_decode_uri(uri.data());
        fs::path p = utf8Path(uri.c_str());
        if (p.is_relative())
            p = mDir / p;
        out = utf8(p.lexically_normal());
        std::error_code ec;
        if (!fs::exists(p, ec))
            mResult.warnings.push_back(std::format("image '{}' not found", out));
        return out;
    }
    std::string data;
    if (!bytes(image, data))
    {
        mResult.warnings.push_back(std::format("image {} has no data", cgltf_image_index(&mData, image)));
        return out;
    }
    std::string mime = image->mime_type ? std::string(image->mime_type) : sniffMime(data);
    if (mime.empty())
        mime = sniffMime(data);
    const fs::path file = mTextureDir / utf8Path(stem(image) + extensionForMime(mime));
    if (!writeFile(file, data))
    {
        mResult.warnings.push_back(std::format("can't write image {}", utf8(file)));
        return out;
    }
    out = utf8(file);
    mResult.images.push_back(out);
    return out;
}

const Pixels* ImageStore::pixels(const cgltf_image* image)
{
    if (!image)
        return nullptr;
    auto it = mPixels.find(image);
    if (it != mPixels.end())
        return it->second.get();
    std::unique_ptr<Pixels>& slot = mPixels[image];
    std::string data, err;
    auto p = std::make_unique<Pixels>();
    if (!bytes(image, data) || !decodeImage(data, *p, &err))
    {
        mResult.warnings.push_back(std::format("image {} can't be decoded{}{}", path(image), err.empty() ? "" : ": ", err));
        return nullptr;
    }
    slot = std::move(p);
    return slot.get();
}

std::string ImageStore::writePng(const std::string& stem, const Pixels& p)
{
    const fs::path file = mTextureDir / utf8Path(stem + ".png");
    std::string png = encodePng(p);
    if (png.empty() || !writeFile(file, png))
    {
        mResult.warnings.push_back(std::format("can't write image {}", utf8(file)));
        return {};
    }
    mResult.images.push_back(utf8(file));
    return utf8(file);
}

std::vector<MaterialSlot> importMaterials(scene::Graph& graph, const cgltf_data& data, ImageStore& images, const ImportOptions& opts,
                                          ImportResult& r)
{
    LILA_ZONE();

    using namespace material;
    std::vector<MaterialSlot> out(data.materials_count);
    const bool makeNodes = opts.materials && types::find("material") != nullptr;
    std::set<std::string> created;
    for (size_t i = 0; i < data.materials_count; ++i)
    {
        const cgltf_material& gm = data.materials[i];
        MaterialSlot& slot = out[i];
        const std::string wanted = fbx::safeName(str(gm.name), "Material");
        const cgltf_pbr_metallic_roughness& pbr = gm.pbr_metallic_roughness;
        const bool specGloss = gm.has_pbr_specular_glossiness && !gm.has_pbr_metallic_roughness;

        // The texture views in priority order for the material's UV set and transform.
        const cgltf_texture_view* views[] = {specGloss ? &gm.pbr_specular_glossiness.diffuse_texture : &pbr.base_color_texture,
                                             &pbr.metallic_roughness_texture, &gm.normal_texture, &gm.occlusion_texture,
                                             &gm.emissive_texture};
        const cgltf_texture_view* lead = nullptr;
        for (const cgltf_texture_view* v : views)
            if (imageOf(*v))
            {
                if (!lead)
                    lead = v;
                else if (texcoordOf(*v) != texcoordOf(*lead))
                    r.warnings.push_back(std::format("material '{}': textures read different UV sets; all use set {}", wanted, texcoordOf(*lead)));
                else if (!nearMat3(transformOf(*v), transformOf(*lead)))
                    r.warnings.push_back(std::format("material '{}': textures have different KHR_texture_transform; the {} one is baked into the UVs",
                                                     wanted, lead == views[0] ? "base color" : "first texture's"));
                if (const cgltf_sampler* s = v->texture->sampler;
                    s && (s->wrap_s != cgltf_wrap_mode_repeat || s->wrap_t != cgltf_wrap_mode_repeat))
                    r.skipped.push_back(std::format("material '{}': {} / {} texture wrap (Lila textures repeat)", wanted, wrapName(s->wrap_s),
                                                    wrapName(s->wrap_t)));
            }
        if (lead)
        {
            slot.uvSet = texcoordOf(*lead);
            slot.transformed = lead->has_transform;
            slot.uvTransform = transformOf(*lead);
        }

        std::vector<std::string> unsupported;
        if (gm.has_clearcoat)
            unsupported.push_back("KHR_materials_clearcoat");
        if (gm.has_transmission)
            unsupported.push_back("KHR_materials_transmission");
        if (gm.has_volume)
            unsupported.push_back("KHR_materials_volume");
        if (gm.has_sheen)
            unsupported.push_back("KHR_materials_sheen");
        if (gm.has_iridescence)
            unsupported.push_back("KHR_materials_iridescence");
        if (gm.has_diffuse_transmission)
            unsupported.push_back("KHR_materials_diffuse_transmission");
        if (gm.has_anisotropy)
            unsupported.push_back("KHR_materials_anisotropy");
        if (gm.has_dispersion)
            unsupported.push_back("KHR_materials_dispersion");
        if (gm.has_specular && (imageOf(gm.specular.specular_texture) || imageOf(gm.specular.specular_color_texture)))
            unsupported.push_back("KHR_materials_specular textures");
        for (const std::string& u : unsupported)
            r.skipped.push_back(std::format("material '{}': {} (no Lila input)", wanted, u));

        if (!makeNodes)
        {
            slot.name = wanted;
            continue;
        }
        if (!created.count(wanted) && materials::find(graph, wanted))
        {
            slot.name = wanted;
            continue;
        }
        Node* n = materials::create(graph, wanted);
        slot.name = n->name();
        created.insert(slot.name);
        r.materials.push_back(slot.name);
        const NodeId id = n->id();
        auto set = [&](int attr, PlugValue v) { graph.setValue({id, attr, -1}, std::move(v)); };
        auto mapPath = [&](const cgltf_texture_view& v) { return images.path(imageOf(v)); };
        const std::string& name = slot.name;

        // Base color and opacity.
        glm::vec4 base(pbr.base_color_factor[0], pbr.base_color_factor[1], pbr.base_color_factor[2], pbr.base_color_factor[3]);
        const cgltf_texture_view* baseView = &pbr.base_color_texture;
        if (specGloss)
        {
            const cgltf_pbr_specular_glossiness& sg = gm.pbr_specular_glossiness;
            base = glm::vec4(sg.diffuse_factor[0], sg.diffuse_factor[1], sg.diffuse_factor[2], sg.diffuse_factor[3]);
            baseView = &sg.diffuse_texture;
            set(kMetallic, 0.0);
            set(kRoughness, std::clamp(1.0 - double(sg.glossiness_factor), 0.0, 1.0));
            r.warnings.push_back(std::format("material '{}': KHR_materials_pbrSpecularGlossiness converted to metallic-roughness (diffuse, 1 - "
                                             "glossiness); its specular-glossiness texture is not kept",
                                             name));
        }
        std::string baseMap;
        if (const cgltf_image* img = imageOf(*baseView))
        {
            baseMap = mapPath(*baseView);
            if (!isWhite(&base[0], 3))
                if (const Pixels* px = images.pixels(img))
                {
                    Pixels scaled = *px;
                    multiplySrgb(scaled, glm::vec3(base));
                    if (std::string p = images.writePng(name + "_BaseColor", scaled); !p.empty())
                        baseMap = p;
                }
            set(kBaseColorMap, baseMap);
        }
        else
            set(kBaseColor, glm::dvec3(base));
        set(kOpacity, double(base.a));
        switch (gm.alpha_mode)
        {
        case cgltf_alpha_mode_mask:
            set(kBlendMode, int64_t(BlendMode::Masked));
            set(kOpacityMaskClip, double(gm.alpha_cutoff));
            break;
        case cgltf_alpha_mode_blend: set(kBlendMode, int64_t(BlendMode::Translucent)); break;
        default: break;
        }
        if (gm.double_sided)
            set(kTwoSided, true);
        if (slot.uvSet != 0)
            set(kUvSet, int64_t(slot.uvSet));

        // Metallic, roughness and occlusion, packed as Unreal's ORM.
        if (!specGloss)
        {
            const double mf = pbr.metallic_factor, rf = pbr.roughness_factor;
            const cgltf_image* mr = imageOf(pbr.metallic_roughness_texture);
            const cgltf_image* occ = imageOf(gm.occlusion_texture);
            const double strength = gm.occlusion_texture.scale;
            if (!mr)
            {
                set(kMetallic, std::clamp(mf, 0.0, 1.0));
                set(kRoughness, std::clamp(rf, 0.0, 1.0));
            }
            if (mr)
            {
                const Pixels* px = images.pixels(mr);
                const bool sameImage = occ == mr && texcoordOf(gm.occlusion_texture) == texcoordOf(pbr.metallic_roughness_texture) &&
                                       nearMat3(transformOf(gm.occlusion_texture), transformOf(pbr.metallic_roughness_texture));
                bool asIs = mf == 1.0 && rf == 1.0 && (occ ? sameImage && strength == 1.0 : false);
                if (!asIs && !occ && mf == 1.0 && rf == 1.0 && px)
                {
                    // Without occlusion the red channel must read as unoccluded; many exporters leave it white anyway.
                    asIs = true;
                    for (size_t k = 0; k < px->rgba.size() && asIs; k += 4)
                        asIs = px->rgba[k] >= 0.999f;
                }
                if (asIs)
                    set(kOrmMap, mapPath(pbr.metallic_roughness_texture));
                else if (px)
                {
                    const Pixels* opx = occ ? images.pixels(occ) : nullptr;
                    Pixels orm = *px;
                    for (int y = 0; y < orm.height; ++y)
                        for (int x = 0; x < orm.width; ++x)
                        {
                            float* t = &orm.rgba[(size_t(y) * size_t(orm.width) + size_t(x)) * 4];
                            float o = 1.0f;
                            if (opx)
                            {
                                const float raw = opx == px ? t[0] : opx->sample((float(x) + 0.5f) / float(orm.width), (float(y) + 0.5f) / float(orm.height)).r;
                                o = 1.0f + float(strength) * (raw - 1.0f);
                            }
                            t[0] = o;
                            t[1] = std::clamp(t[1] * float(rf), 0.0f, 1.0f);
                            t[2] = std::clamp(t[2] * float(mf), 0.0f, 1.0f);
                            t[3] = 1.0f;
                        }
                    if (occ && !sameImage && (opx == nullptr || !nearMat3(transformOf(gm.occlusion_texture), transformOf(pbr.metallic_roughness_texture))))
                        r.warnings.push_back(std::format("material '{}': occlusion packed into the ORM map in the metallic-roughness map's layout", name));
                    if (std::string p = images.writePng(name + "_ORM", orm); !p.empty())
                        set(kOrmMap, p);
                }
                else
                {
                    set(kMetallic, std::clamp(mf, 0.0, 1.0));
                    set(kRoughness, std::clamp(rf, 0.0, 1.0));
                }
            }
            else if (occ)
            {
                std::string aoPath = mapPath(gm.occlusion_texture);
                if (strength != 1.0)
                    if (const Pixels* opx = images.pixels(occ))
                    {
                        Pixels ao = *opx;
                        for (size_t k = 0; k < ao.rgba.size(); k += 4)
                            ao.rgba[k] = 1.0f + float(strength) * (ao.rgba[k] - 1.0f);
                        if (std::string p = images.writePng(name + "_AO", ao); !p.empty())
                            aoPath = p;
                    }
                set(kAmbientOcclusionMap, aoPath);
            }
        }

        // Normal map: glTF's are OpenGL style.
        if (const cgltf_image* img = imageOf(gm.normal_texture))
        {
            std::string normal = mapPath(gm.normal_texture);
            const float scale = gm.normal_texture.scale;
            if (scale != 1.0f)
                if (const Pixels* px = images.pixels(img))
                {
                    Pixels scaled = *px;
                    for (size_t k = 0; k < scaled.rgba.size(); k += 4)
                    {
                        glm::vec3 nrm(scaled.rgba[k] * 2.0f - 1.0f, scaled.rgba[k + 1] * 2.0f - 1.0f, scaled.rgba[k + 2] * 2.0f - 1.0f);
                        nrm = glm::vec3(nrm.x * scale, nrm.y * scale, nrm.z);
                        const float len = glm::length(nrm);
                        nrm = len > 0.0f ? nrm / len : glm::vec3(0, 0, 1);
                        for (int c = 0; c < 3; ++c)
                            scaled.rgba[k + size_t(c)] = nrm[c] * 0.5f + 0.5f;
                    }
                    if (std::string p = images.writePng(name + "_Normal", scaled); !p.empty())
                        normal = p;
                }
            set(kNormalMap, normal);
            set(kNormalFormat, int64_t(NormalFormat::OpenGL));
        }

        // Emissive, with KHR_materials_emissive_strength.
        const glm::vec3 e(gm.emissive_factor[0], gm.emissive_factor[1], gm.emissive_factor[2]);
        const double strength = gm.has_emissive_strength ? double(gm.emissive_strength.emissive_strength) : 1.0;
        if (const cgltf_image* img = imageOf(gm.emissive_texture))
        {
            std::string emissive = mapPath(gm.emissive_texture);
            const float peak = std::max({e.r, e.g, e.b});
            double intensity = strength * double(peak);
            if (peak > 0.0f && (std::abs(e.r - peak) > 1e-6f || std::abs(e.g - peak) > 1e-6f || std::abs(e.b - peak) > 1e-6f))
                if (const Pixels* px = images.pixels(img))
                {
                    Pixels tinted = *px;
                    multiplySrgb(tinted, e / peak);
                    if (std::string p = images.writePng(name + "_Emissive", tinted); !p.empty())
                        emissive = p;
                }
            set(kEmissiveMap, emissive);
            set(kEmissiveIntensity, intensity);
        }
        else if (e != glm::vec3(0.0f))
        {
            set(kEmissiveColor, glm::dvec3(e));
            set(kEmissiveIntensity, strength);
        }

        // Specular reflectance from KHR_materials_ior and KHR_materials_specular: Unreal's specular is F0 / 0.08.
        if (gm.has_ior || gm.has_specular)
        {
            const double ior = gm.has_ior ? double(gm.ior.ior) : 1.5;
            double f0 = std::pow((ior - 1.0) / (ior + 1.0), 2.0);
            if (gm.has_specular)
                f0 *= double(gm.specular.specular_factor) *
                      double(std::max({gm.specular.specular_color_factor[0], gm.specular.specular_color_factor[1], gm.specular.specular_color_factor[2]}));
            set(kSpecular, std::clamp(f0 / 0.08, 0.0, 1.0));
        }

        if (gm.unlit)
        {
            // Lila has no unlit model: the surface emits its base color and reflects nothing.
            if (!baseMap.empty())
            {
                set(kEmissiveMap, baseMap);
                set(kEmissiveIntensity, double(1.0));
                if (gm.alpha_mode == cgltf_alpha_mode_opaque)
                    set(kBaseColorMap, std::string());
            }
            else
                set(kEmissiveColor, glm::dvec3(base));
            set(kBaseColor, glm::dvec3(0.0));
            set(kSpecular, 0.0);
            set(kRoughness, 1.0);
            set(kMetallic, 0.0);
            r.warnings.push_back(std::format("material '{}': KHR_materials_unlit approximated as emissive{}", name,
                                             !baseMap.empty() && gm.alpha_mode != cgltf_alpha_mode_opaque ? " (its base color map stays lit for its alpha)" : ""));
        }
        for (const cgltf_texture_view* v : views)
            if (const cgltf_image* img = imageOf(*v); img && img->mime_type)
                if (std::string mime = lower(img->mime_type); mime != "image/png" && mime != "image/jpeg")
                    r.warnings.push_back(std::format("material '{}': {} image {} may not display in Lila", name, mime, images.path(img)));
    }
    return out;
}

MaterialSnapshot snapshotMaterial(scene::Graph& graph, const std::string& slot)
{
    using namespace material;
    MaterialSnapshot m;
    m.name = slot;
    Node* n = materials::find(graph, slot);
    if (!n)
        return m;
    m.found = true;
    auto path = [&](int attr) {
        const std::string& stored = n->as<std::string>(attr);
        return stored.empty() ? std::string() : materials::resolvePath(stored);
    };
    m.baseColor = n->as<glm::dvec3>(kBaseColor);
    m.baseColorMap = path(kBaseColorMap);
    m.metallic = n->as<double>(kMetallic);
    m.metallicMap = path(kMetallicMap);
    m.specular = n->as<double>(kSpecular);
    m.roughness = n->as<double>(kRoughness);
    m.roughnessMap = path(kRoughnessMap);
    m.normalMap = path(kNormalMap);
    m.normalOpenGl = n->as<int64_t>(kNormalFormat) == int64_t(NormalFormat::OpenGL);
    m.occlusionMap = path(kAmbientOcclusionMap);
    m.ormMap = path(kOrmMap);
    m.packedMro = n->as<int64_t>(kPackedLayout) == int64_t(PackedLayout::MRO);
    m.emissiveColor = n->as<glm::dvec3>(kEmissiveColor);
    m.emissiveMap = path(kEmissiveMap);
    m.emissiveIntensity = n->as<double>(kEmissiveIntensity);
    m.opacity = n->as<double>(kOpacity);
    m.opacityMaskClip = n->as<double>(kOpacityMaskClip);
    m.blendMode = int(n->as<int64_t>(kBlendMode));
    m.twoSided = n->as<bool>(kTwoSided);
    m.uvSet = int(n->as<int64_t>(kUvSet));
    m.shadingModel = int(n->as<int64_t>(kShadingModel));
    return m;
}

json materialJson(const MaterialSnapshot& m, TextureSink& textures, const ExportOptions& opts, std::set<std::string>& extensionsUsed,
                  std::vector<std::string>& warnings)
{
    using namespace material;
    json mat = {{"name", m.name}};
    json pbr = json::object();
    const bool tex = opts.textures;
    auto info = [&](int index) {
        json j = {{"index", index}};
        if (m.uvSet != 0)
            j["texCoord"] = m.uvSet;
        return j;
    };
    auto load = [&](const std::string& path, Pixels& out) {
        std::string err;
        if (loadImage(utf8Path(path), out, &err))
            return true;
        warnings.push_back(std::format("material '{}': {}", m.name, err));
        return false;
    };

    const double alpha = m.blendMode == int(BlendMode::Opaque) ? 1.0 : std::clamp(m.opacity, 0.0, 1.0);
    int baseTex = tex && !m.baseColorMap.empty() ? textures.file(m.baseColorMap) : -1;
    if (baseTex >= 0)
    {
        pbr["baseColorTexture"] = info(baseTex);
        if (alpha != 1.0)
            pbr["baseColorFactor"] = {1.0, 1.0, 1.0, alpha};
    }
    else
        pbr["baseColorFactor"] = {m.baseColor.r, m.baseColor.g, m.baseColor.b, alpha};

    // ORM is glTF's layout already: roughness G, metallic B, and occlusion R read from the same texture.
    int mrTex = -1, occTex = -1;
    double metallic = m.metallic, roughness = m.roughness;
    if (tex && !m.ormMap.empty())
    {
        if (!m.packedMro)
            mrTex = occTex = textures.file(m.ormMap);
        else
            mrTex = occTex = textures.made("mro|" + m.ormMap, m.name + "_ORM", [&](Pixels& p) {
                if (!load(m.ormMap, p))
                    return false;
                for (size_t k = 0; k < p.rgba.size(); k += 4)
                    std::swap(p.rgba[k], p.rgba[k + 2]);
                return true;
            });
        if (mrTex >= 0)
            metallic = roughness = 1.0;
    }
    else if (tex && (!m.metallicMap.empty() || !m.roughnessMap.empty()))
    {
        mrTex = textures.made("mr|" + m.metallicMap + "|" + m.roughnessMap + "|" + m.occlusionMap, m.name + "_ORM", [&](Pixels& p) {
            Pixels metal, rough, ao;
            const bool hasMetal = !m.metallicMap.empty() && load(m.metallicMap, metal);
            const bool hasRough = !m.roughnessMap.empty() && load(m.roughnessMap, rough);
            const bool hasAo = !m.occlusionMap.empty() && load(m.occlusionMap, ao);
            if (!hasMetal && !hasRough)
                return false;
            p.width = std::max({hasMetal ? metal.width : 0, hasRough ? rough.width : 0, hasAo ? ao.width : 0});
            p.height = std::max({hasMetal ? metal.height : 0, hasRough ? rough.height : 0, hasAo ? ao.height : 0});
            p.rgba.assign(size_t(p.width) * size_t(p.height) * 4, 1.0f);
            for (int y = 0; y < p.height; ++y)
                for (int x = 0; x < p.width; ++x)
                {
                    const float u = (float(x) + 0.5f) / float(p.width), v = (float(y) + 0.5f) / float(p.height);
                    float* t = &p.rgba[(size_t(y) * size_t(p.width) + size_t(x)) * 4];
                    if (hasAo)
                        t[0] = ao.sample(u, v).r;
                    if (hasRough)
                        t[1] = rough.sample(u, v).r;
                    if (hasMetal)
                        t[2] = metal.sample(u, v).r;
                }
            return true;
        });
        if (mrTex >= 0)
        {
            if (!m.metallicMap.empty())
                metallic = 1.0;
            if (!m.roughnessMap.empty())
                roughness = 1.0;
            if (!m.occlusionMap.empty())
                occTex = mrTex;
        }
    }
    else if (tex && !m.occlusionMap.empty())
        occTex = textures.file(m.occlusionMap);
    pbr["metallicFactor"] = std::clamp(metallic, 0.0, 1.0);
    pbr["roughnessFactor"] = std::clamp(roughness, 0.0, 1.0);
    if (mrTex >= 0)
        pbr["metallicRoughnessTexture"] = info(mrTex);
    mat["pbrMetallicRoughness"] = pbr;
    if (occTex >= 0)
        mat["occlusionTexture"] = info(occTex);

    if (tex && !m.normalMap.empty())
    {
        const int n = m.normalOpenGl ? textures.file(m.normalMap)
                                     : textures.made("dx|" + m.normalMap, m.name + "_Normal", [&](Pixels& p) {
                                           if (!load(m.normalMap, p))
                                               return false;
                                           for (size_t k = 0; k < p.rgba.size(); k += 4)
                                               p.rgba[k + 1] = 1.0f - p.rgba[k + 1];
                                           return true;
                                       });
        if (n >= 0)
            mat["normalTexture"] = info(n);
    }

    glm::dvec3 emissive = m.emissiveColor * m.emissiveIntensity;
    if (int e = tex && !m.emissiveMap.empty() ? textures.file(m.emissiveMap) : -1; e >= 0)
    {
        mat["emissiveTexture"] = info(e);
        emissive = glm::dvec3(m.emissiveIntensity);
    }
    const double peak = std::max({emissive.r, emissive.g, emissive.b});
    if (peak > 0.0)
    {
        const double strength = std::max(1.0, peak);
        const glm::dvec3 factor = emissive / strength;
        mat["emissiveFactor"] = {factor.r, factor.g, factor.b};
        if (strength > 1.0)
        {
            mat["extensions"]["KHR_materials_emissive_strength"] = {{"emissiveStrength", strength}};
            extensionsUsed.insert("KHR_materials_emissive_strength");
        }
    }

    if (m.blendMode == int(BlendMode::Masked))
    {
        mat["alphaMode"] = "MASK";
        mat["alphaCutoff"] = m.opacityMaskClip;
    }
    else if (m.blendMode == int(BlendMode::Translucent))
        mat["alphaMode"] = "BLEND";
    if (m.twoSided)
        mat["doubleSided"] = true;

    // Unreal's specular 0.5 is F0 0.04, glTF's default; the rest scales F0 through KHR_materials_specular.
    if (std::abs(m.specular - 0.5) > 1e-6)
    {
        const double k = std::max(0.0, m.specular) / 0.5;
        json spec = {{"specularFactor", std::min(k, 1.0)}};
        if (k > 1.0)
            spec["specularColorFactor"] = {k, k, k};
        mat["extensions"]["KHR_materials_specular"] = spec;
        extensionsUsed.insert("KHR_materials_specular");
    }
    if (m.shadingModel != int(ShadingModel::DefaultLit))
        warnings.push_back(std::format("material '{}': its stylised shading model is written as default lit", m.name));
    return mat;
}
} // namespace lila::gltf
