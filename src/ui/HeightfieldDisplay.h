#pragma once

#include "core/Export.h"

#include "render/FrameState.h"
#include "scene/Node.h"

#include <memory>
#include <optional>
#include <string>

namespace lila::geo
{
struct Heightfield;
}
namespace lila::scene
{
class Graph;
}

/**
 * How the viewport shows terrains (meshes whose geometry-nodes modifier makes a heightfield): drawn from their heights
 * on the heightfield draw path, with their material, paint layers and the terrain display options (mask overlay,
 * contour lines, flow arrows), or as their mesh.
 */
namespace lila::ui::heightfieldDisplay
{
/** The user attr on a terrain's mesh node listing its paint layers as JSON (see terrainDisplayLayers). */
inline constexpr const char* kLayersAttr = "terrainLayers";

/** The terrain display options, from the general prefs. */
struct Settings
{
    bool heightfieldDraw = false; // /viewport/heightfieldDraw
    std::string overlay;          // a mask's name, empty for none
    int overlayRamp = 1;          // 0 grey, 1 turbo
    float overlayOpacity = 0.75f;
    float contourInterval = 0.0f; // cm, 0 off
    int contourMajor = 5;
    bool flowArrows = false;
    float arrowSpacing = 48.0f; // device pixels
    float quadPixels = 4.0f;    // level of detail: the finest quads' size in device pixels

    bool operator==(const Settings&) const = default;
    /** Any option only the heightfield draw path can show. */
    bool overlays() const { return !overlay.empty() || contourInterval > 0.0f || flowArrows; }
    /** Terrains draw from their heights: the pref, or an overlay that needs it. */
    bool drawsHeightfields() const { return heightfieldDraw || overlays(); }
};
LILA_UI_API Settings settings();

/** The heightfield the node's last geometry-nodes modifier made, or null. */
LILA_UI_API std::shared_ptr<const geo::Heightfield> heightfieldOf(scene::Graph& g, scene::NodeId id);

/** The node's paint layers as stored (JSON), empty when it has none. */
LILA_UI_API std::string layersSource(const scene::Node& n);

/**
 * The node's heightfield draw, flags left as shaded, or none when it has no heightfield. textured gives it its
 * material and paint layers; pendingMaps (when given) counts layer maps still loading.
 */
LILA_UI_API std::optional<render::HeightfieldDraw> drawFor(scene::Graph& g, scene::NodeId id, const Settings& set, bool textured,
                                                           int* pendingMaps = nullptr);

/** terrainDisplay and terrainDisplayLayers. */
LILA_UI_API void registerCommands();
/** The display options as actions (viewport.terrain.*), and as rows of a menu (with an Overlay submenu of the masks). */
LILA_UI_API void registerActions();
LILA_UI_API void addMenuItems(const std::string& menu);
} // namespace lila::ui::heightfieldDisplay
