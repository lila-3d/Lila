#pragma once

#include "core/Export.h"

#include <functional>

class QWidget;

namespace lila::ui
{
/**
 * What the Delete key removes. A panel claims Delete for its widgets, and the key goes to the claim found from the focus
 * widget outwards, then, when focus claims nothing and is not a list or text field, from the widget under the cursor.
 * With no claim Delete does nothing, so a key pressed in an unrelated panel never deletes from the scene. Delete run from
 * a menu, a button or Python always removes the scene selection.
 */
namespace deletion
{
enum class Target
{
    Nothing,
    Panel,
    Scene,
};

/**
 * Delete inside w and its children (unless one claims it for itself) runs remove, which deletes the panel's own selection
 * through its commands. The claim holds even with nothing selected, so Delete never falls through to the scene. An empty
 * remove claims Delete for nothing.
 */
LILA_UI_API void claim(QWidget* w, std::function<void()> remove);
/** Delete inside w removes the scene selection: the viewport, the outliner, panels that show the scene selection. */
LILA_UI_API void claimScene(QWidget* w);

struct Route
{
    Target target = Target::Nothing;
    QWidget* owner = nullptr; // the claiming widget, or the text field or list that blocked the key
};
/** Where Delete goes with keyboard focus in focus and the cursor over hovered; either may be null. */
LILA_UI_API Route route(QWidget* focus, QWidget* hovered);
/** Delete from the keyboard: routes it, running the claim's remove, or scene for a scene target. */
LILA_UI_API Route run(QWidget* focus, QWidget* hovered, const std::function<void()>& scene);
} // namespace deletion
} // namespace lila::ui
