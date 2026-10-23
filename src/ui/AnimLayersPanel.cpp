#include "ui/AnimLayersPanel.h"

#include "ops/Command.h"
#include "core/Log.h"
#include "scene/AnimLayer.h"
#include "scene/Graph.h"
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
namespace al = scene::anim;

enum Column
{
    kMute,
    kSolo,
    kLock,
    kMode,
    kWeight,
    kName,
};

constexpr int kLayerRole = Qt::UserRole;

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

json selectionNames()
{
    scene::Graph& g = scene::Graph::current();
    json out = json::array();
    for (scene::NodeId id : g.selection())
        out.push_back(g.node(id)->name());
    return out;
}
} // namespace

AnimLayersPanel::AnimLayersPanel(QWidget* parent) : QWidget(parent)
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
    add("plus", "Create an empty layer", [] { run("createAnimLayer", {{"empty", true}}); });
    add("stack-plus", "Create a layer from the selected objects", [] { run("createAnimLayer", {{"args", selectionNames()}}); });
    add("square-plus", "Add the selected objects to the current layer", [this] {
        if (std::string l = currentLayer(); !l.empty() && !selectionNames().empty())
            run("addAnimLayerMembers", {{"args", selectionNames()}, {"layer", l}});
    });
    add("stack-minus", "Remove the selected objects from the current layer", [this] {
        if (std::string l = currentLayer(); !l.empty() && !selectionNames().empty())
            run("removeAnimLayerMembers", {{"args", selectionNames()}, {"layer", l}});
    });
    add("stack-pop", "Extract the selected objects' animation from the current layer to a new one", [this] {
        if (!selectionNames().empty())
        {
            std::string l = currentLayer();
            run("extractAnimLayer", {{"args", selectionNames()}, {"layer", l.empty() ? al::kBaseLayerName : l}});
        }
    });
    add("arrow-up", "Move the current layer up", [this] {
        if (std::string l = currentLayer(); !l.empty())
            run("reorderAnimLayer", {{"args", {l}}, {"by", 1}});
    });
    add("arrow-down", "Move the current layer down", [this] {
        if (std::string l = currentLayer(); !l.empty())
            run("reorderAnimLayer", {{"args", {l}}, {"by", -1}});
    });
    add("copy", "Duplicate the current layer", [this] {
        if (std::string l = currentLayer(); !l.empty())
            run("duplicateAnimLayer", {{"args", {l}}});
    });
    add("arrows-join", "Merge the current layer down", [this] {
        if (std::string l = currentLayer(); !l.empty())
            run("mergeAnimLayers", {{"args", {l}}});
    });
    bar->addStretch(1);
    add("trash", "Delete the current layer and its keys", [this] {
        if (std::string l = currentLayer(); !l.empty())
            run("deleteAnimLayer", {{"args", {l}}});
    });
    v->addLayout(bar);

    mTree = new QTreeWidget(this);
    mTree->setColumnCount(6);
    mTree->setHeaderHidden(true);
    mTree->setRootIsDecorated(false);
    mTree->setIndentation(0);
    mTree->setUniformRowHeights(true);
    mTree->setSelectionMode(QAbstractItemView::SingleSelection);
    mTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    Hotkeys::passThrough(mTree);
    QHeaderView* header = mTree->header();
    header->setStretchLastSection(true);
    header->setMinimumSectionSize(20);
    for (int c : {kMute, kSolo, kLock, kMode})
    {
        header->setSectionResizeMode(c, QHeaderView::Fixed);
        header->resizeSection(c, 22);
    }
    header->setSectionResizeMode(kWeight, QHeaderView::Fixed);
    header->resizeSection(kWeight, 56);
    v->addWidget(mTree, 1);

    connect(mTree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int column) { clicked(item, column); });

    QPointer<AnimLayersPanel> self(this);
    mGraphListener = scene::Graph::current().addListener([self](scene::Change c, scene::NodeId id) {
        if (!self || c == scene::Change::Selection)
            return;
        if (c == scene::Change::Value)
            if (const scene::Node* n = scene::Graph::current().node(id); !n || !al::isLayer(*n))
                return;
        self->scheduleRefresh();
    });
    refresh();
}

AnimLayersPanel::~AnimLayersPanel() { scene::Graph::current().removeListener(mGraphListener); }

void AnimLayersPanel::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    refresh();
}

void AnimLayersPanel::scheduleRefresh()
{
    if (mPending || !isVisible())
        return;
    mPending = true;
    QTimer::singleShot(0, this, [this] {
        mPending = false;
        refresh();
    });
}

void AnimLayersPanel::refresh()
{
    const QColor dim = palette().color(QPalette::Disabled, QPalette::Text);
    json stack = ops::execute("listAnimLayers", json::object(), CallSource::Script);
    if (!stack.is_array())
        return;

    mFilling = true;
    mTree->setUpdatesEnabled(false);
    mTree->clear();
    auto flag = [&](QTreeWidgetItem* item, int col, const char* letter, bool on, const QString& tip) {
        item->setText(col, letter);
        item->setTextAlignment(col, Qt::AlignCenter);
        item->setToolTip(col, tip);
        if (!on)
            item->setForeground(col, dim);
    };
    for (auto it = stack.rbegin(); it != stack.rend(); ++it)
    {
        const json& l = *it;
        const QString name = QString::fromStdString(l["name"].get<std::string>());
        const bool base = l.value("base", false);
        auto* item = new QTreeWidgetItem(mTree);
        item->setData(kName, kLayerRole, base ? QString() : name);
        item->setText(kName, name);
        QFont f = item->font(kName);
        f.setBold(l["active"].get<bool>());
        item->setFont(kName, f);
        if (base)
        {
            item->setToolTip(kName, "The keys under every layer. Click to key here.");
            if (l["active"].get<bool>())
                mTree->setCurrentItem(item);
            continue;
        }
        item->setToolTip(kName, QString("%1 attribute(s). Click to key on this layer.").arg(l["members"].size()));
        flag(item, kMute, "M", l["mute"].get<bool>(), "Mute");
        flag(item, kSolo, "S", l["solo"].get<bool>(), "Solo: mute every layer that isn't soloed");
        item->setIcon(kLock, icon(l["lock"].get<bool>() ? "lock" : "lock-open"));
        item->setToolTip(kLock, "Lock: no keys or membership changes");
        const bool additive = l["mode"] == "additive";
        flag(item, kMode, additive ? "A" : "O", true, additive ? "Additive: adds on top. Click for override." : "Override: replaces by weight. Click for additive.");
        if (l["mute"].get<bool>())
            item->setForeground(kName, dim);

        auto* weight = new QDoubleSpinBox(mTree);
        weight->setRange(0.0, 1.0);
        weight->setSingleStep(0.05);
        weight->setDecimals(2);
        weight->setFrame(false);
        weight->setButtonSymbols(QAbstractSpinBox::NoButtons);
        weight->setValue(l["weight"].get<double>());
        weight->setToolTip("Layer weight");
        const std::string layer = name.toStdString();
        connect(weight, &QDoubleSpinBox::editingFinished, this, [weight, layer] {
            run("editAnimLayer", {{"args", {layer}}, {"weight", weight->value()}});
        });
        mTree->setItemWidget(item, kWeight, weight);
        if (l["active"].get<bool>())
            mTree->setCurrentItem(item);
    }
    mTree->setUpdatesEnabled(true);
    mFilling = false;
}

std::string AnimLayersPanel::currentLayer() const
{
    if (QTreeWidgetItem* it = mTree->currentItem())
        return it->data(kName, kLayerRole).toString().toStdString();
    return {};
}

void AnimLayersPanel::clicked(QTreeWidgetItem* item, int column)
{
    const std::string layer = item->data(kName, kLayerRole).toString().toStdString();
    if (layer.empty())
    {
        run("setActiveAnimLayer", {{"args", {al::kBaseLayerName}}});
        return;
    }
    scene::Graph& g = scene::Graph::current();
    scene::Node* n = g.find(layer);
    if (!n || !al::isLayer(*n))
        return;
    switch (column)
    {
    case kMute: run("editAnimLayer", {{"args", {layer}}, {"mute", !n->as<bool>(al::LayerNode::kMute)}}); break;
    case kSolo: run("editAnimLayer", {{"args", {layer}}, {"solo", !n->as<bool>(al::LayerNode::kSolo)}}); break;
    case kLock: run("editAnimLayer", {{"args", {layer}}, {"lock", !n->as<bool>(al::LayerNode::kLock)}}); break;
    case kMode:
        run("editAnimLayer", {{"args", {layer}}, {"mode", n->as<int64_t>(al::LayerNode::kMode) == 0 ? "override" : "additive"}});
        break;
    case kName: run("setActiveAnimLayer", {{"args", {layer}}}); break;
    default: break;
    }
}

void registerAnimLayersPanel()
{
    panels::add({.id = kAnimLayersDock,
                 .title = "Anim Layers",
                 .create = [](QWidget* p) -> QWidget* { return new AnimLayersPanel(p); },
                 .area = Qt::RightDockWidgetArea,
                 .icon = "stack-2",
                 .open = false,
                 .toggleAction = "window.animLayers"});
}
} // namespace lila::ui
