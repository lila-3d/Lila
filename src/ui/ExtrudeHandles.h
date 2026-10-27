#pragma once

#include "core/Export.h"

#include <glm/glm.hpp>

#include <QPointF>

namespace lila::ui
{
class OverlayPainter;
struct ViewInfo;

/**
 * Handles over a just-run polyExtrude, like Maya's extrude manipulator: an arrow along the extruded faces' normal sets
 * thickness, a square beside it sets offset (the cap's inset). Dragging re-runs the op live through scene::lastOp, so
 * the result stays one undo step and the adjust-last-op popup follows. The cap's move/rotate/scale is the regular
 * manipulator's. With an extrudeModifier selected instead, the handles drag its thickness and offset plugs, set as one
 * setAttr on release.
 */
class LILA_UI_API ExtrudeHandles
{
public:
    enum class Handle
    {
        None,
        Thickness,
        Offset,
    };

    /** True while the last op is a live polyExtrude and its faces are selected, or an extrudeModifier is selected. */
    bool active() const;
    void draw(OverlayPainter& p, const ViewInfo& view, Handle hover) const;
    Handle pick(QPointF px, const ViewInfo& view) const;

    void begin(Handle h, QPointF px, const ViewInfo& view);
    void drag(QPointF px, const ViewInfo& view);
    /** The viewport wrapped the cursor by offset; later drag() positions are taken back by it. */
    void warped(QPointF offset) { mDrag.warp += offset; }
    void end();
    void cancel();
    bool dragging() const { return mDrag.handle != Handle::None; }

private:
    struct Frame
    {
        glm::dvec3 center{0.0}; // world centre of the extruded faces
        glm::dvec3 normal{0, 1, 0};
        glm::dvec3 side{1, 0, 0}; // in the cap's plane, where the offset handle sits
        double worldPerLocal = 1.0; // world length of one cm of thickness
    };
    bool frame(Frame& f) const;
    bool modifierFrame(uint64_t modifier, Frame& f) const;
    /** Screen points of the handles: thickness arrow base and tip, offset square. */
    bool layout(const Frame& f, const ViewInfo& view, QPointF& base, QPointF& tip, QPointF& offset) const;

    struct Drag
    {
        Handle handle = Handle::None;
        Frame frame;
        double startThickness = 0.0;
        double startOffset = 0.0;
        double startParam = 0.0;
        double value = 0.0;
        QPointF lastPx;
        QPointF warp; // total cursor wrap offset during this drag
        /** The extrudeModifier being dragged, 0 for the last op. */
        uint64_t modifier = 0;
    } mDrag;
};
} // namespace lila::ui
