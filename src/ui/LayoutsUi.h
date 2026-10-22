#pragma once

#include <nlohmann/json.hpp>

#include <QObject>

#include <functional>

class QComboBox;
class QDockWidget;
class QMainWindow;
class QMenu;
class QWidget;

namespace lila::ui
{
class DockSidebars;
class ViewportLayout;

/**
 * Named layouts in the main window: the Windows > Layouts menu, the switcher in the menu bar's corner, the manager
 * dialog and the window.layout.* actions. Installs the hooks behind the applyLayout and saveLayout commands.
 */
class LayoutController : public QObject
{
    Q_OBJECT

public:
    /** resetToDefault puts every dock back where the window first built it. */
    LayoutController(QMainWindow* window, DockSidebars* sidebars, ViewportLayout* viewports, std::function<void()> resetToDefault);
    ~LayoutController() override;

    QMenu* menu() const { return mMenu; }

    nlohmann::json capture() const;
    void apply(const nlohmann::json& layout);

    void saveAs();
    void manage();
    /** Adds an applyLayout button to the shelf showing in the shelf dock (or the first shelf). */
    void addToShelf(const std::string& name);

private:
    void fillMenu();
    void refreshSwitcher();
    void run(const char* command, const nlohmann::json& call);
    QDockWidget* dock(const QString& name) const;
    void ensureDocks(const nlohmann::json& docks);
    void applyPreset(const nlohmann::json& preset);
    std::string currentShelf() const;

    QMainWindow* mWindow;
    DockSidebars* mSidebars;
    ViewportLayout* mViewports;
    std::function<void()> mReset;
    QMenu* mMenu = nullptr;
    QComboBox* mSwitcher = nullptr;
    int mPrefListener = 0;
};
} // namespace lila::ui
