#pragma once

#include <cmath>
#include <string>

namespace lila::ui::hotbox
{
enum class Zone
{
    Center,
    North,
    East,
    South,
    West
};

/**
 * Which hotbox zone an offset from the hotbox centre falls in (screen coordinates, y down). The centre zone is the
 * rectangle with half-extents (halfW, halfH) around the menu rows; the other four are split by the diagonals that
 * run out from its corners at 45 degrees, as in Maya.
 */
inline Zone zoneAt(double dx, double dy, double halfW, double halfH)
{
    double ax = std::abs(dx), ay = std::abs(dy);
    if (ax <= halfW && ay <= halfH)
        return Zone::Center;
    if (ay - halfH >= ax - halfW)
        return dy < 0 ? Zone::North : Zone::South;
    return dx < 0 ? Zone::West : Zone::East;
}

inline const char* zoneName(Zone z)
{
    switch (z)
    {
    case Zone::North: return "north";
    case Zone::East: return "east";
    case Zone::South: return "south";
    case Zone::West: return "west";
    default: return "center";
    }
}

enum class KeyUp
{
    Keep,             // a zone marking menu is up; it closes on its own mouse release
    Dismiss,          // close every menu and the hotbox
    TriggerAndDismiss // run the hovered menu item, then dismiss
};

/** What releasing the hotbox key does, as in Maya: it picks the item under the cursor in an open menu. */
inline KeyUp keyUpAction(bool tap, bool menuItemHovered, bool markingMenuOpen)
{
    if (tap)
        return KeyUp::Dismiss;
    if (menuItemHovered)
        return KeyUp::TriggerAndDismiss;
    return markingMenuOpen ? KeyUp::Keep : KeyUp::Dismiss;
}

/**
 * A press on a menu name while another hotbox menu is open: opens that one in its place, so switching menus takes one
 * click rather than one to close and one to open. Menus never open on hover, as in Maya, so an open menu never springs
 * up over what the cursor is travelling to.
 */
inline bool pressSwitchesMenu(int pressed, bool pressedIsCenter, int open)
{
    return pressed >= 0 && !pressedIsCenter && pressed != open;
}

/** Prefs name of the marking menu for a zone and mouse button ("lmb", "mmb", "rmb"): "hotbox_north_lmb". */
inline std::string markingMenuName(Zone z, const char* button)
{
    return std::string("hotbox_") + zoneName(z) + "_" + button;
}
} // namespace lila::ui::hotbox
