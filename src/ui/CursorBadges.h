#pragma once

#include <QPointF>
#include <QString>

#include <functional>
#include <string>
#include <vector>

namespace lila::ui
{
class OverlayPainter;
class ViewportWidget;
}

/**
 * Small icons beside the viewport cursor for states that last only while a key is held (snap keys, pivot edit, soft
 * select radius, precision drags), with a line in the main window's help line saying what the state is and which key
 * holds it. They are overlay drawing only, so they never take input. The "general" pref /viewport/cursorBadges turns
 * them all off.
 */
namespace lila::ui::cursorBadges
{
inline constexpr const char* kEnabledPref = "/viewport/cursorBadges";

struct Badge
{
    std::string id;
    /** A Tabler icon name; it must be one the overlay atlas bakes (kOverlayIcons in Overlay.cpp). */
    const char* icon;
    std::function<bool(const ViewportWidget&)> active;
    /** Help line text while active, such as "Vertex snapping enabled". */
    QString message;
    /** Actions whose bound keys follow the message in brackets; the first bound key of each is shown. */
    std::vector<std::string> actions = {};
    /** Shown in the brackets as is, for modifiers that are not hotkeys ("Shift"). */
    QString key = {};
};

/** Adds a badge, or replaces the one with the same id. Badges show in the order they were added. */
void add(Badge badge);

/** The badges active in view; empty while the pref is off. */
std::vector<const Badge*> active(const ViewportWidget& view);

/** Draws their icons in a row just below and right of the cursor, outlined to read on any background. */
void draw(OverlayPainter& p, QPointF cursor, const std::vector<const Badge*>& badges);

/**
 * Called by a view after it drew, with the badges under the cursor in it, or with none when the cursor is elsewhere.
 * The help line follows the view that has the cursor.
 */
void publish(const ViewportWidget* view, const std::vector<const Badge*>& badges);

/**
 * Where the help line text goes: shown(text) when badges come up or change, shown({}) when they all end, so the
 * receiver can put back what it showed before.
 */
void setStatusSink(std::function<void(const QString&)> shown);

/** The built-in badges: X/C/V snaps, J rotate snap, D pivot edit, B soft select radius, Shift precision, Ctrl plane. */
void registerDefaults();
} // namespace lila::ui::cursorBadges
