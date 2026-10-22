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

/** While the key is held, hovering a menu name drops its menu down. The centre button opens on click only, since the
 * hotbox appears centred on it under the cursor. */
inline bool hoverOpens(bool keyHeld, int hovered, bool hoveredIsCenter, int open)
{
    return keyHeld && hovered >= 0 && !hoveredIsCenter && hovered != open;
}

/** Prefs name of the marking menu for a zone and mouse button ("lmb", "mmb", "rmb"): "hotbox_north_lmb". */
inline std::string markingMenuName(Zone z, const char* button)
{
    return std::string("hotbox_") + zoneName(z) + "_" + button;
}
} // namespace lila::ui::hotbox
