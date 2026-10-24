#pragma once

#include "core/Export.h"
#include "core/ExtensionContext.h"
#include "core/Registry.h"

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
#include <string>
#include <string_view>
#include <vector>

namespace lila::ui
{
/**
 * An interactive viewport tool (weight paint, retopo draw, sculpt, knife...). While one is active the viewport offers it
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
    /** B + LMB/MMB drag sets this tool's brush radius rather than soft select's. */
    virtual bool ownsBrushRadius() const { return false; }
};

/** A tool's status or hint line, centred along the top of the view. Queued and drawn by flushToolMessages once the HUD is placed. */
LILA_UI_API void drawToolMessage(OverlayPainter& p, const ViewInfo& view, const QString& text, const glm::vec4& color = {0.95f, 0.95f, 0.95f, 1.0f});
/** Draws the queued tool messages, moved down past any of the occupied HUD rects they would cover. */
LILA_UI_API void flushToolMessages(OverlayPainter& p, const ViewInfo& view, const std::vector<QRectF>& occupied);

namespace viewportTools
{
/**
 * A registered viewport tool: what activate(id) makes. Extensions register their tools this way rather than calling
 * set() with their own instance, so unloading the extension can end its tool first.
 */
struct ToolDef
{
    std::string id;
    QString label;
    /** A Tabler icon name for toolbars and menus; may be empty. */
    std::string icon;
    std::function<std::unique_ptr<ViewportTool>()> create;
};

LILA_UI_API Registry<ToolDef>& registry();
/** Throws std::invalid_argument when the id is taken. */
LILA_UI_API const ToolDef& add(ToolDef def);
/** For extensions: owned by ctx and removed when it unloads. Null, with a log message, if the id is taken. */
LILA_UI_API const ToolDef* add(ext::Context& ctx, ToolDef def);
/** Removing the definition of the active tool ends the tool first. */
LILA_UI_API bool remove(std::string_view id);

/** Makes the tool registered as id active. False when there is no such tool, or it made none. */
LILA_UI_API bool activate(std::string_view id);
/** The registered id of the active tool; empty when there is none, or it was set() directly. */
LILA_UI_API const std::string& activeId();

/** Replaces the active tool (deactivating the old one). nullptr goes back to select/move/rotate/scale. */
LILA_UI_API void set(std::unique_ptr<ViewportTool> tool);
LILA_UI_API ViewportTool* active();
/** Asks every viewport to redraw, e.g. after a brush stroke. */
LILA_UI_API void requestRedraw();

LILA_UI_API int addListener(std::function<void()> fn);
LILA_UI_API void removeListener(int id);
} // namespace viewportTools
} // namespace lila::ui
