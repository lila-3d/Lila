#include "ui/DisplayLayersPanel.h"

#include "ops/Command.h"
#include "core/Log.h"
#include "scene/DisplayLayers.h"
#include "scene/Graph.h"
#include "ui/Actions.h"
#include "ui/Deletion.h"
#include "ui/Hotkeys.h"
#include "ui/Icons.h"
#include "ui/NodeLookup.h"

#include <QColorDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace lila::ui
{
namespace
{
namespace dl = scene::displayLayers;

enum Column
{
    kV,
    kP,
    kType,
    kSwatch,
    kName,
};

constexpr int kLayerRole = Qt::UserRole;

/** The layer the toolbar and the layers.* actions act on: the row last clicked. */
std::string& activeLayer()
{
    static std::string name;
    return name;
}

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

float toSrgb(double v)
{
    float f = std::clamp(float(v), 0.0f, 1.0f);
    return f <= 0.0031308f ? f * 12.92f : 1.055f * std::pow(f, 1.0f / 2.4f) - 0.055f;
}

double toLinear(double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }

QIcon swatch(const json& color, const QColor& frame)
{
    QPixmap pm(14, 14);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setPen(frame);
    if (color.is_array())
        p.setBrush(QColor::fromRgbF(toSrgb(color[0].get<double>()), toSrgb(color[1].get<double>()), toSrgb(color[2].get<double>())));
    p.drawRect(1, 1, 11, 11);
    return QIcon(pm);
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

/** The selection, or nothing to pass when empty (commands take the selection themselves). */
json selectionNames()
{
    scene::Graph& g = scene::Graph::current();
    json out = json::array();
    for (scene::NodeId id : g.selection())
        out.push_back(g.node(id)->path());
    return out;
}

void createFromSelected()
{
    json r = run("createDisplayLayer", {{"args", selectionNames()}});
    if (r.is_string())
        activeLayer() = r.get<std::string>();
}

void addSelected(const std::string& layer)
{
    if (layer.empty())
    {
        createFromSelected();
        return;
    }
    json args = selectionNames();
    if (args.empty())
        return;
    args.insert(args.begin(), layer);
    run("editDisplayLayerMembers", {{"args", args}});
}

void removeSelected()
{
    if (json args = selectionNames(); !args.empty())
        run("removeFromDisplayLayer", {{"args", args}});
}

const std::string& validActive()
{
    scene::Node* n = findNode(activeLayer());
    if (!n || !dl::isLayer(*n))
    {
        std::vector<scene::Node*> layers = dl::all(scene::Graph::current());
        activeLayer() = layers.empty() ? std::string() : layers.front()->path();
    }
    return activeLayer();
}
} // namespace

DisplayLayersPanel::DisplayLayersPanel(QWidget* parent) : QWidget(parent)
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
    add("plus", "Create an empty layer", [] {
        if (json r = run("createDisplayLayer", {{"empty", true}}); r.is_string())
            activeLayer() = r.get<std::string>();
    });
    add("stack-plus", "Create a layer from the selected objects", [] { createFromSelected(); });
    add("square-plus", "Add the selected objects to the current layer", [this] { addSelected(currentLayer()); });
    add("stack-minus", "Remove the selected objects from their layers", [] { removeSelected(); });
    add("arrow-up", "Move the current layer up", [this] {
        if (QTreeWidgetItem* it = mTree->currentItem(); it && mTree->indexOfTopLevelItem(it) > 0)
            run("reorderDisplayLayer", {{"args", {currentLayer()}}, {"index", mTree->indexOfTopLevelItem(it) - 1}});
    });
    add("arrow-down", "Move the current layer down", [this] {
        if (QTreeWidgetItem* it = mTree->currentItem())
            run("reorderDisplayLayer", {{"args", {currentLayer()}}, {"index", mTree->indexOfTopLevelItem(it) + 1}});
    });
    bar->addStretch(1);
    add("trash", "Delete the current layer (its objects stay)", [this] {
        if (std::string layer = currentLayer(); !layer.empty())
            run("deleteDisplayLayer", {{"args", {layer}}});
    });
    v->addLayout(bar);

    mTree = new QTreeWidget(this);
    mTree->setColumnCount(5);
    mTree->setHeaderHidden(true);
    mTree->setRootIsDecorated(false);
    mTree->setIndentation(0);
    mTree->setUniformRowHeights(true);
    mTree->setSelectionMode(QAbstractItemView::SingleSelection);
    mTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    mTree->setContextMenuPolicy(Qt::CustomContextMenu);
    mTree->setIconSize(QSize(14, 14));
    Hotkeys::passThrough(mTree);
    deletion::claim(mTree, [this] {
        if (std::string layer = currentLayer(); !layer.empty())
            run("deleteDisplayLayer", {{"args", {layer}}});
    });
    QHeaderView* header = mTree->header();
    header->setStretchLastSection(true);
    header->setMinimumSectionSize(20);
    for (int c : {kV, kP, kType, kSwatch})
    {
        header->setSectionResizeMode(c, QHeaderView::Fixed);
        header->resizeSection(c, 22);
    }
    v->addWidget(mTree, 1);

    connect(mTree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int column) { clicked(item, column); });
    connect(mTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int column) {
        if (column == kName)
            rename(item);
    });
    connect(mTree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (mFilling || column != kName)
            return;
        const std::string old = item->data(kName, kLayerRole).toString().toStdString();
        const std::string name = item->text(kName).trimmed().toStdString();
        if (name.empty() || name == old)
        {
            scheduleRefresh();
            return;
        }
        json r = run("displayLayer", {{"args", {old}}, {"name", name}});
        if (r.is_object() && activeLayer() == old)
            activeLayer() = r["name"].get<std::string>();
        scheduleRefresh();
    });
    connect(mTree->itemDelegate(), &QAbstractItemDelegate::closeEditor, this, [this] { mEditing = false; });
    connect(mTree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& at) { contextMenu(at); });

    QPointer<DisplayLayersPanel> self(this);
    mGraphListener = scene::Graph::current().addListener([self](scene::Change c, scene::NodeId id) {
        if (!self || c == scene::Change::Selection)
            return;
        // Dragging an object sends a stream of value changes that never touch a layer row.
        if (c == scene::Change::Value)
            if (const scene::Node* n = scene::Graph::current().node(id); !n || !dl::isLayer(*n))
                return;
        self->scheduleRefresh();
    });
    refresh();
}

DisplayLayersPanel::~DisplayLayersPanel() { scene::Graph::current().removeListener(mGraphListener); }

void DisplayLayersPanel::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    refresh();
}

void DisplayLayersPanel::scheduleRefresh()
{
    if (mPending || !isVisible())
        return;
    mPending = true;
    QTimer::singleShot(0, this, [this] {
        mPending = false;
        // A rebuild would end a rename in progress; it runs when the edit closes.
        if (mEditing)
        {
            QTimer::singleShot(200, this, [this] { scheduleRefresh(); });
            return;
        }
        refresh();
    });
}

void DisplayLayersPanel::refresh()
{
    scene::Graph& g = scene::Graph::current();
    const QColor dim = palette().color(QPalette::Disabled, QPalette::Text);
    const QColor frame = palette().color(QPalette::Mid);
    const std::string active = validActive();

    mFilling = true;
    mTree->setUpdatesEnabled(false);
    mTree->clear();
    for (const scene::Node* layer : dl::all(g))
    {
        const json info = dl::info(g, layer->id());
        const QString name = QString::fromStdString(layer->name());
        auto* item = new QTreeWidgetItem(mTree);
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        item->setData(kName, kLayerRole, QString::fromStdString(layer->path()));
        item->setText(kName, name);
        item->setToolTip(kName, QString("%1 object(s). Click to select them, double-click to rename.").arg(info["members"].size()));

        const bool visible = info["visibility"].get<bool>();
        item->setIcon(kV, icon(visible ? "eye" : "eye-off"));
        item->setToolTip(kV, "V: visibility");

        const bool p = info["hideOnPlayback"].get<bool>();
        item->setText(kP, p ? "" : "P");
        item->setTextAlignment(kP, Qt::AlignCenter);
        item->setToolTip(kP, p ? "P: hidden while the animation plays" : "P: shown while the animation plays");

        const std::string type = info["displayType"].get<std::string>();
        item->setText(kType, type == "template" ? "T" : type == "reference" ? "R" : "");
        item->setTextAlignment(kType, Qt::AlignCenter);
        item->setToolTip(kType, "Display type: blank normal, T template (grey wire, not pickable), R reference (not pickable). Click to cycle.");

        item->setIcon(kSwatch, swatch(info["color"], frame));
        item->setToolTip(kSwatch, info["color"].is_null() ? "No color: click to pick one" : "Wireframe color: click to change");

        if (!visible)
            item->setForeground(kName, dim);
        if (layer->path() == active)
            mTree->setCurrentItem(item);
    }
    mTree->setUpdatesEnabled(true);
    mFilling = false;
}

void DisplayLayersPanel::rename(QTreeWidgetItem* item)
{
    mEditing = true;
    mTree->editItem(item, kName);
}

std::string DisplayLayersPanel::currentLayer() const
{
    if (QTreeWidgetItem* it = mTree->currentItem())
        return it->data(kName, kLayerRole).toString().toStdString();
    return validActive();
}

void DisplayLayersPanel::clicked(QTreeWidgetItem* item, int column)
{
    const std::string layer = item->data(kName, kLayerRole).toString().toStdString();
    activeLayer() = layer;
    scene::Node* n = findNode(layer);
    if (!n || !dl::isLayer(*n))
        return;
    const json info = dl::info(scene::Graph::current(), n->id());
    switch (column)
    {
    case kV: run("displayLayer", {{"args", {layer}}, {"visibility", !info["visibility"].get<bool>()}}); break;
    case kP: run("displayLayer", {{"args", {layer}}, {"hideOnPlayback", !info["hideOnPlayback"].get<bool>()}}); break;
    case kType:
    {
        const std::string t = info["displayType"].get<std::string>();
        run("displayLayer", {{"args", {layer}}, {"displayType", t == "normal" ? "template" : t == "template" ? "reference" : "normal"}});
        break;
    }
    case kSwatch: pickColor(layer); break;
    default: run("selectDisplayLayerMembers", {{"args", {layer}}}); break;
    }
}

void DisplayLayersPanel::pickColor(const std::string& layer)
{
    scene::Node* n = findNode(layer);
    if (!n)
        return;
    const glm::dvec3 start = n->as<glm::dvec3>(scene::DisplayLayer::kColor);
    QColorDialog dlg(QColor::fromRgbF(toSrgb(start.x), toSrgb(start.y), toSrgb(start.z)), this);
    dlg.setWindowTitle("Layer Color");
    dlg.setOption(QColorDialog::DontUseNativeDialog);
    if (dlg.exec() != QDialog::Accepted || !dlg.selectedColor().isValid())
        return;
    const QColor c = dlg.selectedColor();
    run("displayLayer", {{"args", {layer}}, {"color", {toLinear(c.redF()), toLinear(c.greenF()), toLinear(c.blueF())}}});
}

void DisplayLayersPanel::contextMenu(const QPoint& at)
{
    QTreeWidgetItem* item = mTree->itemAt(at);
    QMenu menu(this);
    if (!item)
    {
        menu.addAction(icon("plus"), "Create Empty Layer", [] { run("createDisplayLayer", {{"empty", true}}); });
        menu.addAction(icon("stack-plus"), "Create Layer from Selected", [] { createFromSelected(); });
        menu.exec(mTree->viewport()->mapToGlobal(at));
        return;
    }
    const std::string layer = item->data(kName, kLayerRole).toString().toStdString();
    activeLayer() = layer;
    scene::Node* n = findNode(layer);
    if (!n)
        return;
    const json info = dl::info(scene::Graph::current(), n->id());
    const int row = mTree->indexOfTopLevelItem(item);

    menu.addAction("Select Objects", [layer] { run("selectDisplayLayerMembers", {{"args", {layer}}}); });
    menu.addAction(icon("square-plus"), "Add Selected Objects", [layer] { addSelected(layer); });
    menu.addAction(icon("stack-minus"), "Remove Selected Objects", [] { removeSelected(); });
    menu.addSeparator();
    const std::string type = info["displayType"].get<std::string>();
    for (const char* t : {"normal", "template", "reference"})
    {
        QAction* a = menu.addAction(QString(t).replace(0, 1, QString(t).at(0).toUpper()),
                                    [layer, t] { run("displayLayer", {{"args", {layer}}, {"displayType", t}}); });
        a->setCheckable(true);
        a->setChecked(type == t);
    }
    menu.addSeparator();
    menu.addAction("Set Color...", [this, layer] { pickColor(layer); });
    QAction* noColor = menu.addAction("No Color", [layer] { run("displayLayer", {{"args", {layer}}, {"useColor", false}}); });
    noColor->setEnabled(!info["color"].is_null());
    menu.addSeparator();
    menu.addAction(icon("arrow-up"), "Move Up", [layer, row] { run("reorderDisplayLayer", {{"args", {layer}}, {"index", std::max(0, row - 1)}}); })
        ->setEnabled(row > 0);
    menu.addAction(icon("arrow-down"), "Move Down", [layer, row] { run("reorderDisplayLayer", {{"args", {layer}}, {"index", row + 1}}); })
        ->setEnabled(row + 1 < mTree->topLevelItemCount());
    menu.addAction(icon("pencil"), "Rename", [this, item] { rename(item); });
    menu.addSeparator();
    menu.addAction(icon("trash"), "Delete Layer", [layer] { run("deleteDisplayLayer", {{"args", {layer}}}); });
    menu.exec(mTree->viewport()->mapToGlobal(at));
}

void registerDisplayLayerActions()
{
    actions::add({.id = "layers.create", .label = "Create Empty Display Layer", .category = "Display",
                  .doc = "A new display layer with no objects in it.", .python = "lila.ops.createDisplayLayer(empty=True)",
                  .command = "createDisplayLayer", .call = {{"empty", true}}});
    actions::add({.id = "layers.createFromSelected", .label = "Create Display Layer from Selected", .category = "Display",
                  .run = [] { createFromSelected(); },
                  .doc = "A new display layer holding the selected objects, taken out of the layers they were in.",
                  .python = "lila.ops.createDisplayLayer()"});
    actions::add({.id = "layers.addSelected", .label = "Add Selected to Display Layer", .category = "Display",
                  .run = [] { addSelected(validActive()); },
                  .doc = "Moves the selected objects into the display layer last clicked in the Display Layers panel.",
                  .python = "lila.ops.editDisplayLayerMembers(\"layer1\")"});
    actions::add({.id = "layers.removeSelected", .label = "Remove Selected from Display Layers", .category = "Display",
                  .run = [] { removeSelected(); }, .doc = "Takes the selected objects out of their display layers.",
                  .python = "lila.ops.removeFromDisplayLayer()"});
}
} // namespace lila::ui
