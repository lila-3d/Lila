#include "ui/AnimLayersPanel.h"

#include "ops/Command.h"
#include "core/Log.h"
#include "scene/AnimLayer.h"
#include "scene/Graph.h"
#include "ui/Deletion.h"
#include "ui/Hotkeys.h"
#include "ui/Icons.h"
#include "ui/NodeLookup.h"
#include "ui/Panels.h"
#include "ui/UiCommands.h"
#include "ui/UiCommon.h"

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

constexpr int kLayerRole = Qt::UserRole;
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
    add("plus", "Create an empty layer", [] { runUi("createAnimLayer", {{"empty", true}}); });
    add("stack-plus", "Create a layer from the selected objects", [] { runUi("createAnimLayer", {{"args", selectionNames()}}); });
    add("square-plus", "Add the selected objects to the current layer", [this] {
        if (std::string l = currentLayer(); !l.empty() && !selectionNames().empty())
            runUi("addAnimLayerMembers", {{"args", selectionNames()}, {"layer", l}});
    });
    add("stack-minus", "Remove the selected objects from the current layer", [this] {
        if (std::string l = currentLayer(); !l.empty() && !selectionNames().empty())
            runUi("removeAnimLayerMembers", {{"args", selectionNames()}, {"layer", l}});
    });
    add("stack-pop", "Extract the selected objects' animation from the current layer to a new one", [this] {
        if (!selectionNames().empty())
        {
            std::string l = currentLayer();
            runUi("extractAnimLayer", {{"args", selectionNames()}, {"layer", l.empty() ? al::kBaseLayerName : l}});
        }
    });
    add("arrow-up", "Move the current layer up", [this] {
        if (std::string l = currentLayer(); !l.empty())
            runUi("reorderAnimLayer", {{"args", {l}}, {"by", 1}});
    });
    add("arrow-down", "Move the current layer down", [this] {
        if (std::string l = currentLayer(); !l.empty())
            runUi("reorderAnimLayer", {{"args", {l}}, {"by", -1}});
    });
    add("copy", "Duplicate the current layer", [this] {
        if (std::string l = currentLayer(); !l.empty())
            runUi("duplicateAnimLayer", {{"args", {l}}});
    });
    add("arrows-join", "Merge the current layer down", [this] {
        if (std::string l = currentLayer(); !l.empty())
            runUi("mergeAnimLayers", {{"args", {l}}});
    });
    bar->addStretch(1);
    add("trash", "Delete the current layer and its keys", [this] {
        if (std::string l = currentLayer(); !l.empty())
            runUi("deleteAnimLayer", {{"args", {l}}});
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
    deletion::claim(mTree, [this] {
        if (std::string l = currentLayer(); !l.empty() && l != al::kBaseLayerName)
            runUi("deleteAnimLayer", {{"args", {l}}});
    });
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
            runUi("editAnimLayer", {{"args", {layer}}, {"weight", weight->value()}});
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
        runUi("setActiveAnimLayer", {{"args", {al::kBaseLayerName}}});
        return;
    }
    scene::Node* n = findNode(layer);
    if (!n || !al::isLayer(*n))
        return;
    switch (column)
    {
    case kMute: runUi("editAnimLayer", {{"args", {layer}}, {"mute", !n->as<bool>(al::LayerNode::kMute)}}); break;
    case kSolo: runUi("editAnimLayer", {{"args", {layer}}, {"solo", !n->as<bool>(al::LayerNode::kSolo)}}); break;
    case kLock: runUi("editAnimLayer", {{"args", {layer}}, {"lock", !n->as<bool>(al::LayerNode::kLock)}}); break;
    case kMode:
        runUi("editAnimLayer", {{"args", {layer}}, {"mode", n->as<int64_t>(al::LayerNode::kMode) == 0 ? "override" : "additive"}});
        break;
    case kName: runUi("setActiveAnimLayer", {{"args", {layer}}}); break;
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
