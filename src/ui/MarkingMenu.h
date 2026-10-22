#pragma once

#include <nlohmann/json.hpp>

#include <QWidget>

#include <array>
#include <optional>

namespace lila::ui
{
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
class MarkingMenu : public QWidget
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
    static MarkingMenu* active();

    /** Fires the highlighted item (if any) and closes. False when nothing was highlighted. */
    bool release();
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
    bool mCopied = false;
    Trigger mTrigger;
    Qt::MouseButton mButton;
};
} // namespace lila::ui
