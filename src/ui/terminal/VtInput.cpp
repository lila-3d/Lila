#include "ui/terminal/VtInput.h"

#include <algorithm>

namespace lila::term
{
namespace
{
void appendUtf8Coord(std::string& out, int v)
{
    if (v < 0x80)
        out += char(v);
    else
    {
        out += char(0xC0 | (v >> 6));
        out += char(0x80 | (v & 0x3F));
    }
}
} // namespace

WheelAction wheelAction(const VtScreen& screen, bool shift)
{
    if (shift)
        return screen.altScreen() ? WheelAction::Ignore : WheelAction::ScrollHistory;
    if (screen.mouseTracking() != MouseTracking::Off)
        return WheelAction::SendMouse;
    if (screen.altScreen())
        return screen.alternateScroll() ? WheelAction::SendKeys : WheelAction::Ignore;
    return WheelAction::ScrollHistory;
}

std::string wheelKeys(const VtScreen& screen, bool up)
{
    return std::string(screen.appCursorKeys() ? "\x1bO" : "\x1b[") + (up ? 'A' : 'B');
}

std::string encodeMouse(const VtScreen& screen, MouseEventType type, MouseButton button, int col, int row, MouseModifiers mods)
{
    MouseTracking tracking = screen.mouseTracking();
    bool wheel = button == MouseButton::WheelUp || button == MouseButton::WheelDown;
    switch (tracking)
    {
    case MouseTracking::Off:
        return {};
    case MouseTracking::X10:
        if (type != MouseEventType::Press || wheel || button == MouseButton::None)
            return {};
        mods = {};
        break;
    case MouseTracking::Normal:
        if (type == MouseEventType::Move)
            return {};
        break;
    case MouseTracking::Button:
        if (type == MouseEventType::Move && button == MouseButton::None)
            return {};
        break;
    case MouseTracking::Any:
        break;
    }
    if (wheel && type == MouseEventType::Release)
        return {};

    int code = wheel ? 64 + (button == MouseButton::WheelDown ? 1 : 0) : button == MouseButton::None ? 3 : int(button);
    if (type == MouseEventType::Move)
        code += 32;
    code += (mods.shift ? 4 : 0) + (mods.alt ? 8 : 0) + (mods.ctrl ? 16 : 0);
    int x = std::max(0, col) + 1, y = std::max(0, row) + 1;

    MouseEncoding enc = screen.mouseEncoding();
    if (enc == MouseEncoding::Sgr)
        return "\x1b[<" + std::to_string(code) + ";" + std::to_string(x) + ";" + std::to_string(y) +
               (type == MouseEventType::Release ? "m" : "M");

    // The older encodings can't say which button went up.
    if (type == MouseEventType::Release)
        code = (code & ~3) | 3;
    if (enc == MouseEncoding::Urxvt)
        return "\x1b[" + std::to_string(code + 32) + ";" + std::to_string(x) + ";" + std::to_string(y) + "M";

    int limit = enc == MouseEncoding::Utf8 ? 2015 : 223;
    if (x > limit || y > limit)
        return {};
    std::string out = "\x1b[M";
    out += char(code + 32);
    if (enc == MouseEncoding::Utf8)
    {
        appendUtf8Coord(out, x + 32);
        appendUtf8Coord(out, y + 32);
    }
    else
    {
        out += char(x + 32);
        out += char(y + 32);
    }
    return out;
}
} // namespace lila::term
