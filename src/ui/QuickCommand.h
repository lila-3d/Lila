#pragma once

#include "core/Export.h"

class QWidget;

/**
 * The quick command line: a one-line Python field over the bottom of the window that runs its text the way the script
 * editor runs a line, output going to the script output and status line. Enter runs and closes, Esc closes. It opens
 * on the last command, selected; Up and Down walk the history, which the "commandLine" prefs keep.
 */
namespace lila::ui::quickCommand
{
/** Opens the line over `over`, or closes it when it is open. */
LILA_UI_API void toggle(QWidget* over);

/** Registers window.commandLine. */
LILA_UI_API void registerActions(QWidget* over);
} // namespace lila::ui::quickCommand
