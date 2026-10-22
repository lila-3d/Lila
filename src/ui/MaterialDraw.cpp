#include "ui/MaterialDraw.h"

#include "core/PrefStore.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "ui/TextureCache.h"
#include "ui/ViewportTool.h"

namespace lila::ui
{
namespace
{
using namespace scene::material;

const std::string& str(const scene::Node& n, int attr) { return n.as<std::string>(attr); }
float num(const scene::Node& n, int attr) { return float(n.as<double>(attr)); }
glm::vec3 color(const scene::Node& n, int attr) { return glm::vec3(n.as<glm::dvec3>(attr)); }
std::string file(const scene::Node& n, int attr) { return scene::materials::resolvePath(str(n, attr)); }

render::Material convert(const scene::Node& n)
{
    render::Material m;
    m.baseColor = color(n, kBaseColor);
    m.metallic = num(n, kMetallic);
    m.roughness = num(n, kRoughness);
    m.specular = num(n, kSpecular);
    m.opacity = num(n, kOpacity);
    m.opacityClip = num(n, kOpacityMaskClip);
    m.flags = 0;

    if ((m.baseColorMap = textures::get(file(n, kBaseColorMap), true)))
        m.flags |= render::MatBaseColorMap;
    if ((m.normalMap = textures::get(file(n, kNormalMap), false)))
    {
        m.flags |= render::MatNormalMap;
        if (NormalFormat(n.as<int64_t>(kNormalFormat)) == NormalFormat::OpenGL)
            m.flags |= render::MatNormalGreenUp;
    }

    std::string orm = file(n, kOrmMap), ao = file(n, kAmbientOcclusionMap), rough = file(n, kRoughnessMap),
                metal = file(n, kMetallicMap);
    if ((m.ormMap = textures::packed(orm, ao, rough, metal,
                                     scene::materials::packedChannels(PackedLayout(n.as<int64_t>(kPackedLayout))))))
    {
        if (!orm.empty())
            m.flags |= render::MatOcclusionMap | render::MatRoughnessMap | render::MatMetallicMap;
        else
            m.flags |= (ao.empty() ? 0u : uint32_t(render::MatOcclusionMap)) | (rough.empty() ? 0u : uint32_t(render::MatRoughnessMap)) |
                       (metal.empty() ? 0u : uint32_t(render::MatMetallicMap));
    }

    float intensity = num(n, kEmissiveIntensity);
    if ((m.emissiveMap = textures::get(file(n, kEmissiveMap), true)))
    {
        m.flags |= render::MatEmissiveMap;
        m.emissive = glm::vec3(intensity);
    }
    else
        m.emissive = color(n, kEmissiveColor) * intensity;

    switch (BlendMode(n.as<int64_t>(kBlendMode)))
    {
    case BlendMode::Masked: m.flags |= render::MatMasked; break;
    case BlendMode::Translucent: m.flags |= render::MatTranslucent; break;
    default: break;
    }
    if (n.as<bool>(kTwoSided))
        m.flags |= render::MatTwoSided;
    if (n.as<int64_t>(kUvSet) >= 1)
        m.flags |= render::MatUv1;
    return m;
}

size_t slotCount(const geo::Mesh& mesh)
{
    return std::max<size_t>(mesh.materialNames().size(), 1);
}
} // namespace

bool materialsShown()
{
    return PrefStore::get().value<bool>("general", "/viewport/materials", true);
}

void setMaterialsShown(bool on)
{
    PrefStore::get().set("general", "/viewport/materials", on);
    viewportTools::requestRedraw();
}

bool needsTangents(const geo::Mesh& mesh)
{
    scene::Graph& g = scene::Graph::current();
    for (size_t i = 0; i < slotCount(mesh); ++i)
        if (scene::Node* n = scene::materials::find(g, scene::materials::slotName(mesh, int32_t(i))))
            if (!str(*n, kNormalMap).empty())
                return true;
    return false;
}

std::vector<render::Material> drawMaterials(const geo::Mesh& mesh)
{
    LILA_ZONE();

    scene::Graph& g = scene::Graph::current();
    std::vector<render::Material> out(slotCount(mesh));
    for (size_t i = 0; i < out.size(); ++i)
        if (scene::Node* n = scene::materials::find(g, scene::materials::slotName(mesh, int32_t(i))))
            out[i] = convert(*n);
    return out;
}
} // namespace lila::ui
