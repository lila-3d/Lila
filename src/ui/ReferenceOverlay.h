#pragma once

#include "core/Export.h"

#include <functional>
#include <string>

class QWidget;

namespace lila::ui
{
class ViewportWidget;
}

/**
 * Reference overlays: a reference image glued to a view's screen, in front of the model at low opacity (the sculptor's
 * tracing sheet) or parked opaque beside it. The overlay never takes brush input. In the viewport:
 *   O tap              show or hide the view's overlays
 *   O held             the overlays at full strength while held (blink, to compare)
 *   O + left drag      move the active overlay; right drag scales, middle drag rotates (Shift snaps), wheel sets opacity
 *   Shift+O            adjust mode: handles and a toolbar on the overlay; Esc or Shift+O leaves
 *   Alt+O              the next overlay of the view, alone
 * An overlay bound to a view remembers that camera: Go to View returns to it, and the overlay fades as the view turns
 * away, so a front and a profile photo each show only from their own side.
 */
namespace lila::ui::referenceOverlays
{
/** The drawer, the drag and wheel hooks, the referenceOverlay command and the referenceOverlay.* actions. Call once. */
LILA_UI_API void install(std::function<ViewportWidget*()> activeView);
/** The active viewport's view name, "persp" when there is none. */
LILA_UI_API std::string activeViewName();
/** Shows a set image as an overlay in view (empty: the active view); returns the overlay id, empty on failure. */
LILA_UI_API std::string show(const std::string& imageId, const std::string& view = {});
/** The overlay the hold-O keys and the adjust handles act on in view; empty when it has none. */
LILA_UI_API std::string activeOverlay(const std::string& view);
LILA_UI_API bool adjusting();
/** Clicking the overlay's outliner row: makes it its view's active one, and adjusts it there unless it is hidden. */
LILA_UI_API void activateRow(const std::string& id);
/** The Sculpt panel's Reference Overlay section: the active view's overlays and their main settings. */
LILA_UI_API QWidget* createSculptSection(QWidget* parent);
} // namespace lila::ui::referenceOverlays
