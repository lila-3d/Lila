#pragma once

#include "core/Export.h"

#include "scene/Components.h"
#include "ui/ViewInfo.h"

#include <QPointF>
#include <Qt>

#include <map>
#include <set>
#include <string>
#include <vector>

/**
 * Brush selection: Tab-drag in the viewport (select/move/rotate/scale tools) and the Paint Select tool (select.paintTool).
 * Both pick only components and objects facing the camera and not hidden behind a hilited mesh.
 *
 *   Paint Select tool: LMB drag paints into the selection, Ctrl removes, Shift toggles once per stroke; B + drag or
 *   [ ] sets the radius (general pref /paintSelect/radius, px). Works on objects in object mode. Esc leaves it.
 *   Tab drag: a stroke starting on something selected deselects what the path crosses, any other stroke selects it
 *   (ui/TabSelectLogic.h); Ctrl always removes, Ctrl+Shift always adds. Works on objects in object mode, and in the UV editor.
 */
namespace lila::geo
{
class Mesh;
}

namespace lila::ui::paintSelect
{
inline constexpr const char* kToolId = "paintSelect";

/** Whether a vertex, edge or face of the mesh lies on a face turned toward the camera (any of its faces, for points and edges). */
LILA_UI_API bool frontFacing(const geo::Mesh& mesh, const glm::dmat4& world, scene::ComponentMode kind, uint32_t id, const ViewInfo& view);
/** The general pref /select/backfaceCulling: clicks and marquees skip back-facing components. */
LILA_UI_API bool backfaceCulling();

/** Components of the current mode on the hilited meshes under a screen circle: front facing and unoccluded. */
LILA_UI_API std::map<scene::NodeId, scene::components::Ids> componentsUnder(const ViewInfo& view, QPointF at, double radiusPx);
/** Pickable meshes whose front-facing surface shows within the circle. */
LILA_UI_API std::vector<scene::NodeId> objectsUnder(const ViewInfo& view, QPointF at, double radiusPx);

/**
 * One painted selection change. While painting the selection previews live, kept off the undo stack; end() puts the
 * start back and runs the selection commands for what the stroke covered as one undo step.
 */
class LILA_UI_API Stroke
{
public:
    enum class Op
    {
        Replace,
        Add,
        Remove,
        Toggle,
    };
    /** Click conventions: Shift toggles, Ctrl removes, Ctrl+Shift adds; plain gives `plain`. */
    static Op opFor(Qt::KeyboardModifiers mods, Op plain);

    void begin(Op op);
    /** Begins a component stroke in `mode` whatever the scene's mode, for views that pick their own (the UV editor). */
    void begin(Op op, scene::ComponentMode mode);
    /** Brushes the segment from a to b (widget pixels). */
    void paint(const ViewInfo& view, QPointF a, QPointF b, double radiusPx);
    /** Adds components picked elsewhere to the stroke. */
    void touch(const std::map<scene::NodeId, scene::components::Ids>& under);
    void end();
    /** Puts the starting selection back without recording anything. */
    void cancel();
    bool active() const { return mActive; }
    Op op() const { return mOp; }

private:
    void preview();
    void restoreStart();

    bool mActive = false;
    Op mOp = Op::Add;
    scene::ComponentMode mMode = scene::ComponentMode::Object;
    std::map<scene::NodeId, scene::components::Ids> mStart[3];
    std::vector<scene::NodeId> mStartObjects;
    std::map<scene::NodeId, std::set<uint32_t>> mTouched;
    std::set<scene::NodeId> mTouchedObjects;
};

/** Brush radius for Tab drag, px, by component mode. */
LILA_UI_API double tabRadius(scene::ComponentMode mode);

/** The general pref /select/lasso: viewport drag selection draws a freehand lasso instead of a marquee. */
inline constexpr const char* kLassoPref = "/select/lasso";
/** Drags select with a lasso: the pref is on, or the lasso hold key (select.lassoHoldOn) is down. */
LILA_UI_API bool lassoActive();
LILA_UI_API bool lassoHeld();

/** The Paint Select tool and its action, select.paintTool, and the lasso actions (select.toggleLasso, select.lassoHoldOn/Off). */
LILA_UI_API void registerPaintSelectUi();
} // namespace lila::ui::paintSelect
