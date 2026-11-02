#include "ui/ConnectPopover.h"

#include "core/Log.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "ui/Actions.h"
#include "ui/AttrView.h"
#include "ui/ComponentSheet.h"
#include "ui/Icons.h"
#include "ui/Menus.h"
#include "ui/RelationshipsPanel.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

namespace lila::ui
{
namespace
{
using scene::Graph;
using scene::PlugType;

enum Role
{
    kPlug = Qt::UserRole,
    kKind,      // the plug's type, for compatibility
    kComponent, // -1 or a Vec3 child
};

bool scalarKind(int type, int component)
{
    return component >= 0 || type == int(PlugType::Float) || type == int(PlugType::Int) || type == int(PlugType::Bool) || type == int(PlugType::Enum);
}

bool fits(int fromType, int fromComponent, int toType, int toComponent)
{
    if (scalarKind(toType, toComponent))
        return scalarKind(fromType, fromComponent);
    return fromType == toType && fromComponent < 0 && toComponent < 0;
}

/** The plugs of a node a side lists: outputs (or keyable and added ones) on the left, inputs on the right. */
void fill(QListWidget* list, scene::NodeId id, bool outputs, const QString& filter)
{
    list->clear();
    Graph& g = Graph::current();
    const scene::Node* n = g.node(id);
    if (!n)
        return;
    for (size_t i = 0; i < n->type().attrs.size(); ++i)
    {
        const scene::AttrDef& d = n->type().attrs[i];
        if ((d.flags & (scene::AttrHidden | scene::AttrArray)) || d.type == PlugType::Mesh || d.type == PlugType::Data)
            continue;
        const bool readable = (d.flags & (scene::AttrOutput | scene::AttrKeyable | scene::AttrUser | scene::AttrChannelBox)) != 0;
        if (outputs ? !readable : !(d.flags & scene::AttrInput))
            continue;
        auto add = [&](const std::string& attr, int component) {
            const QString label = attrView::label(d, component);
            if (!filter.isEmpty() && !label.contains(filter, Qt::CaseInsensitive) && !QString::fromStdString(attr).contains(filter, Qt::CaseInsensitive))
                return;
            const std::string plug = n->path() + "." + attr;
            QString text = label;
            if (!outputs)
                if (auto src = g.source(g.plug(plug)))
                    text += QString::fromUtf8("   ← ") + QString::fromStdString(g.plugName(*src));
            auto* item = new QListWidgetItem(text, list);
            item->setData(kPlug, QString::fromStdString(plug));
            item->setData(kKind, int(d.type));
            item->setData(kComponent, component);
            item->setToolTip(QString::fromStdString(plug));
        };
        add(d.name, -1);
        if (d.type == PlugType::Vec3)
            for (int c = 0; c < 3; ++c)
                add(d.childNames[size_t(c)], c);
    }
}
} // namespace

ConnectPopover::ConnectPopover(scene::NodeId source, scene::NodeId target, QWidget* parent)
    : QFrame(parent, Qt::Tool | Qt::FramelessWindowHint), mSource(source), mTarget(target)
{
    setObjectName("connectPopover");
    setFrameShape(QFrame::StyledPanel);
    setMinimumSize(520, 380);
    installEventFilter(this);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(8, 8, 8, 8);
    auto* top = new QHBoxLayout;
    mTitle = new QLabel(this);
    top->addWidget(mTitle, 1);
    auto* swapButton = new QToolButton(this);
    swapButton->setObjectName("connectSwap");
    swapButton->setIcon(icon("arrows-left-right"));
    swapButton->setAutoRaise(true);
    swapButton->setToolTip("Swap source and target");
    connect(swapButton, &QToolButton::clicked, this, [this] { swap(); });
    top->addWidget(swapButton);
    auto* match = new QPushButton("Match by Name", this);
    match->setObjectName("connectMatch");
    match->setToolTip("Wire every output to the same-named input that fits");
    connect(match, &QPushButton::clicked, this, [this] { matchByName(); });
    top->addWidget(match);
    auto* closer = new QToolButton(this);
    closer->setIcon(icon("x"));
    closer->setAutoRaise(true);
    closer->setToolTip("Close (Esc)");
    connect(closer, &QToolButton::clicked, this, &QWidget::close);
    top->addWidget(closer);
    v->addLayout(top);

    auto* grid = new QGridLayout;
    mLeftFilter = new QLineEdit(this);
    mLeftFilter->setObjectName("connectLeftFilter");
    mLeftFilter->setPlaceholderText("Filter outputs");
    mRightFilter = new QLineEdit(this);
    mRightFilter->setObjectName("connectRightFilter");
    mRightFilter->setPlaceholderText("Filter inputs");
    mLeft = new QListWidget(this);
    mLeft->setObjectName("connectLeft");
    mRight = new QListWidget(this);
    mRight->setObjectName("connectRight");
    grid->addWidget(mLeftFilter, 0, 0);
    grid->addWidget(mRightFilter, 0, 1);
    grid->addWidget(mLeft, 1, 0);
    grid->addWidget(mRight, 1, 1);
    v->addLayout(grid, 1);
    mStatus = new QLabel(this);
    mStatus->setObjectName("connectStatus");
    mStatus->setStyleSheet(QString("color: %1;").arg(themeColor("dimText").name()));
    mStatus->setText("Click an output, then an input it fits.");
    v->addWidget(mStatus);

    connect(mLeftFilter, &QLineEdit::textChanged, this, [this] { refill(); });
    connect(mRightFilter, &QLineEdit::textChanged, this, [this] { refill(); });
    connect(mLeft, &QListWidget::currentItemChanged, this, [this] { markCompatible(); });
    connect(mRight, &QListWidget::itemClicked, this, [this](QListWidgetItem* to) {
        if (QListWidgetItem* from = mLeft->currentItem(); from && (to->flags() & Qt::ItemIsEnabled))
            wire(from->data(kPlug).toString().toStdString(), to->data(kPlug).toString().toStdString());
    });
    mListener = Graph::current().addListener([this](scene::Change, scene::NodeId) {
        QMetaObject::invokeMethod(this, [this] {
            if (!Graph::current().node(mSource) || !Graph::current().node(mTarget))
                close();
            else
                refill();
        }, Qt::QueuedConnection);
    });
    refill();
}

ConnectPopover::~ConnectPopover() { Graph::current().removeListener(mListener); }

void ConnectPopover::refill()
{
    Graph& g = Graph::current();
    const scene::Node* s = g.node(mSource);
    const scene::Node* t = g.node(mTarget);
    if (!s || !t)
        return;
    mTitle->setText(QString("<b>%1</b> &rarr; <b>%2</b>").arg(QString::fromStdString(s->name()).toHtmlEscaped(), QString::fromStdString(t->name()).toHtmlEscaped()));
    const QString current = mLeft->currentItem() ? mLeft->currentItem()->data(kPlug).toString() : QString();
    fill(mLeft, mSource, true, mLeftFilter->text().trimmed());
    fill(mRight, mTarget, false, mRightFilter->text().trimmed());
    for (int i = 0; i < mLeft->count(); ++i)
        if (mLeft->item(i)->data(kPlug).toString() == current)
            mLeft->setCurrentRow(i);
    markCompatible();
}

void ConnectPopover::markCompatible()
{
    QListWidgetItem* from = mLeft->currentItem();
    for (int i = 0; i < mRight->count(); ++i)
    {
        QListWidgetItem* to = mRight->item(i);
        const bool ok = !from || fits(from->data(kKind).toInt(), from->data(kComponent).toInt(), to->data(kKind).toInt(), to->data(kComponent).toInt());
        to->setFlags(ok ? Qt::ItemIsEnabled | Qt::ItemIsSelectable : Qt::NoItemFlags);
        QFont f = to->font();
        f.setBold(from && ok);
        to->setFont(f);
    }
}

bool ConnectPopover::wire(const std::string& from, const std::string& to)
{
    try
    {
        ops::execute("connectAttr", {{"args", {from, to}}, {"force", true}}, CallSource::UI);
        mStatus->setText(QString::fromStdString(from + " -> " + to));
        return true;
    }
    catch (const CommandError& e)
    {
        mStatus->setText(e.what());
        return false;
    }
}

int ConnectPopover::matchByName()
{
    Graph& g = Graph::current();
    if (!g.node(mSource) || !g.node(mTarget))
        return 0;
    try
    {
        const json wired = ops::execute("connectByName", {{"args", {g.node(mSource)->path(), g.node(mTarget)->path()}}}, CallSource::UI);
        mStatus->setText(QString("Matched %1 by name").arg(wired.size()));
        return int(wired.size());
    }
    catch (const CommandError& e)
    {
        mStatus->setText(e.what());
        return 0;
    }
}

void ConnectPopover::swap()
{
    std::swap(mSource, mTarget);
    refill();
}

bool ConnectPopover::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape)
    {
        close();
        return true;
    }
    return QFrame::eventFilter(watched, event);
}

namespace connectPopover
{
ConnectPopover* showForSelection(QWidget* anchor)
{
    const auto& sel = Graph::current().selection();
    if (sel.size() != 2)
    {
        log::warn("Connect Attributes: select two objects, the source first");
        return nullptr;
    }
    QWidget* window = anchor ? anchor->window() : QApplication::activeWindow();
    auto* p = new ConnectPopover(sel[0], sel[1], window);
    p->setAttribute(Qt::WA_DeleteOnClose);
    p->adjustSize();
    if (window)
        p->move(window->mapToGlobal(window->rect().center()) - QPoint(p->width() / 2, p->height() / 2));
    p->show();
    return p;
}
} // namespace connectPopover

void registerAttributeUi()
{
    static bool done = false;
    if (std::exchange(done, true))
        return;
    actions::add({.id = "attributes.connect",
                  .label = "Connect Attributes...",
                  .category = "Edit",
                  .run = [] { connectPopover::showForSelection(nullptr); },
                  .enabled = [] { return Graph::current().selection().size() == 2; },
                  .doc = "Wire plugs between the two selected objects, the first as the source: pick pairs, or match by name.",
                  .python = "connectByName('source', 'target')",
                  .icon = "plug-connected"});
    menus::addActions("Edit", {"attributes.connect"});
    registerRelationshipsPanel();
    registerComponentSheet();
}
} // namespace lila::ui
