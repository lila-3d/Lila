#include "ui/OffsetsPanel.h"

#include "ops/Command.h"
#include "core/Log.h"
#include "scene/AnimLayer.h"
#include "scene/Graph.h"
#include "scene/Offsets.h"
#include "ui/Actions.h"
#include "ui/Deletion.h"
#include "ui/NodeLookup.h"
#include "ui/Hotkeys.h"
#include "ui/Icons.h"
#include "ui/Panels.h"

#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPointer>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace lila::ui
{
namespace
{
enum Column
{
    kMute,
    kWeight,
    kName,
    kInfo,
};

constexpr int kOffsetRole = Qt::UserRole;

json run(const char* command, const json& call)
{
    try
    {
        return ops::execute(command, call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::warn("{}", e.what());
        return nullptr;
    }
}

QToolButton* toolButton(QWidget* parent, const char* iconName, const QString& tip)
{
    auto* b = new QToolButton(parent);
    b->setIcon(icon(iconName));
    b->setToolTip(tip);
    b->setAutoRaise(true);
    b->setIconSize(QSize(16, 16));
    return b;
}

/** The active layer when it is an offset, for the actions that work without the panel. */
std::string activeOffset()
{
    scene::Graph& g = scene::Graph::current();
    scene::NodeId l = scene::anim::activeLayer(g);
    return l && scene::offsets::isOffsetLayer(g, l) ? g.node(l)->path() : std::string();
}

QString infoText(const json& o)
{
    QStringList joints;
    for (const json& j : o["joints"])
        joints << QString::fromStdString(j.get<std::string>());
    QString range = o["range"].is_array() ? QString("%1-%2, ease %3").arg(o["range"][0].get<double>()).arg(o["range"][1].get<double>()).arg(o["ease"].get<double>())
                                          : QString("whole take");
    return QString("%1 | %2 | %3").arg(joints.join(", "), QString::fromStdString(o["space"].get<std::string>()), range);
}
} // namespace

OffsetsPanel::OffsetsPanel(QWidget* parent) : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(2, 2, 2, 2);
    v->setSpacing(2);

    auto* bar = new QHBoxLayout;
    bar->setContentsMargins(0, 0, 0, 0);
    bar->setSpacing(1);
    auto add = [&](const char* iconName, const QString& tip, auto fn) {
        QToolButton* b = toolButton(this, iconName, tip);
        connect(b, &QToolButton::clicked, this, fn);
        bar->addWidget(b);
    };
    add("plus", "Offset the selected joints (option box: right-click the action in a menu)", [] { actions::run(json("anim.createOffset")); });
    add("pointer", "Select the current offset's joints", [this] {
        std::string o = currentOffset();
        json list = run("listOffsets", json::object());
        if (o.empty() || !list.is_array())
            return;
        for (const json& e : list)
            if (e["name"] == o)
                run("select", {{"args", e["joints"]}});
    });
    add("arrows-join", "Bake the current offset into the layer below", [this] {
        if (std::string o = currentOffset(); !o.empty())
            run("bakeOffset", {{"args", {o}}});
    });
    bar->addStretch(1);
    add("trash", "Delete the current offset (the animation underneath is untouched)", [this] {
        if (std::string o = currentOffset(); !o.empty())
            run("deleteOffset", {{"args", {o}}});
    });
    v->addLayout(bar);

    mTree = new QTreeWidget(this);
    deletion::claim(mTree, [this] {
        if (std::string o = currentOffset(); !o.empty())
            run("deleteOffset", {{"args", {o}}});
    });
    mTree->setColumnCount(4);
    mTree->setHeaderHidden(true);
    mTree->setRootIsDecorated(false);
    mTree->setIndentation(0);
    mTree->setUniformRowHeights(true);
    mTree->setSelectionMode(QAbstractItemView::SingleSelection);
    mTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    Hotkeys::passThrough(mTree);
    QHeaderView* header = mTree->header();
    header->setStretchLastSection(true);
    header->setSectionResizeMode(kMute, QHeaderView::Fixed);
    header->resizeSection(kMute, 22);
    header->setSectionResizeMode(kWeight, QHeaderView::Fixed);
    header->resizeSection(kWeight, 56);
    header->setSectionResizeMode(kName, QHeaderView::ResizeToContents);
    v->addWidget(mTree, 1);

    connect(mTree, &QTreeWidget::itemClicked, this, [](QTreeWidgetItem* item, int column) {
        const std::string o = item->data(kName, kOffsetRole).toString().toStdString();
        scene::Node* n = findNode(o);
        if (!n)
            return;
        if (column == kMute)
            run("editOffset", {{"args", {o}}, {"mute", !n->as<bool>(scene::anim::LayerNode::kMute)}});
        else if (column == kName || column == kInfo)
            run("setActiveAnimLayer", {{"args", {o}}});
    });

    QPointer<OffsetsPanel> self(this);
    mGraphListener = scene::Graph::current().addListener([self](scene::Change c, scene::NodeId id) {
        if (!self || c == scene::Change::Selection)
            return;
        if (c == scene::Change::Value)
            if (const scene::Node* n = scene::Graph::current().node(id); !n || !scene::anim::isLayer(*n))
                return;
        self->scheduleRefresh();
    });
    refresh();
}

OffsetsPanel::~OffsetsPanel() { scene::Graph::current().removeListener(mGraphListener); }

void OffsetsPanel::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    refresh();
}

void OffsetsPanel::scheduleRefresh()
{
    if (mPending || !isVisible())
        return;
    mPending = true;
    QTimer::singleShot(0, this, [this] {
        mPending = false;
        refresh();
    });
}

void OffsetsPanel::refresh()
{
    json list = ops::execute("listOffsets", json::object(), CallSource::Script);
    if (!list.is_array())
        return;
    const QColor dim = palette().color(QPalette::Disabled, QPalette::Text);
    const std::string active = activeOffset();
    mTree->setUpdatesEnabled(false);
    mTree->clear();
    for (auto it = list.rbegin(); it != list.rend(); ++it)
    {
        const json& o = *it;
        const std::string name = o["name"].get<std::string>();
        auto* item = new QTreeWidgetItem(mTree);
        item->setData(kName, kOffsetRole, QString::fromStdString(name));
        item->setText(kName, QString::fromStdString(name));
        item->setText(kInfo, infoText(o));
        item->setToolTip(kName, "Click to pose on this offset: the whole take follows.");
        item->setToolTip(kInfo, infoText(o));
        item->setText(kMute, "M");
        item->setTextAlignment(kMute, Qt::AlignCenter);
        item->setToolTip(kMute, "Mute");
        const bool mute = o["mute"].get<bool>();
        if (!mute)
            item->setForeground(kMute, dim);
        else
        {
            item->setForeground(kName, dim);
            item->setForeground(kInfo, dim);
        }
        QFont f = item->font(kName);
        f.setBold(name == active);
        item->setFont(kName, f);

        auto* weight = new QDoubleSpinBox(mTree);
        weight->setRange(0.0, 1.0);
        weight->setSingleStep(0.05);
        weight->setDecimals(2);
        weight->setFrame(false);
        weight->setButtonSymbols(QAbstractSpinBox::NoButtons);
        weight->setValue(o["weight"].get<double>());
        weight->setToolTip("Offset weight");
        connect(weight, &QDoubleSpinBox::editingFinished, this, [weight, name] { run("editOffset", {{"args", {name}}, {"weight", weight->value()}}); });
        mTree->setItemWidget(item, kWeight, weight);
        if (name == active)
            mTree->setCurrentItem(item);
    }
    mTree->setUpdatesEnabled(true);
}

std::string OffsetsPanel::currentOffset() const
{
    if (QTreeWidgetItem* it = mTree->currentItem())
        return it->data(kName, kOffsetRole).toString().toStdString();
    return {};
}

void registerOffsetsPanel()
{
    static bool done = false;
    if (std::exchange(done, true))
        return;
    actions::add({.id = "anim.createOffset",
                  .label = "Create Offset",
                  .category = "Animation",
                  .doc = "Offset the selected joints for polish on a new additive layer: one adjustable value per joint, "
                         "constant over the take or eased over a range. Pose on it at any frame and the whole take follows.",
                  .python = "createOffset()",
                  .command = "createOffset"});
    actions::add({.id = "anim.bakeOffset",
                  .label = "Bake Offset",
                  .category = "Animation",
                  .run =
                      [] {
                          if (std::string o = activeOffset(); !o.empty())
                              run("bakeOffset", {{"args", {o}}});
                      },
                  .enabled = [] { return !activeOffset().empty(); },
                  .doc = "Merge the active offset into the layer below it, sampled every frame."});
    actions::add({.id = "anim.deleteOffset",
                  .label = "Delete Offset",
                  .category = "Animation",
                  .run =
                      [] {
                          if (std::string o = activeOffset(); !o.empty())
                              run("deleteOffset", {{"args", {o}}});
                      },
                  .enabled = [] { return !activeOffset().empty(); },
                  .doc = "Delete the active offset; the animation underneath is untouched."});
    panels::add({.id = kOffsetsDock,
                 .title = "Offsets",
                 .create = [](QWidget* p) -> QWidget* { return new OffsetsPanel(p); },
                 .area = Qt::RightDockWidgetArea,
                 .icon = "adjustments",
                 .open = false,
                 .toggleAction = "window.offsets"});
}
} // namespace lila::ui
