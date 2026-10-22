#pragma once

#include <nlohmann/json.hpp>

#include <QWidget>

class QComboBox;
class QDockWidget;
class QLayout;
class QMenu;
class QTabBar;
class QStackedWidget;

namespace lila::ui
{
/**
 * Shelves live in prefs as "shelves/<Name>" ({"items": [{"label", "icon", "target", "tooltip"}]}), ordered by the general
 * pref /shelves/order. "icon" names a Tabler icon (see ui::icon). A target is anything actions::run accepts: an action
 * id, {"command": ...} or {"python": ...}.
 */
namespace shelves
{
std::vector<std::string> names();
nlohmann::json items(const std::string& shelf);
void setItems(const std::string& shelf, const nlohmann::json& items);
void addItem(const std::string& shelf, const nlohmann::json& item);
void create(const std::string& shelf);
void remove(const std::string& shelf);
bool rename(const std::string& shelf, const std::string& to);
} // namespace shelves

/**
 * One shelf's buttons: a single row when docked, a wrapping grid of square icon buttons when floating. Buttons drag to
 * reorder (also onto another shelf) and right-click to edit.
 */
class ShelfPage : public QWidget
{
    Q_OBJECT

public:
    ShelfPage(std::string shelf, bool floating, QWidget* parent = nullptr);
    ~ShelfPage() override;
    void setShelf(std::string shelf);
    void rebuild();
    const std::string& shelf() const { return mShelf; }

    /** Opens the button editor for a new button and appends it. */
    void addButton();
    /** The ways to add a button: new Python or command button, paste, and every registered action. */
    void fillAddMenu(QMenu* menu);

signals:
    void triggered();

protected:
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    void itemMenu(int index, QPoint global);
    int dropIndex(QPoint pos) const;

    std::string mShelf;
    bool mFloating;
    QLayout* mLayout = nullptr;
    QPoint mPressPos;
    int mPrefListener = 0;
};

/** The docked shelf strip with a tab per shelf. */
class ShelfPanel : public QWidget
{
    Q_OBJECT

public:
    explicit ShelfPanel(QWidget* parent = nullptr);
    ~ShelfPanel() override;

    /** A thin grip in place of the dock's title bar, so the shelf keeps its vertical space but can still be dragged. */
    static QWidget* dockTitleBar(QDockWidget* dock);

private:
    void rebuild();

    QTabBar* mTabs = nullptr;
    QStackedWidget* mPages = nullptr;
    std::vector<std::string> mNames;
    int mPrefListener = 0;
};

/**
 * The floating shelf window. toggle() opens it pinned, staying up until toggled again or closed. popup() opens it
 * without taking focus: hold its hotkey and release over a button to run it, or tap and click; it stays until the
 * hotkey is pressed again unless /floatingShelf/closeOnClickAway or /floatingShelf/closeAfterRun is set. Where it
 * opens is the general pref /floatingShelf/openAt (cursor, viewport or screen).
 */
class FloatingShelf : public QWidget
{
    Q_OBJECT

public:
    static void toggle();
    static void popup();
    static void releaseKey();
    static FloatingShelf* active();

protected:
    void keyPressEvent(QKeyEvent* e) override;
    void closeEvent(QCloseEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    FloatingShelf(QWidget* parent, bool pinned);
    ~FloatingShelf() override;
    static void open(bool pinned);
    void build();
    void reloadPicker();
    void showShelf(const std::string& name);
    void place();
    void settingsMenu(QMenu* menu);

    QComboBox* mPicker = nullptr;
    ShelfPage* mPage = nullptr;
    bool mPinned = false;
    qint64 mOpenedAt = 0;
    int mPrefListener = 0;
};

void registerShelfActions();
} // namespace lila::ui
