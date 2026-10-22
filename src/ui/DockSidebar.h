#pragma once

#include <QStringList>
#include <QWidget>

#include <map>

class QDockWidget;
class QMainWindow;
class QTabBar;
class QToolBar;

namespace lila::ui
{
/**
 * Lays the bottom dock area out Maya's way: `tabs` in one tab group in that order with `current` on top, and `row`
 * as its own full-width row under the group. Docks already in the window are moved; hidden ones stay hidden.
 */
void arrangeBottomDocks(QMainWindow* window, const QList<QDockWidget*>& tabs, QDockWidget* current, QDockWidget* row);

/**
 * Opens a panel the way a menu or hotkey should: expanded if it was collapsed, its tab raised, a usable size and
 * keyboard focus. Works for docks of any main window, managed by DockSidebars or not.
 */
void revealDock(QDockWidget* dock);

/** A window toggle: tucks the panel into its sidebar (or hides it) when it is showing, otherwise reveals it. */
void toggleDock(QDockWidget* dock);

/** Dock title bar: title, then collapse, float and close buttons. Double-click floats or re-docks. */
class DockTitleBar : public QWidget
{
    Q_OBJECT

public:
    explicit DockTitleBar(QDockWidget* dock);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void collapseRequested();

protected:
    void paintEvent(QPaintEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;

private:
    QDockWidget* mDock;
};

/**
 * Collapsing a docked panel hides it and leaves a tab on a thin strip along its window edge; clicking the tab brings it
 * back where it was. Collapsed panels persist through the layout prefs.
 */
class DockSidebars : public QObject
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

signals:
    void changed();

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    QToolBar* strip(Qt::DockWidgetArea area);
    void refreshStrips();
    void hookTabBars();
    QDockWidget* dockForTab(QTabBar* bar, int index) const;
    void rememberSize(QDockWidget* dock);
    void restoreSize(QDockWidget* dock);
    /** Gives a docked panel squeezed to almost nothing its remembered size, or a default share of the window. */
    void ensureUsableSize(QDockWidget* dock);

    QMainWindow* mWindow;
    std::map<Qt::DockWidgetArea, QToolBar*> mStrips;
    std::map<QDockWidget*, QWidget*> mTabs;
    std::map<QDockWidget*, QSize> mSizes;
    std::map<QString, int> mTabSizes;
    bool mPressOnCurrentTab = false;
};
} // namespace lila::ui
