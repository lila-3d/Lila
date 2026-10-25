#include "ui/DockDragCancel.h"

#include <QApplication>
#include <QDockWidget>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QPointer>
#include <QTimer>

#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#pragma comment(lib, "user32.lib")
#endif

namespace lila::ui::dockDragCancel
{
namespace
{
// Somewhere no window or dock area can be, to move the drag to so Qt drops its pending dock.
constexpr int kFar = 30000;

bool isClass(const QObject* o, const char* name)
{
    return o && qstrcmp(o->metaObject()->className(), name) == 0;
}

bool escapeHeld()
{
#ifdef _WIN32
    return GetAsyncKeyState(VK_ESCAPE) & 0x8000;
#else
    return false;
#endif
}

/**
 * Qt drags a panel in one of two ways: by its title bar, with the dock grabbing the mouse, or out of a tab bar, where
 * the tab bar keeps getting the moves and positions the floating dock. Either way the drop is only planned while the
 * mouse moves, so cancelling clears the plan once and then moves the window itself, keeping Qt from planning again.
 */
class DragCancel : public QObject
{
public:
    explicit DragCancel(QObject* parent) : QObject(parent)
    {
        mPoll.setInterval(30);
        connect(&mPoll, &QTimer::timeout, this, &DragCancel::poll);
    }

protected:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        switch (e->type())
        {
        case QEvent::MouseButtonPress:
            if (static_cast<QMouseEvent*>(e)->button() == Qt::LeftButton)
                mTabPress = isClass(obj, "QMainWindowTabBar") ? static_cast<QWidget*>(obj) : nullptr;
            return false;
        case QEvent::MouseMove:
            if (mCancelled)
            {
                if (obj != mSource)
                    return false;
                if (mWindow)
                    mWindow->move(static_cast<QMouseEvent*>(e)->globalPosition().toPoint() + mOffset);
                return true;
            }
            if (!mPoll.isActive() && draggedDock())
                mPoll.start();
            return false;
        case QEvent::Move:
            // Moving the window would plan a drop again for a tab bar drag; the far move is let through.
            return mCancelled && ownsWindow(obj) && static_cast<QMoveEvent*>(e)->pos().x() > -kFar / 2;
        case QEvent::ShortcutOverride:
        case QEvent::KeyPress:
        {
            if (static_cast<QKeyEvent*>(e)->key() != Qt::Key_Escape || (!mCancelled && !draggedDock()))
                return false;
            if (e->type() == QEvent::KeyPress)
                cancel();
            return true;
        }
        case QEvent::MouseButtonRelease:
            if (static_cast<QMouseEvent*>(e)->button() != Qt::LeftButton)
                return false;
            mPoll.stop();
            mTabPress = nullptr;
            if (mCancelled)
                QTimer::singleShot(0, this, &DragCancel::finish);
            return false;
        default: return false;
        }
    }

private:
    QDockWidget* draggedDock() const
    {
        if (!(QGuiApplication::mouseButtons() & Qt::LeftButton))
            return nullptr;
        if (auto* dock = qobject_cast<QDockWidget*>(QWidget::mouseGrabber()))
            return dock;
        if (mTabPress)
            return qobject_cast<QDockWidget*>(QApplication::topLevelAt(QCursor::pos()));
        return nullptr;
    }

    bool ownsWindow(QObject* obj) const
    {
        auto* w = qobject_cast<QWidget*>(obj);
        return w && mWindow && (w == mWindow || (qobject_cast<QDockWidget*>(w) && w->window() == mWindow));
    }

    void cancel()
    {
        if (mCancelled)
            return;
        QDockWidget* dock = draggedDock();
        if (!dock)
            return;
        bool grabbed = QWidget::mouseGrabber() == dock;
        mSource = grabbed ? static_cast<QWidget*>(dock) : mTabPress.data();
        mWindow = dock->window();
        if (!mSource || !mWindow)
            return;

        QList<QDockWidget*> docks{dock};
        if (mWindow != dock)
            docks = mWindow->findChildren<QDockWidget*>(Qt::FindDirectChildrenOnly);
        for (QDockWidget* d : docks)
        {
            mAreas.emplace_back(d, d->allowedAreas());
            d->setAllowedAreas(Qt::NoDockWidgetArea);
        }

        QPoint keep = mWindow->pos();
        mOffset = keep - QCursor::pos();
        QPointF away(-kFar, -kFar);
        QMouseEvent move(QEvent::MouseMove, mSource->mapFromGlobal(away), away, Qt::NoButton, Qt::LeftButton,
                         QGuiApplication::keyboardModifiers());
        QCoreApplication::sendEvent(mSource, &move);
        mCancelled = true;
        if (mWindow)
            mWindow->move(keep);
    }

    void finish()
    {
        for (auto& [dock, areas] : mAreas)
            if (dock)
                dock->setAllowedAreas(areas);
        mAreas.clear();
        mCancelled = false;
        mSource = nullptr;
        mWindow = nullptr;
    }

    void poll()
    {
        if (!(QGuiApplication::mouseButtons() & Qt::LeftButton))
        {
            mPoll.stop();
            if (mCancelled)
                finish();
            return;
        }
        // The key event never arrives when another app has the keyboard focus during the drag.
        if (!mCancelled && escapeHeld())
            cancel();
    }

    QTimer mPoll;
    QPointer<QWidget> mTabPress;
    QPointer<QWidget> mSource;
    QPointer<QWidget> mWindow;
    QPoint mOffset;
    std::vector<std::pair<QPointer<QDockWidget>, Qt::DockWidgetAreas>> mAreas;
    bool mCancelled = false;
};
} // namespace

void install()
{
    static QPointer<DragCancel> filter;
    if (!filter)
    {
        filter = new DragCancel(qApp);
        qApp->installEventFilter(filter);
    }
}
} // namespace lila::ui::dockDragCancel
