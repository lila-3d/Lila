#pragma once

#include "core/Export.h"

#include <QMainWindow>
#include <QPointer>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <vector>

#include "io/Recovery.h"

class QDockWidget;
class QLabel;
class QTimer;
class QToolButton;

namespace lila::ui
{
namespace menus
{
class MenuBuilder;
}
namespace panels
{
struct Panel;
}
class DockSidebars;
class LayoutController;
class ViewportLayout;

class LILA_UI_API MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow();
    ~MainWindow() override;

    /**
     * Run once the window is up: offers crash recovery for stale (crashed) sessions, then the startup dialog when
     * showStartup and recovery did not open anything.
     */
    void startup(const std::vector<io::recovery::Session>& stale, bool showStartup);
    /** File > Open on a given file (.lila, .lilaa, .ma, .mb), as the command line opens one. */
    void openFile(const std::filesystem::path& path) { openScene(path); }

protected:
    void closeEvent(QCloseEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void moveEvent(QMoveEvent* e) override;

private:
    void registerActions();
    QMenu* refreshKeysOnShow(QMenu* menu);
    /** Lila's own panels, in the order their docks are made (a panel's place() may refer to earlier ones). */
    void registerPanels();
    void createPanelDock(const panels::Panel& p);
    void removePanelDock(const std::string& id);
    void buildMenus();
    void buildPanelMenus();
    void buildStatusBar();
    QDockWidget* addDock(const QString& title, const QString& objectName, QWidget* content, Qt::DockWidgetArea area);
    void run(const char* command, const nlohmann::json& call = nlohmann::json::object());
    void restoreLayout();
    void applyDefaultDockSizes();
    void resetLayout();
    /** The menu bar and shelf dock as the /ui/showMenuBar and /ui/showShelf prefs say. */
    void applyChromePrefs();
    /** Raises the channel box in its tab group, or the tab that was up before it when it already is (Maya's Ctrl+A). */
    void toggleChannelBox();
    void toggleUvEditor();
    void renameSelection();
    /** Create > Camera from View: a camera node posed and lensed like the active view's camera, selected. */
    void createCameraFromView();
    void scheduleLayoutSave();
    void saveLayout();
    void refreshRecentFolders();
    void refreshWorkspaceLabel();
    /** Puts the workspace name at the right of the menu bar, beside the layout switcher. */
    void placeWorkspaceLabel();
    void openFolder();
    void showAbout();
    bool confirmDiscard();
    /** target: save there without asking (Increment and Save). */
    bool saveScene(bool saveAs, const std::filesystem::path& target = {});
    void incrementAndSave();
    void showVersions();
    void openScene(const std::filesystem::path& path);
    void addRecentFile(const std::filesystem::path& path);
    void refreshTitle();
    void refreshSourceStatus();
    void importFile();
    void convertMayaFolder();
    void exportSelection();
    void sendToUnreal();

    ViewportLayout* mViewports = nullptr;
    QPointer<QMenu> mRecentMenu;
    menus::MenuBuilder* mMenus = nullptr;
    int mPanelListener = 0;
    QTimer* mAutosaveTimer = nullptr;
    QLabel* mWorkspaceLabel = nullptr;
    QLabel* mSourceLabel = nullptr;
    QToolButton* mMcpButton = nullptr;
    QTimer* mLayoutTimer = nullptr;
    bool mRestoring = false;
    QByteArray mDefaultState;
    DockSidebars* mSidebars = nullptr;
    LayoutController* mLayouts = nullptr;
    QPointer<QDockWidget> mTabBeforeChannelBox;
    bool mShelfShown = true;
};
} // namespace lila::ui
