#pragma once

#include "core/Export.h"
#include "core/Registry.h"
#include "ui/Owner.h"

#include <nlohmann/json.hpp>

#include <QElapsedTimer>
#include <QWidget>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lila::ui
{
/**
 * Marking menus contributed in code or from an extension's resources/markingMenus. They are defaults under the user's
 * prefs, so the editor can still change them. A name "<set>/<name>" puts a menu in a set: while a mode with that set
 * is active, opening "<name>" shows the set's version when it has one.
 */
namespace markingMenus
{
LILA_UI_API Registry<nlohmann::json>& registry();
/** def has the shape described on MarkingMenu. */
LILA_UI_API void add(const std::string& name, nlohmann::json def, std::string owner = kLilaOwner);
LILA_UI_API uint64_t add(ext::Context& ctx, const std::string& name, nlohmann::json def);
LILA_UI_API bool remove(const std::string& name);

/** Every menu of a set at once: {"<name>": def, ...}. */
LILA_UI_API void addSet(const std::string& set, const nlohmann::json& menus, std::string owner = kLilaOwner);
LILA_UI_API uint64_t addSet(ext::Context& ctx, const std::string& set, const nlohmann::json& menus);
/** Removes every contributed menu in the set. Returns the count. */
LILA_UI_API size_t removeSet(const std::string& set);

/** One item added to an existing menu: into a compass slot ("N" ... "NW", replacing what was there) or "overflow". */
struct ItemAdd
{
    std::string menu;
    std::string slot;
    nlohmann::json item; // {"label", "action", ...} as in a definition
};
LILA_UI_API Registry<ItemAdd>& itemRegistry();
/** Returns the item's id. */
LILA_UI_API std::string addItem(ItemAdd add, std::string owner = kLilaOwner);
LILA_UI_API uint64_t addItem(ext::Context& ctx, ItemAdd add);
LILA_UI_API bool removeItem(const std::string& id);

/** The set the current mode uses, tried before plain names; empty for none. */
LILA_UI_API void setActiveSet(std::string set);
LILA_UI_API const std::string& activeSet();

/** What opening name shows: the active set's version or the plain one, with added items applied. Empty when neither exists. */
LILA_UI_API nlohmann::json resolve(const std::string& name);
/** Names of the contributed menus, sets included ("set/name"). */
LILA_UI_API std::vector<std::string> contributedNames();

/**
 * Which viewport menu a press opens: base + "Object" over an object, base + "Empty" over nothing. In a component mode
 * it is always the "Object" menu, since that is the component menu and the empty one is for object mode.
 */
LILA_UI_API std::string viewportMenuName(const std::string& base, bool overObject, bool componentMode);
} // namespace markingMenus

/**
 * Maya-style radial menu. Eight compass slots plus an overflow list under the south slot; items may nest. Pick by
 * direction, so a quick flick-and-release works without looking. Lives while a key or mouse button is held and
 * fires on release.
 *
 * Definition (prefs file "markingMenus/<name>"):
 *   { "N": {"label": "...", "action": <target>}, "NE": {..., "children": {<same shape>}}, "overflow": [ {...} ] }
 * An overflow item {"insert": "cameraNodes"} becomes one Look Through item per camera node in the scene, and
 * {"insert": "materials"} one Assign item per material (its option box shows the material in the Material Editor).
 * An overflow item {"separator": true} is a divider line between groups of rows.
 * An overflow item with "children" cascades: its list opens to the right when the cursor reaches the row's arrow.
 * "optionAction" gives an item an option box that runs that target instead of the action's options dialog. An item
 * whose action is disabled (Action::enabled), or that has "enabled": false, is greyed out and can't be picked.
 */
class LILA_UI_API MarkingMenu : public QWidget
{
    Q_OBJECT

public:
    enum class Trigger
    {
        Key,
        MouseButton
    };

    /** Shows the named menu at the cursor. Returns nullptr when the menu doesn't exist. */
    static MarkingMenu* popup(const std::string& name, Trigger trigger, Qt::MouseButton button = Qt::NoButton,
                              std::optional<QPoint> at = std::nullopt);
    /** Shows a menu built on the spot (same shape as a definition). Returns nullptr for an empty one. */
    static MarkingMenu* popupDefinition(const nlohmann::json& def, Trigger trigger, Qt::MouseButton button = Qt::NoButton,
                                        std::optional<QPoint> at = std::nullopt);
    static MarkingMenu* active();

    /** Fires the highlighted item (if any) and closes. False when nothing was highlighted. */
    bool release();
    /** Closes without running anything. */
    void dismiss();
    /** The key holding the menu came up: commits the item under the cursor, or its options on its option box, as a
     * mouse release does. Keeps the menu open and returns false when the cursor is on no item. */
    bool keyReleased();
    Qt::MouseButton button() const { return mButton; }

protected:
    void paintEvent(QPaintEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;

private:
    struct Item
    {
        QString label;
        nlohmann::json action;
        nlohmann::json children; // nested menu definition, or null
        QRectF rect;
        bool enabled = true; // drawn greyed and never highlighted when false
        bool separator = false; // an overflow divider line: no label, never highlighted
        QRectF box; // the option box square at the right end of rect, empty when the action has none
        nlohmann::json boxAction; // what the option box runs; null runs the action's options dialog
    };
    struct Level
    {
        QPointF center;
        std::array<std::optional<Item>, 8> compass;
        std::vector<Item> overflow;
        std::optional<QPointF> cascade; // top left of a cascaded overflow list, which then has no compass
    };

    MarkingMenu(const nlohmann::json& def, Trigger trigger, Qt::MouseButton button, QPoint at);
    Level buildLevel(const nlohmann::json& def, QPointF center, std::optional<QPointF> cascade = std::nullopt) const;
    void layout(Level& level) const;
    void updateHighlight(QPointF pos);
    Item* highlighted();

    std::vector<Level> mLevels;
    QPointF mCursor;
    int mSlot = -1;     // highlighted compass slot on the top level
    int mOverflow = -1; // highlighted overflow row
    bool mOnBox = false; // the cursor is on the highlighted item's option box
    QElapsedTimer mOnBoxSince; // a compass box only counts once the cursor has rested on it, so a flick never opens it
    bool mCopied = false;
    Trigger mTrigger;
    Qt::MouseButton mButton;
};
} // namespace lila::ui
