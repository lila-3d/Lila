#pragma once

#include "core/Export.h"
#include "scene/SplitMesh.h"

#include <QPointF>

#include <vector>

namespace lila::ui
{
/**
 * Anim proxy UI: proxy pieces select their joint when clicked, the Split Mesh and Anim Proxy tools (place split planes,
 * preview the pieces, confirm), and the rig.* anim proxy actions for the Rig > Anim Proxy menu.
 */
LILA_UI_API void registerAnimProxyUi();

struct ViewInfo;
/** The active Split Mesh or Anim Proxy tool's planes, world space; empty when neither is active. */
LILA_UI_API std::vector<scene::split::Plane> splitToolPlanes();
/** The plane the active split tool would take at px (by its rim or centre), -1 for none. */
LILA_UI_API int splitToolPlaneAt(QPointF px, const ViewInfo& view);
} // namespace lila::ui
