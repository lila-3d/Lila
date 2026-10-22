#include "ui/Hotbox.h"

#include "core/PrefStore.h"
#include "ui/Actions.h"
#include "ui/MarkingMenu.h"
#include "ui/OptionBox.h"
#include "ui/ViewportWidget.h"

#include <QApplication>
#include <QCursor>
#include <QKeyEvent>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>

#include <algorithm>

namespace lila::ui
{
namespace
{
constexpr double kRowHeight = 22.0;
constexpr double kRowGap = 6.0;
constexpr double kButtonGap = 2.0;

Hotbox* gHotbox = nullptr;

const char* buttonName(Qt::MouseButton b)
{
    switch (b)
    {
    case Qt::MiddleButton: return "mmb";
    case Qt::RightButton: return "rmb";
    default: return "lmb";
    }
}

std::string rowPointer(const char* row)
{
    return std::string("/hotbox/rows/") + row;
}
} // namespace

Hotbox* Hotbox::instance()
{
    return gHotbox;
}

void Hotbox::install(QMainWindow* main, std::vector<QMenu*> panelMenus)
{
    gHotbox = new Hotbox(main, std::move(panelMenus));
    actions::add({"hotbox.show", "Hotbox", "Hotbox", [] { gHotbox->showAt(QCursor::pos(), false); }, nullptr,
                  "Shows the hotbox at the cursor: every menu as a row of buttons, with a marking menu per zone and mouse button."});
    actions::add({"hotbox.showAllRows", "Show All Rows", "Hotbox", [] {
                      gHotbox->setRowVisible("main", true);
                      gHotbox->setRowVisible("panel", true);
                  }, nullptr, "Shows every menu row in the hotbox."});
    actions::add({"hotbox.hideAllRows", "Hide All Rows", "Hotbox", [] {
                      gHotbox->setRowVisible("main", false);
                      gHotbox->setRowVisible("panel", false);
                  }, nullptr, "Hides the hotbox's menu rows, leaving only its zones and centre row."});
    actions::add({"hotbox.toggleMainRow", "Main Menu Row", "Hotbox", [] { gHotbox->setRowVisible("main", !gHotbox->rowVisible("main")); },
                  nullptr, "Shows or hides the hotbox row of menubar menus."});
    actions::add({"hotbox.togglePanelRow", "Panel Menu Row", "Hotbox",
                  [] { gHotbox->setRowVisible("panel", !gHotbox->rowVisible("panel")); }, nullptr,
                  "Shows or hides the hotbox row of viewport panel menus."});
}

Hotbox::Hotbox(QMainWindow* main, std::vector<QMenu*> panelMenus)
    : QWidget(main, Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint | Qt::WindowDoesNotAcceptFocus),
      mMain(main), mPanelMenus(std::move(panelMenus))
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setMouseTracking(true);
}

bool Hotbox::rowVisible(const char* row) const
{
    return PrefStore::get().value<bool>("general", rowPointer(row), true);
}

void Hotbox::setRowVisible(const char* row, bool on)
{
    PrefStore::get().set("general", rowPointer(row), on);
    if (isVisible())
    {
        layoutButtons();
        update();
    }
}

void Hotbox::showAt(QPoint global, bool keyHeld)
{
    if (mOpenMenu && mOpenMenu->isVisible())
        mOpenMenu->close();
    // Views act on the active viewport, so the one under the cursor becomes it, as Maya's hotbox acts on that panel.
    for (QWidget* w = QApplication::widgetAt(global); w; w = w->parentWidget())
        if (auto* view = qobject_cast<ViewportWidget*>(w))
        {
            view->setFocus(Qt::OtherFocusReason);
            break;
        }
    setGeometry(mMain->geometry());
    mCenter = QPointF(global - geometry().topLeft());
    mKeyHeld = keyHeld;
    mInteracted = false;
    mHover = mOpen = -1;
    layoutButtons();
    show();
    raise();
    qApp->installEventFilter(this);
    update();
}

void Hotbox::keyReleased(bool tap)
{
    mKeyHeld = false;
    QAction* hovered = nullptr;
    std::string options;
    if (mOpenMenu && mOpenMenu->isVisible())
        // The innermost open popup, so an item in a submenu counts.
        if (auto* top = qobject_cast<QMenu*>(QApplication::activePopupWidget()))
        {
            hovered = top->activeAction();
            options = optionBox::optionsAt(top, hovered, QCursor::pos().x(),
                                           QGuiApplication::queryKeyboardModifiers() & Qt::ShiftModifier);
        }
    if (hovered && (!hovered->isEnabled() || hovered->isSeparator() || hovered->menu()))
        hovered = nullptr;
    switch (hotbox::keyUpAction(tap, hovered, MarkingMenu::active()))
    {
    case hotbox::KeyUp::Keep: return;
    case hotbox::KeyUp::Dismiss:
        closePopups();
        hide();
        return;
    case hotbox::KeyUp::TriggerAndDismiss:
    {
        QPointer<QAction> action = hovered;
        closePopups();
        hide();
        if (!options.empty())
            QTimer::singleShot(0, [options] { optionBox::open(options); });
        else if (action)
            action->trigger();
        return;
    }
    }
}

void Hotbox::closePopups()
{
    while (QWidget* popup = QApplication::activePopupWidget())
        popup->close();
}

void Hotbox::hideIfIdle()
{
    if (!mKeyHeld && !(mOpenMenu && mOpenMenu->isVisible()) && !MarkingMenu::active())
        hide();
}

void Hotbox::hideEvent(QHideEvent* e)
{
    qApp->removeEventFilter(this);
    if (mOpenMenu && mOpenMenu->isVisible())
        closePopups();
    mKeyHeld = false;
    QWidget::hideEvent(e);
}

void Hotbox::layoutButtons()
{
    mButtons.clear();
    QFont bold = font();
    bold.setBold(true);
    auto makeRow = [&](std::vector<Button> row, double y) {
        double total = 0;
        for (Button& b : row)
        {
            QFontMetricsF fm(b.center ? bold : font());
            b.rect.setSize({fm.horizontalAdvance(b.label) + (b.center ? 28 : 18), kRowHeight - 2});
            total += b.rect.width() + kButtonGap;
        }
        double x = mCenter.x() - (total - kButtonGap) / 2;
        for (Button& b : row)
        {
            b.rect.moveTopLeft({x, y - b.rect.height() / 2});
            x += b.rect.width() + kButtonGap;
            mButtons.push_back(std::move(b));
        }
    };

    if (rowVisible("main"))
    {
        std::vector<Button> row;
        for (QAction* a : mMain->menuBar()->actions())
            if (QPointer<QMenu> m = a->menu(); m && a->isVisible())
                row.push_back({a->text().remove('&'), {}, [m] { return m.data(); }});
        makeRow(std::move(row), mCenter.y() - kRowHeight - kRowGap);
    }
    makeRow({{"Recent Commands", {}, [this] { return recentMenu(); }},
             {"Lila", {}, [] { return static_cast<QMenu*>(nullptr); }, true},
             {"Hotbox Controls", {}, [this] {
                  if (!mControls)
                      mControls = new QMenu(this);
                  fillControls(mControls);
                  return mControls;
              }}},
            mCenter.y());
    if (rowVisible("panel"))
    {
        std::vector<Button> row;
        for (QMenu* m : mPanelMenus)
            row.push_back({m->title().remove('&'), {}, [m] { return m; }});
        makeRow(std::move(row), mCenter.y() + kRowHeight + kRowGap);
    }

    // Kept inside the window, so a hotbox opened near an edge shifts rather than clipping.
    QRectF box;
    for (const Button& b : mButtons)
        box |= b.rect;
    QRectF area = QRectF(rect()).adjusted(8, 8, -8, -8);
    QPointF shift(std::min(0.0, area.right() - box.right()) + std::max(0.0, area.left() - box.left()),
                  std::min(0.0, area.bottom() - box.bottom()) + std::max(0.0, area.top() - box.top()));
    for (Button& b : mButtons)
        b.rect.translate(shift);
    mCenter += shift;
    box.translate(shift);

    double halfW = std::max({box.right() - mCenter.x(), mCenter.x() - box.left(), 50.0}) + 10;
    double halfH = std::max({box.bottom() - mCenter.y(), mCenter.y() - box.top(), 12.0}) + 10;
    mCenterZone = QRectF(mCenter.x() - halfW, mCenter.y() - halfH, 2 * halfW, 2 * halfH);
}

QMenu* Hotbox::recentMenu()
{
    if (!mRecent)
    {
        mRecent = new QMenu(this);
        mRecent->setToolTipsVisible(true);
    }
    mRecent->clear();
    for (const nlohmann::json& target : actions::recent())
    {
        QAction* a = mRecent->addAction(QString::fromStdString(actions::labelFor(target)), [target] { actions::run(target); });
        a->setToolTip(QString::fromStdString(target.is_string() ? actions::tooltip(target.get<std::string>()) : actions::pythonFor(target)));
    }
    if (mRecent->isEmpty())
        mRecent->addAction("(none)")->setEnabled(false);
    return mRecent;
}

void Hotbox::fillControls(QMenu* menu)
{
    menu->clear();
    menu->setToolTipsVisible(true);
    auto toggle = [&](const char* label, const char* row, const char* id) {
        QAction* a = menu->addAction(label, [this, row] { setRowVisible(row, !rowVisible(row)); });
        a->setCheckable(true);
        a->setChecked(rowVisible(row));
        a->setToolTip(QString::fromStdString(actions::tooltip(id)));
    };
    toggle("Main Menu Row", "main", "hotbox.toggleMainRow");
    toggle("Panel Menu Row", "panel", "hotbox.togglePanelRow");
    menu->addSeparator();
    menu->addAction("Show All", [] { actions::run("hotbox.showAllRows"); })->setToolTip(QString::fromStdString(actions::tooltip("hotbox.showAllRows")));
    menu->addAction("Hide All", [] { actions::run("hotbox.hideAllRows"); })->setToolTip(QString::fromStdString(actions::tooltip("hotbox.hideAllRows")));
}

void Hotbox::openCameraMenu(Qt::MouseButton button, QPoint global)
{
    MarkingMenu* mm = MarkingMenu::popup(hotbox::markingMenuName(hotbox::Zone::Center, "lmb"), MarkingMenu::Trigger::MouseButton, button, global);
    if (!mm)
        return;
    mMarkingMenu = mm;
    connect(mm, &QObject::destroyed, this, [this] { QTimer::singleShot(0, this, &Hotbox::hideIfIdle); });
}

void Hotbox::openMenu(int index)
{
    QMenu* m = mButtons[index].menu();
    if (!m)
        return;
    if (mOpenMenu && mOpenMenu != m && mOpenMenu->isVisible())
        closePopups();
    disconnect(mHideConnection);
    disconnect(mTriggerConnection);
    mInteracted = true;
    mOpen = index;
    mOpenMenu = m;
    // Picking an item ends the hotbox even while the key is still down; the release then has nothing to do.
    mTriggerConnection = connect(m, &QMenu::triggered, this, [this] {
        disconnect(mTriggerConnection);
        mKeyHeld = false;
        QTimer::singleShot(0, this, [this] { hide(); });
    });
    mHideConnection = connect(m, &QMenu::aboutToHide, this, [this] {
        disconnect(mHideConnection);
        mOpen = -1;
        update();
        // Deferred: a click that closed this menu may be opening another one.
        QTimer::singleShot(0, this, &Hotbox::hideIfIdle);
    });
    m->popup(mapToGlobal(mButtons[index].rect.bottomLeft() + QPointF(0, 2)).toPoint());
    update();
}

void Hotbox::openZoneMenu(hotbox::Zone zone, Qt::MouseButton button, QPoint global)
{
    MarkingMenu* mm = MarkingMenu::popup(hotbox::markingMenuName(zone, buttonName(button)), MarkingMenu::Trigger::MouseButton,
                                         button, global);
    if (!mm)
        return;
    mMarkingMenu = mm;
    connect(mm, &QObject::destroyed, this, [this] { QTimer::singleShot(0, this, &Hotbox::hideIfIdle); });
}

void Hotbox::mousePressEvent(QMouseEvent* e)
{
    mInteracted = true;
    QPointF pos = e->position();
    if (int i = buttonAt(pos); i >= 0 && mButtons[i].center)
    {
        openCameraMenu(e->button(), e->globalPosition().toPoint());
        return;
    }
    if (e->button() == Qt::LeftButton)
        for (int i = 0; i < int(mButtons.size()); ++i)
            if (mButtons[i].rect.contains(pos))
            {
                openMenu(i);
                return;
            }
    QPointF d = pos - mCenter;
    openZoneMenu(hotbox::zoneAt(d.x(), d.y(), mCenterZone.width() / 2, mCenterZone.height() / 2), e->button(),
                 e->globalPosition().toPoint());
}

int Hotbox::buttonAt(QPointF pos) const
{
    for (int i = 0; i < int(mButtons.size()); ++i)
        if (mButtons[i].rect.contains(pos))
            return i;
    return -1;
}

void Hotbox::hoverButton(int index)
{
    if (index >= int(mButtons.size()))
        index = -1;
    if (index != mHover)
    {
        mHover = index;
        update();
    }
    if (hotbox::hoverOpens(mKeyHeld, index, index >= 0 && mButtons[index].center, mOpen))
        openMenu(index);
}

void Hotbox::mouseMoveEvent(QMouseEvent* e)
{
    hoverButton(buttonAt(e->position()));
}

void Hotbox::leaveEvent(QEvent*)
{
    mHover = -1;
    update();
}

bool Hotbox::eventFilter(QObject* obj, QEvent* e)
{
    switch (e->type())
    {
    case QEvent::ApplicationDeactivate:
        hide();
        break;
    case QEvent::MouseButtonPress:
        if (auto* w = qobject_cast<QWidget*>(obj))
        {
            QWidget* win = w->window();
            if (win != this && !qobject_cast<QMenu*>(win) && !qobject_cast<MarkingMenu*>(win))
                hide();
        }
        break;
    case QEvent::MouseMove:
        // An open menu takes every mouse move, so hovering another menu name is seen here.
        if (mKeyHeld && qobject_cast<QMenu*>(obj))
        {
            QPoint global = static_cast<QMouseEvent*>(e)->globalPosition().toPoint();
            if (QApplication::topLevelAt(global) == this)
            {
                int index = buttonAt(mapFromGlobal(global).toPointF());
                // Deferred: this may close the menu that is delivering the event.
                QTimer::singleShot(0, this, [this, index] { hoverButton(index); });
            }
        }
        break;
    case QEvent::KeyPress:
        if (static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape && !(mOpenMenu && mOpenMenu->isVisible()) && !MarkingMenu::active())
        {
            hide();
            return true;
        }
        break;
    default: break;
    }
    return false;
}

void Hotbox::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    // Never fully transparent: zero-alpha pixels of a layered window let clicks through to the window below.
    p.fillRect(rect(), QColor(0, 0, 0, 40));

    p.setPen(QPen(QColor(210, 210, 210, 80), 1));
    double reach = width() + height();
    for (QPointF corner : {mCenterZone.topLeft(), mCenterZone.topRight(), mCenterZone.bottomLeft(), mCenterZone.bottomRight()})
    {
        QPointF dir(corner.x() < mCenter.x() ? -1 : 1, corner.y() < mCenter.y() ? -1 : 1);
        p.drawLine(corner, corner + dir * reach);
    }

    const QColor bg(38, 38, 38, 225), border(92, 92, 92, 230), text(222, 222, 222);
    const QColor hiBg(150, 150, 150, 245), hiText(15, 15, 15);
    QFont bold = font();
    bold.setBold(true);
    for (int i = 0; i < int(mButtons.size()); ++i)
    {
        const Button& b = mButtons[i];
        bool hi = i == mHover || i == mOpen;
        p.setPen(QPen(b.center ? QColor(150, 150, 150) : border, 1));
        p.setBrush(hi ? hiBg : bg);
        p.drawRoundedRect(b.rect, 3, 3);
        p.setFont(b.center ? bold : font());
        p.setPen(hi ? hiText : text);
        p.drawText(b.rect, Qt::AlignCenter, b.label);
    }
}
} // namespace lila::ui
