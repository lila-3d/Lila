#pragma once

#include "core/Export.h"

#include <QApplication>
#include <QPointer>
#include <QStringList>
#include <QWidget>

#include <map>
#include <set>
#include <string>
#include <vector>

class QDockWidget;
class QHBoxLayout;
class QMainWindow;
class QTabBar;
class QToolBar;

namespace lila::ui
{
/**
 * Lays the bottom dock area out Maya's way: `tabs` in one tab group in that order with `current` on top, and `row`
 * as its own full-width row under the group. Docks already in the window are moved; hidden ones stay hidden.
 */
LILA_UI_API void arrangeBottomDocks(QMainWindow* window, const QList<QDockWidget*>& tabs, QDockWidget* current, QDockWidget* row);

/**
 * Opens a panel the way a menu or hotkey should: expanded if it was collapsed, its tab raised, a usable size and
 * keyboard focus. Works for docks of any main window, managed by DockSidebars or not.
 */
LILA_UI_API void revealDock(QDockWidget* dock);

/** Dynamic property on a dock: its width (or height) as a tab when it has no size of its own remembered yet. */
inline constexpr const char* kDefaultTabExtent = "lilaDefaultTabExtent";
/** Dynamic property on a dock: shown, it takes the narrowest width its column allows, whatever it was before. */
inline constexpr const char* kHugTabExtent = "lilaHugTabExtent";

/** Gives the dock's title bar a help button opening a docs page ("topology", "curves?id=pipe"); empty removes it. */
LILA_UI_API void setDockDocs(QDockWidget* dock, const std::string& page);

/** Shown, not collapsed and not behind another tab of its group. */
LILA_UI_API bool dockShowing(const QDockWidget* dock);

/** Window toggles run from the command palette only open or raise: naming a window there means "show me it". */
LILA_UI_API bool togglesRevealOnly();

/** A window toggle for top-level windows: closes the shown window of type T, if there is one, and says whether it did. */
template <class T> bool closeShownWindow()
{
    if (togglesRevealOnly())
        return false;
    for (QWidget* w : QApplication::topLevelWidgets())
        if (auto* t = qobject_cast<T*>(w); t && t->isVisible() && !t->isMinimized())
        {
            t->close();
            return true;
        }
    return false;
}

/** A window toggle: tucks the panel into its sidebar (or hides it) when it is showing, otherwise reveals it. */
LILA_UI_API void toggleDock(QDockWidget* dock);

/**
 * Dock title bar: a semibold title, then a docs button when it has a page, collapse, float and close, which stay dim
 * until the pointer is over the bar. Double-click floats or re-docks.
 */
class LILA_UI_API DockTitleBar : public QWidget
{
    Q_OBJECT

public:
    explicit DockTitleBar(QDockWidget* dock);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    void setDocsPage(const std::string& page);

Q_SIGNALS:
    void collapseRequested();

protected:
    void paintEvent(QPaintEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void enterEvent(QEnterEvent* e) override;
    void leaveEvent(QEvent* e) override;

private:
    void lightButtons(bool lit);
    /** Where the title text has to stop: the left edge of the first shown button. */
    int buttonsLeft() const;

    QDockWidget* mDock;
    QHBoxLayout* mButtons = nullptr;
    QWidget* mHelp = nullptr;
};

/**
 * Collapsing a docked panel hides it and leaves a tab on a thin strip along its window edge; clicking the tab brings it
 * back where it was. The tab stays, checked, while the panel is open, so the same click collapses it again; it goes when
 * the panel is moved, floated or closed. The bottom strip sits at the left of the status bar rather than in a band of
 * its own. Collapsed panels persist through the layout prefs.
 */
class LILA_UI_API DockSidebars : public QObject
{
    Q_OBJECT

public:
    explicit DockSidebars(QMainWindow* window);

    /** Gives the dock the collapsible title bar. */
    void manage(QDockWidget* dock);
    void collapse(QDockWidget* dock);
    void expand(QDockWidget* dock);
    /** See revealDock. */
    void reveal(QDockWidget* dock);
    bool isCollapsed(const QDockWidget* dock) const;

    QStringList collapsedNames() const;
    void restoreCollapsed(const QStringList& names);
    void expandAll();
    /** Hides the dock, dropping its sidebar tab if it is collapsed. */
    void close(QDockWidget* dock);
    /** Re-docks any shown dock that a restored state left laid out off the window. */
    void rescueStranded();

    /**
     * Each tabified dock's own width (left and right areas) or height (top and bottom), by object name. Clicking a
     * dock's tab brings back the size it had when it was last the current tab.
     */
    std::map<QString, int> tabSizes() const { return mTabSizes; }
    void setTabSizes(std::map<QString, int> sizes) { mTabSizes = std::move(sizes); }
    /** Takes the size of every docked panel showing now as the one it comes back at, over any remembered earlier. */
    void rememberShownSizes();

Q_SIGNALS:
    void changed();

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    QToolBar* strip(Qt::DockWidgetArea area);
    /** Puts a strip on its window edge, or the bottom one into the status bar; false when it already was. */
    bool placeStrip(Qt::DockWidgetArea area, QToolBar* bar);
    void refreshStrips();
    /** Removes the dock's strip tab, open or collapsed. */
    void dropTab(QDockWidget* dock);
    /** Once the current layout change settles, drops an open dock's tab if the dock was closed, floated or moved. */
    void dropStaleTabLater(QDockWidget* dock);
    void hookTabBars();
    QDockWidget* dockForTab(QTabBar* bar, int index) const;
    void rememberSize(QDockWidget* dock);
    void restoreSize(QDockWidget* dock);
    /** Gives a docked panel squeezed to almost nothing its remembered size, or a default share of the window. */
    void ensureUsableSize(QDockWidget* dock);
    /** Snapshots the side areas' panel widths when a drag starts on a dock separator. */
    void beginEdgeDrag(const QPoint& at);
    /**
     * Qt shares a side area's change in width across all its columns; this hands it all to the column next to the
     * viewport, so dragging the area's edge resizes only the panel beside it.
     */
    void keepOuterColumns();

    /** A side-area panel's width when an edge drag began, and whether it is in the column next to the viewport. */
    struct EdgeDragDock
    {
        QPointer<QDockWidget> dock;
        int width = 0;
        bool inner = false;
    };
    struct EdgeDrag
    {
        std::vector<EdgeDragDock> docks;
        int extent = 0;
    };

    QMainWindow* mWindow;
    std::vector<EdgeDrag> mEdgeDrags;
    std::map<Qt::DockWidgetArea, QToolBar*> mStrips;
    std::map<QDockWidget*, QWidget*> mTabs;
    /** Docks whose strip tab stays while they are open. */
    std::set<QDockWidget*> mOpen;
    std::map<QDockWidget*, QSize> mSizes;
    std::map<QString, int> mTabSizes;
    bool mPressOnCurrentTab = false;
    QPoint mTabPressPos;
    bool mCollapsedByClick = false;
};
} // namespace lila::ui
