#pragma once

#include "core/Export.h"
#include "core/ExtensionContext.h"
#include "ui/ViewportState.h"

#include <QPointF>
#include <QString>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace lila::ui
{
class OverlayPainter;
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
    /**
     * An overlay icon id (ui/Overlay.h overlayIcons). An id that isn't registered is taken as a Tabler icon name and
     * registered as one.
     */
    std::string icon;
    std::function<bool(const ViewportState&)> active;
    /** Help line text while active, such as "Vertex snapping enabled". */
    QString message;
    /** Actions whose bound keys follow the message in brackets; the first bound key of each is shown. */
    std::vector<std::string> actions = {};
    /** Shown in the brackets as is, for modifiers that are not hotkeys ("Shift"). */
    QString key = {};
};

/** Adds a badge, or replaces the one with the same id in place. Badges show in the order they were added. */
LILA_UI_API void add(Badge badge, std::string owner = {});
/** For extensions: owned by ctx and removed when it unloads. 0, with a log message, when the id is taken. */
LILA_UI_API ext::Context::Handle add(ext::Context& ctx, Badge badge);
LILA_UI_API bool remove(std::string_view id);
/** Everything owner added. Returns the count. */
LILA_UI_API size_t removeOwned(std::string_view owner);
/** Badge ids in display order. */
LILA_UI_API std::vector<std::string> ids();

/** The badges active in view; empty while the pref is off. Valid until badges are next added or removed. */
LILA_UI_API std::vector<const Badge*> active(const ViewportState& view);

/** Draws their icons in a row just below and right of the cursor, outlined to read on any background. */
LILA_UI_API void draw(OverlayPainter& p, QPointF cursor, const std::vector<const Badge*>& badges);

/**
 * Called by a view after it drew, with the badges under the cursor in it, or with none when the cursor is elsewhere.
 * view only identifies the caller. The help line follows the view that has the cursor.
 */
LILA_UI_API void publish(const void* view, const std::vector<const Badge*>& badges);

/**
 * Where the help line text goes: shown(text) when badges come up or change, shown({}) when they all end, so the
 * receiver can put back what it showed before.
 */
LILA_UI_API void setStatusSink(std::function<void(const QString&)> shown);

/**
 * The built-in badges: X/C/V/N snaps, the snap surface, J rotate snap, D pivot edit, B soft select radius, Tab select or deselect, L lasso,
 * Shift precision, Ctrl plane, Ctrl+MMB surface slide.
 */
LILA_UI_API void registerDefaults();
} // namespace lila::ui::cursorBadges
