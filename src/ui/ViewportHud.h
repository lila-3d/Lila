#pragma once

#include "render/FrameState.h"
#include "scene/Graph.h"
#include "ui/Manipulator.h"
#include "ui/Overlay.h"

#include <QRectF>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class QMenu;

namespace lila::ui
{
class ViewportWidget;
}

/**
 * Maya's Heads Up Display adapted to Lila: per-viewport readouts stacked in the view's corners so they never overlap.
 * Each viewport panel keeps its own choices in the general prefs under /viewport/hud/panel<N>/.
 */
namespace lila::ui::hud
{
/** A line of an item's readout that can be hidden on its own, from the item's option box. */
struct Part
{
    const char* id; // "verts"; its action is "viewport.hud.<item>.<id>"
    const char* label;
};

struct Item
{
    const char* id; // "polyCount"; its action is "viewport.hud.<id>"
    const char* label;
    const char* doc;
    std::vector<Part> parts = {};
};

/** Every HUD element, in menu order. */
const std::vector<Item>& items();

/** The panel's own choice for an item, ignoring the show-all switch. */
bool itemOn(int panel, std::string_view item);
/** Turning an item on also turns the show-all switch on, so it never waits invisibly behind it. */
void setItemOn(int panel, std::string_view item, bool on);
/** The panel's Show/Hide all switch. */
bool allShown(int panel);
void setAllShown(int panel, bool on);
/** On and not hidden by the show-all switch: what actually draws. */
bool shown(int panel, std::string_view item);
/** Whether a part of an item draws while the item does; every part is on by default. */
bool partOn(int panel, std::string_view item, std::string_view part);
void setPartOn(int panel, std::string_view item, std::string_view part, bool on);

/** Check state of a "viewport.hud.*" action for a view, nullopt for any other id. */
std::optional<bool> actionState(const std::string& id, const ViewportWidget* view);

/** The HUD menu of a viewport: the show-all switch, then one checkable entry per item, then options. */
void fillMenu(QMenu* menu, ViewportWidget* view);

/**
 * Registers "viewport.hud.toggleAll", one "viewport.hud.<id>" per item and one "viewport.hud.<id>.<part>" per part, all
 * acting on active(). Items with parts get an option box listing them.
 */
void registerActions(std::function<ViewportWidget*()> active);

struct Context
{
    int panel = 0;
    ViewInfo view;
    /** The view's isolate; null shows everything. */
    std::function<bool(scene::NodeId)> shownNode;
    double framesPerSecond = 0.0;
    /** GPU time of the last drawn frame and the ambient occlusion part of it; 0 when unknown. */
    double gpuMs = 0.0;
    double aoMs = 0.0;
    /** Receives the rect of every readout drawn, so tool messages can avoid them. */
    std::vector<QRectF>* drawn = nullptr;
};

/**
 * The text readouts: poly count, smooth preview, selection and texel density top left; object details, symmetry and
 * soft select top right; current frame and frame rate bottom right. Returns true while a count is provisional, so the
 * caller redraws shortly for the exact one.
 */
bool draw(OverlayPainter& p, const Context& ctx);

/** World axes at the origin, a constant size on screen, letters at their tips. */
void drawOriginAxis(render::FrameState& s, OverlayPainter& p, const ViewInfo& view);
} // namespace lila::ui::hud
