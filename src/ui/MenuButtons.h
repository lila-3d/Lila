#pragma once

#include "core/Export.h"

class QAction;
class QToolButton;

namespace lila::ui
{
/**
 * Right-clicking a dropdown button runs `item` from its menu straight away instead of opening the menu, so the usual
 * pick is one click. The button's tooltip names it. Pass null to remove it.
 */
LILA_UI_API void setRightClickAction(QToolButton* button, QAction* item);

/** The item a right-click on button runs, or null. */
LILA_UI_API QAction* rightClickAction(const QToolButton* button);
} // namespace lila::ui
