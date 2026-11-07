#include "ui/AnimLayersPanel.h"

#include "ops/Command.h"
#include "core/Log.h"
#include "scene/AnimLayer.h"
#include "scene/Graph.h"
#include "ui/Controls.h"
#include "ui/Deletion.h"
#include "ui/Hotkeys.h"
#include "ui/Icons.h"
#include "ui/NodeLookup.h"
#include "ui/Panels.h"
#include "ui/SpinScrub.h"
#include "ui/Theme.h"
#include "ui/UiCommands.h"
#include "ui/UiCommon.h"

#include <QApplication>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
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
    bar->setSpacing(2);
    auto onLayer = [this](auto fn) {
        return [this, fn] {
            if (std::string l = currentLayer(); !l.empty())
                fn(l);
        };
    };
    auto push = [&](const QString& text, const QString& tip, auto fn) {
        auto* b = new QToolButton(this);
        b->setText(text);
        b->setToolTip(tip);
        controls::setRole(b, "push");
        connect(b, &QToolButton::clicked, this, fn);
        bar->addWidget(b);
        return b;
    };
    QToolButton* create = push("New", "New Layer\nA layer holding the selected objects' animated channels", [] {
        runUi("createAnimLayer", {{"args", selectionNames()}});
    });
    create->setPopupMode(QToolButton::MenuButtonPopup);
    auto* createMenu = new QMenu(create);
    createMenu->addAction("New From Selection", [] { runUi("createAnimLayer", {{"args", selectionNames()}}); });
    createMenu->addAction("New Empty Layer", [] { runUi("createAnimLayer", {{"empty", true}}); });
    create->setMenu(createMenu);
    push("Add", "Add Selected\nAdd the selected objects to the current layer", onLayer([](const std::string& l) {
        if (!selectionNames().empty())
            runUi("addAnimLayerMembers", {{"args", selectionNames()}, {"layer", l}});
    }));
    push("Remove", "Remove Selected\nRemove the selected objects from the current layer", onLayer([](const std::string& l) {
        if (!selectionNames().empty())
            runUi("removeAnimLayerMembers", {{"args", selectionNames()}, {"layer", l}});
    }));

    auto* more = new QToolButton(this);
    more->setIcon(icon("dots"));
    more->setToolTip("More Layer Commands");
    more->setPopupMode(QToolButton::InstantPopup);
    controls::setRole(more, "overflow");
    auto* moreMenu = new QMenu(more);
    moreMenu->addAction(icon("stack-pop"), "Extract to New Layer", [this] {
        if (!selectionNames().empty())
        {
            std::string l = currentLayer();
            runUi("extractAnimLayer", {{"args", selectionNames()}, {"layer", l.empty() ? al::kBaseLayerName : l}});
        }
    })->setToolTip("Move the selected objects' animation from the current layer to a new one");
    moreMenu->addAction(icon("copy"), "Duplicate", onLayer([](const std::string& l) { runUi("duplicateAnimLayer", {{"args", {l}}}); }));
    moreMenu->addAction(icon("arrows-join"), "Merge Down", onLayer([](const std::string& l) { runUi("mergeAnimLayers", {{"args", {l}}}); }));
    moreMenu->addSeparator();
    moreMenu->addAction(icon("arrow-up"), "Move Up", onLayer([](const std::string& l) { runUi("reorderAnimLayer", {{"args", {l}}, {"by", 1}}); }));
    moreMenu->addAction(icon("arrow-down"), "Move Down", onLayer([](const std::string& l) { runUi("reorderAnimLayer", {{"args", {l}}, {"by", -1}}); }));
    moreMenu->setToolTipsVisible(true);
    more->setMenu(moreMenu);
    bar->addWidget(more);
    bar->addStretch(1);
    QToolButton* trash = toolButton(this, "trash", "Delete Layer\nDelete the current layer and its keys");
    connect(trash, &QToolButton::clicked, this, onLayer([](const std::string& l) { runUi("deleteAnimLayer", {{"args", {l}}}); }));
    bar->addWidget(trash);
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
    mEmpty = emptyStateLabel("No animation layers. Click New to make one from the selection.", this);
    mEmpty->setContentsMargins(8, 6, 8, 6);
    v->addWidget(mEmpty);

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

        auto* weight = new controls::SliderField(mTree);
        weight->setRange(0.0, 1.0);
        weight->setSingleStep(0.05);
        weight->setDecimals(2);
        weight->setValue(l["weight"].get<double>());
        weight->setToolTip("Weight");
        setSpinDefault(weight, 1.0);
        const std::string layer = name.toStdString();
        auto commit = [weight, layer] { runUi("editAnimLayer", {{"args", {layer}}, {"weight", weight->value()}}); };
        connect(weight, &QDoubleSpinBox::editingFinished, this, commit);
        // A middle-click reset changes the value with no edit to finish; a held button means a scrub still going.
        connect(weight, &QDoubleSpinBox::valueChanged, this, [weight, commit] {
            if (!weight->hasFocus() && QApplication::mouseButtons() == Qt::NoButton)
                commit();
        });
        mTree->setItemWidget(item, kWeight, weight);
        if (l["active"].get<bool>())
            mTree->setCurrentItem(item);
    }
    mTree->setUpdatesEnabled(true);
    mEmpty->setVisible(mTree->topLevelItemCount() <= 1);
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
