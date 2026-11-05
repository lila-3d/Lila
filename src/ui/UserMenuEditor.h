#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <QWidget>

#include <functional>
#include <string>

class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QTreeWidget;

namespace lila::ui
{
/**
 * Builds the user's own menus (see UserMenus.h): where each one goes (the menu bar, or a submenu or section of any
 * menu), and its rows, separators, sections and submenus. Edits stay in the window until Save.
 */
class LILA_UI_API UserMenuEditor : public QWidget
{
    Q_OBJECT

public:
    /** What an action row runs. */
    enum class Target
    {
        Action,
        Python,
        Command,
    };

    explicit UserMenuEditor(QWidget* parent = nullptr);
    ~UserMenuEditor() override;

    /** Shows the stored menu with this title; exposed for tests and screenshots. */
    void openMenu(const std::string& title);
    /** Selects a row by JSON pointer into the open menu ("/items/2/items/0"). */
    void selectRow(const std::string& pointer);

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    void load();
    void save();
    void setDirty(bool dirty);
    void refreshMenuList();
    void showMenu();
    void refreshPlacement();
    void refreshPositions();
    void rebuildTree();
    void showRow();
    void refreshProblem();

    nlohmann::json* menu();
    nlohmann::json* row();
    /** The array holding the selected row and its index there; the menu's own items with nothing selected. */
    nlohmann::json* rowList(size_t* index);
    void editMenu(const std::function<void(nlohmann::json&)>& fn);
    void editRow(const std::function<void(nlohmann::json&)>& fn);
    void addRow(nlohmann::json r);
    void removeRow();
    void moveRow(int delta);
    void tearOffMenu();

    nlohmann::json mDoc = nlohmann::json::array();
    int mCur = -1;
    std::string mSel;
    bool mDirty = false;
    bool mLoading = false;
    int mPrefListener = 0;

    QListWidget* mMenus = nullptr;
    QPushButton* mDelete = nullptr;
    QLineEdit* mTitle = nullptr;
    QFormLayout* mPlaceForm = nullptr;
    QComboBox* mPlace = nullptr;
    QComboBox* mParent = nullptr;
    QComboBox* mPosition = nullptr;
    QTreeWidget* mTree = nullptr;
    QPushButton* mRemove = nullptr;
    QPushButton* mUp = nullptr;
    QPushButton* mDown = nullptr;
    QPushButton* mTearOff = nullptr;

    QFormLayout* mRowForm = nullptr;
    QLabel* mRowTitle = nullptr;
    QLineEdit* mLabel = nullptr;
    QComboBox* mTarget = nullptr;
    QStackedWidget* mPages = nullptr;
    QLineEdit* mActionSearch = nullptr;
    QListWidget* mActionList = nullptr;
    QPlainTextEdit* mPython = nullptr;
    QPlainTextEdit* mCommand = nullptr;
    QLineEdit* mTooltip = nullptr;
    QWidget* mRowEditor = nullptr;

    QLabel* mProblem = nullptr;
    QPushButton* mSave = nullptr;
    QPushButton* mRevert = nullptr;
};

/** Opens the editor, or raises it. */
LILA_UI_API void openUserMenuEditor();
} // namespace lila::ui
