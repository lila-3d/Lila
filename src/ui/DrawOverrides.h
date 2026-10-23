#pragma once

#include "core/Export.h"
#include "core/ExtensionContext.h"
#include "core/Registry.h"

#include "render/FrameState.h"
#include "scene/Node.h"
#include "ui/Overlay.h"
#include "ui/ViewInfo.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>

/**
 * Per node type drawing and picking (Maya's MPxDrawOverride): a node type, typically an extension's, says how its
 * nodes look in the viewport and how clicks find them. The viewport does the rest: it skips hidden nodes and the
 * camera it looks through, works out each node's highlight, draws highlighted nodes after the others (so a selected
 * node wins at equal depth), applies display layers and the selection mask, and selects the node a pick returns.
 * Lila's locators are drawn by the override "lila.locator".
 *
 * An override covers its node type and every type derived from it, unless a more derived type has its own. A mesh
 * type with an override is drawn and picked by it instead of as a mesh.
 */
namespace lila::ui::drawOverrides
{
enum class Highlight : uint8_t
{
    None,
    UnderSelected, // a descendant of a selected node
    Preselected,   // under the cursor
    Selected,
};

struct DrawContext
{
    render::FrameState& frame; // 3D: lines, linesOnTop, meshes, tris3d (linear colors)
    OverlayPainter& painter;   // 2D, widget pixels (sRGB colors)
    const ViewInfo& view;
    const scene::Node& node;
    glm::dmat4 world{1.0};
    Highlight highlight = Highlight::None;
    /** The line color Lila uses for this highlight (and the node's display layer), linear, for frame.lines. */
    glm::vec4 color{0.02f, 0.02f, 0.02f, 1.0f};
};

struct PickContext
{
    const ViewInfo& view;
    const scene::Node& node;
    glm::dmat4 world{1.0};
    QPointF a, b; // the click point twice, or a marquee's corners
    bool click = true;
};

struct DrawOverride
{
    std::string id;
    /** The node type it draws, and the types derived from it. */
    std::string nodeType;
    std::function<void(DrawContext&)> draw;
    /**
     * Click: distance in pixels from the cursor to the drawing, nullopt for a miss (the viewport takes the nearest hit
     * within 6 px). Marquee: any value when the node is inside the box, nullopt when not. Null pick: never picked.
     */
    std::function<std::optional<double>(const PickContext&)> pick;
};

/** Holds "lila.locator" from first use. */
LILA_UI_API Registry<DrawOverride>& registry();

/** Throws std::invalid_argument when the id is taken. */
LILA_UI_API const DrawOverride& add(DrawOverride o);
/** For extensions: owned by ctx and removed when it unloads. Null, with a log message, if the id is taken. */
LILA_UI_API const DrawOverride* add(ext::Context& ctx, DrawOverride o);
LILA_UI_API bool remove(std::string_view id);

/** The override for nodes of type t: the one registered for t or its nearest base type. Null when none. */
LILA_UI_API const DrawOverride* forType(const scene::NodeType& t);
} // namespace lila::ui::drawOverrides
