#include "ui/DockSidebar.h"

#include "ui/Icons.h"

#include <QAction>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QPointer>
#include <QStyleOptionToolButton>
#include <QTabBar>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>

#include <algorithm>

namespace lila::ui
{
namespace
{
constexpr int kTitleHeight = 22;

QToolButton* titleButton(const QString& iconName, const QString& tip, QWidget* parent)
{
    auto* b = new QToolButton(parent);
    b->setIcon(icon(iconName));
    b->setIconSize(QSize(14, 14));
    b->setToolTip(tip);
    b->setAutoRaise(true);
    b->setFixedSize(18, 18);
    b->setFocusPolicy(Qt::NoFocus);
    return b;
}

/** A tab on a sidebar strip; draws its label rotated on the left and right strips. */
class SideTab : public QToolButton
{
public:
    SideTab(const QString& text, Qt::Orientation orientation, QWidget* parent) : QToolButton(parent), mOrientation(orientation)
    {
        setText(text);
        setAutoRaise(true);
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
    }

    QSize sizeHint() const override
    {
        QSize s = fontMetrics().size(Qt::TextSingleLine, text()) + QSize(16, 8);
        return mOrientation == Qt::Vertical ? s.transposed() : s;
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        QStyleOptionToolButton opt;
        initStyleOption(&opt);
        opt.text.clear();
        style()->drawComplexControl(QStyle::CC_ToolButton, &opt, &p, this);
        p.setPen(palette().color(QPalette::ButtonText));
        if (mOrientation == Qt::Vertical)
        {
            p.translate(0, height());
            p.rotate(-90);
            p.drawText(QRect(0, 0, height(), width()), Qt::AlignCenter, text());
        }
        else
            p.drawText(rect(), Qt::AlignCenter, text());
    }

private:
    Qt::Orientation mOrientation;
};
const std::map<Qt::DockWidgetArea, std::pair<Qt::ToolBarArea, const char*>>& stripAreas()
{
    static const std::map<Qt::DockWidgetArea, std::pair<Qt::ToolBarArea, const char*>> areas = {
        {Qt::LeftDockWidgetArea, {Qt::LeftToolBarArea, "sidebarLeft"}},
        {Qt::RightDockWidgetArea, {Qt::RightToolBarArea, "sidebarRight"}},
        {Qt::TopDockWidgetArea, {Qt::TopToolBarArea, "sidebarTop"}},
        {Qt::BottomDockWidgetArea, {Qt::BottomToolBarArea, "sidebarBottom"}},
    };
    return areas;
}
} // namespace

void arrangeBottomDocks(QMainWindow* window, const QList<QDockWidget*>& tabs, QDockWidget* current, QDockWidget* row)
{
    bool rowHidden = row->testAttribute(Qt::WA_WState_ExplicitShowHide) && row->isHidden();
    // Adding or splitting a dock that is already laid out leaves its old item behind as well; tabifying moves it.
    window->removeDockWidget(row);
    if (window->dockWidgetArea(tabs.front()) != Qt::BottomDockWidgetArea)
        window->addDockWidget(Qt::BottomDockWidgetArea, tabs.front());
    for (qsizetype i = 1; i < tabs.size(); ++i)
        window->tabifyDockWidget(tabs.front(), tabs[i]);
    window->addDockWidget(Qt::BottomDockWidgetArea, row, Qt::Vertical);
    row->setVisible(!rowHidden);
    window->setCorner(Qt::BottomLeftCorner, Qt::BottomDockWidgetArea);
    window->setCorner(Qt::BottomRightCorner, Qt::BottomDockWidgetArea);
    current->raise();
}

DockTitleBar::DockTitleBar(QDockWidget* dock) : QWidget(dock), mDock(dock)
{
    auto* h = new QHBoxLayout(this);
    h->setContentsMargins(6, 2, 2, 2);
    h->setSpacing(0);
    h->addStretch(1);
    QToolButton* collapse = titleButton("minus", "Collapse to the side", this);
    QToolButton* floatBtn = titleButton("picture-in-picture", "Float", this);
    QToolButton* close = titleButton("x", "Close", this);
    h->addWidget(collapse);
    h->addWidget(floatBtn);
    h->addWidget(close);
    connect(collapse, &QToolButton::clicked, this, &DockTitleBar::collapseRequested);
    connect(floatBtn, &QToolButton::clicked, dock, [dock] { dock->setFloating(!dock->isFloating()); });
    connect(close, &QToolButton::clicked, dock, &QDockWidget::close);
    connect(dock, &QDockWidget::topLevelChanged, collapse, [collapse](bool floating) { collapse->setVisible(!floating); });
    connect(dock, &QDockWidget::windowTitleChanged, this, [this] { update(); });
}

QSize DockTitleBar::sizeHint() const
{
    return {80, kTitleHeight};
}

QSize DockTitleBar::minimumSizeHint() const
{
    return {60, kTitleHeight};
}

void DockTitleBar::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(58, 58, 58));
    p.setPen(palette().color(QPalette::WindowText));
    p.drawText(rect().adjusted(6, 0, -60, 0), Qt::AlignVCenter | Qt::AlignLeft,
               fontMetrics().elidedText(mDock->windowTitle(), Qt::ElideRight, width() - 66));
}

void DockTitleBar::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton)
        mDock->setFloating(!mDock->isFloating());
}

DockSidebars::DockSidebars(QMainWindow* window) : QObject(window), mWindow(window)
{
    // QMainWindow makes the tab bars of tabified docks on demand; each is hooked when it is first polished.
    window->installEventFilter(this);
}

void DockSidebars::hookTabBars()
{
    for (QTabBar* bar : mWindow->findChildren<QTabBar*>(Qt::FindDirectChildrenOnly))
        if (!bar->property("lilaDockTabs").toBool())
        {
            bar->setProperty("lilaDockTabs", true);
            bar->installEventFilter(this);
        }
}

QDockWidget* DockSidebars::dockForTab(QTabBar* bar, int index) const
{
    if (index < 0 || index >= bar->count())
        return nullptr;
    // QMainWindow keeps the dock's address in the tab data.
    auto id = bar->tabData(index).value<quintptr>();
    QDockWidget* byTitle = nullptr;
    for (QDockWidget* d : mWindow->findChildren<QDockWidget*>())
    {
        if (quintptr(d) == id)
            return d;
        if (!byTitle && d->windowTitle() == bar->tabText(index) && !d->isFloating())
            byTitle = d;
    }
    return byTitle;
}

void DockSidebars::rememberSize(QDockWidget* dock)
{
    if (!dock || dock->isFloating() || !dock->isVisible())
        return;
    Qt::DockWidgetArea area = mWindow->dockWidgetArea(dock);
    if (area == Qt::NoDockWidgetArea)
        return;
    bool sideways = area == Qt::LeftDockWidgetArea || area == Qt::RightDockWidgetArea;
    mTabSizes[dock->objectName()] = sideways ? dock->width() : dock->height();
}

void DockSidebars::restoreSize(QDockWidget* dock)
{
    if (!dock || dock->isFloating() || !dock->isVisible())
        return;
    Qt::DockWidgetArea area = mWindow->dockWidgetArea(dock);
    if (area == Qt::NoDockWidgetArea)
        return;
    int want = 0;
    if (auto it = mTabSizes.find(dock->objectName()); it != mTabSizes.end())
        want = it->second;
    else if (QVariant extent = dock->property(kDefaultTabExtent); extent.isValid())
        want = extent.toInt();
    if (want <= 0)
        return;
    bool sideways = area == Qt::LeftDockWidgetArea || area == Qt::RightDockWidgetArea;
    mWindow->resizeDocks({dock}, {want}, sideways ? Qt::Horizontal : Qt::Vertical);
    emit changed();
}

bool DockSidebars::eventFilter(QObject* obj, QEvent* e)
{
    if (obj == mWindow)
    {
        if (e->type() == QEvent::ChildPolished)
            hookTabBars();
        // A state restored before the first show is applied lazily by the layout, dropping the strips it doesn't name.
        else if (e->type() == QEvent::Show || e->type() == QEvent::Resize || e->type() == QEvent::LayoutRequest)
            for (auto& [area, bar] : mStrips)
                if (mWindow->toolBarArea(bar) != stripAreas().at(area).first)
                {
                    QTimer::singleShot(0, this, &DockSidebars::refreshStrips);
                    break;
                }
        return false;
    }
    bool isMouse = e->type() == QEvent::MouseButtonPress || e->type() == QEvent::MouseButtonRelease || e->type() == QEvent::MouseButtonDblClick;
    auto* mouse = isMouse ? static_cast<QMouseEvent*>(e) : nullptr;
    if (!mouse)
        return false;
    if (auto* bar = qobject_cast<QTabBar*>(obj))
    {
        int index = bar->tabAt(mouse->position().toPoint());
        if (mouse->button() == Qt::MiddleButton)
        {
            if (e->type() == QEvent::MouseButtonRelease)
                if (QDockWidget* d = dockForTab(bar, index))
                    close(d);
            return index >= 0;
        }
        if (mouse->button() == Qt::LeftButton && e->type() == QEvent::MouseButtonDblClick)
        {
            // The first click of a double-click on another tab only switches to it; the tab must already have been showing.
            if (!mPressOnCurrentTab || index < 0 || index != bar->currentIndex())
                return false;
            QPointer<QDockWidget> d = dockForTab(bar, index);
            QTimer::singleShot(0, this, [this, d] {
                if (d)
                    collapse(d);
            });
            return true;
        }
        if (mouse->button() == Qt::LeftButton && e->type() == QEvent::MouseButtonPress)
            mPressOnCurrentTab = index >= 0 && index == bar->currentIndex();
        if (mouse->button() == Qt::LeftButton && e->type() == QEvent::MouseButtonPress && index >= 0 && index != bar->currentIndex())
        {
            rememberSize(dockForTab(bar, bar->currentIndex()));
            QPointer<QDockWidget> next = dockForTab(bar, index);
            QTimer::singleShot(0, this, [this, next] { restoreSize(next); });
        }
        return false;
    }
    if (mouse->button() == Qt::MiddleButton && e->type() != QEvent::MouseButtonDblClick)
        for (auto& [dock, tab] : mTabs)
            if (tab == obj)
            {
                if (e->type() == QEvent::MouseButtonRelease)
                    close(dock);
                return true;
            }
    return false;
}

void DockSidebars::close(QDockWidget* dock)
{
    if (auto it = mTabs.find(dock); it != mTabs.end())
    {
        it->second->deleteLater();
        mTabs.erase(it);
        mSizes.erase(dock);
        refreshStrips();
    }
    rememberSize(dock);
    dock->close();
    emit changed();
}

void DockSidebars::manage(QDockWidget* dock)
{
    auto* bar = new DockTitleBar(dock);
    dock->setTitleBarWidget(bar);
    connect(bar, &DockTitleBar::collapseRequested, this, [this, dock] { collapse(dock); });
    // Showing a collapsed panel from a menu or hotkey expands it.
    connect(dock, &QDockWidget::visibilityChanged, this, [this, dock](bool visible) {
        if (visible && isCollapsed(dock))
            expand(dock);
    });
    connect(dock->toggleViewAction(), &QAction::triggered, this, [this, dock](bool checked) {
        if (checked)
            reveal(dock);
    });
}

void DockSidebars::reveal(QDockWidget* dock)
{
    // As when its tab is clicked: the tab it covers keeps its size for later, and it gets back its own.
    bool tabbed = !dock->isFloating() && !mWindow->tabifiedDockWidgets(dock).isEmpty();
    if (tabbed && !dockShowing(dock))
        for (QDockWidget* other : mWindow->tabifiedDockWidgets(dock))
            if (dockShowing(other))
                rememberSize(other);
    if (isCollapsed(dock))
        expand(dock);
    else
    {
        dock->show();
        dock->raise();
    }
    // The dock layout settles after the show, so the tab, size and focus are fixed up once it has.
    QPointer<QDockWidget> d = dock;
    QTimer::singleShot(0, this, [this, d, tabbed] {
        if (!d || !d->isVisible())
            return;
        d->raise();
        if (d->isFloating())
            d->activateWindow();
        else
        {
            if (tabbed)
                restoreSize(d);
            ensureUsableSize(d);
        }
        if (QWidget* w = d->widget())
            w->setFocus(Qt::OtherFocusReason);
    });
}

void DockSidebars::ensureUsableSize(QDockWidget* dock)
{
    Qt::DockWidgetArea area = mWindow->dockWidgetArea(dock);
    // The top area holds slim strips like the shelf, which are meant to stay small.
    if (area == Qt::NoDockWidgetArea || area == Qt::TopDockWidgetArea)
        return;
    bool sideways = area == Qt::LeftDockWidgetArea || area == Qt::RightDockWidgetArea;
    QSizePolicy policy = dock->widget() ? dock->widget()->sizePolicy() : QSizePolicy();
    if (!((sideways ? policy.horizontalPolicy() : policy.verticalPolicy()) & QSizePolicy::GrowFlag))
        return;
    constexpr int kSqueezed = 200;
    int extent = sideways ? dock->width() : dock->height();
    int hint = sideways ? dock->sizeHint().width() : dock->sizeHint().height();
    if (extent >= kSqueezed)
        return;
    int want = std::max(hint, (sideways ? mWindow->width() : mWindow->height()) * 3 / 10);
    if (auto it = mTabSizes.find(dock->objectName()); it != mTabSizes.end() && it->second >= kSqueezed)
        want = it->second;
    mWindow->resizeDocks({dock}, {want}, sideways ? Qt::Horizontal : Qt::Vertical);
    emit changed();
}

void revealDock(QDockWidget* dock)
{
    if (!dock)
        return;
    auto* window = qobject_cast<QMainWindow*>(dock->parentWidget());
    if (auto* sidebars = window ? window->findChild<DockSidebars*>(Qt::FindDirectChildrenOnly) : nullptr)
    {
        sidebars->reveal(dock);
        return;
    }
    dock->show();
    dock->raise();
    if (QWidget* w = dock->widget())
        w->setFocus(Qt::OtherFocusReason);
}

bool dockShowing(const QDockWidget* dock)
{
    if (!dock || !dock->isVisible())
        return false;
    auto* window = qobject_cast<QMainWindow*>(dock->parentWidget());
    if (!window || dock->isFloating() || window->tabifiedDockWidgets(const_cast<QDockWidget*>(dock)).isEmpty())
        return true;
    // Asked of the tab bar rather than visibleRegion(), which comes back empty for a dock whose children are opaque.
    for (QTabBar* bar : window->findChildren<QTabBar*>(Qt::FindDirectChildrenOnly))
        for (int i = 0; i < bar->count(); ++i)
            if (bar->tabData(i).value<quintptr>() == quintptr(dock) || (bar->tabText(i) == dock->windowTitle() && bar->tabData(i).isNull()))
                return !bar->isVisible() || i == bar->currentIndex();
    return true;
}

void toggleDock(QDockWidget* dock)
{
    if (!dock)
        return;
    if (!dockShowing(dock))
    {
        revealDock(dock);
        return;
    }
    auto* window = qobject_cast<QMainWindow*>(dock->parentWidget());
    if (auto* sidebars = window && !dock->isFloating() ? window->findChild<DockSidebars*>(Qt::FindDirectChildrenOnly) : nullptr)
        sidebars->collapse(dock);
    else
        dock->hide();
}

QToolBar* DockSidebars::strip(Qt::DockWidgetArea area)
{
    auto it = mStrips.find(area);
    if (it != mStrips.end())
    {
        if (Qt::ToolBarArea toolArea = stripAreas().at(area).first; mWindow->toolBarArea(it->second) != toolArea)
            mWindow->addToolBar(toolArea, it->second);
        return it->second;
    }
    auto [toolArea, name] = stripAreas().at(area);
    auto* bar = new QToolBar(mWindow);
    bar->setObjectName(name);
    bar->setMovable(false);
    bar->setFloatable(false);
    bar->toggleViewAction()->setVisible(false);
    bar->setContextMenuPolicy(Qt::PreventContextMenu);
    bar->setStyleSheet("QToolBar { background: #2c2c2c; border: none; padding: 0; spacing: 2px; }");
    mWindow->addToolBar(toolArea, bar);
    bar->hide();
    mStrips[area] = bar;
    return bar;
}

void DockSidebars::collapse(QDockWidget* dock)
{
    if (dock->isFloating() || isCollapsed(dock))
        return;
    Qt::DockWidgetArea area = mWindow->dockWidgetArea(dock);
    if (area == Qt::NoDockWidgetArea)
        return;
    QToolBar* bar = strip(area);
    Qt::Orientation orientation = (area == Qt::LeftDockWidgetArea || area == Qt::RightDockWidgetArea) ? Qt::Vertical : Qt::Horizontal;
    bar->setOrientation(orientation);
    auto* tab = new SideTab(dock->windowTitle(), orientation, bar);
    tab->setToolTip("Expand " + dock->windowTitle());
    tab->installEventFilter(this);
    QAction* action = bar->addWidget(tab);
    connect(tab, &QToolButton::clicked, this, [this, dock] { expand(dock); });
    connect(tab, &QObject::destroyed, bar, [bar, action] { bar->removeAction(action); });
    mTabs[dock] = tab;
    mSizes[dock] = dock->size();
    // Freed space goes to the viewport, not to the panels left in the area.
    QList<QDockWidget*> neighbours;
    QList<int> sizes;
    for (QDockWidget* other : mWindow->findChildren<QDockWidget*>())
        if (other != dock && other->isVisible() && !other->isFloating() && mWindow->dockWidgetArea(other) == area)
        {
            neighbours << other;
            sizes << (orientation == Qt::Vertical ? other->width() : other->height());
        }
    dock->hide();
    if (!neighbours.isEmpty())
        mWindow->resizeDocks(neighbours, sizes, orientation == Qt::Vertical ? Qt::Horizontal : Qt::Vertical);
    refreshStrips();
    emit changed();
}

void DockSidebars::expand(QDockWidget* dock)
{
    auto it = mTabs.find(dock);
    if (it == mTabs.end())
        return;
    QWidget* tab = it->second;
    mTabs.erase(it);
    tab->deleteLater();
    dock->show();
    dock->raise();
    if (auto size = mSizes.find(dock); size != mSizes.end() && size->second.width() > 40 && size->second.height() > 40)
    {
        Qt::DockWidgetArea area = mWindow->dockWidgetArea(dock);
        bool sideways = area == Qt::LeftDockWidgetArea || area == Qt::RightDockWidgetArea;
        mWindow->resizeDocks({dock}, {sideways ? size->second.width() : size->second.height()}, sideways ? Qt::Horizontal : Qt::Vertical);
        mSizes.erase(size);
    }
    refreshStrips();
    emit changed();
}

bool DockSidebars::isCollapsed(const QDockWidget* dock) const
{
    return mTabs.count(const_cast<QDockWidget*>(dock)) > 0;
}

QStringList DockSidebars::collapsedNames() const
{
    QStringList names;
    for (auto& [dock, tab] : mTabs)
        names << dock->objectName();
    return names;
}

void DockSidebars::restoreCollapsed(const QStringList& names)
{
    for (const QString& name : names)
        // Collapsed straight from hidden: showing it first would hand its area the dock's old size to keep.
        if (auto* dock = mWindow->findChild<QDockWidget*>(name); dock && !dock->isFloating())
            collapse(dock);
    QTimer::singleShot(0, this, &DockSidebars::rescueStranded);
}

void DockSidebars::rescueStranded()
{
    QRect inside = mWindow->rect();
    for (QDockWidget* d : mWindow->findChildren<QDockWidget*>())
    {
        if (d->parentWidget() != mWindow || !d->isVisible() || d->isFloating() || inside.intersects(d->geometry()))
            continue;
        bool shownTab = false;
        for (QDockWidget* other : mWindow->tabifiedDockWidgets(d))
            shownTab |= other->isVisible() && inside.intersects(other->geometry());
        Qt::DockWidgetArea area = mWindow->dockWidgetArea(d);
        if (shownTab || area == Qt::NoDockWidgetArea)
            continue;
        // A restored state can leave a shown dock laid out off the window while it still holds space in its area.
        mWindow->removeDockWidget(d);
        mWindow->addDockWidget(area, d);
        d->show();
    }
}

void DockSidebars::expandAll()
{
    while (!mTabs.empty())
        expand(mTabs.begin()->first);
}

void DockSidebars::refreshStrips()
{
    for (auto& [area, bar] : mStrips)
    {
        // restoreState drops toolbars its state doesn't name, leaving them unplaced at the window's corner.
        if (Qt::ToolBarArea toolArea = stripAreas().at(area).first; mWindow->toolBarArea(bar) != toolArea)
            mWindow->addToolBar(toolArea, bar);
        bool any = false;
        for (auto& [dock, tab] : mTabs)
            any |= tab->parentWidget() == bar;
        bar->setVisible(any);
    }
}
} // namespace lila::ui
