#pragma once

#include "ui/terminal/VtScreen.h"

#include <string>

namespace lila::term
{
/** What a mouse wheel notch over the terminal should do, given the program's modes. */
enum class WheelAction : uint8_t
{
    ScrollHistory, // move the local view through the scrollback
    SendMouse,     // report wheel buttons 64/65 to the program, which asked for mouse tracking
    SendKeys,      // alternate screen without mouse tracking: cursor up/down (DECSET 1007)
    Ignore,
};

/** Shift always scrolls the local history, as in Windows Terminal; otherwise the program's modes decide. */
WheelAction wheelAction(const VtScreen& screen, bool shift);

/** Cursor up or down keys for one wheel notch, honouring application cursor keys. */
std::string wheelKeys(const VtScreen& screen, bool up);

enum class MouseButton : uint8_t
{
    Left,
    Middle,
    Right,
    None, // motion with no button held
    WheelUp,
    WheelDown,
};

enum class MouseEventType : uint8_t
{
    Press,
    Release,
    Move,
};

struct MouseModifiers
{
    bool shift = false;
    bool alt = false;
    bool ctrl = false;
};

/**
 * The report for one mouse event at a 0-based cell under the screen's tracking mode and encoding. Empty when the mode
 * doesn't report this event (motion in 1000, releases in X10, a coordinate the legacy encoding can't carry).
 */
std::string encodeMouse(const VtScreen& screen, MouseEventType type, MouseButton button, int col, int row, MouseModifiers mods = {});
} // namespace lila::term
