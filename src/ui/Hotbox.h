#pragma once

#include "ui/HotboxZone.h"

#include <QPointer>
#include <QWidget>

#include <functional>
#include <vector>

class QMainWindow;
class QMenu;

namespace lila::ui
{
/**
 * Maya's hotbox: a translucent overlay over the main window, centred on the cursor, with every menubar menu and the
 * viewport panel menus as rows of buttons that drop down the real QMenus. Clicking empty space opens the marking menu
 * "hotbox_<zone>_<lmb|mmb|rmb>" for the zone under the cursor (north, south, east, west, centre).
 *
 * Shown by Hotkeys when a key's hold target is "hotbox.show"; it stays up while the key is held and while one of its
 * menus is open. Visible rows are in prefs general /hotbox/rows.
 */
class Hotbox : public QWidget
{
    Q_OBJECT

public:
    /** Creates the hotbox for main and registers the hotbox.* actions. panelMenus form the second row. */
    static void install(QMainWindow* main, std::vector<QMenu*> panelMenus);
    static Hotbox* instance();

    /** Shows centred on global. keyHeld: a hotkey is down, so the overlay hides when keyReleased() says so. */
    void showAt(QPoint global, bool keyHeld);
    /** The hotkey came up. Runs the item hovered in an open menu, if any, and dismisses (kept only while a zone
     * marking menu is up). */
    void keyReleased(bool tap);
    /** Something was clicked since showAt, so a quick release is not a tap. */
    bool interacted() const { return mInteracted; }

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* e) override;
    void hideEvent(QHideEvent* e) override;

private:
    struct Button
    {
        QString label;
        QRectF rect;
        std::function<QMenu*()> menu;
        bool center = false;
    };

    Hotbox(QMainWindow* main, std::vector<QMenu*> panelMenus);
    void layoutButtons();
    void openMenu(int index);
    void openZoneMenu(hotbox::Zone zone, Qt::MouseButton button, QPoint global);
    void hideIfIdle();
    void closePopups();
    int buttonAt(QPointF pos) const;
    void hoverButton(int index);
    bool rowVisible(const char* row) const;
    void setRowVisible(const char* row, bool on);
    QMenu* recentMenu();
    /** The centre label opens the centre zone's camera marking menu (hotbox_center_lmb) on any button, as Maya's does. */
    void openCameraMenu(Qt::MouseButton button, QPoint global);
    void fillControls(QMenu* menu);

    QMainWindow* mMain;
    std::vector<QMenu*> mPanelMenus;
    std::vector<Button> mButtons;
    QPointF mCenter;
    QRectF mCenterZone;
    int mHover = -1;
    int mOpen = -1;
    QPointer<QMenu> mOpenMenu;
    QPointer<QWidget> mMarkingMenu;
    QMenu* mRecent = nullptr;
    QMenu* mControls = nullptr;
    QMetaObject::Connection mHideConnection;
    QMetaObject::Connection mTriggerConnection;
    bool mKeyHeld = false;
    bool mInteracted = false;
};
} // namespace lila::ui
