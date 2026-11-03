#include "ui/UnfrozenTransforms.h"

#include "core/Log.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "ui/Actions.h"
#include "ui/GizmoMath.h"
#include "ui/Hotkeys.h"
#include "ui/Panels.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>
#include <unordered_set>

namespace lila::ui
{
namespace
{
enum Scope
{
    kSelection,
    kHierarchies,
    kScene,
};

enum Column
{
    kName,
    kTranslate,
    kRotate,
    kScale,
};

constexpr int kIdRole = Qt::UserRole;

QPointer<UnfrozenTransformsPanel> sPanel;

QString number(double v)
{
    QString s = QString::number(v, 'f', 3);
    while (s.endsWith('0'))
        s.chop(1);
    if (s.endsWith('.'))
        s.chop(1);
    return s == "-0" ? "0" : s;
}

QString triple(const nlohmann::json& v) { return number(v[0].get<double>()) + ", " + number(v[1].get<double>()) + ", " + number(v[2].get<double>()); }

/** The rotate snap presets, as the viewport toolbar offers them, smallest first. */
void addSteps(QComboBox* combo)
{
    const QString deg(QChar(0x00B0));
    std::vector<std::pair<double, QString>> steps;
    for (double v : gizmo::kRotateSteps)
        steps.push_back({v, number(v) + deg});
    for (int n : gizmo::kTurnDivisions)
    {
        const double v = gizmo::turnDivision(n);
        if (std::none_of(steps.begin(), steps.end(), [v](const auto& s) { return std::abs(s.first - v) < 1e-9; }))
            steps.push_back({v, number(v) + deg + "  (360/" + QString::number(n) + ")"});
    }
    std::sort(steps.begin(), steps.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    combo->addItem("Off", 0.0);
    for (const auto& [v, text] : steps)
        combo->addItem(text, v);
}

QStringList namesOf(const QList<QTreeWidgetItem*>& items)
{
    QStringList names;
    for (QTreeWidgetItem* item : items)
        names << item->text(kName);
    return names;
}

nlohmann::json toJson(const QStringList& names)
{
    nlohmann::json out = nlohmann::json::array();
    for (const QString& n : names)
        out.push_back(n.toStdString());
    return out;
}
} // namespace

UnfrozenTransformsPanel::UnfrozenTransformsPanel(QWidget* parent) : QWidget(parent)
{
    sPanel = this;
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(4);
    auto* form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(3);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    v->addLayout(form);

    auto row = [this](std::initializer_list<QWidget*> widgets) {
        auto* w = new QWidget(this);
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(6);
        for (QWidget* x : widgets)
            h->addWidget(x);
        h->addStretch(1);
        return w;
    };

    mScope = new QComboBox(this);
    mScope->addItems({"Selection", "Selection hierarchies", "Scene"});
    mScope->setCurrentIndex(kHierarchies);
    mScope->setToolTip("Selection: the objects loaded. Hierarchies: those and everything under them. Scene: every mesh, curve and group.");
    mLoad = new QPushButton("Load Selection", this);
    mLoad->setToolTip("Check the current selection from now on");
    form->addRow("Check", row({mScope, mLoad}));

    mTranslate = new QCheckBox("Translate", this);
    mRotate = new QCheckBox("Rotate", this);
    mScale = new QCheckBox("Scale", this);
    for (QCheckBox* c : {mTranslate, mRotate, mScale})
        c->setChecked(true);
    form->addRow("Channels", row({mTranslate, mRotate, mScale}));

    mStep = new QComboBox(this);
    addSteps(mStep);
    mStep->setToolTip("Rotations on whole multiples of this count as frozen");
    mInvert = new QCheckBox("Invert", this);
    mInvert->setToolTip("List only rotations on the step");
    form->addRow("Rotate step", row({mStep, mInvert}));

    mList = new QTreeWidget(this);
    mList->setColumnCount(4);
    mList->setHeaderLabels({"Object", "Translate", "Rotate", "Scale"});
    mList->setRootIsDecorated(false);
    mList->setUniformRowHeights(true);
    mList->setAllColumnsShowFocus(true);
    mList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    mList->header()->setStretchLastSection(false);
    mList->header()->setSectionResizeMode(kName, QHeaderView::Stretch);
    for (int c : {kTranslate, kRotate, kScale})
        mList->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    Hotkeys::passThrough(mList);
    v->addWidget(mList, 1);

    auto* footer = new QHBoxLayout;
    mCount = new QLabel(this);
    mCount->setForegroundRole(QPalette::PlaceholderText);
    mSelectAll = new QPushButton("Select All", this);
    mFreeze = new QPushButton("Freeze", this);
    mFreeze->setToolTip("Freeze Transformations on the selected rows");
    footer->addWidget(mCount);
    footer->addStretch(1);
    footer->addWidget(mSelectAll);
    footer->addWidget(mFreeze);
    v->addLayout(footer);

    mRefreshTimer = new QTimer(this);
    mRefreshTimer->setSingleShot(true);
    connect(mRefreshTimer, &QTimer::timeout, this, [this] { refresh(); });

    connect(mLoad, &QPushButton::clicked, this, [this] { loadSelection(); });
    connect(mScope, &QComboBox::currentIndexChanged, this, [this] { refresh(); });
    for (QCheckBox* c : {mTranslate, mRotate, mScale, mInvert})
        connect(c, &QCheckBox::toggled, this, [this] { refresh(); });
    connect(mStep, &QComboBox::currentIndexChanged, this, [this] { refresh(); });
    connect(mList, &QTreeWidget::itemSelectionChanged, this, [this] { selectRows(); });
    connect(mSelectAll, &QPushButton::clicked, mList, &QTreeWidget::selectAll);
    connect(mFreeze, &QPushButton::clicked, this, [this] {
        const QStringList names = namesOf(mList->selectedItems());
        if (names.isEmpty())
            return;
        try
        {
            ops::execute("freezeTransforms", {{"args", toJson(names)}}, CallSource::UI);
        }
        catch (const CommandError& e)
        {
            log::warn("{}", e.what());
        }
        refresh();
    });

    QPointer<UnfrozenTransformsPanel> self(this);
    mGraphListener = scene::Graph::current().addListener([self](scene::Change c, scene::NodeId id) {
        if (!self || !self->isVisible())
            return;
        if (c == scene::Change::Selection)
        {
            if (!std::exchange(self->mSyncPending, true))
                QTimer::singleShot(0, self, [self] {
                    self->mSyncPending = false;
                    self->syncFromScene();
                });
            return;
        }
        if (c == scene::Change::Connection || c == scene::Change::Attrs)
            return;
        if (c == scene::Change::Value)
        {
            // Settles first, so a drag or playback refreshes once it stops rather than every frame.
            const scene::Node* n = scene::Graph::current().node(id);
            if (n && n->type().isA("transform"))
                self->scheduleRefresh(150);
            return;
        }
        self->scheduleRefresh();
    });
    updateButtons();
}

UnfrozenTransformsPanel::~UnfrozenTransformsPanel() { scene::Graph::current().removeListener(mGraphListener); }

void UnfrozenTransformsPanel::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    refresh();
}

void UnfrozenTransformsPanel::loadSelection()
{
    scene::Graph& g = scene::Graph::current();
    mLoaded.clear();
    for (scene::NodeId id : g.selection())
        if (const scene::Node* n = g.node(id); n && n->type().isA("transform"))
            mLoaded.push_back(id);
    {
        const QSignalBlocker block(mScope);
        if (mLoaded.empty())
            mScope->setCurrentIndex(kScene);
        else if (mScope->currentIndex() == kScene)
            mScope->setCurrentIndex(kHierarchies);
    }
    refresh();
}

void UnfrozenTransformsPanel::scheduleRefresh(int delayMs)
{
    if (isVisible())
        mRefreshTimer->start(delayMs);
}

void UnfrozenTransformsPanel::refresh()
{
    mRefreshTimer->stop();
    mInvert->setEnabled(mStep->currentData().toDouble() > 0.0);
    const QString current = mList->currentItem() ? mList->currentItem()->text(kName) : QString();
    const int scroll = mList->verticalScrollBar()->value();
    mSyncing = true;
    mList->clear();

    auto placeholder = [this](const QString& text) {
        auto* item = new QTreeWidgetItem(mList, {text});
        item->setFlags(Qt::NoItemFlags);
        item->setFirstColumnSpanned(true);
        return item;
    };

    scene::Graph& g = scene::Graph::current();
    nlohmann::json call = {{"translate", mTranslate->isChecked()},
                           {"rotate", mRotate->isChecked()},
                           {"scale", mScale->isChecked()},
                           {"rotateStep", mStep->currentData().toDouble()},
                           {"invert", mInvert->isChecked()}};
    const int scope = mScope->currentIndex();
    if (scope == kScene)
        call["scene"] = true;
    else
    {
        nlohmann::json names = nlohmann::json::array();
        for (scene::NodeId id : mLoaded)
            if (const scene::Node* n = g.node(id))
                names.push_back(n->path());
        call["args"] = names;
        call["hierarchy"] = scope == kHierarchies;
        if (names.empty())
        {
            placeholder("Nothing loaded: select objects, then Load Selection");
            mCount->clear();
            mSyncing = false;
            updateButtons();
            return;
        }
    }

    nlohmann::json rows;
    try
    {
        rows = ops::execute("unfrozenTransforms", call, CallSource::Script);
    }
    catch (const CommandError& e)
    {
        placeholder(QString::fromStdString(e.what()))->setForeground(0, themeColor("statusError"));
        mCount->clear();
        mSyncing = false;
        updateButtons();
        return;
    }

    const QColor dim = palette().color(QPalette::Disabled, QPalette::Text);
    for (const nlohmann::json& r : rows)
    {
        const std::string name = r["name"].get<std::string>();
        auto* item = new QTreeWidgetItem(mList, {QString::fromStdString(name), triple(r["translate"]), triple(r["rotate"]), triple(r["scale"])});
        if (const scene::Node* n = g.find(name))
            item->setData(kName, kIdRole, QVariant::fromValue<qulonglong>(n->id()));
        // Channels that didn't put the object on the list read dimmer, so the reason stands out.
        const nlohmann::json& unfrozen = r["unfrozen"];
        for (auto [column, channel] : {std::pair{kTranslate, "translate"}, {kRotate, "rotate"}, {kScale, "scale"}})
            if (std::find(unfrozen.begin(), unfrozen.end(), channel) == unfrozen.end())
                item->setForeground(column, dim);
        if (item->text(kName) == current)
            mList->setCurrentItem(item, 0, QItemSelectionModel::NoUpdate);
    }
    if (rows.empty())
        placeholder("All frozen");
    mCount->setText(rows.empty() ? QString() : QString("%1 unfrozen").arg(rows.size()));
    mSyncing = false;
    syncFromScene();
    mList->verticalScrollBar()->setValue(scroll);
}

void UnfrozenTransformsPanel::selectRows()
{
    updateButtons();
    if (mSyncing)
        return;
    const QStringList names = namesOf(mList->selectedItems());
    try
    {
        if (names.isEmpty())
            ops::execute("select", {{"clear", true}}, CallSource::UI);
        else
            ops::execute("select", {{"args", toJson(names)}}, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::warn("{}", e.what());
    }
}

void UnfrozenTransformsPanel::syncFromScene()
{
    const std::vector<scene::NodeId>& sel = scene::Graph::current().selection();
    const std::unordered_set<scene::NodeId> selected(sel.begin(), sel.end());
    mSyncing = true;
    for (int i = 0; i < mList->topLevelItemCount(); ++i)
    {
        QTreeWidgetItem* item = mList->topLevelItem(i);
        const QVariant id = item->data(kName, kIdRole);
        item->setSelected(id.isValid() && selected.contains(id.toULongLong()));
    }
    mSyncing = false;
    updateButtons();
}

void UnfrozenTransformsPanel::updateButtons()
{
    bool any = false;
    for (int i = 0; i < mList->topLevelItemCount() && !any; ++i)
        any = mList->topLevelItem(i)->flags() & Qt::ItemIsSelectable;
    mSelectAll->setEnabled(any);
    mFreeze->setEnabled(!mList->selectedItems().isEmpty());
}

void registerUnfrozenTransformsPanel()
{
    static bool done = false;
    if (std::exchange(done, true))
        return;
    panels::add({.id = kUnfrozenTransformsDock,
                 .title = "Unfrozen Transforms",
                 .create = [](QWidget* p) -> QWidget* { return new UnfrozenTransformsPanel(p); },
                 .area = Qt::RightDockWidgetArea,
                 .icon = "snowflake-off",
                 .open = false,
                 .toggleAction = "window.unfrozenTransforms",
                 .floatFirst = true,
                 .floatWidth = 560,
                 .floatHeight = 520,
                 .docs = "essentials?id=find-unfrozen-transforms"});
    actions::add({.id = "modify.findUnfrozenTransforms",
                  .label = "Find Unfrozen Transforms",
                  .category = "Modify",
                  .run =
                      [] {
                          panels::show(kUnfrozenTransformsDock);
                          if (sPanel)
                              sPanel->loadSelection();
                      },
                  .doc = "Lists the selected objects and everything under them (or with nothing selected, every mesh, curve and group) whose translate, "
                         "rotate or scale is not identity. Filter by channel and by rotate step; click rows to select them.",
                  .python = "unfrozenTransforms()"});
}
} // namespace lila::ui
