#pragma once

#include "core/Export.h"

#include "scene/Plug.h"
#include "scene/TransformOps.h"
#include "ui/ExtrudeHandles.h"
#include "ui/Gizmo.h"
#include "ui/Overlay.h"
#include "ui/Tools.h"

#include <QPointF>
#include <QString>

#include <array>
#include <optional>
#include <string>
#include <vector>

class QKeyEvent;

namespace lila::ui
{
/**
 * The move/rotate/scale manipulator. Owned per viewport, driven by the global tool state. While dragging it edits the
 * scene with undo off, re-applying the whole delta from the start state each step (no drift); on release it restores
 * the start state and runs the equivalent command once, so undo and the script echo see a single clean edit.
 *
 * The default gizmo provider, gizmos::kDefault.
 */
class LILA_UI_API Manipulator : public Gizmo
{
public:
    enum class Handle
    {
        None,
        X,
        Y,
        Z,
        PlaneYZ,
        PlaneXZ,
        PlaneXY,
        Center,   // move in the view plane, or uniform scale
        ViewRing, // rotate about the view axis
        Free,     // trackball rotate
    };

    void setView(const ViewInfo& v) override { mView = v; }
    bool supports(std::string_view mode) const override;

    /** False when there is nothing to manipulate (select tool, empty selection). */
    bool visible() override;
    void draw(OverlayPainter& p) override;

    /** Updates the hovered handle; true when it changed (redraw). */
    bool hover(QPointF px) override;
    /**
     * True when the press grabbed a handle. Middle button drags the last used handle from anywhere; with Shift it waits
     * for the drag's direction and constrains to the axis that best matches it.
     */
    bool press(QPointF px, Qt::MouseButton button, Qt::KeyboardModifiers mods) override;
    void drag(QPointF px, Qt::KeyboardModifiers mods) override;
    void release() override;
    void cancel() override;
    bool dragging() const override { return mDrag.has_value() || mExtrude.dragging(); }
    bool hovering() const override { return mHover != Handle::None || mExtrudeHover != ExtrudeHandles::Handle::None; }
    /** The hovered handle only drives locked channels, so pressing it does nothing. */
    bool hoveringLocked() const override { return mHover != Handle::None && handleLocked(mHover); }
    /** Shift is slowing the current drag down. */
    bool precisionDrag() const override { return mDrag && mDrag->precise; }
    /** The current drag is Ctrl's plane of the two axes other than the one pressed. */
    bool ctrlPlaneDrag() const { return mDrag && mDrag->ctrlPlane; }
    /** Numeric entry while dragging. True when consumed. */
    bool keyPress(QKeyEvent* e) override;
    QString typed() const override { return mDrag ? mDrag->typed : QString(); }
    QString readout() const override;
    /** Mouse delta since the last drag() in pixels; the viewport uses it to wrap the cursor at edges. */
    void warped(QPointF offset) override;

private:
    struct Saved
    {
        scene::NodeId id = scene::kNoNode;
        scene::PlugRef plug; // component drags: the base mesh plug
        std::array<scene::PlugValue, 8> values;
    };
    struct Drag
    {
        Handle handle = Handle::None;
        QPointF pressPx;
        QPointF virtualPx; // accumulates scaled mouse motion for precision drags
        QPointF lastPx;
        glm::dvec3 startPivot{0.0};
        glm::dmat3 frame{1.0};
        bool pivotEdit = false;
        bool components = false; // dragging mesh components, not objects
        bool edgeOnPlane = false; // a plane drag whose plane the view looks along
        bool axisFromDrag = false; // Shift+MMB: no precision slow-down, axis picked from the drag direction
        bool axisPending = false;  // still waiting for enough motion to pick the axis
        bool linearRotate = false; // a ring turned by straight-line motion along linearDir
        bool precise = false;      // Shift held at the last update
        bool ctrlPlane = false;
        bool alongNormal = false; // each component point moves along its own normal by the N axis distance
        QPointF linearDir;
        double linearSign = 1.0;
        Tool tool = Tool::Move;
        std::vector<Saved> saved;
        std::vector<glm::dvec3> startPivots;
        std::vector<glm::dmat3> startPivotFrames;
        double startAngle = 0.0;
        glm::dvec3 startHit{0.0};
        // results of the last update
        glm::dvec3 moveDelta{0.0};
        glm::dquat rotation{1, 0, 0, 0};
        double angle = 0.0; // degrees, for the readout
        glm::dvec3 rotateAxis{0, 1, 0};
        glm::dvec3 scale{1.0};
        QString typed;
    };

    void refresh();
    /** Which frame axes the tool's channels are locked along on every target, as the move/rotate/scale commands see them. */
    void refreshLocks(Tool t);
    /** True when every channel the handle would change is locked: it draws greyed and does not drag. */
    bool handleLocked(Handle h) const;
    /** Takes the locked axes out of the drag's move delta and scale. */
    void maskLocked();
    Tool effectiveTool() const;
    double handleLength() const;
    glm::dvec3 axis(int i) const { return mFrame[i]; }
    /** Shift+MMB: once the drag is long enough, turns it into a drag on the axis it runs along. */
    void pickAxisFromDrag(QPointF px);
    Handle pick(QPointF px) const;
    void apply();
    void restore();
    void update(QPointF px, Qt::KeyboardModifiers mods);
    void commit();
    /** World-space transform the current drag applies to components. */
    glm::dmat4 componentXform() const;
    glm::dvec3 snapGrid(const glm::dvec3& p, const glm::dvec3& mask) const;
    /** Nearest mesh vertex/edge point under the cursor for V/C snapping, skipping whatever is being moved. */
    std::optional<glm::dvec3> snapTarget(QPointF px, unsigned snaps) const;

    void drawMove(OverlayPainter& p);
    void drawRotate(OverlayPainter& p);
    void drawScale(OverlayPainter& p);
    void drawReadout(OverlayPainter& p);
    glm::vec4 handleColor(Handle h, int axisIndex) const;
    std::vector<QPointF> ringPoints(int axisIndex, bool frontOnly) const;

    ViewInfo mView;
    std::vector<scene::NodeId> mTargets;
    scene::NodeId mLead = scene::kNoNode;
    bool mComponents = false;
    bool mNormal = false; // the move tool's Normal orientation on components: a single N handle, frame[2]
    glm::dvec3 mPivot{0.0};
    glm::dmat3 mFrame{1.0};
    glm::bvec3 mLockedAxes{false};
    bool mAllLocked = false; // every channel of the tool, for the view ring and trackball
    Handle mHover = Handle::None;
    Handle mLastHandle = Handle::None;
    std::optional<Drag> mDrag;
    ExtrudeHandles mExtrude;
    ExtrudeHandles::Handle mExtrudeHover = ExtrudeHandles::Handle::None;
};
} // namespace lila::ui
