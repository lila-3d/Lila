#pragma once

#include "render/FrameState.h"
#include "ui/Manipulator.h"
#include "ui/Overlay.h"

#include <QCursor>
#include <QRectF>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace lila::ui
{
/**
 * An interactive viewport tool (weight paint, quad draw, sculpt, knife...). While one is active the viewport offers it
 * every mouse and key event first, except Alt navigation; returning true consumes the event. draw() adds to the
 * frame: 3D lines and meshes into `s`, 2D overlay into `p`.
 */
class ViewportTool
{
public:
    virtual ~ViewportTool() = default;
    virtual const char* name() const = 0;

    virtual void activate() {}
    virtual void deactivate() {}

    virtual bool mousePress(QMouseEvent* e, const ViewInfo& view) { (void)e, (void)view; return false; }
    virtual bool mouseMove(QMouseEvent* e, const ViewInfo& view) { (void)e, (void)view; return false; }
    virtual bool mouseRelease(QMouseEvent* e, const ViewInfo& view) { (void)e, (void)view; return false; }
    virtual bool wheel(QWheelEvent* e, const ViewInfo& view) { (void)e, (void)view; return false; }
    virtual bool keyPress(QKeyEvent* e, const ViewInfo& view) { (void)e, (void)view; return false; }

    virtual void draw(render::FrameState& s, OverlayPainter& p, const ViewInfo& view) { (void)s, (void)p, (void)view; }
    /** The viewport's cursor while this tool is active; nullopt is the arrow. */
    virtual std::optional<QCursor> cursor() const { return std::nullopt; }
};

/** A tool's status or hint line, centred along the top of the view. Queued and drawn by flushToolMessages once the HUD is placed. */
void drawToolMessage(OverlayPainter& p, const ViewInfo& view, const QString& text, const glm::vec4& color = {0.95f, 0.95f, 0.95f, 1.0f});
/** Draws the queued tool messages, moved down past any of the occupied HUD rects they would cover. */
void flushToolMessages(OverlayPainter& p, const ViewInfo& view, const std::vector<QRectF>& occupied);

namespace viewportTools
{
/** Replaces the active tool (deactivating the old one). nullptr goes back to select/move/rotate/scale. */
void set(std::unique_ptr<ViewportTool> tool);
ViewportTool* active();
/** Asks every viewport to redraw, e.g. after a brush stroke. */
void requestRedraw();

int addListener(std::function<void()> fn);
void removeListener(int id);
} // namespace viewportTools
} // namespace lila::ui
