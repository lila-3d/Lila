#pragma once

#include "core/Export.h"

#include "scene/LodGroup.h"

#include <functional>

class QMenu;

namespace lila::ui
{
class ViewportWidget;
struct ViewInfo;
}

/**
 * Which level of LOD groups each viewport shows: by screen size as Unreal picks it, one fixed level, or all of them.
 * Each panel keeps its own choice in the general prefs under /viewport/lod/panel<N>/; panels that never chose follow the
 * defaults at kMode and kLevel. A group's lodDisplay attribute overrides the view for that group.
 */
namespace lila::ui::lodDisplay
{
inline constexpr const char* kMode = "/viewport/lod/mode";
inline constexpr const char* kLevel = "/viewport/lod/level";

LILA_UI_API scene::lod::Mode mode(int panel);
LILA_UI_API int level(int panel);
/** Sets the panel's own mode; level only applies to Mode::Level. */
LILA_UI_API void set(int panel, scene::lod::Mode mode, int level = 0);

/** The panel's choice with the view's camera, for scene::lod::hidden. */
LILA_UI_API scene::lod::View view(int panel, const ViewInfo& info);

/** viewport.lod.screenSize, viewport.lod.all and viewport.lod.level0..7 on the active view; edit.lodGroup and edit.generateLods. */
LILA_UI_API void registerActions(std::function<ViewportWidget*()> active);
/** The panel toolbar's menu for one view. */
LILA_UI_API void fillMenu(QMenu* menu, ViewportWidget* view);
} // namespace lila::ui::lodDisplay
