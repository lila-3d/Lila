#pragma once

#include "core/Export.h"

class QWidget;

/**
 * The command palette: a one-line field just above the bottom edge of the active viewport (of the window without one),
 * following it as the layout changes. Typing lists the actions and commands that match, best nearest the field; Enter
 * runs the highlighted one. Text that reads as Python (calls, assignments, literals) runs the way the script editor
 * runs a line instead, as does Shift+Enter. Esc closes. It opens on the last line, selected; Up and Down walk the
 * history, which the "commandPalette" prefs keep.
 */
namespace lila::ui::commandPalette
{
/** Opens the palette over `over` (the main window), or closes it when it is open. */
LILA_UI_API void toggle(QWidget* over);

/** Registers window.commandPalette. */
LILA_UI_API void registerActions(QWidget* over);
} // namespace lila::ui::commandPalette
