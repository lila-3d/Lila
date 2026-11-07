#include "ui/TopBar.h"

#include "link/UnrealLink.h"
#include "ui/Actions.h"
#include "ui/Icons.h"
#include "ui/MenuContext.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QTimer>
#include <QToolButton>

namespace lila::ui
{
namespace
{
/** A menu title's horizontal padding on the bar (the app sheet's QMenuBar::item padding, both sides). */
constexpr int kMenuPadding = 24;

/**
 * The link state as a dot: filled when connected, a ring when not, so it reads without its colour. Drawn at the
 * screen's pixel ratio so it stays sharp at any UI scale.
 */
QIcon linkDot(bool connected, qreal dpr)
{
    const int size = 16;
    QPixmap pm(QSize(size, size) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF dot(4.5, 4.5, 7, 7);
    if (connected)
    {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#6cc24a"));
        p.drawEllipse(dot);
    }
    else
    {
        p.setPen(QPen(themeColor("dimText"), 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(dot);
    }
    return QIcon(pm);
}

void addActionRow(QMenu* menu, const char* id)
{
    if (const Action* a = actions::find(id))
        menu->addAction(icon(QString::fromStdString(actions::iconFor(id))), QString::fromStdString(a->label), [id] { actions::run(nlohmann::json(id)); })
            ->setToolTip(QString::fromStdString(actions::tooltip(id)));
}
} // namespace

TopBar::TopBar(QMenuBar* bar, QWidget* layoutTabs, QWidget* folder, QWidget* modes, QWidget* parent) : QWidget(parent), mBar(bar)
{
    setObjectName("topBar");
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 6, 0);
    row->setSpacing(8);
    row->addStretch(1);
    if (layoutTabs)
        row->addWidget(layoutTabs);
    row->addStretch(1);
    mFolder = folder;
    if (folder)
        row->addWidget(folder);
    mModes = modes;
    if (modes)
        row->addWidget(modes);

    mPin = new QToolButton(this);
    mPin->setObjectName("menuPin");
    mPin->setIcon(icon("pin"));
    mPin->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    mPin->setToolTip("The menu bar is pinned to this menu set. Click to let it follow what you are doing again.");
    connect(mPin, &QToolButton::clicked, this, [] { menuContext::unpin(); });
    row->addWidget(mPin);

    auto* search = mSearch = new QToolButton(this);
    search->setObjectName("topSearch");
    search->setIcon(icon("search"));
    search->setText("Search commands");
    search->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    search->setToolTip(QString::fromStdString(actions::tooltip("window.searchMenus")));
    search->setFixedWidth(240);
    connect(search, &QToolButton::clicked, this, [] { actions::run(nlohmann::json("window.searchMenus")); });
    row->addWidget(search);

    mUnreal = new QToolButton(this);
    mUnreal->setObjectName("unrealSend");
    mUnreal->setText("Send to Unreal");
    mUnreal->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    mUnreal->setPopupMode(QToolButton::MenuButtonPopup);
    auto* menu = new QMenu(mUnreal);
    connect(menu, &QMenu::aboutToShow, menu, [menu] {
        menu->clear();
        for (const char* id : {"unreal.connect", "unreal.sync", "unreal.sendBake", "unreal.expressionRig", "animated.sendAgain", "animated.sendOptions"})
            addActionRow(menu, id);
    });
    mUnreal->setMenu(menu);
    connect(mUnreal, &QToolButton::clicked, this, [] { actions::run(nlohmann::json("unreal.send")); });
    row->addWidget(mUnreal);

    // The link has no change signal; its state is a cheap read.
    auto* poll = new QTimer(this);
    poll->setInterval(1500);
    connect(poll, &QTimer::timeout, this, [this] { refreshUnreal(); });
    poll->start();
    refreshUnreal();

    mFullWidth = layout()->sizeHint().width();
    menuContext::onChanged([this] { refreshPin(); });
    refreshPin();
    bar->installEventFilter(this);
}

void TopBar::refreshPin()
{
    const std::string set = menuContext::pinned();
    mPin->setVisible(!set.empty());
    mPin->setText(QString::fromStdString(set) + " Menus");
    relayout();
}

void TopBar::refreshUnreal()
{
    const nlohmann::json status = link::Link::get().status();
    const bool connected = status.value("connected", false);
    if (connected == mConnected && !mUnreal->icon().isNull())
        return;
    mConnected = connected;
    mUnreal->setIcon(linkDot(connected, devicePixelRatioF()));
    const std::string project = status.value("project", std::string());
    mUnreal->setToolTip(connected ? QString("Send the selection to Unreal%1. The arrow has Sync, Send Bake and the rest.")
                                        .arg(project.empty() ? QString() : " (" + QString::fromStdString(project) + ")")
                                  : QString("Unreal is not connected: sending connects first. Start the Unreal Editor with the LilaLink "
                                            "plugin; the arrow has Connect to Unreal."));
}

void TopBar::relayout()
{
    // The bar sits in the menu bar's corner, which gets only its own size: give it the room right of the last menu.
    // Measured from the titles, not actionGeometry: a menu already pushed into the overflow has no geometry.
    int menusRight = 4;
    const QFontMetrics fm = mBar->fontMetrics();
    for (QAction* a : mBar->actions())
        if (a->isVisible())
            menusRight += fm.horizontalAdvance(QString(a->text()).remove('&')) + kMenuPadding;
    const int room = mBar->width() - menusRight - 16;
    // Short of room, the search field drops to its icon and the folder name to its tooltip before any menu is pushed off.
    const bool compact = room < mFullWidth;
    mSearch->setToolButtonStyle(compact ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon);
    mSearch->setFixedWidth(compact ? 30 : 240);
    if (mFolder)
        mFolder->setVisible(!compact);
    // The mode list stays reachable from Ctrl+T (each mode is an action) when there is no room for it here.
    if (mModes)
        mModes->setMaximumWidth(compact ? 0 : QWIDGETSIZE_MAX);
    const int width = std::max(layout()->sizeHint().width(), room);
    if (width == mWidth)
        return;
    mWidth = width;
    updateGeometry();
    // The menu bar lays its corner out from sizeHint, and only when its own layout runs.
    QEvent request(QEvent::LayoutRequest);
    QApplication::sendEvent(mBar, &request);
}

QSize TopBar::sizeHint() const
{
    return QSize(std::max(mWidth, layout()->sizeHint().width()), layout()->sizeHint().height());
}

bool TopBar::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == mBar && (event->type() == QEvent::Resize || event->type() == QEvent::ActionChanged || event->type() == QEvent::ActionAdded ||
                            event->type() == QEvent::ActionRemoved))
        QTimer::singleShot(0, this, [this] { relayout(); });
    return QWidget::eventFilter(watched, event);
}
} // namespace lila::ui
