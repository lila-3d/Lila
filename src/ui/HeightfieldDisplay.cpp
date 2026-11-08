#include "ui/HeightfieldDisplay.h"

#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "geo/Heightfield.h"
#include "gn/Gn.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/SceneIndex.h"
#include "ui/Actions.h"
#include "ui/MaterialDraw.h"
#include "ui/Menus.h"
#include "ui/TextureCache.h"

#include <QActionGroup>
#include <QMenu>

#include <algorithm>
#include <cmath>

namespace lila::ui::heightfieldDisplay
{
namespace
{
using T = ArgType;

constexpr const char* kPrefFile = "general";
constexpr const char* kDrawPref = "/viewport/heightfieldDraw";
constexpr const char* kOverlayPref = "/viewport/terrainOverlay";
constexpr const char* kRampPref = "/viewport/terrainOverlayRamp";
constexpr const char* kOpacityPref = "/viewport/terrainOverlayOpacity";
constexpr const char* kContourPref = "/viewport/terrainContours";
constexpr const char* kMajorPref = "/viewport/terrainContourMajor";
constexpr const char* kArrowsPref = "/viewport/terrainFlowArrows";
constexpr const char* kSpacingPref = "/viewport/terrainArrowSpacing";
constexpr const char* kDetailPref = "/viewport/terrainQuadPixels";

float srgbChannelToLinear(double c)
{
    c = std::clamp(c, 0.0, 1.0);
    return float(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4));
}

/** "#rrggbb" or [r, g, b] (display 0 to 1) as linear rgb. */
std::optional<glm::vec3> parseColor(const json& j)
{
    if (j.is_array() && j.size() >= 3 && j[0].is_number())
        return glm::vec3(srgbChannelToLinear(j[0].get<double>()), srgbChannelToLinear(j[1].get<double>()), srgbChannelToLinear(j[2].get<double>()));
    if (j.is_string())
    {
        const std::string s = j.get<std::string>();
        if (s.size() == 7 && s[0] == '#')
        {
            const unsigned long v = std::stoul(s.substr(1), nullptr, 16);
            return glm::vec3(srgbChannelToLinear(((v >> 16) & 0xff) / 255.0), srgbChannelToLinear(((v >> 8) & 0xff) / 255.0),
                             srgbChannelToLinear((v & 0xff) / 255.0));
        }
    }
    return std::nullopt;
}

/** A mask of h as values the renderer keeps, sharing h's lifetime; null when h has no such mask. */
render::GridValues maskValues(const std::shared_ptr<const geo::Heightfield>& h, const std::string& name)
{
    if (name.empty())
        return nullptr;
    const std::vector<float>* m = h->findMask(name);
    if (!m || m->size() < h->size())
        return nullptr;
    return render::GridValues(h, m);
}

json parseLayers(const std::string& source)
{
    if (source.empty())
        return json::array();
    json j = json::parse(source, nullptr, false);
    return j.is_array() ? j : json::array();
}

/** The terrain mesh a node names: the mesh itself, or the mesh a geometry-nodes modifier sits on. */
scene::Node* terrainMesh(scene::Graph& g, const std::string& name, const char* cmd)
{
    scene::Node* n = g.find(name);
    if (!n)
        throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
    if (gn::isGnModifier(*n))
        if (const scene::NodeId mesh = scene::modifiers::meshOf(g, n->id()); mesh != scene::kNoNode)
            n = g.node(mesh);
    g.evaluate();
    if (!heightfieldOf(g, n->id()))
        throw CommandError(std::string(cmd) + ": " + n->path() + " makes no heightfield (make a terrain with terrainCreate)");
    return n;
}

json settingsJson(const Settings& s)
{
    return {{"draw", s.drawsHeightfields() ? "heightfield" : "mesh"},
            {"heightfieldDrawPref", s.heightfieldDraw},
            {"overlay", s.overlay},
            {"overlayRamp", s.overlayRamp == 0 ? "grey" : "turbo"},
            {"overlayOpacity", s.overlayOpacity},
            {"contours", s.contourInterval},
            {"contourMajor", s.contourMajor},
            {"flowArrows", s.flowArrows},
            {"arrowSpacing", s.arrowSpacing},
            {"quadPixels", s.quadPixels}};
}

/** Every mean of a mask over its grid, 0 when missing. */
double meanOf(const std::vector<float>* m)
{
    if (!m || m->empty())
        return 0.0;
    double sum = 0.0;
    for (float v : *m)
        sum += std::isfinite(v) ? double(v) : 0.0;
    return sum / double(m->size());
}

json layerReport(const geo::Heightfield& h, const json& layers)
{
    json out = json::array();
    for (const json& l : layers)
    {
        const std::string name = l.value("name", std::string());
        const std::string mask = l.value("mask", name);
        const std::vector<float>* m = h.findMask(mask);
        json e{{"name", name}, {"mask", mask}, {"found", m != nullptr}, {"coverage", meanOf(m)}};
        for (const char* map : {"baseColor", "normal"})
            if (l.contains(map) && l[map].is_string())
                if (std::string err = textures::error(l[map].get<std::string>()); !err.empty())
                    e[std::string(map) + "Error"] = err;
        out.push_back(std::move(e));
    }
    return out;
}
} // namespace

Settings settings()
{
    const PrefStore& p = PrefStore::get();
    Settings s;
    s.heightfieldDraw = p.value<bool>(kPrefFile, kDrawPref, true);
    s.overlay = p.value<std::string>(kPrefFile, kOverlayPref, "");
    s.overlayRamp = p.value<std::string>(kPrefFile, kRampPref, "turbo") == "grey" ? 0 : 1;
    s.overlayOpacity = float(std::clamp(p.value<double>(kPrefFile, kOpacityPref, 0.75), 0.0, 1.0));
    s.contourInterval = float(std::max(p.value<double>(kPrefFile, kContourPref, 0.0), 0.0));
    s.contourMajor = std::clamp(p.value<int>(kPrefFile, kMajorPref, 5), 1, 100);
    s.flowArrows = p.value<bool>(kPrefFile, kArrowsPref, false);
    s.arrowSpacing = float(std::clamp(p.value<double>(kPrefFile, kSpacingPref, 48.0), 8.0, 512.0));
    s.quadPixels = float(std::clamp(p.value<double>(kPrefFile, kDetailPref, 4.0), 1.0, 64.0));
    return s;
}

std::shared_ptr<const geo::Heightfield> heightfieldOf(scene::Graph& g, scene::NodeId id)
{
    const std::vector<scene::NodeId> stack = scene::modifiers::stack(g, id);
    for (auto it = stack.rbegin(); it != stack.rend(); ++it)
        if (const scene::Node* m = g.node(*it); m && gn::isGnModifier(*m))
            if (auto h = gn::modifierHeightfield(m->id()); h && !h->empty())
                return h;
    return nullptr;
}

std::string layersSource(const scene::Node& n)
{
    const int i = n.type().attrIndex(kLayersAttr);
    if (i < 0)
        return {};
    const scene::PlugValue& v = n.value(i);
    return std::holds_alternative<std::string>(v) ? std::get<std::string>(v) : std::string();
}

std::optional<render::HeightfieldDraw> drawFor(scene::Graph& g, scene::NodeId id, const Settings& set, bool textured, int* pendingMaps)
{
    std::shared_ptr<const geo::Heightfield> h = heightfieldOf(g, id);
    const scene::Node* n = g.node(id);
    if (!h || !n)
        return std::nullopt;
    LILA_ZONE();
    render::HeightfieldDraw d;
    d.heights = std::shared_ptr<const std::vector<float>>(h, &h->heights);
    d.nx = h->nx;
    d.nz = h->nz;
    d.cell = float(h->cell);
    d.origin = glm::vec2(h->origin);
    d.node = uint64_t(id);
    if (textured)
    {
        if (const scene::MeshPtr& mesh = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh))
            d.materials = drawMaterials(*mesh);
        for (const json& l : parseLayers(layersSource(*n)))
        {
            if (!l.is_object())
                continue;
            const std::string name = l.value("name", std::string());
            render::HeightfieldLayer layer;
            layer.weights = maskValues(h, l.value("mask", name));
            if (!layer.weights)
                continue;
            if (l.contains("color"))
                if (auto c = parseColor(l["color"]))
                    layer.color = *c;
            layer.roughness = float(std::clamp(l.value("roughness", 0.8), 0.0, 1.0));
            layer.tiling = float(std::max(l.value("tiling", 400.0), 1.0));
            auto map = [&](const char* key, bool srgb) -> render::TexturePtr {
                const std::string path = l.value(key, std::string());
                if (path.empty())
                    return nullptr;
                render::TexturePtr t = textures::get(path, srgb);
                if (!t && pendingMaps && textures::error(path).empty())
                    ++*pendingMaps;
                return t;
            };
            layer.baseColorMap = map("baseColor", true);
            layer.normalMap = map("normal", false);
            d.layers.push_back(std::move(layer));
        }
    }
    d.overlay = maskValues(h, set.overlay);
    d.overlayRamp = set.overlayRamp;
    d.overlayOpacity = set.overlayOpacity;
    d.contourInterval = set.contourInterval;
    d.contourMajor = set.contourMajor;
    d.flowArrows = set.flowArrows;
    d.flow = maskValues(h, "flow");
    d.arrowSpacing = set.arrowSpacing;
    d.quadPixels = set.quadPixels;
    return d;
}

void registerCommands()
{
    ops::registerCommand({
        .name = "terrainDisplay",
        .doc = "How the viewport shows terrains: drawn from their heights (the heightfield draw: level of detail for 8K to 16K grids, "
               "paint layers) or as their mesh, and the terrain display options, which only the heightfield draw shows: a mask "
               "over the surface in a colour ramp, height contour lines and flow arrows down the slopes. A display option "
               "switches terrains to the heightfield draw while it is on. Returns the settings and, per terrain, its grid and masks.",
        .flags =
            {
                {"draw", "", T::String, nullptr, "heightfield or mesh: the /viewport/heightfieldDraw pref.", {"heightfield", "mesh"}},
                {"overlay", "ov", T::String, nullptr, "A mask to show over the surface (flow, wear, deposit, a paint layer...); empty for none."},
                {"overlayRamp", "", T::String, nullptr, "Colour ramp of the overlay: turbo (data) or grey.", {"turbo", "grey"}},
                {"overlayOpacity", "", T::Float, nullptr, "How strongly the overlay covers the shading.", {}, {}, 0.0, 1.0},
                {"contours", "c", T::Float, nullptr, "Contour line interval in cm; 0 for none.", {}, {}, 0.0, 1.0e7},
                {"contourMajor", "", T::Int, nullptr, "Every this many contour lines is drawn bolder.", {}, {}, 1.0, 100.0},
                {"flowArrows", "fa", T::Bool, nullptr, "Arrows down the slopes, stronger where the flow mask is."},
                {"arrowSpacing", "", T::Float, nullptr, "Rough spacing of the flow arrows in pixels.", {}, {}, 8.0, 512.0},
                {"quadPixels", "qp", T::Float, nullptr, "Level of detail of the heightfield draw: how many pixels the finest grid quads span. Lower is finer and slower; 4 by default.", {}, {}, 1.0, 64.0},
            },
        .undoable = false,
        .run = [](const Args& a) -> json {
            PrefStore& p = PrefStore::get();
            if (a.has("draw"))
                p.set(kPrefFile, kDrawPref, a.get<std::string>("draw") == "heightfield");
            if (a.has("overlay"))
            {
                std::string o = a.get<std::string>("overlay");
                p.set(kPrefFile, kOverlayPref, o == "none" ? std::string() : o);
            }
            if (a.has("overlayRamp"))
                p.set(kPrefFile, kRampPref, a.get<std::string>("overlayRamp"));
            if (a.has("overlayOpacity"))
                p.set(kPrefFile, kOpacityPref, a.get<double>("overlayOpacity"));
            if (a.has("contours"))
                p.set(kPrefFile, kContourPref, a.get<double>("contours"));
            if (a.has("contourMajor"))
                p.set(kPrefFile, kMajorPref, a.get<int>("contourMajor"));
            if (a.has("flowArrows"))
                p.set(kPrefFile, kArrowsPref, a.get<bool>("flowArrows"));
            if (a.has("arrowSpacing"))
                p.set(kPrefFile, kSpacingPref, a.get<double>("arrowSpacing"));
            if (a.has("quadPixels"))
                p.set(kPrefFile, kDetailPref, a.get<double>("quadPixels"));
            const Settings s = settings();
            json out = settingsJson(s);
            if (!s.heightfieldDraw && s.overlays())
                out["note"] = "terrains use the heightfield draw while a display option is on";
            scene::Graph& g = scene::Graph::current();
            g.evaluate();
            json terrains = json::array();
            bool overlayFound = false;
            for (scene::Node* n : scene::sceneIndex::ofType(g, "mesh"))
                if (auto h = heightfieldOf(g, n->id()))
                {
                    json masks = json::array();
                    for (const auto& [name, values] : h->masks)
                        masks.push_back(name);
                    overlayFound = overlayFound || h->findMask(s.overlay) != nullptr;
                    terrains.push_back({{"node", n->path()},
                                        {"samples", {h->nx, h->nz}},
                                        {"cell", h->cell},
                                        {"masks", masks},
                                        {"layers", parseLayers(layersSource(*n)).size()}});
                }
            out["terrains"] = terrains;
            if (!s.overlay.empty() && !overlayFound)
            {
                out["warning"] = "no terrain has a mask named " + s.overlay;
                log::warn("terrainDisplay: no terrain has a mask named {}", s.overlay);
            }
            return out;
        },
        .examples = {"terrainDisplay(draw='heightfield')", "terrainDisplay(overlay='flow', contours=1000, contourMajor=5)",
                     "terrainDisplay(flowArrows=True, overlay='')"},
    });

    ops::registerCommand({
        .name = "terrainDisplayLayers",
        .doc = "The paint layers a terrain shows in the viewport's heightfield draw: each paints its colour, roughness and "
               "maps over the terrain's material where its mask's weights (0 to 1) lie. A layer is {name, mask (default name), "
               "color ('#rrggbb' or [r, g, b] 0 to 1), roughness, baseColor (image file), normal (image file, DirectX), tiling "
               "(cm per repeat of the maps, seen from above)}. Without layers, the list is returned with each mask's coverage. "
               "Stored on the terrain's mesh as the terrainLayers attribute (JSON), which layer rules may write directly.",
        .positionalDoc = "The terrain: its mesh or its geometry-nodes modifier; the selection when none.",
        .flags =
            {
                {"layers", "l", T::Any, nullptr, "The layers, replacing the ones there: a list of {name, mask, color, roughness, baseColor, normal, tiling}."},
                {"clear", "", T::Bool, false, "Remove every layer."},
            },
        .run = [](const Args& a) -> json {
            const std::vector<std::string> objects = a.objects();
            if (objects.empty())
                throw CommandError("terrainDisplayLayers: name a terrain or select one");
            scene::Graph& g = scene::Graph::current();
            scene::Node* n = terrainMesh(g, objects.front(), "terrainDisplayLayers");
            if (a.has("layers") || a.get<bool>("clear", false))
            {
                json layers = a.get<bool>("clear", false) ? json::array() : a.flags["layers"];
                if (!layers.is_array())
                    throw CommandError("terrainDisplayLayers: layers is a list of {name, mask, color, roughness, baseColor, normal, tiling}");
                for (const json& l : layers)
                {
                    if (!l.is_object() || !l.contains("name") || !l["name"].is_string())
                        throw CommandError("terrainDisplayLayers: every layer needs a name");
                    if (l.contains("color") && !parseColor(l["color"]))
                        throw CommandError("terrainDisplayLayers: layer " + l["name"].get<std::string>() + " has a colour that is neither '#rrggbb' nor [r, g, b]");
                }
                int i = n->type().attrIndex(kLayersAttr);
                if (i < 0)
                    i = g.addAttr(n->id(), scene::AttrDef{.name = kLayersAttr, .type = scene::PlugType::String, .defaultValue = std::string(),
                                                          .flags = scene::AttrHidden, .doc = "Paint layers the viewport's heightfield draw shows (JSON)."});
                g.setValue({n->id(), i}, layers.dump());
            }
            std::shared_ptr<const geo::Heightfield> h = heightfieldOf(g, n->id());
            json masks = json::array();
            for (const auto& [name, values] : h->masks)
                masks.push_back(name);
            return {{"node", n->path()}, {"layers", layerReport(*h, parseLayers(layersSource(*n)))}, {"masks", masks},
                    {"draw", settings().drawsHeightfields() ? "heightfield" : "mesh"}};
        },
        .examples = {"terrainDisplayLayers('Terrain', layers=[{'name': 'rock', 'mask': 'wear', 'color': '#7a7068', 'roughness': 0.9},\n"
                     "    {'name': 'sand', 'mask': 'deposit', 'color': [0.76, 0.66, 0.48], 'baseColor': 'textures/sand.png', 'tiling': 300}])",
                     "terrainDisplayLayers('Terrain')"},
    });
}

void registerActions()
{
    actions::add({.id = "viewport.terrain.heightfieldDraw",
                  .label = "Heightfield Draw",
                  .category = "Display",
                  .run = [] { ops::execute("terrainDisplay", {{"draw", settings().heightfieldDraw ? "mesh" : "heightfield"}}); },
                  .doc = "Terrains draw from their heights: level of detail for huge grids, paint layers and terrain overlays.",
                  .checked = [] { return settings().heightfieldDraw; }});
    actions::add({.id = "viewport.terrain.contours",
                  .label = "Contours",
                  .category = "Display",
                  .run =
                      [] {
                          // Off keeps the last interval, so turning it back on restores it.
                          PrefStore& p = PrefStore::get();
                          const double interval = settings().contourInterval;
                          if (interval > 0.0)
                              p.set(kPrefFile, "/viewport/terrainContoursLast", interval);
                          const double back = p.value<double>(kPrefFile, "/viewport/terrainContoursLast", 1000.0);
                          ops::execute("terrainDisplay", {{"contours", interval > 0.0 ? 0.0 : back}});
                      },
                  .doc = "Height contour lines over terrains; set the interval with terrainDisplay(contours=...).",
                  .checked = [] { return settings().contourInterval > 0.0f; }});
    actions::add({.id = "viewport.terrain.flowArrows",
                  .label = "Flow Arrows",
                  .category = "Display",
                  .run = [] { ops::execute("terrainDisplay", {{"flowArrows", !settings().flowArrows}}); },
                  .doc = "Arrows down the slopes of terrains, stronger along the flow mask's channels.",
                  .checked = [] { return settings().flowArrows; }});
}

void addMenuItems(const std::string& menu)
{
    menus::addActions(menu, {"viewport.terrain.heightfieldDraw", "viewport.terrain.contours", "viewport.terrain.flowArrows"});
    menus::addSubmenu(menu, "Overlay", [](QMenu* m) {
        QObject::connect(m, &QMenu::aboutToShow, m, [m] {
            m->clear();
            std::vector<std::string> masks;
            scene::Graph& g = scene::Graph::current();
            for (scene::Node* n : scene::sceneIndex::ofType(g, "mesh"))
                if (auto h = heightfieldOf(g, n->id()))
                    for (const auto& entry : h->masks)
                        if (std::ranges::find(masks, entry.first) == masks.end())
                            masks.push_back(entry.first);
            const std::string current = settings().overlay;
            auto* group = new QActionGroup(m);
            auto row = [&](const QString& label, const std::string& mask) {
                QAction* a = m->addAction(label);
                a->setCheckable(true);
                a->setChecked(current == mask);
                group->addAction(a);
                QObject::connect(a, &QAction::triggered, a, [mask] { ops::execute("terrainDisplay", {{"overlay", mask}}); });
            };
            row("None", "");
            for (const std::string& mask : masks)
                row(QString::fromStdString(mask), mask);
        });
    });
}
} // namespace lila::ui::heightfieldDisplay
