#pragma once

#include "core/Export.h"

#include "scene/Node.h"

#include <functional>
#include <span>
#include <vector>

class QObject;

namespace lila::scene
{
class Graph;
}

/**
 * The bake cage in the viewport, independent of any tool: per group, a translucent grey shell with its wire where the
 * cage is (the editable cage mesh, or the auto cage with its painted offsets), and with live preview on, the lows drawn
 * with a low resolution preview bake (normal map, missed rays glowing red), rebaked as the cage changes.
 *
 * Actions: bake.showCage (toggles the selected groups' cage), bake.livePreview, bake.estimateCage.
 */
namespace lila::ui::bakeCage
{
/** The bake group whose low (or cage) the mesh is, or null. */
LILA_UI_API scene::Node* groupOfMesh(scene::Graph& g, scene::NodeId mesh);
/** Groups of the selection: selected groups and the groups of selected lows and cages. */
LILA_UI_API std::vector<scene::NodeId> selectedGroups(scene::Graph& g);

LILA_UI_API bool shown(scene::NodeId group);
LILA_UI_API void setShown(scene::NodeId group, bool on);
/** Live preview rebakes of the shown and painted groups. Session setting, off by default. */
LILA_UI_API bool livePreview();
LILA_UI_API void setLivePreview(bool on);
/** Calls fn after setShown or setLivePreview, while owner lives. */
LILA_UI_API void onChanged(QObject* owner, std::function<void()> fn);

/**
 * A stroke in progress on a low: the shell and preview use these bakeOffset values (one per point slot) instead of the
 * scene's. rebake asks for a preview now when the last one is done (strokes throttle themselves through this).
 */
LILA_UI_API void setWorkingOffsets(scene::NodeId low, std::span<const float> values, bool rebake);
LILA_UI_API void clearWorkingOffsets(scene::NodeId low);
/** Also show this group's shell and preview while a paint tool works on it, without changing setShown. */
LILA_UI_API void setPainting(scene::NodeId group);

/** Viewport drawer, material hook and the actions. */
LILA_UI_API void registerBakeCageView();
} // namespace lila::ui::bakeCage
