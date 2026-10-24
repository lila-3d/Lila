#pragma once

#include "core/Export.h"
#include "core/Registry.h"
#include "ui/Owner.h"

#include <QWidget>

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class QMenu;
class QToolButton;

/**
 * Icon bars built from registered items, like the viewport's icon row: buttons that run actions and show their state,
 * and dropdown menus. The tool box at the window's left edge and the outliner's row are icon bars; an extension adds
 * items to either (or to a bar of its own) and they go when it unloads.
 */
namespace lila::ui::iconBars
{
/** The vertical bar at the main window's left edge: selection tools and display aids. */
inline constexpr const char* kToolBox = "toolBox";
/** The row along the top of the outliner. */
inline constexpr const char* kOutliner = "outliner";

enum class Kind
{
    Action,    // a button running `action`
    Menu,      // a dropdown; with an `action` too, a click runs it and a long press or right-click opens the menu
    Separator,
};

struct Item
{
    std::string id;  // unique; add() makes one from the bar and the action or icon when empty
    std::string bar; // kToolBox, kOutliner, or any other bar's name
    Kind kind = Kind::Action;
    std::string action;
    std::string icon;  // a Tabler icon name
    std::string label; // the tooltip's title; the action's label when empty
    /** Kind::Menu without `fill`: its rows, as action ids; "" adds a separator and "#Title" a section. */
    std::vector<std::string> menuActions;
    /** Kind::Menu: fills the menu each time it opens, instead of menuActions. */
    std::function<void(QMenu*)> fill;
    /** Whether the button shows as on; null asks the bar (which knows the actions' states). */
    std::function<bool()> checked;
    /** An item id in the same bar to follow or precede; empty appends. */
    std::string after;
    std::string before;
};

LILA_UI_API Registry<Item>& registry();
/** Adds, or replaces the item with its id. Returns the id. */
LILA_UI_API std::string add(Item item, std::string owner = kLilaOwner);
/** Action items in order; "" adds a separator. Icons pair with the ids. */
LILA_UI_API void addActions(const std::string& bar, std::initializer_list<std::pair<const char*, const char*>> actionIcons,
                            std::string owner = kLilaOwner);
/** Owned by the extension and removed when it unloads. 0, with a log message, when the id is taken. */
LILA_UI_API uint64_t add(ext::Context& ctx, Item item);
LILA_UI_API bool remove(const std::string& id);
/** The items of a bar in display order: Lila's first, then after and before applied. */
LILA_UI_API std::vector<const Item*> itemsIn(const std::string& bar);

/** Whether an action shows as on, nullopt when it is not a toggle. */
using StateFn = std::function<std::optional<bool>(const std::string& action)>;

/** Adds menuActions-style rows to menu, checkable where state says the action is a toggle. */
LILA_UI_API void addActionRows(QMenu* menu, const std::vector<std::string>& rows, const StateFn& state);

/**
 * One bar's items as buttons along orientation, at iconSize. Rebuilds when the bar's items change and refreshes check
 * states when prefs, tools or the selection mode change. What does not fit goes into a chevron menu at the end.
 */
class LILA_UI_API IconBar : public QWidget
{
    Q_OBJECT

public:
    IconBar(std::string bar, Qt::Orientation orientation, int iconSize, StateFn state, QWidget* parent = nullptr);
    ~IconBar() override;

    const std::string& bar() const { return mBar; }
    void refresh();
    /**
     * Takes whatever length its layout gives and overflows into the chevron, asking for only one button's worth: for a
     * toolbar, which would otherwise hide the whole bar behind its own extension button.
     */
    void setFill(bool fill);
    /** Takes whatever length its layout gives and wraps into further columns (or rows) instead of overflowing. */
    void setWrap(bool wrap);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void resizeEvent(QResizeEvent* e) override;
    bool event(QEvent* e) override;

private:
    struct Button
    {
        QWidget* widget = nullptr;
        QToolButton* button = nullptr; // null for separators
        Item item;
    };

    void rebuild();
    void scheduleRebuild();
    void scheduleRefresh();
    void layoutItems();
    void layoutWrapped();
    /** How many columns (or rows) the items need wrapped at length. */
    int lines(int length) const;
    QSize sizeHintOneLine() const;
    std::optional<bool> stateOf(const Item& item) const;
    void fillMenu(QMenu* menu, const Item& item) const;
    void refreshTooltips();
    int along(QSize s) const;
    int across(QSize s) const;

    std::string mBar;
    Qt::Orientation mOrientation;
    int mIconSize;
    StateFn mState;
    std::vector<Button> mButtons;
    QToolButton* mChevron = nullptr;
    bool mFill = false;
    bool mWrap = false;
    int mLines = 1;
    bool mRebuildQueued = false;
    bool mRefreshQueued = false;
    int mRegistryListener = 0;
    int mPrefListener = 0;
    int mToolsListener = 0;
    int mViewportToolListener = 0;
    int mComponentListener = 0;
};
} // namespace lila::ui::iconBars
