#include "ui/IconBars.h"

#include "core/ExtensionContext.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "scene/Components.h"
#include "ui/Actions.h"
#include "ui/Arrange.h"
#include "ui/Icons.h"
#include "ui/Tools.h"
#include "ui/ViewportTool.h"

#include <QAction>
#include <QContextMenuEvent>
#include <QEvent>
#include <QFrame>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>

#include <algorithm>

namespace lila::ui::iconBars
{
namespace
{
constexpr int kSpacing = 1;
constexpr int kMargin = 2;
constexpr int kSeparator = 7;

/**
 * A bar button. One with a dropdown shows a corner mark; one that also runs an action opens the dropdown on a press of
 * the mark, a press held past a moment, or a right-click, and runs the action on a plain click.
 */
class BarButton : public QToolButton
{
public:
    explicit BarButton(QWidget* parent) : QToolButton(parent)
    {
        mHold.setSingleShot(true);
        mHold.setInterval(350);
        QObject::connect(&mHold, &QTimer::timeout, this, [this] {
            mHeld = true;
            popup();
        });
    }

    /** The dropdown of a button that also runs an action, kept off QToolButton so its own popup logic stays out of the way. */
    QMenu* dropdown = nullptr;

protected:
    void paintEvent(QPaintEvent* e) override
    {
        QToolButton::paintEvent(e);
        if (!dropdown && !menu())
            return;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(palette().color(isEnabled() ? QPalette::ButtonText : QPalette::PlaceholderText));
        const double s = mark();
        const QPointF c(width() - 2.0, height() - 2.0);
        p.drawPolygon(QPolygonF{c, c - QPointF(s, 0.0), c - QPointF(0.0, s)});
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        mHeld = false;
        if (dropdown && e->button() == Qt::LeftButton)
        {
            // The corner mark is a target of its own, a little larger than drawn.
            const double reach = mark() + 6.0;
            if (e->position().x() > width() - reach && e->position().y() > height() - reach)
            {
                mHeld = true;
                popup();
                return;
            }
            mHold.start();
        }
        QToolButton::mousePressEvent(e);
    }

    void mouseReleaseEvent(QMouseEvent* e) override
    {
        mHold.stop();
        if (mHeld)
        {
            // The press opened the menu, so its release must not also run the action.
            mHeld = false;
            setDown(false);
            return;
        }
        QToolButton::mouseReleaseEvent(e);
    }

    void contextMenuEvent(QContextMenuEvent* e) override
    {
        if (dropdown)
            popup();
        else if (menu())
            showMenu();
        else
            QToolButton::contextMenuEvent(e);
    }

private:
    double mark() const { return std::max(4.0, iconSize().width() / 6.0); }

    void popup()
    {
        mHold.stop();
        setDown(false);
        dropdown->popup(mapToGlobal(QPoint(width(), 0)));
    }

    QTimer mHold;
    bool mHeld = false;
};

std::string defaultId(const Item& item)
{
    std::string base = item.bar + "/" + (item.kind == Kind::Separator ? std::string("-") : !item.action.empty() ? item.action : item.icon + item.label);
    if (!registry().contains(base))
        return base;
    for (int n = 2;; ++n)
        if (std::string id = base + "#" + std::to_string(n); !registry().contains(id))
            return id;
}

QString labelOf(const Item& item)
{
    if (!item.label.empty())
        return QString::fromStdString(item.label);
    const Action* a = actions::find(item.action);
    return QString::fromStdString(a ? a->label : item.action);
}
} // namespace

Registry<Item>& registry()
{
    static Registry<Item> r;
    return r;
}

std::string add(Item item, std::string owner)
{
    if (item.id.empty())
        item.id = defaultId(item);
    std::string id = item.id;
    registry().put(id, std::move(item), std::move(owner));
    return id;
}

void addActions(const std::string& bar, std::initializer_list<std::pair<const char*, const char*>> actionIcons, std::string owner)
{
    for (auto [action, iconName] : actionIcons)
        add({.bar = bar, .kind = *action ? Kind::Action : Kind::Separator, .action = action, .icon = iconName ? iconName : ""}, owner);
}

uint64_t add(ext::Context& ctx, Item item)
{
    if (!item.id.empty() && registry().contains(item.id))
    {
        ctx.log(LogLevel::Error, "icon bar item '" + item.id + "' already exists");
        return 0;
    }
    std::string id = add(std::move(item), ctx.owner());
    return ctx.track("iconBarItem:" + id, [id, own = ctx.owner()] {
        if (const std::string* o = registry().ownerOf(id); o && *o == own)
            registry().remove(id);
    });
}

bool remove(const std::string& id)
{
    return registry().remove(id);
}

std::vector<const Item*> itemsIn(const std::string& bar)
{
    std::vector<Item*> items;
    for (Item* i : registry().allInOrder())
        if (i->bar == bar)
            items.push_back(i);
    std::stable_partition(items.begin(), items.end(), [](Item* i) {
        const std::string* owner = registry().ownerOf(i->id);
        return owner && *owner == kLilaOwner;
    });
    std::vector<Item*> ordered = arrange(items, [](Item* i) { return i->id; },
                                         [](Item* i) { return std::pair<std::string, std::string>{i->after, i->before}; });
    return {ordered.begin(), ordered.end()};
}

void addActionRows(QMenu* menu, const std::vector<std::string>& rows, const StateFn& state)
{
    for (const std::string& row : rows)
    {
        if (row.empty())
        {
            menu->addSeparator();
            continue;
        }
        if (row.front() == '#')
        {
            menu->addSection(QString::fromStdString(row.substr(1)));
            continue;
        }
        const Action* a = actions::find(row);
        QAction* qa = menu->addAction(QString::fromStdString(a ? a->label : row));
        qa->setData(QString::fromStdString(row));
        qa->setToolTip(QString::fromStdString(actions::tooltip(row)));
        qa->setIcon(icon(QString::fromStdString(actions::iconFor(row))));
        qa->setEnabled(actions::isEnabled(nlohmann::json(row)));
        if (std::optional<bool> on = state ? state(row) : std::nullopt)
        {
            qa->setCheckable(true);
            qa->setChecked(*on);
        }
        QObject::connect(qa, &QAction::triggered, menu, [row] { actions::run(row); });
    }
    menu->setToolTipsVisible(true);
}

IconBar::IconBar(std::string bar, Qt::Orientation orientation, int iconSize, StateFn state, QWidget* parent)
    : QWidget(parent), mBar(std::move(bar)), mOrientation(orientation), mIconSize(iconSize), mState(std::move(state))
{
    setObjectName("iconBar");
    setAttribute(Qt::WA_StyledBackground);
    setStyleSheet(R"(
        #iconBar { background: #383838; }
        #iconBar QToolButton { border: none; border-radius: 2px; padding: 3px; }
        #iconBar QToolButton:hover { background: #4a4a4a; }
        #iconBar QToolButton:checked { background: #5c5c5c; }
        #iconBar QToolButton::menu-indicator { image: none; width: 0; }
        #iconBar QFrame#iconBarSeparator { color: #4c4c4c; }
    )");

    mChevron = new QToolButton(this);
    mChevron->setIcon(icon(mOrientation == Qt::Vertical ? "chevrons-down" : "chevrons-right"));
    mChevron->setIconSize(QSize(std::min(mIconSize, 16), std::min(mIconSize, 16)));
    mChevron->setToolTip("More");
    mChevron->setPopupMode(QToolButton::InstantPopup);
    auto* overflow = new QMenu(mChevron);
    mChevron->setMenu(overflow);
    connect(overflow, &QMenu::aboutToShow, this, [this, overflow] {
        overflow->clear();
        overflow->setToolTipsVisible(true);
        for (const Button& b : mButtons)
        {
            if (!b.widget->isHidden())
                continue;
            if (!b.button)
            {
                overflow->addSeparator();
                continue;
            }
            if (b.item.kind == Kind::Action)
            {
                overflow->addAction(b.button->defaultAction());
                continue;
            }
            QMenu* sub = overflow->addMenu(icon(QString::fromStdString(b.item.icon)), labelOf(b.item));
            Item item = b.item;
            connect(sub, &QMenu::aboutToShow, sub, [this, sub, item] {
                sub->clear();
                if (!item.action.empty())
                    addActionRows(sub, {item.action, ""}, mState);
                fillMenu(sub, item);
            });
        }
    });

    QPointer<IconBar> self(this);
    auto changed = [self](RegistryChange, const std::string&, const std::string&) {
        if (self)
            self->scheduleRebuild();
    };
    mRegistryListener = registry().addListener(changed);
    auto later = [self] {
        if (self)
            QMetaObject::invokeMethod(self, [self] {
                if (self)
                    self->scheduleRefresh();
            });
    };
    mPrefListener = PrefStore::get().onChanged([later](const std::string&) { later(); });
    mToolsListener = tools::addListener(later);
    mViewportToolListener = viewportTools::addListener(later);
    mComponentListener = scene::components::addListener(later);
    rebuild();
}

IconBar::~IconBar()
{
    registry().removeListener(mRegistryListener);
    PrefStore::get().removeOnChanged(mPrefListener);
    tools::removeListener(mToolsListener);
    viewportTools::removeListener(mViewportToolListener);
    scene::components::removeListener(mComponentListener);
}

int IconBar::along(QSize s) const
{
    return mOrientation == Qt::Vertical ? s.height() : s.width();
}

int IconBar::across(QSize s) const
{
    return mOrientation == Qt::Vertical ? s.width() : s.height();
}

std::optional<bool> IconBar::stateOf(const Item& item) const
{
    if (item.checked)
        return item.checked();
    if (item.action.empty() || !mState)
        return std::nullopt;
    return mState(item.action);
}

void IconBar::fillMenu(QMenu* menu, const Item& item) const
{
    if (item.fill)
        item.fill(menu);
    else
        addActionRows(menu, item.menuActions, mState);
}

void IconBar::scheduleRebuild()
{
    if (std::exchange(mRebuildQueued, true))
        return;
    QTimer::singleShot(0, this, [this] {
        mRebuildQueued = false;
        rebuild();
    });
}

void IconBar::scheduleRefresh()
{
    if (std::exchange(mRefreshQueued, true))
        return;
    QTimer::singleShot(0, this, [this] {
        mRefreshQueued = false;
        refresh();
    });
}

void IconBar::rebuild()
{
    for (Button& b : mButtons)
    {
        b.widget->hide();
        b.widget->deleteLater();
    }
    mButtons.clear();
    const QSize iconSize(mIconSize, mIconSize);
    for (const Item* item : itemsIn(mBar))
    {
        Button b;
        b.item = *item;
        if (item->kind == Kind::Separator)
        {
            auto* line = new QFrame(this);
            line->setFrameShape(mOrientation == Qt::Vertical ? QFrame::HLine : QFrame::VLine);
            line->setObjectName("iconBarSeparator");
            b.widget = line;
            mButtons.push_back(std::move(b));
            continue;
        }
        auto* button = new BarButton(this);
        button->setIconSize(iconSize);
        const QIcon ic = icon(QString::fromStdString(item->icon));
        if (!item->action.empty())
        {
            auto* a = new QAction(ic, labelOf(*item), button);
            button->setDefaultAction(a);
            std::string action = item->action;
            connect(a, &QAction::triggered, this, [this, action] {
                actions::run(action);
                refresh();
            });
        }
        else
        {
            button->setIcon(ic);
            button->setToolTip(labelOf(*item));
        }
        if (item->kind == Kind::Menu)
        {
            auto* menu = new QMenu(button);
            Item copy = *item;
            connect(menu, &QMenu::aboutToShow, this, [this, menu, copy] {
                menu->clear();
                menu->setToolTipsVisible(true);
                fillMenu(menu, copy);
            });
            if (item->action.empty())
            {
                button->setMenu(menu);
                button->setPopupMode(QToolButton::InstantPopup);
            }
            else
                button->dropdown = menu;
        }
        b.widget = b.button = button;
        mButtons.push_back(std::move(b));
    }
    for (Button& b : mButtons)
        b.widget->show();
    mChevron->raise();
    refresh();
    refreshTooltips();
    updateGeometry();
    layoutItems();
}

void IconBar::refresh()
{
    for (Button& b : mButtons)
    {
        if (!b.button)
            continue;
        std::optional<bool> on = stateOf(b.item);
        const bool enabled = b.item.action.empty() || actions::isEnabled(nlohmann::json(b.item.action));
        if (QAction* a = b.button->defaultAction())
        {
            QSignalBlocker block(a);
            a->setCheckable(on.has_value());
            a->setChecked(on.value_or(false));
            a->setEnabled(enabled);
        }
        else
        {
            b.button->setCheckable(on.has_value());
            b.button->setChecked(on.value_or(false));
        }
    }
}

void IconBar::refreshTooltips()
{
    for (Button& b : mButtons)
    {
        if (!b.button)
            continue;
        QString tip = b.item.action.empty() ? labelOf(b.item) : QString::fromStdString(actions::tooltip(b.item.action));
        if (!b.item.label.empty() && !b.item.action.empty())
            tip = labelOf(b.item) + "\n" + tip;
        if (b.item.kind == Kind::Menu && !b.item.action.empty())
            tip += "\nHold, right-click or press the corner mark for more";
        if (QAction* a = b.button->defaultAction())
        {
            a->setText(labelOf(b.item));
            a->setToolTip(tip);
        }
        else
            b.button->setToolTip(tip);
    }
}

QSize IconBar::sizeHint() const
{
    int length = 2 * kMargin, thickness = mIconSize + 8, longest = 0;
    for (const Button& b : mButtons)
    {
        if (!b.button)
        {
            length += kSeparator + kSpacing;
            continue;
        }
        QSize s = b.button->sizeHint();
        length += along(s) + kSpacing;
        longest = std::max(longest, along(s));
        thickness = std::max(thickness, across(s) + 2 * kMargin);
    }
    if (mFill)
        length = 2 * kMargin + longest + kSpacing + along(mChevron->sizeHint());
    if (mWrap)
    {
        length = 2 * kMargin + longest;
        thickness *= lines(mOrientation == Qt::Vertical ? height() : width());
    }
    return mOrientation == Qt::Vertical ? QSize(thickness, length) : QSize(length, thickness);
}

QSize IconBar::minimumSizeHint() const
{
    QSize s = sizeHint();
    return mOrientation == Qt::Vertical ? QSize(s.width(), 0) : QSize(0, s.height());
}

void IconBar::setFill(bool fill)
{
    mFill = fill;
    setSizePolicy(mOrientation == Qt::Vertical ? QSizePolicy::Preferred : QSizePolicy::Expanding,
                  mOrientation == Qt::Vertical ? QSizePolicy::Expanding : QSizePolicy::Preferred);
    updateGeometry();
}

void IconBar::setWrap(bool wrap)
{
    mWrap = wrap;
    setSizePolicy(mOrientation == Qt::Vertical ? QSizePolicy::Preferred : QSizePolicy::Expanding,
                  mOrientation == Qt::Vertical ? QSizePolicy::Expanding : QSizePolicy::Preferred);
    updateGeometry();
}

int IconBar::lines(int length) const
{
    if (length <= 0)
        return 1;
    int count = 1, pos = kMargin;
    for (const Button& b : mButtons)
    {
        const int extent = b.button ? along(b.button->sizeHint()) : kSeparator;
        if (pos + extent > length - kMargin && pos > kMargin)
        {
            ++count;
            pos = kMargin;
            if (!b.button)
                continue;
        }
        pos += extent + kSpacing;
    }
    return count;
}

void IconBar::layoutWrapped()
{
    const int length = mOrientation == Qt::Vertical ? height() : width();
    const int line = across(sizeHintOneLine());
    int pos = kMargin, index = 0;
    for (Button& b : mButtons)
    {
        const int extent = b.button ? along(b.button->sizeHint()) : kSeparator;
        if (pos + extent > length - kMargin && pos > kMargin)
        {
            ++index;
            pos = kMargin;
        }
        // A separator that would start a line has nothing to separate there.
        const bool leading = !b.button && pos == kMargin;
        b.widget->setVisible(!leading);
        if (leading)
            continue;
        const int size = b.button ? across(b.button->sizeHint()) : line - 2 * kMargin - 4;
        const int offset = index * line + (line - size) / 2;
        if (mOrientation == Qt::Vertical)
            b.widget->setGeometry(offset, pos, size, extent);
        else
            b.widget->setGeometry(pos, offset, extent, size);
        pos += extent + kSpacing;
    }
    mChevron->setVisible(false);
    if (lines(length) != mLines)
    {
        mLines = lines(length);
        updateGeometry();
    }
}

QSize IconBar::sizeHintOneLine() const
{
    int thickness = mIconSize + 8;
    for (const Button& b : mButtons)
        if (b.button)
            thickness = std::max(thickness, across(b.button->sizeHint()) + 2 * kMargin);
    return mOrientation == Qt::Vertical ? QSize(thickness, 0) : QSize(0, thickness);
}

void IconBar::layoutItems()
{
    if (mWrap)
        return layoutWrapped();
    const int length = mOrientation == Qt::Vertical ? height() : width();
    const int thickness = mOrientation == Qt::Vertical ? width() : height();
    const int chevron = along(mChevron->sizeHint());
    auto place = [&](QWidget* w, int pos, int extent, int size) {
        int offset = (thickness - size) / 2;
        if (mOrientation == Qt::Vertical)
            w->setGeometry(offset, pos, size, extent);
        else
            w->setGeometry(pos, offset, extent, size);
    };
    int pos = kMargin;
    const int end = length - kMargin;
    bool overflowed = false;
    for (size_t i = 0; i < mButtons.size(); ++i)
    {
        Button& b = mButtons[i];
        const int extent = b.button ? along(b.button->sizeHint()) : kSeparator;
        // The last item may use the chevron's space; anything before it has to leave room for the chevron.
        const int limit = i + 1 == mButtons.size() ? end : end - chevron;
        if (!overflowed && pos + extent > limit)
            overflowed = true;
        b.widget->setVisible(!overflowed);
        if (overflowed)
            continue;
        const int size = b.button ? across(b.button->sizeHint()) : thickness - 2 * kMargin - 4;
        place(b.widget, pos, extent, size);
        pos += extent + kSpacing;
    }
    mChevron->setVisible(overflowed);
    if (overflowed)
        place(mChevron, end - chevron, chevron, across(mChevron->sizeHint()));
}

void IconBar::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    layoutItems();
}

bool IconBar::event(QEvent* e)
{
    // Hotkeys and states change while the pointer is elsewhere, so both catch up when it comes over the bar.
    if (e->type() == QEvent::Enter)
    {
        refreshTooltips();
        refresh();
    }
    else if (e->type() == QEvent::Show || e->type() == QEvent::LayoutRequest)
        layoutItems();
    return QWidget::event(e);
}
} // namespace lila::ui::iconBars
