#include "ui/DockSidebar.h"

#include "ui/Actions.h"
#include "ui/DocsUi.h"
#include "ui/Icons.h"
#include "ui/LocateFlash.h"
#include "ui/Panels.h"
#include "ui/Theme.h"

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPainter>
#include <QStatusBar>
#include <QStyle>
#include <QPointer>
#include <QStyleOptionToolButton>
#include <QTabBar>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>

#include <algorithm>
#include <climits>
#include <map>
#include <set>

namespace lila::ui
{
namespace
{
constexpr int kTitleHeight = 26;
/** How visible the title bar's buttons are until the pointer reaches the bar. */
constexpr qreal kDimButtons = 0.4;

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
        setCheckable(true);
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
/**
 * The extent resizeDocks has to be given to keep a shown dock the size it is: its slot, which for a dock in a tab group
 * or beside others in a column is larger than the dock. Recording the dock's own size instead loses the difference on
 * every hide and show. Measured by resizing to the dock's size and putting back what that took off.
 */
int slotExtent(QMainWindow* window, QDockWidget* dock, Qt::Orientation orientation)
{
    auto measure = [&] { return orientation == Qt::Horizontal ? dock->width() : dock->height(); };
    const int own = measure();
    if (!dock->isVisible() || own <= 0)
        return own;
    // Applied now: the dock's geometry only changes when the layout runs, and measuring before then sees no loss.
    window->resizeDocks({dock}, {own}, orientation);
    window->layout()->activate();
    const int lost = own - measure();
    if (lost != 0)
    {
        window->resizeDocks({dock}, {own + lost}, orientation);
        window->layout()->activate();
    }
    return own + lost;
}

const std::map<Qt::DockWidgetArea, std::pair<Qt::ToolBarArea, const char*>>& stripAreas()
{
    static const std::map<Qt::DockWidgetArea, std::pair<Qt::ToolBarArea, const char*>> areas = {
        {Qt::LeftDockWidgetArea, {Qt::LeftToolBarArea, "sidebarLeft"}},
        {Qt::RightDockWidgetArea, {Qt::RightToolBarArea, "sidebarRight"}},
        {Qt::TopDockWidgetArea, {Qt::TopToolBarArea, "sidebarTop"}},
        // Not "sidebarBottom": saved layouts name a toolbar that, and restoring one would pull the strip out of the status bar.
        {Qt::BottomDockWidgetArea, {Qt::BottomToolBarArea, "sidebarStatus"}},
    };
    return areas;
}

constexpr const char* kTabMinimum = "lilaTabMinimum";
constexpr QSize kRelaxedMinimum(1, 1);

/**
 * Qt sizes a tab group to the largest minimum of all its tabs' contents, shown or not. A hidden dock's content
 * drops its minimum so only the shown tab limits the group; showing it restores the old one, unless the panel set
 * a new minimum of its own while hidden.
 */
void relaxHiddenMinimum(QDockWidget* dock, bool visible)
{
    QWidget* content = dock->widget();
    if (!content)
        return;
    const QVariant saved = content->property(kTabMinimum);
    if (visible && saved.isValid())
    {
        if (content->minimumSize() == kRelaxedMinimum)
            content->setMinimumSize(saved.toSize());
        content->setProperty(kTabMinimum, QVariant());
    }
    else if (!visible && !saved.isValid())
    {
        content->setProperty(kTabMinimum, content->minimumSize());
        content->setMinimumSize(kRelaxedMinimum);
    }
}

/**
 * Qt passes focus out of a dock being hidden to the next widget in the chain, often the time slider's spin box, which
 * then takes every letter hotkey as typing. The viewport gets it instead.
 */
void releaseFocus(QMainWindow* window, QDockWidget* dock)
{
    if (QWidget* focus = QApplication::focusWidget(); focus && dock->isAncestorOf(focus))
        if (QWidget* central = window ? window->centralWidget() : nullptr)
            central->setFocus(Qt::OtherFocusReason);
}

/** A docked tab group of a side area with a panel open, and where it sits. */
struct DockGroup
{
    QList<QDockWidget*> docks;
    QDockWidget* current = nullptr;
    QRect rect;
    bool ownColumn = false;
};

bool keepsOwnColumn(const QDockWidget* d) { return d->property(kOwnColumn).toBool(); }

std::vector<DockGroup> openGroups(QMainWindow* window, Qt::DockWidgetArea area)
{
    std::vector<DockGroup> out;
    std::set<QDockWidget*> seen;
    for (QDockWidget* d : window->findChildren<QDockWidget*>(Qt::FindDirectChildrenOnly))
    {
        if (seen.count(d) || d->isFloating() || window->dockWidgetArea(d) != area)
            continue;
        DockGroup g;
        g.docks = {d};
        g.docks += window->tabifiedDockWidgets(d);
        // Background tabs count as visible but sit off the window, so the group's place is its showing tab's.
        for (QDockWidget* m : g.docks)
        {
            seen.insert(m);
            if (m->isVisible() && (!g.current || dockShowing(m)))
                g.current = m;
        }
        if (!g.current)
            continue;
        g.rect = g.current->geometry();
        g.ownColumn = std::all_of(g.docks.begin(), g.docks.end(), keepsOwnColumn);
        out.push_back(std::move(g));
    }
    return out;
}

/** The other groups split into the columns they stand in, left to right, each top to bottom. */
std::vector<std::vector<DockGroup>> columnsOf(std::vector<DockGroup> groups)
{
    std::sort(groups.begin(), groups.end(), [](const DockGroup& a, const DockGroup& b) { return a.rect.left() < b.rect.left(); });
    std::vector<std::vector<DockGroup>> columns;
    int right = INT_MIN;
    for (DockGroup& g : groups)
    {
        if (columns.empty() || g.rect.left() > right)
            columns.emplace_back();
        right = std::max(right, g.rect.right());
        columns.back().push_back(std::move(g));
    }
    for (auto& c : columns)
        std::sort(c.begin(), c.end(), [](const DockGroup& a, const DockGroup& b) { return a.rect.top() < b.rect.top(); });
    return columns;
}

/** Set while the side area is rebuilt and until its signals have run, so the rebuild doesn't start another. */
bool gArrangingColumns = false;

/** An own-column dock's width and its neighbour columns' when it closed, for it to open at again. */
struct ClosedColumns
{
    int own = 0;
    std::vector<QPointer<QDockWidget>> heads;
    QList<int> widths;
};
std::map<const QDockWidget*, ClosedColumns> gClosedColumns;

/** Reopened where it closed, Qt splits the area's current width; this gives every column back the width it had. */
void restoreClosedWidths(QMainWindow* window, QDockWidget* dock)
{
    auto it = gClosedColumns.find(dock);
    if (it == gClosedColumns.end())
        return;
    ClosedColumns closed = std::move(it->second);
    gClosedColumns.erase(it);
    QList<QDockWidget*> docks{dock};
    QList<int> widths{std::max(closed.own, dock->property(kDefaultTabExtent).toInt())};
    for (size_t i = 0; i < closed.heads.size(); ++i)
        if (QDockWidget* h = closed.heads[i]; h && h->isVisible() && !h->isFloating() && window->dockWidgetArea(h) == window->dockWidgetArea(dock))
        {
            docks.push_back(h);
            widths.push_back(closed.widths[qsizetype(i)]);
        }
    window->resizeDocks(docks, widths, Qt::Horizontal);
}

/**
 * Gives an own-column dock a full-height column on the viewport side of its area, tabbed with any other own-column
 * dock there. Qt's side areas stack their root vertically, so a panel dropped or opened there otherwise lands above or
 * below the panels it should stand beside.
 */
void arrangeOwnColumn(QDockWidget* dock)
{
    auto* window = qobject_cast<QMainWindow*>(dock->parentWidget());
    if (gArrangingColumns || !window || !window->isVisible() || dock->isFloating() || !dock->isVisible())
        return;
    const Qt::DockWidgetArea area = window->dockWidgetArea(dock);
    if (area != Qt::LeftDockWidgetArea && area != Qt::RightDockWidgetArea)
        return;
    // Tabbed in with ordinary panels on purpose: leave it in their column.
    const QList<QDockWidget*> tabs = window->tabifiedDockWidgets(dock);
    if (!std::all_of(tabs.begin(), tabs.end(), keepsOwnColumn))
        return;
    window->layout()->activate();
    const bool right = area == Qt::RightDockWidgetArea;
    std::vector<DockGroup> own, others;
    for (DockGroup& g : openGroups(window, area))
        (g.ownColumn ? own : others).push_back(std::move(g));
    auto mine = std::find_if(own.begin(), own.end(), [dock](const DockGroup& g) { return g.docks.contains(dock); });
    if (mine == own.end())
        return;
    const QRect at = mine->rect;
    bool placed = own.size() == 1;
    for (const DockGroup& g : others)
        if ((g.rect.left() <= at.right() && at.left() <= g.rect.right()) || (right ? g.rect.left() < at.left() : g.rect.right() > at.right()))
            placed = false;
    if (placed)
    {
        restoreClosedWidths(window, dock);
        return;
    }
    gClosedColumns.erase(dock);

    gArrangingColumns = true;
    DockGroup column{.docks = {}, .current = dock, .rect = at, .ownColumn = true};
    for (const DockGroup& g : own)
        column.docks += g.docks;
    column.docks.removeAll(dock);
    column.docks.prepend(dock);
    std::vector<std::vector<DockGroup>> columns = columnsOf(std::move(others));
    // The other columns keep their widths; the own column takes the rest of the area, and at least its default.
    int areaLeft = at.left(), areaRight = at.right(), othersWidth = 0;
    for (const auto& c : columns)
    {
        int width = 0;
        for (const DockGroup& g : c)
        {
            areaLeft = std::min(areaLeft, g.rect.left());
            areaRight = std::max(areaRight, g.rect.right());
            width = std::max(width, g.rect.width());
        }
        othersWidth += width;
    }
    const int handles = int(columns.size()) * window->style()->pixelMetric(QStyle::PM_DockWidgetSeparatorExtent, nullptr, window);
    column.rect.setWidth(std::max(areaRight - areaLeft + 1 - othersWidth - handles, dock->property(kDefaultTabExtent).toInt()));
    columns.insert(right ? columns.begin() : columns.end(), std::vector<DockGroup>{column});

    // Qt applies a layout restored for a maximized window lazily, keeping a copy of it that shares the layout's items;
    // splits and tabs apply it first, removals don't, so a removal would leave the copy pointing at deleted items. A
    // split of the dock against itself applies it and takes the dock out (it isn't found to split against).
    window->splitDockWidget(dock, dock, Qt::Horizontal);
    // Shown again straight away: Qt places hidden docks unreliably, and the moves below carry shown ones. The layout
    // stays off until every dock is back: showing one runs it, and half rebuilt it still holds the removed docks' items.
    QLayout* layout = window->layout();
    layout->setEnabled(false);
    std::map<QDockWidget*, bool> shown;
    for (const auto& c : columns)
        for (const DockGroup& g : c)
            for (QDockWidget* d : g.docks)
            {
                shown[d] = !d->isHidden();
                window->removeDockWidget(d);
                d->show();
            }
    QDockWidget* previous = nullptr;
    for (const auto& c : columns)
    {
        QDockWidget* top = nullptr;
        QDockWidget* above = nullptr;
        for (const DockGroup& g : c)
        {
            QDockWidget* head = g.docks.front();
            if (!top && !previous)
                window->addDockWidget(area, head, Qt::Horizontal);
            else if (!top)
                window->splitDockWidget(previous, head, Qt::Horizontal);
            else
                window->splitDockWidget(above, head, Qt::Vertical);
            top = top ? top : head;
            above = head;
        }
        previous = top;
    }
    // Tabs join after every split: splitting from a dock already in a tab group nests the new one inside the group.
    for (const auto& c : columns)
        for (const DockGroup& g : c)
            for (QDockWidget* t : g.docks.mid(1))
                window->tabifyDockWidget(g.docks.front(), t);
    for (auto& [d, on] : shown)
        d->setVisible(on);
    layout->setEnabled(true);
    layout->activate();

    struct Sizes
    {
        std::vector<QPointer<QDockWidget>> docks;
        QList<int> sizes;
    };
    Sizes widths;
    std::vector<Sizes> heights;
    std::vector<QPointer<QDockWidget>> currents;
    for (const auto& c : columns)
    {
        Sizes& stack = heights.emplace_back();
        int width = 0;
        for (const DockGroup& g : c)
        {
            stack.docks.emplace_back(g.docks.front());
            stack.sizes.push_back(g.rect.height());
            width = std::max(width, g.rect.width());
            currents.emplace_back(g.current);
        }
        widths.docks.push_back(stack.docks.front());
        widths.sizes.push_back(width);
    }
    // Sized and raised once the dock layout has updated: a dock just re-added or re-tabbed has no item or tab before.
    QTimer::singleShot(0, window, [window, currents, widths, heights] {
        auto resize = [window](const Sizes& s, Qt::Orientation o) {
            QList<QDockWidget*> docks;
            for (const QPointer<QDockWidget>& d : s.docks)
            {
                if (!d)
                    return;
                docks.push_back(d);
            }
            if (docks.size() > 1)
                window->resizeDocks(docks, s.sizes, o);
        };
        for (const QPointer<QDockWidget>& d : currents)
            if (d)
                d->raise();
        for (const Sizes& s : heights)
            resize(s, Qt::Vertical);
        resize(widths, Qt::Horizontal);
        gArrangingColumns = false;
    });
}
/**
 * An own-column dock closing hands its width back to the viewport rather than to the columns beside it. Called as it
 * hides, while the other columns still have their widths.
 */
void keepColumnWidths(QDockWidget* dock)
{
    auto* window = qobject_cast<QMainWindow*>(dock->parentWidget());
    if (gArrangingColumns || !window || !window->isVisible() || dock->isFloating())
        return;
    const Qt::DockWidgetArea area = window->dockWidgetArea(dock);
    if (area != Qt::LeftDockWidgetArea && area != Qt::RightDockWidgetArea)
        return;
    std::vector<DockGroup> others;
    for (DockGroup& g : openGroups(window, area))
        if (!g.ownColumn)
            others.push_back(std::move(g));
    std::vector<QPointer<QDockWidget>> heads;
    QList<int> widths;
    for (const auto& c : columnsOf(std::move(others)))
    {
        int width = 0;
        for (const DockGroup& g : c)
            width = std::max(width, g.rect.width());
        heads.emplace_back(c.front().docks.front());
        widths.push_back(width);
    }
    if (heads.empty())
        return;
    const int own = dock->width();
    QTimer::singleShot(0, window, [window, area, own, d = QPointer<QDockWidget>(dock), heads, widths] {
        // Still open behind another tab, or another own-column dock still holds the column.
        if (!d || !d->isHidden())
            return;
        for (QDockWidget* o : window->findChildren<QDockWidget*>(Qt::FindDirectChildrenOnly))
            if (o != d && keepsOwnColumn(o) && o->isVisible() && !o->isFloating() && window->dockWidgetArea(o) == area)
                return;
        QList<QDockWidget*> docks;
        for (const QPointer<QDockWidget>& h : heads)
        {
            if (!h)
                return;
            docks.push_back(h);
        }
        window->resizeDocks(docks, widths, Qt::Horizontal);
        gClosedColumns[d.data()] = {own, heads, widths};
    });
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

constexpr const char* kDocsPage = "lilaDocsPage";

void setDockDocs(QDockWidget* dock, const std::string& page)
{
    dock->setProperty(kDocsPage, QString::fromStdString(page));
    if (auto* bar = qobject_cast<DockTitleBar*>(dock->titleBarWidget()))
        bar->setDocsPage(page);
}

DockTitleBar::DockTitleBar(QDockWidget* dock) : QWidget(dock), mDock(dock)
{
    auto* h = mButtons = new QHBoxLayout(this);
    h->setContentsMargins(8, 4, 4, 4);
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
    setDocsPage(dock->property(kDocsPage).toString().toStdString());
}

void DockTitleBar::lightButtons(bool lit)
{
    for (int i = 0; i < mButtons->count(); ++i)
        if (QWidget* w = mButtons->itemAt(i)->widget())
        {
            auto* effect = qobject_cast<QGraphicsOpacityEffect*>(w->graphicsEffect());
            if (!effect)
                w->setGraphicsEffect(effect = new QGraphicsOpacityEffect(w));
            effect->setOpacity(lit ? 1.0 : kDimButtons);
        }
}

void DockTitleBar::enterEvent(QEnterEvent* e)
{
    QWidget::enterEvent(e);
    lightButtons(true);
}

void DockTitleBar::leaveEvent(QEvent* e)
{
    QWidget::leaveEvent(e);
    lightButtons(false);
}

void DockTitleBar::setDocsPage(const std::string& page)
{
    delete mHelp;
    mHelp = nullptr;
    if (!page.empty())
    {
        mHelp = docs::helpButton(page, this);
        mButtons->insertWidget(1, mHelp);
    }
    lightButtons(underMouse());
    update();
}

int DockTitleBar::buttonsLeft() const
{
    int left = width();
    for (int i = 0; i < mButtons->count(); ++i)
        if (QWidget* w = mButtons->itemAt(i)->widget(); w && !w->isHidden())
            left = std::min(left, w->x());
    return left;
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
    p.fillRect(rect(), themeShade(58));
    // A hairline under the header, so it reads as the panel's top edge rather than a row of its content.
    p.fillRect(QRect(0, height() - 1, width(), 1), themeColor("separator"));
    QFont title = font();
    title.setWeight(QFont::DemiBold);
    p.setFont(title);
    p.setPen(themeColor("text"));
    QRect text = rect().adjusted(8, 0, 0, -1);
    text.setRight(buttonsLeft() - 4);
    p.drawText(text, Qt::AlignVCenter | Qt::AlignLeft, p.fontMetrics().elidedText(mDock->windowTitle(), Qt::ElideRight, text.width()));
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
    mTabSizes[dock->objectName()] = slotExtent(mWindow, dock, sideways ? Qt::Horizontal : Qt::Vertical);
}

void DockSidebars::rememberShownSizes()
{
    for (QDockWidget* d : mWindow->findChildren<QDockWidget*>())
        if (d->parentWidget() == mWindow && dockShowing(d))
            rememberSize(d);
}

void DockSidebars::restoreSize(QDockWidget* dock)
{
    if (!dock || dock->isFloating() || !dock->isVisible())
        return;
    Qt::DockWidgetArea area = mWindow->dockWidgetArea(dock);
    if (area == Qt::NoDockWidgetArea)
        return;
    int want = 0;
    if (dock->property(kHugTabExtent).toBool())
        want = 1;
    else if (auto it = mTabSizes.find(dock->objectName()); it != mTabSizes.end())
        want = it->second;
    else if (QVariant extent = dock->property(kDefaultTabExtent); extent.isValid())
        want = extent.toInt();
    if (want <= 0)
        return;
    bool sideways = area == Qt::LeftDockWidgetArea || area == Qt::RightDockWidgetArea;
    mWindow->resizeDocks({dock}, {want}, sideways ? Qt::Horizontal : Qt::Vertical);
    Q_EMIT changed();
}

bool DockSidebars::eventFilter(QObject* obj, QEvent* e)
{
    if (obj == mWindow)
    {
        if (e->type() == QEvent::ChildPolished)
            hookTabBars();
        else if (e->type() == QEvent::MouseButtonPress && static_cast<QMouseEvent*>(e)->button() == Qt::LeftButton)
            beginEdgeDrag(static_cast<QMouseEvent*>(e)->position().toPoint());
        else if (e->type() == QEvent::MouseMove && !mEdgeDrags.empty())
            QTimer::singleShot(0, this, &DockSidebars::keepOuterColumns);
        else if (e->type() == QEvent::MouseButtonRelease && !mEdgeDrags.empty())
            QTimer::singleShot(0, this, [this] {
                keepOuterColumns();
                mEdgeDrags.clear();
            });
        // A state restored before the first show is applied lazily by the layout, dropping the strips it doesn't name.
        else if (e->type() == QEvent::Show || e->type() == QEvent::Resize || e->type() == QEvent::LayoutRequest)
            for (auto& [area, bar] : mStrips)
                if (area != Qt::BottomDockWidgetArea && mWindow->toolBarArea(bar) != stripAreas().at(area).first)
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
            bool swallow = mCollapsedByClick;
            mCollapsedByClick = false;
            return swallow;
        }
        // Clicking the tab already showing collapses it; a drag past the threshold is a move or undock instead.
        if (mouse->button() == Qt::LeftButton && e->type() == QEvent::MouseButtonRelease)
        {
            bool click = (mouse->position().toPoint() - mTabPressPos).manhattanLength() < QApplication::startDragDistance();
            if (mPressOnCurrentTab && click && index >= 0 && index == bar->currentIndex())
            {
                mPressOnCurrentTab = false;
                mCollapsedByClick = true;
                QPointer<QDockWidget> d = dockForTab(bar, index);
                QTimer::singleShot(0, this, [this, d] {
                    if (d)
                        collapse(d);
                });
            }
            return false;
        }
        if (mouse->button() == Qt::LeftButton && e->type() == QEvent::MouseButtonPress)
        {
            mPressOnCurrentTab = index >= 0 && index == bar->currentIndex();
            mTabPressPos = mouse->position().toPoint();
            mCollapsedByClick = false;
        }
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

void DockSidebars::dropTab(QDockWidget* dock)
{
    auto it = mTabs.find(dock);
    if (it == mTabs.end())
        return;
    it->second->deleteLater();
    mTabs.erase(it);
    mOpen.erase(dock);
    mSizes.erase(dock);
    refreshStrips();
}

void DockSidebars::dropStaleTabLater(QDockWidget* dock)
{
    // Later: these come mid layout change (a restoreState among them), and moving the strips then breaks the layout.
    QTimer::singleShot(0, this, [this, d = QPointer<QDockWidget>(dock)] {
        if (!d || !mOpen.count(d))
            return;
        const Qt::DockWidgetArea area = mWindow->dockWidgetArea(d);
        auto tab = mTabs.find(d);
        const bool moved = tab == mTabs.end() || !mStrips.count(area) || tab->second->parentWidget() != mStrips[area];
        if (d->isHidden() || d->isFloating() || moved)
            dropTab(d);
    });
}

void DockSidebars::close(QDockWidget* dock)
{
    dropTab(dock);
    rememberSize(dock);
    dock->close();
    Q_EMIT changed();
}

void DockSidebars::manage(QDockWidget* dock)
{
    auto* bar = new DockTitleBar(dock);
    dock->setTitleBarWidget(bar);
    connect(bar, &DockTitleBar::collapseRequested, this, [this, dock] { collapse(dock); });
    // Showing a collapsed panel from a menu or hotkey expands it.
    relaxHiddenMinimum(dock, dock->isVisible());
    connect(dock, &QDockWidget::visibilityChanged, this, [this, dock](bool visible) {
        relaxHiddenMinimum(dock, visible);
        if (visible && isCollapsed(dock))
            expand(dock);
        // Closed from its own title bar: an open panel's tab goes with it. Hidden behind another tab is not closed.
        else if (!visible && mOpen.count(dock))
            dropStaleTabLater(dock);
    });
    connect(dock, &QDockWidget::topLevelChanged, this, [this, dock] { dropStaleTabLater(dock); });
    connect(dock, &QDockWidget::dockLocationChanged, this, [this, dock] { dropStaleTabLater(dock); });
    connect(dock->toggleViewAction(), &QAction::triggered, this, [this, dock](bool checked) {
        if (checked)
        {
            reveal(dock);
            flashLocate(dock);
        }
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
            // Checked again here: a dock that was hidden only rejoins its tab group once shown.
            if (tabbed || !mWindow->tabifiedDockWidgets(d).isEmpty() || d->property(kHugTabExtent).toBool())
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
    if (area == Qt::NoDockWidgetArea || area == Qt::TopDockWidgetArea || dock->property(kHugTabExtent).toBool())
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
    Q_EMIT changed();
}

void revealDock(QDockWidget* dock)
{
    if (!dock)
        return;
    auto* window = qobject_cast<QMainWindow*>(dock->parentWidget());
    auto* sidebars = window ? window->findChild<DockSidebars*>(Qt::FindDirectChildrenOnly) : nullptr;
    // Out of a collapsed strip first, so the strip lets go of it before it floats.
    if (sidebars && sidebars->isCollapsed(dock))
        sidebars->reveal(dock);
    panels::floatUnplaced(dock);
    if (panels::floatingMisplaced(dock))
        panels::placeFloating(dock);
    if (sidebars && !dock->isFloating())
    {
        sidebars->reveal(dock);
        flashLocate(dock);
        return;
    }
    dock->show();
    dock->raise();
    if (dock->isFloating())
        dock->activateWindow();
    if (QWidget* w = dock->widget())
        w->setFocus(Qt::OtherFocusReason);
    flashLocate(dock);
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

bool togglesRevealOnly()
{
    return actions::currentVia() == actions::Via::Palette;
}

void toggleDock(QDockWidget* dock)
{
    if (!dock)
        return;
    if (!dockShowing(dock) || togglesRevealOnly())
    {
        revealDock(dock);
        return;
    }
    auto* window = qobject_cast<QMainWindow*>(dock->parentWidget());
    releaseFocus(window, dock);
    if (auto* sidebars = window && !dock->isFloating() ? window->findChild<DockSidebars*>(Qt::FindDirectChildrenOnly) : nullptr)
        sidebars->collapse(dock);
    else
        dock->hide();
}

void holdOwnColumns(QMainWindow* window)
{
    gArrangingColumns = true;
    gClosedColumns.clear();
    // Two hops: the arranging a placement queues is itself a zero timer posted while it runs.
    QTimer::singleShot(0, window, [window] { QTimer::singleShot(0, window, [] { gArrangingColumns = false; }); });
}

void keepOwnColumn(QDockWidget* dock)
{
    dock->setProperty(kOwnColumn, true);
    QObject::connect(dock, &QObject::destroyed, [dock] { gClosedColumns.erase(dock); });
    auto later = [dock] { QTimer::singleShot(0, dock, [dock] { arrangeOwnColumn(dock); }); };
    QObject::connect(dock, &QDockWidget::dockLocationChanged, dock, later);
    QObject::connect(dock, &QDockWidget::topLevelChanged, dock, [later](bool floating) {
        if (!floating)
            later();
    });
    QObject::connect(dock, &QDockWidget::visibilityChanged, dock, [dock, later](bool visible) {
        if (visible)
            later();
        else
            keepColumnWidths(dock);
    });
}

bool DockSidebars::placeStrip(Qt::DockWidgetArea area, QToolBar* bar)
{
    if (area == Qt::BottomDockWidgetArea)
    {
        QStatusBar* status = mWindow->statusBar();
        if (bar->parentWidget() == status)
            return false;
        mWindow->removeToolBar(bar);
        status->insertWidget(0, bar);
        return true;
    }
    Qt::ToolBarArea toolArea = stripAreas().at(area).first;
    if (bar->parentWidget() == mWindow && mWindow->toolBarArea(bar) == toolArea)
        return false;
    mWindow->addToolBar(toolArea, bar);
    return true;
}

QToolBar* DockSidebars::strip(Qt::DockWidgetArea area)
{
    auto it = mStrips.find(area);
    if (it != mStrips.end())
    {
        placeStrip(area, it->second);
        return it->second;
    }
    auto name = stripAreas().at(area).second;
    auto* bar = new QToolBar(mWindow);
    bar->setObjectName(name);
    bar->setMovable(false);
    bar->setFloatable(false);
    bar->toggleViewAction()->setVisible(false);
    bar->setContextMenuPolicy(Qt::PreventContextMenu);
    bar->setStyleSheet("QToolBar { background: #2c2c2c; border: none; padding: 0; spacing: 2px; }");
    placeStrip(area, bar);
    bar->hide();
    mStrips[area] = bar;
    return bar;
}

void DockSidebars::beginEdgeDrag(const QPoint& at)
{
    mEdgeDrags.clear();
    // Separators are the only parts of the window with no child under them; a press on a child propagates here too.
    if (mWindow->childAt(at))
        return;
    for (Qt::DockWidgetArea area : {Qt::LeftDockWidgetArea, Qt::RightDockWidgetArea})
    {
        EdgeDrag drag;
        int left = INT_MAX, right = INT_MIN;
        for (QDockWidget* d : mWindow->findChildren<QDockWidget*>())
            // A tab behind another is parked outside the window.
            if (!d->isFloating() && dockShowing(d) && mWindow->dockWidgetArea(d) == area && mWindow->rect().intersects(d->geometry()))
            {
                drag.docks.push_back({d, d->width()});
                left = std::min(left, d->x());
                right = std::max(right, d->x() + d->width());
            }
        bool columns = false;
        for (EdgeDragDock& d : drag.docks)
        {
            d.inner = area == Qt::RightDockWidgetArea ? d.dock->x() == left : d.dock->x() + d.width == right;
            columns |= !d.inner;
        }
        if (!columns)
            continue;
        drag.extent = right - left;
        mEdgeDrags.push_back(std::move(drag));
    }
}

void DockSidebars::keepOuterColumns()
{
    for (const EdgeDrag& drag : mEdgeDrags)
    {
        int left = INT_MAX, right = INT_MIN;
        for (const EdgeDragDock& d : drag.docks)
            if (d.dock && d.dock->isVisible())
            {
                left = std::min(left, d.dock->x());
                right = std::max(right, d.dock->x() + d.dock->width());
            }
        const int delta = (right - left) - drag.extent;
        if (left > right || delta == 0)
            continue;
        QList<QDockWidget*> docks;
        QList<int> widths;
        for (const EdgeDragDock& d : drag.docks)
            if (d.dock && d.dock->isVisible())
            {
                docks << d.dock;
                widths << d.width + (d.inner ? delta : 0);
            }
        mWindow->resizeDocks(docks, widths, Qt::Horizontal);
    }
}

void DockSidebars::collapse(QDockWidget* dock)
{
    if (dock->isFloating() || isCollapsed(dock))
        return;
    Qt::DockWidgetArea area = mWindow->dockWidgetArea(dock);
    if (area == Qt::NoDockWidgetArea)
        return;
    rememberSize(dock);
    releaseFocus(mWindow, dock);
    QToolBar* bar = strip(area);
    Qt::Orientation orientation = (area == Qt::LeftDockWidgetArea || area == Qt::RightDockWidgetArea) ? Qt::Vertical : Qt::Horizontal;
    if (auto open = mTabs.find(dock); open != mTabs.end())
    {
        mOpen.erase(dock);
        auto* tab = static_cast<QToolButton*>(open->second);
        tab->setChecked(false);
        tab->setToolTip("Expand " + dock->windowTitle());
    }
    else
    {
        bar->setOrientation(orientation);
        auto* tab = new SideTab(dock->windowTitle(), orientation, bar);
        tab->setToolTip("Expand " + dock->windowTitle());
        tab->installEventFilter(this);
        QAction* action = bar->addWidget(tab);
        connect(tab, &QToolButton::clicked, this, [this, dock] { isCollapsed(dock) ? expand(dock) : collapse(dock); });
        connect(tab, &QObject::destroyed, bar, [bar, action] { bar->removeAction(action); });
        mTabs[dock] = tab;
    }
    mSizes[dock] = QSize(slotExtent(mWindow, dock, Qt::Horizontal), slotExtent(mWindow, dock, Qt::Vertical));
    // Freed space goes to the viewport, not to the panels left in the area. Only the tabs showing: a tab behind
    // another holds a stale size that would win over the shown one's.
    QList<QDockWidget*> neighbours;
    QList<int> sizes;
    const Qt::Orientation across = orientation == Qt::Vertical ? Qt::Horizontal : Qt::Vertical;
    for (QDockWidget* other : mWindow->findChildren<QDockWidget*>())
        if (other != dock && dockShowing(other) && !other->isFloating() && mWindow->dockWidgetArea(other) == area)
        {
            neighbours << other;
            sizes << slotExtent(mWindow, other, across);
        }
    dock->hide();
    if (!neighbours.isEmpty())
        mWindow->resizeDocks(neighbours, sizes, across);
    refreshStrips();
    Q_EMIT changed();
}

void DockSidebars::expand(QDockWidget* dock)
{
    auto it = mTabs.find(dock);
    if (it == mTabs.end() || mOpen.count(dock))
        return;
    mOpen.insert(dock);
    auto* tab = static_cast<QToolButton*>(it->second);
    tab->setChecked(true);
    tab->setToolTip("Collapse " + dock->windowTitle());
    const bool tabbed = !dock->isFloating() && !mWindow->tabifiedDockWidgets(dock).isEmpty();
    if (tabbed)
        for (QDockWidget* other : mWindow->tabifiedDockWidgets(dock))
            if (dockShowing(other))
                rememberSize(other);
    dock->show();
    dock->raise();
    if (auto size = mSizes.find(dock); size != mSizes.end() && size->second.width() > 40 && size->second.height() > 40)
    {
        Qt::DockWidgetArea area = mWindow->dockWidgetArea(dock);
        bool sideways = area == Qt::LeftDockWidgetArea || area == Qt::RightDockWidgetArea;
        if (!tabbed)
            mWindow->resizeDocks({dock}, {sideways ? size->second.width() : size->second.height()}, sideways ? Qt::Horizontal : Qt::Vertical);
        mSizes.erase(size);
    }
    // In a tab group the slot is the group's, still at the width of the tab that was showing: back to this tab's own.
    if (tabbed || dock->property(kHugTabExtent).toBool())
        QTimer::singleShot(0, this, [this, d = QPointer<QDockWidget>(dock)] {
            if (d && d->isVisible())
                restoreSize(d);
        });
    refreshStrips();
    Q_EMIT changed();
}

bool DockSidebars::isCollapsed(const QDockWidget* dock) const
{
    auto* d = const_cast<QDockWidget*>(dock);
    return mTabs.count(d) > 0 && !mOpen.count(d);
}

QStringList DockSidebars::collapsedNames() const
{
    QStringList names;
    for (auto& [dock, tab] : mTabs)
        if (!mOpen.count(dock))
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
    std::vector<QDockWidget*> all;
    for (auto& [dock, tab] : mTabs)
        all.push_back(dock);
    // A layout change starts from no tabs at all: the ones it collapses come back, open panels' tabs don't.
    for (QDockWidget* dock : all)
    {
        expand(dock);
        dropTab(dock);
    }
}

void DockSidebars::refreshStrips()
{
    for (auto& [area, bar] : mStrips)
    {
        // restoreState drops toolbars its state doesn't name, leaving them unplaced at the window's corner.
        placeStrip(area, bar);
        bool any = false;
        for (auto& [dock, tab] : mTabs)
            any |= tab->parentWidget() == bar;
        bar->setVisible(any);
    }
}
} // namespace lila::ui
