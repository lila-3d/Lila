#include "ui/MaterialDraw.h"

#include "core/PrefStore.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "ui/TexLive.h"
#include "ui/TextureCache.h"
#include "ui/ViewportTool.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

namespace lila::ui
{
namespace
{
using namespace scene::material;

const std::string& str(const scene::Node& n, int attr) { return n.as<std::string>(attr); }
float num(const scene::Node& n, int attr) { return float(n.as<double>(attr)); }
glm::vec3 color(const scene::Node& n, int attr) { return glm::vec3(n.as<glm::dvec3>(attr)); }
std::string file(const scene::Node& n, int attr) { return scene::materials::resolvePath(str(n, attr)); }

/** A stepped pseudo-random offset per boil frame, so strokes jump between frames and scrub deterministically. */
glm::vec2 boilOffset(double seconds, double fps)
{
    const int64_t frame = int64_t(std::floor(seconds * std::max(fps, 0.1) + 1e-6));
    uint32_t h = uint32_t(frame) * 747796405u + 2891336453u;
    h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
    h = (h >> 22u) ^ h;
    return {float(h & 0xffffu) / 65536.0f, float(h >> 16u) / 65536.0f};
}

render::MaterialStyle style(const scene::Node& n)
{
    render::MaterialStyle s;
    s.model = render::ShadingModel(std::clamp<int64_t>(n.as<int64_t>(kShadingModel), 0, int64_t(render::ShadingModel::Eye)));
    if (s.model == render::ShadingModel::DefaultLit || s.model == render::ShadingModel::Unlit)
        return s;
    s.painterlyStrength = num(n, kPainterlyStrength);
    s.strokeNormal = num(n, kStrokeNormal);
    s.strokeScale = num(n, kStrokeScale);
    s.strokeLayerRatio = num(n, kStrokeLayerRatio);
    s.strokeMapping = uint32_t(std::clamp<int64_t>(n.as<int64_t>(kStrokeMapping), 0, 3));
    s.paletteCount = int(n.as<int64_t>(kPaletteCount));
    s.paletteShadow = color(n, kPaletteShadow);
    s.paletteLight = color(n, kPaletteLight);
    s.paletteAccent = color(n, kPaletteAccent);
    s.paletteContrast = num(n, kPaletteContrast);
    s.paletteSpread = num(n, kPaletteSpread);
    s.specularTint = num(n, kSpecularTint);
    if (n.as<bool>(kBoil))
        s.boilOffset = boilOffset(scene::Graph::current().time(), n.as<double>(kBoilFps));
    s.toonShadowColor = color(n, kToonShadowColor);
    s.toonLightTint = color(n, kToonLightTint);
    s.toonThreshold = num(n, kToonThreshold);
    s.toonSoftness = num(n, kToonSoftness);
    s.toonOuterOffset = num(n, kToonOuterOffset);
    s.toonOuterColor = color(n, kToonOuterColor);
    s.rimColor = color(n, kRimColor);
    s.rimWidth = num(n, kRimWidth);
    s.rimStrength = num(n, kRimStrength);
    s.foliageBottomColor = color(n, kFoliageBottomColor);
    s.foliageTopColor = color(n, kFoliageTopColor);
    s.foliageGradient = num(n, kFoliageGradient);
    s.foliageBottomShadow = num(n, kFoliageBottomShadow);
    s.foliageTopHighlight = num(n, kFoliageTopHighlight);
    s.foliageTranslucency = num(n, kFoliageTranslucency);
    s.foliageWrap = num(n, kFoliageWrap);
    s.subsurfaceColor = color(n, kSubsurfaceColor);
    s.subsurfaceAmount = num(n, kSubsurfaceAmount);
    s.sheenColor = color(n, kSheenColor);
    s.sheenRoughness = num(n, kSheenRoughness);
    s.clearcoat = num(n, kClearcoat);
    s.clearcoatRoughness = num(n, kClearcoatRoughness);
    s.anisotropy = num(n, kAnisotropy);
    s.anisotropyRotation = num(n, kAnisotropyRotation);
    s.hairShift = num(n, kHairShift);
    s.irisRadius = num(n, kIrisRadius);
    s.irisDepth = num(n, kIrisDepth);
    s.limbalRing = num(n, kLimbalRing);
    if (s.model == render::ShadingModel::Painterly)
        s.strokeMap = textures::get(scene::materials::strokeSheetPath(str(n, kStrokeMap)), false);
    return s;
}

/** A texture set bound to the material replaces the maps it has channels for. */
void applyTextureSet(const scene::Node& n, render::Material& m)
{
    std::optional<texLive::Maps> live = texLive::mapsFor(n.name());
    if (!live || !live->ready)
        return;
    if (live->baseColor)
    {
        m.baseColorMap = live->baseColor;
        m.flags |= render::MatBaseColorMap;
    }
    if (live->normal)
    {
        m.normalMap = live->normal;
        m.flags |= render::MatNormalMap | render::MatNormalGreenUp;
    }
    if (live->orm)
    {
        m.ormMap = live->orm;
        m.flags &= ~uint32_t(render::MatOcclusionMap | render::MatRoughnessMap | render::MatMetallicMap);
        m.flags |= (live->occlusion ? uint32_t(render::MatOcclusionMap) : 0u) | (live->roughness ? uint32_t(render::MatRoughnessMap) : 0u) |
                   (live->metallic ? uint32_t(render::MatMetallicMap) : 0u);
    }
    if (live->emissive)
    {
        m.emissiveMap = live->emissive;
        m.flags |= render::MatEmissiveMap;
        m.emissive = glm::vec3(num(n, kEmissiveIntensity));
    }
}

/** render::Material::sampling bits of one map. */
uint32_t samplingBits(const scene::Node& n, int mapAttr)
{
    const TextureSampling s = scene::materials::sampling(n, mapAttr);
    return uint32_t(s.wrapU) | uint32_t(s.wrapV) << 2 | (s.filter == TextureFilter::Nearest ? 16u : 0u);
}

/** The sampling of the four viewport maps; the packed map follows the first separate map set when there is no ORM map. */
uint32_t samplingOf(const scene::Node& n)
{
    int orm = kOrmMap;
    if (str(n, kOrmMap).empty())
        for (int a : {kAmbientOcclusionMap, kRoughnessMap, kMetallicMap})
            if (!str(n, a).empty())
            {
                orm = a;
                break;
            }
    return samplingBits(n, kBaseColorMap) | samplingBits(n, kNormalMap) << 8 | samplingBits(n, orm) << 16 | samplingBits(n, kEmissiveMap) << 24;
}

render::Material convert(const scene::Node& n)
{
    render::Material m;
    m.baseColor = color(n, kBaseColor);
    m.metallic = num(n, kMetallic);
    m.roughness = num(n, kRoughness);
    m.specular = num(n, kSpecular);
    m.bevelRadius = num(n, kBevelRadius);
    m.opacity = num(n, kOpacity);
    m.opacityClip = num(n, kOpacityMaskClip);
    m.flags = 0;
    m.sampling = samplingOf(n);

    if ((m.baseColorMap = textures::get(file(n, kBaseColorMap), true, true)))
        m.flags |= render::MatBaseColorMap;
    if ((m.normalMap = textures::get(file(n, kNormalMap), false, true)))
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
    if ((m.emissiveMap = textures::get(file(n, kEmissiveMap), true, true)))
    {
        m.flags |= render::MatEmissiveMap;
        m.emissive = glm::vec3(intensity);
    }
    else
        m.emissive = color(n, kEmissiveColor) * intensity;
    applyTextureSet(n, m);

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
    m.style = style(n);
    return m;
}

size_t slotCount(const geo::Mesh& mesh)
{
    return std::max<size_t>(mesh.materialNames().size(), 1);
}

bool sameMaps(const std::optional<texLive::Maps>& a, const std::optional<texLive::Maps>& b)
{
    if (a.has_value() != b.has_value())
        return false;
    return !a || (a->baseColor == b->baseColor && a->normal == b->normal && a->orm == b->orm && a->emissive == b->emissive &&
                  a->occlusion == b->occlusion && a->roughness == b->roughness && a->metallic == b->metallic && a->ready == b->ready);
}

/**
 * One slot name's converted material. Reused while the material node has not recomputed, no texture has arrived and
 * its texture set's maps are the same; checked again only when the scene, a texture or a redraw request moved the token.
 */
struct SlotEntry
{
    const scene::Node* node = nullptr;
    uint64_t pass = 0;
    uint64_t textures = 0;
    uint64_t version = 0, redraws = 0;
    bool timed = false; // boils: follows the scene time
    double time = 0.0;
    std::optional<texLive::Maps> live;
    render::Material material;
    bool normalMap = false;
};

struct SlotCache
{
    std::unordered_map<std::string, SlotEntry> slots;
    scene::Graph* graph = nullptr;
    int graphListener = 0;
    uint64_t textures = 1; // images that finished loading, which convert() picks up
    uint64_t redraws = 1;  // texture sets announce new maps this way
    uint64_t epoch = 1;    // bumped when a known slot's material changes or the slots are dropped
};

SlotCache& slotCache()
{
    static SlotCache c;
    static const bool listening = [] {
        textures::addListener([] { ++slotCache().textures; });
        viewportTools::addListener([] { ++slotCache().redraws; });
        return true;
    }();
    (void)listening;
    scene::Graph& g = scene::Graph::current();
    if (c.graph != &g)
    {
        if (c.graph)
            c.graph->removeListener(c.graphListener);
        c.graph = &g;
        c.slots.clear();
        ++c.epoch;
        // Which node a slot name finds changes only when nodes come, go or change names.
        c.graphListener = g.addListener(
            [](scene::Change change, scene::NodeId) {
                if (change == scene::Change::Created || change == scene::Change::Deleted || change == scene::Change::Renamed ||
                    change == scene::Change::Cleared || change == scene::Change::Reset)
                {
                    slotCache().slots.clear();
                    ++slotCache().epoch;
                }
            },
            true);
    }
    return c;
}

const SlotEntry& slotEntry(const std::string& slot)
{
    SlotCache& c = slotCache();
    scene::Graph& g = *c.graph;
    auto [it, added] = c.slots.try_emplace(slot);
    SlotEntry& e = it->second;
    if (!added && e.version == g.version() && e.redraws == c.redraws && e.textures == c.textures)
        return e;
    if (added)
        e.node = scene::materials::find(g, slot);
    std::optional<texLive::Maps> live = e.node ? texLive::mapsFor(e.node->name()) : std::nullopt;
    if (added || (e.node && e.node->evaluatedPass() != e.pass) || e.textures != c.textures || !sameMaps(live, e.live) || (e.timed && e.time != g.time()))
    {
        if (!added)
            ++c.epoch;
        e.material = e.node ? convert(*e.node) : render::Material{};
        e.pass = e.node ? e.node->evaluatedPass() : 0;
        e.live = std::move(live);
        e.timed = e.node && e.material.style.model != render::ShadingModel::DefaultLit && e.node->as<bool>(kBoil);
        e.time = g.time();
        e.textures = c.textures;
        e.normalMap = e.node && (!str(*e.node, kNormalMap).empty() || (e.live && e.live->normal));
    }
    e.version = g.version();
    e.redraws = c.redraws;
    return e;
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

uint64_t materialsEpoch()
{
    SlotCache& c = slotCache();
    for (auto& [slot, e] : c.slots)
        slotEntry(slot);
    return c.epoch;
}

bool needsTangents(const geo::Mesh& mesh)
{
    for (size_t i = 0; i < slotCount(mesh); ++i)
        if (slotEntry(scene::materials::slotName(mesh, int32_t(i))).normalMap)
            return true;
    return false;
}

std::vector<render::Material> drawMaterials(const geo::Mesh& mesh)
{
    std::vector<render::Material> out(slotCount(mesh));
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = slotEntry(scene::materials::slotName(mesh, int32_t(i))).material;
    return out;
}
} // namespace lila::ui
