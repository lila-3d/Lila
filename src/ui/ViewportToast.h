#pragma once

#include "core/Export.h"

#include <QRectF>
#include <QString>

#include <functional>
#include <vector>

class QWidget;

namespace lila::ui
{
class OverlayPainter;
struct ViewInfo;

/**
 * Short confirmations that pop up in the viewport's bottom-right corner and fade out, newest at the bottom. Python and
 * MCP reach them through the viewportToast command.
 */
namespace viewportToast
{
enum class Kind
{
    Info,
    Error, // red accent: something failed
};

/** Shows text in view, or in every viewport when view is null. A toast with the same text restarts instead of stacking. */
LILA_UI_API void show(const QString& text, QWidget* view = nullptr, int durationMs = 2000, Kind kind = Kind::Info);

/** Shows text in the active viewport, or in every viewport when there is none. */
LILA_UI_API void showActive(const QString& text, int durationMs = 2000, Kind kind = Kind::Info);

/** Sets what showActive() and the viewportToast command target. */
LILA_UI_API void setActive(std::function<QWidget*()> active);

/** Registers the viewportToast command. In batch mode it does nothing, there being no viewport. */
LILA_UI_API void registerCommands();

/** Draws view's live toasts, stacked up from the bottom-right corner clear of the occupied rects. */
LILA_UI_API void draw(OverlayPainter& p, const ViewInfo& info, const QWidget* view, const std::vector<QRectF>& occupied);
} // namespace viewportToast
} // namespace lila::ui
