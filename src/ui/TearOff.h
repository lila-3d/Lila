#pragma once

#include "core/Export.h"

#include <glm/vec3.hpp>

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
 * tabs and is saved with layouts and the session like any other panel. Clicking a section's title tears off just that
 * section's rows, as a dock whose path ends in "#<section>".
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

/**
 * Shows the menu's torn-off dock, creating it floating at `at` (the cursor when unset) if it has none yet. With a section,
 * only the rows under that section title.
 */
LILA_UI_API QDockWidget* tearOff(QMenu* menu, std::optional<QPoint> at = std::nullopt, const QString& section = {});

/**
 * Tears the menu (or one section of it) off under the cursor and carries it there until the next click drops it; Esc
 * or a right click puts it back. A dock already docked is only revealed.
 */
LILA_UI_API QDockWidget* tearOffToPlace(QMenu* menu, const QString& section = {});

/** A dock property, true while tearOffToPlace is carrying it. */
inline constexpr const char* kPlacingProperty = "lilaPlacing";

/** A torn-off row being dragged: the action id. */
inline constexpr const char* kActionMime = "application/x-lila-action";

/** Whether a menu item or torn-off row of this action can be dragged into a viewport: the Create actions. */
LILA_UI_API bool draggable(const std::string& actionId);

/**
 * Runs the action as one undo step and moves what it created to `at`, keeping the new objects' layout. Returns the new
 * top-level transforms; empty when it created none (it still ran).
 */
LILA_UI_API std::vector<std::string> runAt(const std::string& actionId, const glm::dvec3& at);

/** Lets a viewport take a dragged Create row: drops it on the surface under the cursor, else on the ground plane. */
LILA_UI_API void registerDropHandler();

/** Creates the docks for these dock names that do not exist yet, so a restored window state can place them. */
LILA_UI_API void recreate(QMainWindow* window, const std::vector<std::string>& dockNames);

/** The torn-off panel inside a dock: a button per row of the menu, rebuilt when the menu's rows change. */
class LILA_UI_API TearOffPanel : public QWidget
{
    Q_OBJECT

public:
    /** section: show only the rows under the separator with that title, up to the next titled one. */
    explicit TearOffPanel(QMenu* menu, const QString& section = {}, QWidget* parent = nullptr);
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
    QString mSection;
    QWidget* mBody = nullptr;
    QVBoxLayout* mRows = nullptr;
    std::vector<QAction*> mShown;
    bool mRebuildQueued = false;
    QPoint mPressPos;
};
} // namespace lila::ui::tearoff
