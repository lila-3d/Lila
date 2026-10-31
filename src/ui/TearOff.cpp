#include "ui/TearOff.h"

#include "core/Log.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "scene/TransformOps.h"
#include "ui/Actions.h"
#include "ui/DockSidebar.h"
#include "ui/Icons.h"
#include "ui/OptionBox.h"
#include "ui/Panels.h"
#include "ui/Shelf.h"
#include "ui/TearOffLogic.h"
#include "ui/ViewportHooks.h"

#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QDockWidget>
#include <QDrag>
#include <QMimeData>
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

#ifdef _WIN32
#include <windows.h>
#endif

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
    // Bounded: a popup that refuses to close would otherwise spin here forever.
    for (int i = 0; i < 32; ++i)
        if (QWidget* p = QApplication::activePopupWidget())
            p->close();
        else
            break;
}

/**
 * A drag out of a closed menu, done without the platform's drag: on Windows a native drag begun right after a popup
 * closes under a held button never gets the release. The ghost grabs the mouse and sends Qt's drag events to the widget
 * under the cursor, so viewports take it exactly as they take a drag from a torn-off row.
 */
class MenuDrag : public QLabel
{
public:
    MenuDrag(const QString& actionId, const QPixmap& pixmap) : QLabel(nullptr, Qt::ToolTip | Qt::FramelessWindowHint)
    {
        setObjectName("lilaMenuDrag");
        setAttribute(Qt::WA_DeleteOnClose);
        setPixmap(pixmap);
        setWindowOpacity(0.8);
        mMime.setData(kActionMime, actionId.toUtf8());
        follow(QCursor::pos());
        show();
        grabMouse(Qt::DragCopyCursor);
        grabKeyboard();
        // Polled as well as grabbed: after a popup closes under a held button, Windows may send the moves and the release elsewhere.
        auto* poll = new QTimer(this);
        connect(poll, &QTimer::timeout, this, [this] {
            const QPoint at = QCursor::pos();
            if (!leftButtonDown())
                release(at, QApplication::keyboardModifiers());
            else if (at != mLast)
                follow(at);
        });
        poll->start(15);
    }

protected:
    void mouseMoveEvent(QMouseEvent* e) override { follow(e->globalPosition().toPoint()); }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        mReleaseSeen = true;
        // Closing the menu can hand over a release the button never made; only a real one drops.
        if (!leftButtonDown())
            release(e->globalPosition().toPoint(), e->modifiers());
    }

    void keyPressEvent(QKeyEvent* e) override
    {
        if (e->key() == Qt::Key_Escape && !mDone)
        {
            mDone = true;
            leave();
            finish();
        }
    }

private:
    /** The button as the hardware has it; offscreen (tests) only the release events count. */
    bool leftButtonDown() const
    {
        if (QGuiApplication::platformName() == "offscreen")
            return !mReleaseSeen;
#ifdef _WIN32
        return (GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) & 0x8000) != 0;
#else
        return QGuiApplication::mouseButtons() & Qt::LeftButton;
#endif
    }

    void release(QPoint global, Qt::KeyboardModifiers mods)
    {
        if (mDone)
            return;
        mDone = true;
        follow(global);
        if (mTarget && mAccepted)
        {
            QDropEvent drop(QPointF(mTarget->mapFromGlobal(global)), Qt::CopyAction, &mMime, Qt::NoButton, mods);
            QApplication::sendEvent(mTarget, &drop);
        }
        finish();
    }

    /** The widget under the cursor that takes drops, skipping this ghost. */
    QWidget* dropWidgetAt(QPoint global) const
    {
        QWidget* under = QApplication::widgetAt(global);
        // widgetAt misses native children such as the viewports' Vulkan surfaces: look through the window's widget tree.
        for (QWidget* top : QApplication::topLevelWidgets())
            if (!under && top != this && top->isVisible() && top->frameGeometry().contains(global))
                under = top->childAt(top->mapFromGlobal(global));
        for (QWidget* w = under; w; w = w->parentWidget())
            if (w->acceptDrops())
                return w;
        return nullptr;
    }

    void follow(QPoint global)
    {
        mLast = global;
        move(global + QPoint(14, 14));
        QWidget* target = dropWidgetAt(global);
        if (target != mTarget)
        {
            leave();
            mTarget = target;
            if (mTarget)
            {
                QDragEnterEvent enter(mTarget->mapFromGlobal(global), Qt::CopyAction, &mMime, Qt::LeftButton, QApplication::keyboardModifiers());
                QApplication::sendEvent(mTarget, &enter);
                mAccepted = enter.isAccepted();
            }
        }
        if (!mTarget || !mAccepted)
            return;
        QDragMoveEvent hover(mTarget->mapFromGlobal(global), Qt::CopyAction, &mMime, Qt::LeftButton, QApplication::keyboardModifiers());
        QApplication::sendEvent(mTarget, &hover);
        mAccepted = hover.isAccepted();
    }

    void leave()
    {
        if (mTarget)
        {
            QDragLeaveEvent left;
            QApplication::sendEvent(mTarget, &left);
        }
        mTarget = nullptr;
        mAccepted = false;
    }

    void finish()
    {
        releaseKeyboard();
        releaseMouse();
        close();
    }

    QMimeData mMime;
    QPointer<QWidget> mTarget;
    QPoint mLast;
    bool mAccepted = false;
    bool mDone = false;
    bool mReleaseSeen = false;
};

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
        if (e->type() == QEvent::MouseMove && dragOut(qobject_cast<QMenu*>(obj), static_cast<QMouseEvent*>(e)))
            return true;
        if (e->type() != QEvent::MouseButtonPress && e->type() != QEvent::MouseButtonRelease && e->type() != QEvent::MouseButtonDblClick)
            return false;
        auto* menu = qobject_cast<QMenu*>(obj);
        auto* me = static_cast<QMouseEvent*>(e);
        if (!menu || me->button() != Qt::LeftButton)
            return false;
        QPoint pos = me->position().toPoint();
        if (e->type() == QEvent::MouseButtonPress)
        {
            QAction* a = menu->rect().contains(pos) ? menu->actionAt(pos) : nullptr;
            const bool canDrag = a && !a->menu() && draggable(a->data().toString().toStdString());
            mPressMenu = canDrag ? menu : nullptr;
            mPressed = canDrag ? a : nullptr;
        }
        else
            mPressed = nullptr;
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

private:
    /** A Create item pressed and then dragged out of its menu: closes the menus and drags it, as from a torn-off row. */
    bool dragOut(QMenu* menu, QMouseEvent* me)
    {
        if (!(me->buttons() & Qt::LeftButton))
        {
            mPressed = nullptr;
            return false;
        }
        const QPoint global = me->globalPosition().toPoint();
        // Over the menus or the menu bar the held button still picks items; the drag starts once it is clear of them.
        QWidget* under = QApplication::widgetAt(global);
        if (qobject_cast<QMenu*>(under) || qobject_cast<QMenuBar*>(under))
            return false;
        if (!menu || !mPressed || !mPressMenu)
            return false;
        const QString id = mPressed->data().toString();
        const QPixmap pixmap = mPressMenu->grab(mPressMenu->actionGeometry(mPressed));
        mPressed = nullptr;
        mPressMenu = nullptr;
        closeMenus();
        // Once the popup's own mouse handling has unwound, so the ghost's grab is the only one.
        QTimer::singleShot(0, qApp, [id, pixmap] { new MenuDrag(id, pixmap); });
        return true;
    }

    QPointer<QMenu> mPressMenu;
    QPointer<QAction> mPressed;
};

RowFilter* rowFilter()
{
    static QPointer<RowFilter> f;
    if (!f)
        f = new RowFilter(qApp);
    return f;
}

QString dockNameFor(QMenu* menu, const QString& section)
{
    std::vector<std::string> path = pathOf(menu).value_or(std::vector<std::string>{itemLabel(menu->title().toStdString())});
    if (!section.isEmpty())
        path.push_back("#" + section.toStdString());
    return QString::fromStdString(dockName(path));
}

/** The top-left that puts a window of this size at `pos`, kept on the screen under it. */
QPoint onScreen(QPoint pos, QSize size)
{
    if (QScreen* screen = QGuiApplication::screenAt(pos))
    {
        QRect avail = screen->availableGeometry();
        pos.setX(std::clamp(pos.x(), avail.left(), std::max(avail.left(), avail.right() - size.width())));
        pos.setY(std::clamp(pos.y(), avail.top(), std::max(avail.top(), avail.bottom() - size.height())));
    }
    return pos;
}

constexpr QPoint kCursorInDock(20, 12);

/**
 * Carries a floating dock under the cursor until the next click, which drops it there. Esc or a right click takes it
 * back: a dock made for the placing closes, one that was already open returns to where it was.
 */
class Placer : public QObject
{
public:
    Placer(QDockWidget* dock, bool created) : QObject(dock), mDock(dock), mCreated(created), mStart(dock->pos())
    {
        dock->setProperty(kPlacingProperty, true);
        qApp->installEventFilter(this);
        QApplication::setOverrideCursor(Qt::SizeAllCursor);
        auto* poll = new QTimer(this);
        connect(poll, &QTimer::timeout, this, [this] { follow(); });
        poll->start(15);
        follow();
    }

    ~Placer() override { finish(); }

protected:
    bool eventFilter(QObject*, QEvent* e) override
    {
        switch (e->type())
        {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
            if (!mDone)
                static_cast<QMouseEvent*>(e)->button() == Qt::LeftButton ? drop() : cancel();
            return true;
        case QEvent::MouseButtonRelease:
            // The release of the dropping click is still this placer's; after it nothing is.
            if (mDone)
                deleteLater();
            return true;
        case QEvent::KeyPress:
            if (!mDone && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape)
            {
                cancel();
                return true;
            }
            return false;
        default: return false;
        }
    }

private:
    void follow()
    {
        if (mDock && !mDone)
            mDock->move(onScreen(QCursor::pos() - kCursorInDock, mDock->size()));
    }

    void drop()
    {
        follow();
        finish();
        QTimer::singleShot(500, this, [this] { deleteLater(); });
    }

    void cancel()
    {
        finish();
        if (!mDock)
            return;
        if (mCreated)
            mDock->close();
        else
            mDock->move(mStart);
        QTimer::singleShot(500, this, [this] { deleteLater(); });
    }

    void finish()
    {
        if (mDone)
            return;
        mDone = true;
        QApplication::restoreOverrideCursor();
        if (mDock)
            mDock->setProperty(kPlacingProperty, QVariant());
    }

    QPointer<QDockWidget> mDock;
    bool mCreated;
    QPoint mStart;
    bool mDone = false;
};

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
    dock->setProperty(panels::kOwnSizeProperty, true);
    return dock;
}

/** Big enough for every row, up to a cap past which the panel scrolls. */
QSize fittedSize(TearOffPanel* panel)
{
    QSize body = panel->preferredSize();
    int height = body.height() + 28;
    // A clamped height brings in the vertical scroll bar, which would otherwise cover the option box column.
    if (height > 640)
        body.rwidth() += panel->style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, panel);
    return QSize(std::clamp(body.width(), 160, 360), std::clamp(height, 80, 640));
}

constexpr QSize kMaxFloating(400, 700);
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
    QString name = dockNameFor(menu, section);
    if (window)
        if (auto* existing = window->findChild<QDockWidget*>(name))
        {
            revealDock(existing);
            // A floating size the panel never asked for (another window's placement, a layout saved elsewhere) is refitted.
            auto* panel = qobject_cast<TearOffPanel*>(existing->widget());
            if (panel && existing->isFloating() && (existing->width() > kMaxFloating.width() || existing->height() > kMaxFloating.height()))
            {
                const QSize size = fittedSize(panel);
                existing->setGeometry(QRect(onScreen(at.value_or(QCursor::pos()) - kCursorInDock, size), size));
            }
            return existing;
        }

    QDockWidget* dock = makeDock(window, menu, name, section);
    auto* panel = static_cast<TearOffPanel*>(dock->widget());
    dock->setFloating(true);
    const QSize size = fittedSize(panel);
    dock->setGeometry(QRect(onScreen(at.value_or(QCursor::pos()) - kCursorInDock, size), size));
    dock->show();
    dock->raise();
    return dock;
}

QDockWidget* tearOffToPlace(QMenu* menu, const QString& section)
{
    if (!menu)
        return nullptr;
    QMainWindow* window = windowOf(menu);
    QDockWidget* existing = window ? window->findChild<QDockWidget*>(dockNameFor(menu, section)) : nullptr;
    QDockWidget* dock = tearOff(menu, QCursor::pos(), section);
    if (dock && dock->isFloating() && !dock->property(kPlacingProperty).toBool())
        new Placer(dock, !existing);
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

// ------------------------------------------------------------------- dragging Create rows

bool draggable(const std::string& actionId)
{
    const Action* a = actions::find(actionId);
    return a && a->category == "Create";
}

std::vector<std::string> runAt(const std::string& actionId, const glm::dvec3& at)
{
    scene::Graph& g = scene::Graph::current();
    const std::vector<scene::NodeId> before = g.nodes();
    UndoChunk chunk("Create " + actions::displayLabel(actionId));
    {
        actions::ViaScope via(actions::Via::Menu);
        actions::run(actionId);
    }
    const std::vector<scene::NodeId> after = g.nodes();
    std::vector<scene::NodeId> created;
    std::ranges::set_difference(after, before, std::back_inserter(created));
    std::vector<scene::NodeId> roots;
    for (scene::NodeId id : created)
        if (scene::Node* n = g.node(id); n && n->type().isA("transform") && !std::ranges::binary_search(created, g.parent(id)))
            roots.push_back(id);
    if (roots.empty())
        return {};
    const glm::dvec3 by = at - scene::xformops::worldPivot(g, roots.front());
    std::vector<std::string> names;
    for (scene::NodeId id : roots)
        names.push_back(g.node(id)->path());
    try
    {
        ops::execute("move", {{"args", names}, {"by", {by.x, by.y, by.z}}}, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
    return names;
}

void registerDropHandler()
{
    viewportHooks::setDropHandler("tearOffCreate", [](const QMimeData& mime, const viewportHooks::DropTarget& t, bool commit) {
        if (t.left || !mime.hasFormat(kActionMime))
            return false;
        if (!commit)
            return true;
        glm::dvec3 at;
        if (t.face != UINT32_MAX)
            at = t.position;
        else
        {
            glm::dvec3 o, d;
            t.view.ray(t.px, o, d);
            // With the ground behind the camera or edge on, the point on the cursor's ray nearest the origin.
            const double ground = std::abs(d.y) < 1e-9 ? -1.0 : -o.y / d.y;
            at = o + d * (ground > 0.0 ? ground : std::max(glm::dot(-o, d), 0.0));
        }
        const std::string id = mime.data(kActionMime).toStdString();
        // After the drag's event loop unwinds: the action may open a dialog or rebuild the row that started the drag.
        QTimer::singleShot(0, qApp, [id, at] { runAt(id, at); });
        return true;
    });
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
    auto* button = qobject_cast<QPushButton*>(obj);
    if (!button || !button->property("dragActionId").isValid())
        return QWidget::eventFilter(obj, e);
    if (e->type() == QEvent::MouseButtonPress)
        mPressPos = static_cast<QMouseEvent*>(e)->position().toPoint();
    else if (e->type() == QEvent::MouseMove)
    {
        auto* m = static_cast<QMouseEvent*>(e);
        if ((m->buttons() & Qt::LeftButton) && (m->position().toPoint() - mPressPos).manhattanLength() >= QApplication::startDragDistance())
        {
            auto* mime = new QMimeData;
            mime->setData(kActionMime, button->property("dragActionId").toString().toUtf8());
            auto* drag = new QDrag(button);
            drag->setMimeData(mime);
            drag->setPixmap(button->grab());
            drag->setHotSpot(mPressPos);
            button->setDown(false);
            drag->exec(Qt::CopyAction);
            return true;
        }
    }
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
        if (!sub && draggable(id))
        {
            button->setProperty("dragActionId", QString::fromStdString(id));
            button->installEventFilter(this);
        }
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
