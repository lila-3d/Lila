#pragma once

#include "core/Export.h"

class QObject;

namespace lila::ui
{
/**
 * Spin boxes, combo boxes and sliders across the app take the wheel only once focused (clicked or tabbed into), so
 * wheel scrolling a panel keeps scrolling it when the cursor passes over them. A wheel turned anywhere else takes the
 * focus back from such a field. Installed once on the application.
 */
LILA_UI_API void installWheelGuard(QObject* parent);
} // namespace lila::ui
