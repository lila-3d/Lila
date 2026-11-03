#include "ui/ViewportWidgetDetail.h"

#include "scene/LightingChannels.h"
#include "scene/SceneIndex.h"

namespace lila::ui
{
using namespace viewportDetail;

namespace
{
/** Everything outside a mesh node that its draw depends on, read once per frame. */
struct MeshSettings
{
    bool render = false; // a render view: shaded, no wire, highlight, isolation or preselection
    bool wireframe = false, wireOnShaded = false, showMaterials = false, smoothSubdivisions = false, inherit = false, maskShown = true;
    render::VertexColorMode vertexColors = render::VertexColorMode::Off;
    render::ColorChannel colorChannel = render::ColorChannel::Rgb;
    glm::vec4 componentWire{0.0f};
    float componentWireWidth = 1.0f;
    uint64_t isolation = 0, materials = 0, epoch = 0, structure = 0;

    bool operator==(const MeshSettings&) const = default;
};

/** One mesh node's draws as its last rebuild made them, before this frame's selection and snap highlights. */
struct MeshEntry
{
    const scene::Node* node = nullptr;
    scene::NodeId id = scene::kNoNode;
    uint64_t pass = 0; // node->evaluatedPass() when built; 0 rebuilds on the next frame
    uint64_t settings = 0;
    bool hilite = false;
    std::vector<render::MeshDraw> draws; // empty when the node draws nothing
    scene::MeshPtr shown;
    glm::dmat4 world{1.0};
    scene::NodeId arrayed = scene::kNoNode;
};

/** The node's draws under these settings, or none when it is hidden. */
void buildEntry(MeshEntry& e, scene::Graph& g, const MeshSettings& set, const scene::sceneIndex::Hierarchy& hierarchy,
                const scene::isolation::Set* iso, bool hilite)
{
    const scene::Node& n = *e.node;
    const scene::NodeId id = n.id();
    e.pass = n.evaluatedPass();
    e.hilite = hilite;
    e.draws.clear();
    e.shown.reset();
    if (!hierarchy.visible(id) || (iso && !iso->shows(id)) || !set.maskShown)
        return;
    const int smooth = int(n.as<int64_t>(scene::MeshNode::kSmoothPreview));
    const scene::MeshPtr& outMesh = n.as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    scene::MeshPtr shown = smoothedFor(outMesh, smooth);
    std::vector<render::Material> hooked;
    const bool previewed = shown && viewportHooks::hookedMaterials(id, *shown, hooked);
    const bool textured = (set.showMaterials || previewed) && shown;
    const bool tangents = textured && (previewed || needsTangents(*shown));
    // Geometry nodes instances draw each referenced mesh once, instanced, unless something needs the flat mesh's own draw.
    const auto cageWire = set.smoothSubdivisions ? nullptr : smoothCageFor(outMesh, smooth);
    auto part = isolate::masked(iso, id, shown, smooth, tangents, cageWire.get());
    gn::InstanceViewPtr instanced;
    if (smooth == 0 && !previewed && !hilite && !part && !cageWire)
        instanced = gn::instanceViewFor(g, id);
    auto rm = instanced ? nullptr : renderMeshFor(shown, tangents, id);
    if (!rm && !instanced)
    {
        // A mesh with faces but no render copy yet tries again next frame.
        if (shown && shown->numFaces() > 0)
            e.pass = 0;
        return;
    }
    render::MeshDraw d;
    if (part)
        d.mesh = part->mesh;
    else if (cageWire)
        d.mesh = cageWireMeshFor(*cageWire, tangents);
    else
        d.mesh = std::move(rm);
    if (previewed)
        d.materials = std::move(hooked);
    else if (textured && !instanced)
        d.materials = drawMaterials(*shown);
    e.world = shownWorld(n);
    e.shown = shown;
    d.world = glm::mat4(e.world);
    d.node = uint64_t(id);
    d.lightingChannels = scene::lightingChannels::mask(n);
    if (set.vertexColors != render::VertexColorMode::Off && shown && !geo::colors::current(*shown).empty())
    {
        d.vertexColors = set.vertexColors;
        d.colorChannel = set.colorChannel;
    }
    d.flags = 0;
    // Lattices are cages to see through, so they only ever draw as wire.
    const bool wireOnly = n.type().isA("lattice");
    const scene::displayLayers::Display layer = scene::displayLayers::display(g, id);
    const bool templated = layer.type == scene::displayLayers::DisplayType::Template;
    if (!set.wireframe && !wireOnly && !templated)
        d.flags |= render::DrawShaded;
    if (set.wireframe || set.wireOnShaded || wireOnly || templated || hilite)
        d.flags |= render::DrawWire;
    if (templated && !set.wireframe)
        d.flags |= render::DrawWireDepth;
    if (layer.color)
        d.wireColor = glm::vec4(viewportDisplay::layerTint(glm::vec3(*layer.color)), 1.0f);
    else if (templated)
        d.wireColor = kTemplateWireColor;
    if (hilite)
    {
        if (set.componentWire.a > 0.0f)
            d.wireColor = set.componentWire;
        d.wireWidth = set.componentWireWidth;
    }
    e.arrayed = hierarchy.arrayAbove(id);
    if (e.arrayed != scene::kNoNode)
        d.instances = arrayPlacements(g, id, n.as<glm::dmat4>(scene::Transform::kWorldMatrix));
    if (instanced)
    {
        render::FrameState parts;
        pushInstanceDraws(parts, d, *instanced, textured);
        e.draws = std::move(parts.meshes);
    }
    else
        e.draws.push_back(std::move(d));
}

MeshSettings meshSettings(scene::Graph& g, const ViewportWidget::RenderView* rv, const scene::isolation::Set& iso, uint64_t epoch, double dpr)
{
    MeshSettings set;
    const PrefStore& p = PrefStore::get();
    set.render = rv != nullptr;
    set.wireframe = !rv && p.value<std::string>("general", "/viewport/displayMode", "shaded") == "wireframe";
    set.wireOnShaded = !rv && p.value<bool>("general", "/viewport/wireOnShaded", false);
    set.showMaterials = materialsShown() && !viewportDisplay::defaultMaterial();
    set.smoothSubdivisions = p.value<bool>("general", viewportDisplay::kSmoothSubdivisions, false);
    set.inherit = scene::inheritVisibility();
    set.maskShown = scene::selectionMask::shown(scene::selectionMask::Kind::Mesh);
    set.vertexColors = viewportDisplay::vertexColorMode();
    set.colorChannel = viewportDisplay::colorChannel();
    set.componentWire = componentPalette().wire;
    set.componentWireWidth = viewportDisplay::hairlineWidth(viewportDisplay::componentThickness().edge, dpr);
    set.isolation = rv ? 0 : iso.version;
    set.materials = materialsEpoch();
    set.epoch = epoch;
    set.structure = g.structure();
    return set;
}
} // namespace

struct ViewportWidget::MeshDrawCache
{
    uint64_t structure = 0;
    std::vector<MeshEntry> entries; // parallel to sceneIndex::ofType(g, "mesh")
    MeshSettings settings;
    uint64_t settingsId = 0;
    std::unordered_map<const scene::NodeType*, bool> overridden;
};

void ViewportWidget::drawMeshNodes(render::FrameState& s, const RenderView* rv, const std::unordered_set<scene::NodeId>& lodHidden,
                                   const std::unordered_set<scene::NodeId>& underSelection, bool highlight,
                                   std::vector<displayAids::MeshEntry>* aids)
{
    LILA_ZONE();
    scene::Graph& g = scene::Graph::current();
    if (!mMeshDraws)
        mMeshDraws = std::make_unique<MeshDrawCache>();
    MeshDrawCache& c = *mMeshDraws;
    const std::vector<scene::Node*>& meshes = scene::sceneIndex::ofType(g, "mesh");
    const MeshSettings set = meshSettings(g, rv, mIsolation, mDisplayEpoch, devicePixelRatioF());
    const scene::sceneIndex::Hierarchy& hierarchy = scene::sceneIndex::hierarchy(g, set.inherit);
    const scene::isolation::Set* iso = rv ? nullptr : &mIsolation;

    // A render view draws its own way and keeps nothing.
    MeshDrawCache scratch;
    MeshDrawCache& cache = rv ? scratch : c;
    if (!rv && !(set == c.settings))
    {
        c.settings = set;
        ++c.settingsId;
        c.overridden.clear();
    }
    if (cache.structure != g.structure())
    {
        // Keep the entries of nodes that are still there; new nodes start empty.
        std::unordered_map<scene::NodeId, MeshEntry> old;
        for (MeshEntry& e : cache.entries)
            old.emplace(e.id, std::move(e));
        cache.entries.assign(meshes.size(), {});
        for (size_t i = 0; i < meshes.size(); ++i)
            if (auto it = old.find(meshes[i]->id()); it != old.end() && it->second.node == meshes[i])
                cache.entries[i] = std::move(it->second);
            else
            {
                cache.entries[i].node = meshes[i];
                cache.entries[i].id = meshes[i]->id();
            }
        cache.structure = g.structure();
    }

    std::unordered_set<scene::NodeId> hilite;
    const bool componentMode = scene::components::mode() != scene::ComponentMode::Object;
    if (!rv && componentMode)
        for (scene::NodeId id : hilited(mIsolation))
            hilite.insert(id);
    std::unordered_set<scene::NodeId> selected;
    if (highlight && !componentMode)
        selected.insert(g.selection().begin(), g.selection().end());
    const viewportDisplay::SelectionOrderColors order = viewportDisplay::selectionOrderColors(g);
    const glm::vec4 snapColor = themeLinear("wireSnapSurface", kSnapSurfaceWireColor);
    const std::vector<scene::NodeId> snaps = rv ? std::vector<scene::NodeId>() : surfaceSnap::surfaces();
    s.meshes.reserve(s.meshes.size() + meshes.size());
    const bool preselecting = !rv && mPreselect.mode == scene::ComponentMode::Object && !componentMode && mPreselect.node != scene::kNoNode;

    for (size_t i = 0; i < meshes.size(); ++i)
    {
        MeshEntry& e = cache.entries[i];
        const scene::Node& n = *meshes[i];
        const scene::NodeId id = n.id();
        auto [over, added] = cache.overridden.try_emplace(&n.type());
        if (added)
            over->second = overridden(n);
        if (over->second || lodHidden.contains(id) || viewportHooks::hiddenInView(id))
            continue;
        const bool hot = hilite.contains(id);
        if (e.pass != n.evaluatedPass() || e.settings != cache.settingsId || e.hilite != hot || rv)
        {
            LILA_ZONE_N("buildFrame rebuild mesh node");
            buildEntry(e, g, set, hierarchy, iso, hot);
            e.settings = cache.settingsId;
        }
        if (e.draws.empty())
            continue;
        if (aids)
            aids->push_back({e.shown, e.world, {}});
        uint32_t flags = 0;
        if (selected.contains(id))
            flags = order.other(id) ? render::DrawSelected | render::DrawSelectedOther : render::DrawSelected;
        else if (preselecting && (mPreselect.node == id || (e.arrayed != scene::kNoNode && preselectedArray(g, id, mPreselect.node))))
            flags = render::DrawPreselect;
        else if (!componentMode && underSelection.contains(id))
            flags = render::DrawUnderSelected;
        const bool snap = !snaps.empty() && std::ranges::find(snaps, id) != snaps.end();
        for (const render::MeshDraw& base : e.draws)
        {
            render::MeshDraw& d = s.meshes.emplace_back(base);
            d.flags |= flags;
            if (snap)
            {
                d.flags |= render::DrawWire;
                if (!hot || set.componentWire.a <= 0.0f)
                    d.wireColor = snapColor;
            }
        }
    }
}
} // namespace lila::ui
