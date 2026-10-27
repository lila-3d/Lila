#pragma once

#include "core/Export.h"

class QWidget;

/**
 * The quick command line: a one-line Python field just above the bottom edge of the active viewport (of the window
 * without one), following it as the layout changes. It runs its text the way the script editor runs a line, output
 * going to the script output and status line. Enter runs and closes, Esc closes. It opens on the last command,
 * selected; Up and Down walk the history, which the "commandLine" prefs keep.
 */
namespace lila::ui::quickCommand
{
/** Opens the line over `over` (the main window), or closes it when it is open. */
LILA_UI_API void toggle(QWidget* over);

/** Registers window.commandLine. */
LILA_UI_API void registerActions(QWidget* over);
} // namespace lila::ui::quickCommand
