#pragma once

#include "core/Export.h"

#include <glm/glm.hpp>

#include <QPointF>
#include <QString>

#include <functional>

class QKeyEvent;
class QMouseEvent;
class QPainter;

/**
 * A tool that borrows the UV editor's canvas (bake cage painting in UV space): while one is set it gets the editor's left
 * button and plain mouse moves and draws over the layout; navigation (Alt, middle and right buttons) stays the editor's.
 */
namespace lila::ui::uvTools
{
/** The canvas a tool works on: UV <-> widget pixels. */
struct Canvas
{
    std::function<QPointF(const glm::dvec2&)> toScreen;
    std::function<glm::dvec2(const QPointF&)> toUv;
    /** Widget pixels per UV unit. */
    double pixelsPerUv = 1.0;
};

class UvTool
{
public:
    virtual ~UvTool() = default;
    /** True takes the event. */
    virtual bool mousePress(QMouseEvent*, const Canvas&) { return false; }
    virtual bool mouseMove(QMouseEvent*, const Canvas&) { return false; }
    virtual bool mouseRelease(QMouseEvent*, const Canvas&) { return false; }
    virtual bool keyPress(QKeyEvent*, const Canvas&) { return false; }
    virtual void leave() {}
    virtual void draw(QPainter&, const Canvas&) {}
    /** A line for the editor's HUD, empty for none. */
    virtual QString hud() const { return {}; }
};

/** Not owned; the tool clears it (set(nullptr)) before it goes away. */
LILA_UI_API void set(UvTool* tool);
LILA_UI_API UvTool* active();
/** Repaints every UV editor. */
LILA_UI_API void requestRedraw();
/** UV editors register how they repaint. */
LILA_UI_API int addRedrawListener(std::function<void()> fn);
LILA_UI_API void removeRedrawListener(int id);
} // namespace lila::ui::uvTools
