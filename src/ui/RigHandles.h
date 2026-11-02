#pragma once

#include "core/Export.h"

#include "scene/Plug.h"
#include "ui/ViewInfo.h"

#include <QPointF>

#include <optional>
#include <vector>

/**
 * Viewport gizmos for rig elements: IK limit arcs and cones on joints (drag their ends to change the limits, through the
 * ikLimit command), and the spline chain, rubber hose and hose hand nodes, which have no transform of their own: their
 * curve and joints, the hose's bend and bow direction, and a clickable glyph that selects the node.
 */
namespace lila::ui::rigHandles
{
/** general pref: draw the IK limits of every joint that has them, not only the selected ones. */
inline constexpr const char* kShowAllLimits = "/rig/showIkLimits";

/** IK limit handles on a joint, for nodeHandles. */
enum LimitHandle
{
    kLimitMin,
    kLimitMax,
    kLimitCone,
};

/** Where a limit handle of the joint sits in the view; nothing when the joint has no such handle or it is off screen. */
LILA_UI_API std::optional<QPointF> limitHandleAt(scene::NodeId joint, LimitHandle h, const ViewInfo& view);

/** A spline chain, hose limb or hose hand's glyph: clicking it selects the node. */
struct Glyph
{
    scene::NodeId node = scene::kNoNode;
    QPointF at;
};
/** The glyphs drawn in the view, for the nodes shown. */
LILA_UI_API std::vector<Glyph> glyphs(const ViewInfo& view);

/** Registers the joint limit handles, the drawers and pickers, and the "rig.showIkLimits" action. */
void registerRigHandles();
} // namespace lila::ui::rigHandles
