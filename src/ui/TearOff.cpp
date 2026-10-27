#include "ui/TearOff.h"

#include "core/Log.h"
#include "ui/DockSidebar.h"
#include "ui/Icons.h"
#include "ui/OptionBox.h"
#include "ui/Shelf.h"
#include "ui/TearOffLogic.h"

#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QDockWidget>
#include <QEnterEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace lila::ui::tearoff
{
namespace
{
constexpr const char* kEnabled = "lilaTearOff"; // Theme.cpp's style reads both
constexpr const char* kHotSection = "lilaHotSection";

std::function<QDockWidget*(const QString&, const QString&, QWidget*)>& dockHook()
{
    static std::function<QDockWidget*(const QString&, const QString&, QWidget*)> hook;
    return hook;
}

/** The menu bar or menu the menu hangs from, the menu bar first. */
QWidget* containerOf(QMenu* menu)
{
    QWidget* parentMenu = nullptr;
    for (QObject* o : menu->menuAction()->associatedObjects())
    {
        if (auto* bar = qobject_cast<QMenuBar*>(o))
            return bar;
        if (auto* m = qobject_cast<QMenu*>(o); m && m != menu && !parentMenu)
            parentMenu = m;
    }
    return parentMenu;
}

QMainWindow* windowOf(QMenu* menu)
{
    for (QMenu* cur = menu; cur;)
    {
        QWidget* c = containerOf(cur);
        if (auto* bar = qobject_cast<QMenuBar*>(c))
            if (auto* w = qobject_cast<QMainWindow*>(bar->window()))
                return w;
        cur = qobject_cast<QMenu*>(c);
    }
    for (QWidget* w : QApplication::topLevelWidgets())
        if (auto* mw = qobject_cast<QMainWindow*>(w))
            return mw;
    return nullptr;
}

/** The dashed row Qt draws above the first item when tear-off is enabled. */
bool inTearOffRow(QMenu* menu, QPoint pos)
{
    if (!menu->isTearOffEnabled() || !menu->rect().contains(pos))
        return false;
    for (QAction* a : menu->actions())
        if (a->isVisible())
            return pos.y() < menu->actionGeometry(a).top();
    return false;
}

/** The titled separator under the click, whose section tears off on its own. */
QAction* sectionAt(QMenu* menu, QPoint pos)
{
    QAction* a = menu->rect().contains(pos) ? menu->actionAt(pos) : nullptr;
    return a && a->isSeparator() && !a->text().isEmpty() ? a : nullptr;
}

void closeMenus()
{
    while (QWidget* p = QApplication::activePopupWidget())
        p->close();
}

/** Takes the click on the tear-off row before Qt's own tear-off, which would make a plain floating menu. */
class RowFilter : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        if (e->type() == QEvent::MouseMove || e->type() == QEvent::Leave || e->type() == QEvent::Hide)
            if (auto* menu = qobject_cast<QMenu*>(obj))
            {
                // Section titles highlight on hover like the tear-off row; the style reads the hot rect.
                QAction* a = e->type() == QEvent::MouseMove ? sectionAt(menu, static_cast<QMouseEvent*>(e)->position().toPoint()) : nullptr;
                QRect hot = a ? menu->actionGeometry(a) : QRect();
                if (menu->property(kHotSection).toRect() != hot)
                {
                    menu->setProperty(kHotSection, hot);
                    menu->update();
                }
            }
        if (e->type() != QEvent::MouseButtonPress && e->type() != QEvent::MouseButtonRelease && e->type() != QEvent::MouseButtonDblClick)
            return false;
        auto* menu = qobject_cast<QMenu*>(obj);
        auto* me = static_cast<QMouseEvent*>(e);
        if (!menu || me->button() != Qt::LeftButton)
            return false;
        QPoint pos = me->position().toPoint();
        QString section;
        if (QAction* a = sectionAt(menu, pos))
            section = a->text();
        else if (!inTearOffRow(menu, pos))
            return false;
        if (e->type() == QEvent::MouseButtonRelease)
        {
            QPointer<QMenu> m = menu;
            QPoint at = me->globalPosition().toPoint();
            closeMenus();
            QTimer::singleShot(0, qApp, [m, at, section] {
                if (m)
                    tearOff(m, at, section);
            });
        }
        return true;
    }
};

RowFilter* rowFilter()
{
    static QPointer<RowFilter> f;
    if (!f)
        f = new RowFilter(qApp);
    return f;
}

QDockWidget* makeDock(QMainWindow* window, QMenu* menu, const QString& name, const QString& section)
{
    QString title = section.isEmpty() ? QString::fromStdString(itemLabel(menu->title().toStdString())) : section;
    auto* panel = new TearOffPanel(menu, section);
    QDockWidget* dock = nullptr;
    if (dockHook())
        dock = dockHook()(title, name, panel);
    else
    {
        dock = new QDockWidget(title, window);
        dock->setObjectName(name);
        dock->setWidget(panel);
        if (window)
            window->addDockWidget(Qt::RightDockWidgetArea, dock);
    }
    return dock;
}
} // namespace

QMenu* enable(QMenu* menu)
{
    if (!menu || menu->property(kEnabled).toBool())
        return menu;
    menu->setProperty(kEnabled, true);
    menu->setTearOffEnabled(true);
    menu->installEventFilter(rowFilter());
    auto enableSubmenus = [menu] {
        for (QAction* a : menu->actions())
            if (QMenu* sub = a->menu())
                enable(sub);
    };
    enableSubmenus();
    QObject::connect(menu, &QMenu::aboutToShow, menu, enableSubmenus);
    return menu;
}

void setDockHook(std::function<QDockWidget*(const QString&, const QString&, QWidget*)> addDock)
{
    dockHook() = std::move(addDock);
}

std::optional<std::vector<std::string>> pathOf(QMenu* menu)
{
    std::vector<std::string> path;
    for (QMenu* cur = menu; cur && path.size() < 32;)
    {
        path.insert(path.begin(), itemLabel(cur->title().toStdString()));
        QWidget* c = containerOf(cur);
        if (qobject_cast<QMenuBar*>(c))
            return path;
        cur = qobject_cast<QMenu*>(c);
    }
    return std::nullopt;
}

QMenu* findMenu(QMainWindow* window, const std::vector<std::string>& path)
{
    if (!window || path.empty())
        return nullptr;
    QList<QAction*> rows = window->menuBar()->actions();
    QMenu* found = nullptr;
    for (const std::string& title : path)
    {
        found = nullptr;
        for (QAction* a : rows)
            if (QMenu* m = a->menu(); m && itemLabel(a->text().toStdString()) == title)
            {
                found = m;
                break;
            }
        if (!found)
            return nullptr;
        // Some menus only fill in their rows, submenus included, as they open.
        Q_EMIT found->aboutToShow();
        rows = found->actions();
    }
    return found;
}

QDockWidget* tearOff(QMenu* menu, std::optional<QPoint> at, const QString& section)
{
    if (!menu)
        return nullptr;
    QMainWindow* window = windowOf(menu);
    std::vector<std::string> path = pathOf(menu).value_or(std::vector<std::string>{itemLabel(menu->title().toStdString())});
    if (!section.isEmpty())
        path.push_back("#" + section.toStdString());
    QString name = QString::fromStdString(dockName(path));
    if (window)
        if (auto* existing = window->findChild<QDockWidget*>(name))
        {
            revealDock(existing);
            return existing;
        }

    QDockWidget* dock = makeDock(window, menu, name, section);
    auto* panel = static_cast<TearOffPanel*>(dock->widget());
    dock->setFloating(true);
    QSize body = panel->preferredSize();
    int height = body.height() + 28;
    // A clamped height brings in the vertical scroll bar, which would otherwise cover the option box column.
    if (height > 640)
        body.rwidth() += panel->style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, panel);
    QSize size(std::clamp(body.width(), 160, 360), std::clamp(height, 80, 640));
    QPoint pos = at.value_or(QCursor::pos()) - QPoint(20, 12);
    if (QScreen* screen = QGuiApplication::screenAt(pos))
    {
        QRect avail = screen->availableGeometry();
        pos.setX(std::clamp(pos.x(), avail.left(), std::max(avail.left(), avail.right() - size.width())));
        pos.setY(std::clamp(pos.y(), avail.top(), std::max(avail.top(), avail.bottom() - size.height())));
    }
    dock->setGeometry(QRect(pos, size));
    dock->show();
    dock->raise();
    return dock;
}

void recreate(QMainWindow* window, const std::vector<std::string>& dockNames)
{
    if (!window)
        return;
    for (const std::string& name : dockNames)
    {
        QString qname = QString::fromStdString(name);
        if (window->findChild<QDockWidget*>(qname))
            continue;
        std::optional<std::vector<std::string>> path = parseDockName(name);
        if (!path)
            continue;
        QString section;
        if (path->size() > 1 && path->back().starts_with('#'))
        {
            section = QString::fromStdString(path->back().substr(1));
            path->pop_back();
        }
        QMenu* menu = findMenu(window, *path);
        if (!menu)
        {
            log::warn("layout: no menu at {} to tear off", name.substr(kDockPrefix.size()));
            continue;
        }
        makeDock(window, menu, qname, section);
    }
}

// ------------------------------------------------------------------- TearOffPanel

TearOffPanel::TearOffPanel(QMenu* menu, const QString& section, QWidget* parent) : QWidget(parent), mMenu(menu), mSection(section)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    mBody = new QWidget(scroll);
    mRows = new QVBoxLayout(mBody);
    mRows->setContentsMargins(2, 2, 2, 2);
    mRows->setSpacing(0);
    scroll->setWidget(mBody);
    root->addWidget(scroll);
    setStyleSheet("QPushButton#tearOffRow { text-align: left; padding: 3px 6px; border: none; background: transparent; }"
                  "QPushButton#tearOffRow:hover { background: #3c3c3c; }"
                  "QPushButton#tearOffRow:pressed { background: #505050; }"
                  "QPushButton#tearOffRow:disabled { color: #666666; }"
                  "QFrame#tearOffSeparator { background: #3a3a3a; }");
    if (mMenu)
    {
        mMenu->installEventFilter(this);
        connect(mMenu, &QObject::destroyed, this, &TearOffPanel::scheduleRebuild);
    }
    refresh();
    rebuild();
}

QMenu* TearOffPanel::menu() const
{
    return mMenu;
}

QSize TearOffPanel::preferredSize() const
{
    return mBody->sizeHint() + QSize(8, 4);
}

void TearOffPanel::refresh()
{
    if (mMenu)
        Q_EMIT mMenu->aboutToShow();
}

void TearOffPanel::enterEvent(QEnterEvent* e)
{
    refresh();
    QWidget::enterEvent(e);
}

bool TearOffPanel::eventFilter(QObject* obj, QEvent* e)
{
    if (obj == mMenu && (e->type() == QEvent::ActionAdded || e->type() == QEvent::ActionRemoved))
        scheduleRebuild();
    return QWidget::eventFilter(obj, e);
}

void TearOffPanel::scheduleRebuild()
{
    if (mRebuildQueued)
        return;
    mRebuildQueued = true;
    QTimer::singleShot(0, this, [this] {
        mRebuildQueued = false;
        rebuild();
    });
}

void TearOffPanel::rebuild()
{
    std::vector<QAction*> now;
    bool in = mSection.isEmpty();
    if (mMenu)
        for (QAction* a : mMenu->actions())
        {
            bool titled = a->isSeparator() && !a->text().isEmpty();
            if (!mSection.isEmpty() && titled)
            {
                in = a->text() == mSection;
                continue;
            }
            if (in)
                now.push_back(a);
        }
    if (now == mShown && mRows->count() > 0)
        return;
    mShown = now;
    // deleteLater: a rebuild can follow a click on one of these rows.
    while (QLayoutItem* item = mRows->takeAt(0))
    {
        if (QWidget* w = item->widget())
        {
            w->hide();
            w->deleteLater();
        }
        delete item;
    }
    if (!mMenu)
    {
        mRows->addWidget(new QLabel("This menu no longer exists.", mBody));
        mRows->addStretch(1);
        return;
    }

    for (QAction* a : now)
    {
        if (a->isSeparator())
        {
            auto* sep = new QWidget(mBody);
            auto* h = new QHBoxLayout(sep);
            h->setContentsMargins(a->text().isEmpty() ? 0 : 6, 2, 0, 2);
            h->setSpacing(6);
            if (!a->text().isEmpty())
            {
                auto* title = new QLabel(a->text(), sep);
                title->setObjectName("tearOffSection");
                title->setEnabled(false);
                h->addWidget(title);
            }
            auto* line = new QFrame(sep);
            line->setObjectName("tearOffSeparator");
            line->setFixedHeight(1);
            h->addWidget(line, 1, Qt::AlignVCenter);
            sep->setVisible(a->isVisible());
            connect(a, &QAction::changed, sep, [sep, a] { sep->setVisible(a->isVisible()); });
            mRows->addWidget(sep);
            continue;
        }
        QPointer<QMenu> sub = a->menu();
        std::string id = a->data().toString().toStdString();
        auto* row = new QWidget(mBody);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(0);
        auto* button = new QPushButton(row);
        button->setObjectName("tearOffRow");
        button->setFlat(true);
        button->setFocusPolicy(Qt::NoFocus);
        button->setIconSize(QSize(14, 14));
        button->setProperty("tearOffAction", QVariant::fromValue(static_cast<QObject*>(a)));
        h->addWidget(button, 1);
        if (sub)
        {
            auto* arrow = new QLabel(row);
            arrow->setPixmap(icon("chevron-right").pixmap(12, 12));
            arrow->setContentsMargins(0, 0, 4, 0);
            h->addWidget(arrow);
        }
        else if (!id.empty() && optionBox::available(id))
        {
            auto* box = new QToolButton(row);
            box->setIcon(icon("app-window"));
            box->setIconSize(QSize(12, 12));
            box->setAutoRaise(true);
            box->setFixedWidth(18);
            box->setFocusPolicy(Qt::NoFocus);
            box->setToolTip("Options (or Shift+click the item)");
            connect(box, &QToolButton::clicked, box, [id] {
                if (shelves::addModifierHeld())
                    shelves::addToCurrent(nlohmann::json{{"optionBox", id}});
                else
                    optionBox::open(id);
            });
            h->addWidget(box);
        }
        auto sync = [button, row, a] {
            button->setText(QString::fromStdString(itemLabel(a->text().toStdString())));
            button->setToolTip(a->toolTip());
            if (a->isCheckable())
                button->setIcon(icon(a->isChecked() ? "square-check" : "square"));
            else
                button->setIcon(a->icon());
            button->setEnabled(a->isEnabled());
            row->setVisible(a->isVisible());
        };
        sync();
        connect(a, &QAction::changed, button, sync);
        QPointer<QAction> action = a;
        connect(button, &QPushButton::clicked, this, [this, action, sub, button, id] {
            if (sub)
            {
                sub->popup(button->mapToGlobal(QPoint(button->width(), 0)));
                return;
            }
            if (!action)
                return;
            if (shelves::addModifierHeld() && !id.empty())
            {
                shelves::addToCurrent(id);
                return;
            }
            if ((QApplication::keyboardModifiers() & Qt::ShiftModifier) && !id.empty() && optionBox::available(id))
            {
                optionBox::open(id);
                return;
            }
            action->trigger();
            // Running it may change what the menu shows, such as check states.
            QTimer::singleShot(0, this, &TearOffPanel::refresh);
        });
        mRows->addWidget(row);
    }
    mRows->addStretch(1);
}
} // namespace lila::ui::tearoff
