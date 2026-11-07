#include "bake/MapMath.h"
#include "bake/MapsInternal.h"

#include "bake/Backend.h"
#include "bake/BakeInputs.h"
#include "scene/Graph.h"
#include "scene/Materials.h"

#include <algorithm>

/** Vertex colour and material transfer: the high's colour sets and material inputs, sampled where each ray hit. */
namespace lila::bake
{
using namespace mapmath;

namespace
{
template <class T> T plugAs(scene::Graph& g, scene::NodeId id, int attr, T fallback)
{
    const scene::PlugValue v = g.value({id, attr, -1});
    if (const T* p = std::get_if<T>(&v))
        return *p;
    return fallback;
}

enum class Channel
{
    BaseColor,
    Roughness,
    Metallic,
    Emissive,
    Opacity,
};

struct SlotSource
{
    const MaterialInput* material = nullptr;
    std::shared_ptr<const TextureSampler> map;
    /** The texture channel a scalar reads (packed maps). */
    int channel = 0;
    /** The map's values are sRGB-encoded. */
    bool srgb = false;
};

struct MaterialPrep
{
    /** Per group, per high, per material slot. */
    std::vector<std::vector<std::vector<SlotSource>>> slots;
};

const char* channelMap(Channel c)
{
    switch (c)
    {
    case Channel::BaseColor: return "baseColor";
    case Channel::Roughness: return "roughness";
    case Channel::Metallic: return "metallic";
    case Channel::Emissive: return "emissive";
    case Channel::Opacity: return "materialOpacity";
    }
    return "";
}

const MaterialInput& neutralMaterial()
{
    static const MaterialInput m;
    return m;
}

Prepare materialPrepare(Channel channel)
{
    return [channel](const MapSettings&, const BakeInputs& inputs) -> std::shared_ptr<const void> {
        auto prep = std::make_shared<MaterialPrep>();
        TextureCache cache;
        prep->slots.resize(inputs.groups.size());
        for (size_t g = 0; g < inputs.groups.size(); ++g)
        {
            const GroupInputs& gi = inputs.groups[g];
            prep->slots[g].resize(gi.highs.size());
            for (size_t h = 0; h < gi.highs.size(); ++h)
            {
                const MeshSoup& soup = gi.highs[h].soup;
                auto& slots = prep->slots[g][h];
                slots.resize(soup.materialNames.size());
                for (size_t m = 0; m < soup.materialNames.size(); ++m)
                {
                    auto it = inputs.materials.find(soup.materialNames[m]);
                    const MaterialInput& mat = it != inputs.materials.end() ? it->second : neutralMaterial();
                    SlotSource& src = slots[m];
                    src.material = &mat;
                    if (soup.uvs.empty())
                        continue;
                    const TransferSource ts = transferSource(mat, channelMap(channel));
                    src.map = cache.get(ts.texture);
                    src.channel = ts.channel;
                    src.srgb = ts.srgb;
                    if (!src.map && !ts.fallback.empty())
                    {
                        src.map = cache.get(ts.fallback);
                        src.channel = ts.fallbackChannel;
                        src.srgb = ts.fallbackSrgb;
                    }
                }
            }
        }
        return prep;
    };
}

CpuKernel materialKernel(Channel channel)
{
    return [channel](const MapContext& ctx, std::span<const TexelSample> samples, std::span<const HitRecord> hits, std::span<glm::vec4> out,
                     std::span<uint8_t> valid) {
        const auto* prep = static_cast<const MaterialPrep*>(ctx.prepared);
        for (size_t i = 0; i < samples.size(); ++i)
        {
            const HitRecord& h = hits[i];
            valid[i] = (h.flags & kHitValid) && prep ? 1 : 0;
            if (!valid[i])
                continue;
            const HighInput& high = ctx.group.highs[h.high];
            const uint32_t slot = h.triangle < high.soup.materials.size() ? high.soup.materials[h.triangle] : 0u;
            const auto& slots = prep->slots[ctx.groupIndex][h.high];
            const SlotSource& src = slot < slots.size() ? slots[slot] : SlotSource{};
            const MaterialInput& m = src.material ? *src.material : neutralMaterial();
            const glm::vec4 t = src.map ? src.map->sample(hitUv(high, h)) : glm::vec4(1.0f);
            // Colour outputs are written sRGB-encoded and scalars linear, whatever the source's encoding.
            const glm::vec3 encoded = src.srgb ? glm::vec3(t) : linearToSrgb(glm::vec3(t));
            const glm::vec3 linear = src.srgb ? srgbToLinear(glm::vec3(t)) : glm::vec3(t);
            switch (channel)
            {
            case Channel::BaseColor:
                out[i] = glm::vec4(src.map ? encoded : linearToSrgb(m.baseColor), 1.0f);
                break;
            case Channel::Roughness:
            {
                const float v = src.map ? linear[src.channel] : m.roughness;
                out[i] = glm::vec4(v, v, v, 1.0f);
                break;
            }
            case Channel::Metallic:
            {
                const float v = src.map ? linear[src.channel] : m.metallic;
                out[i] = glm::vec4(v, v, v, 1.0f);
                break;
            }
            case Channel::Emissive:
            {
                const glm::vec3 lin = (src.map ? linear : m.emissive) * m.emissiveIntensity;
                out[i] = glm::vec4(linearToSrgb(lin), 1.0f);
                break;
            }
            case Channel::Opacity:
            {
                const float v = m.blendMode == 0 ? 1.0f : std::clamp(m.opacity * (src.map ? t.a : 1.0f), 0.0f, 1.0f);
                out[i] = glm::vec4(v, v, v, 1.0f);
                break;
            }
            }
        }
    };
}

/** The named colour set's corners on a high, null when it has none of that name (empty name: the first set). */
const std::vector<glm::vec4>* colorSet(const MeshSoup& soup, const std::string& name)
{
    for (size_t k = 0; k < soup.colorSetNames.size(); ++k)
        if (name.empty() || soup.colorSetNames[k] == name)
        {
            const std::vector<glm::vec4>& v = k ? soup.moreColors[k - 1] : soup.colors;
            return v.empty() ? nullptr : &v;
        }
    return nullptr;
}

void vertexColorKernel(const MapContext& ctx, std::span<const TexelSample> samples, std::span<const HitRecord> hits, std::span<glm::vec4> out,
                       std::span<uint8_t> valid)
{
    const std::string set = ctx.settings.options.value("set", std::string());
    for (size_t i = 0; i < samples.size(); ++i)
    {
        const HitRecord& h = hits[i];
        valid[i] = 0;
        if (!(h.flags & kHitValid))
            continue;
        const std::vector<glm::vec4>* c = colorSet(ctx.group.highs[h.high].soup, set);
        if (!c)
            continue;
        out[i] = atHit(*c, h);
        valid[i] = 1;
    }
}

/** The material's base colour, or with option source vertexColor the high's vertex colours (polypaint). */
CpuKernel baseColorKernel()
{
    return [material = materialKernel(Channel::BaseColor)](const MapContext& ctx, std::span<const TexelSample> samples,
                                                          std::span<const HitRecord> hits, std::span<glm::vec4> out, std::span<uint8_t> valid) {
        if (ctx.settings.options.value("source", std::string("material")) == "vertexColor")
            vertexColorKernel(ctx, samples, hits, out, valid);
        else
            material(ctx, samples, hits, out, valid);
    };
}
} // namespace

TransferSource transferSource(const MaterialInput& mat, std::string_view map)
{
    TransferSource s;
    if (map == "baseColor" || map == "materialOpacity")
    {
        s.texture = mat.baseColorMap;
        s.channel = 3;
        s.srgb = mat.baseColorSrgb;
    }
    else if (map == "emissive")
    {
        s.texture = mat.emissiveMap;
        s.srgb = mat.emissiveSrgb;
    }
    else if (map == "roughness" || map == "metallic")
    {
        const std::array<int, 3> packed = scene::materials::packedChannels(scene::material::PackedLayout(mat.packedLayout));
        const std::string& own = map == "roughness" ? mat.roughnessMap : mat.metallicMap;
        const bool ownSrgb = map == "roughness" ? mat.roughnessSrgb : mat.metallicSrgb;
        if (!mat.ormMap.empty())
        {
            s.texture = mat.ormMap;
            s.channel = packed[map == "roughness" ? 1 : 2];
            s.srgb = mat.ormSrgb;
            s.fallback = own;
            s.fallbackSrgb = ownSrgb;
        }
        else
        {
            s.texture = own;
            s.srgb = ownSrgb;
        }
    }
    return s;
}

MaterialInput materialInput(scene::Graph& g, const std::string& slot)
{
    using namespace scene::material;
    MaterialInput m;
    scene::Node* n = scene::materials::find(g, slot);
    if (!n)
        return m;
    const scene::NodeId id = n->id();
    m.found = true;
    m.baseColor = glm::vec3(plugAs(g, id, kBaseColor, glm::dvec3(0.5)));
    m.metallic = float(plugAs(g, id, kMetallic, 0.0));
    m.roughness = float(plugAs(g, id, kRoughness, 0.5));
    m.emissive = glm::vec3(plugAs(g, id, kEmissiveColor, glm::dvec3(0.0)));
    m.emissiveIntensity = float(plugAs(g, id, kEmissiveIntensity, 1.0));
    m.opacity = float(plugAs(g, id, kOpacity, 1.0));
    m.blendMode = int(plugAs(g, id, kBlendMode, int64_t(0)));
    m.normalOpenGL = NormalFormat(plugAs(g, id, kNormalFormat, int64_t(0))) == NormalFormat::OpenGL;
    m.packedLayout = int(plugAs(g, id, kPackedLayout, int64_t(0)));
    auto path = [&](int attr) {
        const std::string p = plugAs(g, id, attr, std::string());
        return p.empty() ? p : scene::materials::resolvePath(p);
    };
    m.baseColorMap = path(kBaseColorMap);
    m.metallicMap = path(kMetallicMap);
    m.roughnessMap = path(kRoughnessMap);
    m.normalMap = path(kNormalMap);
    m.ormMap = path(kOrmMap);
    m.emissiveMap = path(kEmissiveMap);
    m.occlusionMap = path(kAmbientOcclusionMap);
    m.baseColorSrgb = scene::materials::srgb(*n, kBaseColorMap);
    m.metallicSrgb = scene::materials::srgb(*n, kMetallicMap);
    m.roughnessSrgb = scene::materials::srgb(*n, kRoughnessMap);
    m.ormSrgb = scene::materials::srgb(*n, kOrmMap);
    m.emissiveSrgb = scene::materials::srgb(*n, kEmissiveMap);
    return m;
}

void registerMaterialMaps()
{
    registerMap({.id = "vertexColor",
                 .label = "Vertex Colour",
                 .suffix = "vertexcolor",
                 .doc = "The high's vertex colours, interpolated where each ray hit. option set (a colour set name; empty: the first).",
                 .background = {1.0f, 1.0f, 1.0f, 1.0f},
                 .linear = false,
                 .cpu = vertexColorKernel});
    const char* transfer = " Transferred from the high's material (its texture where set, else its constant) at the hit's UV.";
    registerMap({.id = "baseColor",
                 .label = "Base Colour",
                 .suffix = "basecolor",
                 .doc = std::string("sRGB base colour.") + transfer +
                        " Option source vertexColor reads the high's vertex colours instead (polypaint), option set naming the colour set.",
                 .background = {0.5f, 0.5f, 0.5f, 1.0f},
                 .linear = false,
                 .cpu = baseColorKernel(),
                 .prepare = materialPrepare(Channel::BaseColor)});
    registerMap({.id = "roughness",
                 .label = "Roughness",
                 .suffix = "roughness",
                 .doc = std::string("Linear roughness; a packed ORM/MRO map wins over the roughness map.") + transfer,
                 .background = {0.5f, 0.5f, 0.5f, 1.0f},
                 .cpu = materialKernel(Channel::Roughness),
                 .prepare = materialPrepare(Channel::Roughness)});
    registerMap({.id = "metallic",
                 .label = "Metallic",
                 .suffix = "metallic",
                 .doc = std::string("Linear metallic; a packed ORM/MRO map wins over the metallic map.") + transfer,
                 .background = {0.0f, 0.0f, 0.0f, 1.0f},
                 .cpu = materialKernel(Channel::Metallic),
                 .prepare = materialPrepare(Channel::Metallic)});
    registerMap({.id = "emissive",
                 .label = "Emissive",
                 .suffix = "emissive",
                 .doc = std::string("sRGB emissive colour scaled by emissiveIntensity (8 and 16 bit files clip above 1).") + transfer,
                 .background = {0.0f, 0.0f, 0.0f, 1.0f},
                 .linear = false,
                 .cpu = materialKernel(Channel::Emissive),
                 .prepare = materialPrepare(Channel::Emissive)});
    registerMap({.id = "materialOpacity",
                 .label = "Material Opacity",
                 .suffix = "matopacity",
                 .doc = std::string("The high's opacity: 1 for opaque materials, else opacity times the base colour map's alpha (leaf "
                                    "cards). Coverage of the low's UVs is the alpha map.") +
                        transfer,
                 .background = {0.0f, 0.0f, 0.0f, 1.0f},
                 .cpu = materialKernel(Channel::Opacity),
                 .prepare = materialPrepare(Channel::Opacity)});
}
} // namespace lila::bake
