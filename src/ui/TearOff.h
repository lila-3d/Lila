#pragma once

#include "core/Export.h"

#include <QPoint>
#include <QMenu>
#include <QPointer>
#include <QString>
#include <QWidget>

#include <functional>
#include <optional>
#include <string>
#include <vector>

class QDockWidget;
class QMainWindow;
class QVBoxLayout;

/**
 * Maya's tear-off menus. Menu bar menus and their submenus get a dashed row at the top; clicking it turns the menu into
 * a panel of buttons bound to the same QActions, in a dock named for the menu's path (see TearOffLogic.h), so it docks,
 * tabs and is saved with layouts and the session like any other panel.
 */
namespace lila::ui::tearoff
{
/** Adds the tear-off row to the menu, and to its submenus as they appear. Returns the menu. */
LILA_UI_API QMenu* enable(QMenu* menu);

/** How a torn-off menu becomes a dock: the main window installs its addDock. */
LILA_UI_API void setDockHook(std::function<QDockWidget*(const QString& title, const QString& objectName, QWidget* content)> addDock);

/** The menu's path from the menu bar, or nullopt when it is not reachable from one. */
LILA_UI_API std::optional<std::vector<std::string>> pathOf(QMenu* menu);

/** The menu at that path in the window's menu bar. */
LILA_UI_API QMenu* findMenu(QMainWindow* window, const std::vector<std::string>& path);

/** Shows the menu's torn-off dock, creating it floating at `at` (the cursor when unset) if it has none yet. */
LILA_UI_API QDockWidget* tearOff(QMenu* menu, std::optional<QPoint> at = std::nullopt);

/** Creates the docks for these dock names that do not exist yet, so a restored window state can place them. */
LILA_UI_API void recreate(QMainWindow* window, const std::vector<std::string>& dockNames);

/** The torn-off panel inside a dock: a button per row of the menu, rebuilt when the menu's rows change. */
class LILA_UI_API TearOffPanel : public QWidget
{
    Q_OBJECT

public:
    explicit TearOffPanel(QMenu* menu, QWidget* parent = nullptr);
    QMenu* menu() const;
    /** Big enough to show every row without scrolling. */
    QSize preferredSize() const;
    /** Re-runs the menu's aboutToShow, which refreshes check states and fills menus built on show. */
    void refresh();

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;
    void enterEvent(QEnterEvent* e) override;

private:
    void scheduleRebuild();
    void rebuild();

    QPointer<QMenu> mMenu;
    QWidget* mBody = nullptr;
    QVBoxLayout* mRows = nullptr;
    std::vector<QAction*> mShown;
    bool mRebuildQueued = false;
};
} // namespace lila::ui::tearoff
