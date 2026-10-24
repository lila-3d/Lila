#include "ui/Outliner.h"

#include "ops/Command.h"
#include "core/Events.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "io/References.h"
#include "scene/Graph.h"
#include "scene/ObjectSets.h"
#include "ui/Actions.h"
#include "ui/Hotkeys.h"
#include "ui/IconBars.h"
#include "ui/Icons.h"
#include "ui/OutlinerLogic.h"
#include "ui/Theme.h"
#include "ui/ViewportToolbar.h"

#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QDrag>
#include <QMimeData>
#include <QDropEvent>
#include <QHash>
#include <QHeaderView>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace lila::ui
{
namespace
{
constexpr int kIdRole = Qt::UserRole + 1;
constexpr int kStyleRole = Qt::UserRole + 2; // last applied style, so restyle only touches rows that changed
constexpr int kUnloadedRefRole = Qt::UserRole + 3; // namespace of an unloaded reference's placeholder row
// Set rows carry the set's id here rather than in kIdRole, so clicking one selects its members, not the set node.
constexpr int kSetRole = Qt::UserRole + 4;
constexpr int kSetMemberRole = Qt::UserRole + 5; // a member row's member string, as setMembers lists it

std::vector<Outliner*> gOutliners;

/** The outliner under the cursor, else the first visible one. */
Outliner* activeOutliner()
{
    for (QWidget* w = QApplication::widgetAt(QCursor::pos()); w; w = w->parentWidget())
        if (auto* o = qobject_cast<Outliner*>(w))
            return o;
    for (Outliner* o : gOutliners)
        if (o->isVisible())
            return o;
    return nullptr;
}

/** A type icon, dimmed for hidden rows, with a chain-link badge in the corner for referenced ones. */
QIcon rowIcon(const QString& name, bool referenced, bool dim)
{
    static QHash<QString, QIcon> cache;
    QString key = name + (referenced ? "/r" : "") + (dim ? "/d" : "");
    if (auto it = cache.find(key); it != cache.end())
        return *it;
    QIcon base = icon(name);
    if (base.isNull())
        base = icon("point");
    QIcon::Mode mode = dim ? QIcon::Disabled : QIcon::Normal;
    QIcon out;
    for (int size : {16, 32})
    {
        QPixmap pm = base.pixmap(QSize(size, size), mode);
        if (referenced)
        {
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing);
            int b = size * 9 / 16;
            QRect badge(size - b, size - b, b, b);
            p.setPen(Qt::NoPen);
            p.setBrush(themeShade(40));
            p.drawEllipse(badge);
            p.drawPixmap(badge.adjusted(1, 1, -1, -1), icon("link").pixmap(QSize(b - 2, b - 2), QIcon::Active));
        }
        out.addPixmap(pm);
    }
    cache.insert(key, out);
    return out;
}

/** "|group1|pCube1", Maya's full DAG path. */
std::string dagPath(scene::Graph& g, scene::NodeId id)
{
    std::string out;
    for (scene::NodeId p = id; p != scene::kNoNode; p = g.parent(p))
        if (const scene::Node* n = g.node(p))
            out = "|" + n->name() + out;
    return out;
}

std::vector<scene::NodeId> selectedIds()
{
    return scene::Graph::current().selection();
}

json namesOf(const std::vector<scene::NodeId>& ids)
{
    json names = json::array();
    for (scene::NodeId id : ids)
        if (scene::Node* n = scene::Graph::current().node(id))
            names.push_back(n->path());
    return names;
}

void runCommand(const char* name, const json& call)
{
    try
    {
        ops::execute(name, call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

constexpr const char* kShowSetsPref = "/outliner/showSets";

bool showSets()
{
    return PrefStore::get().value<bool>("general", kShowSetsPref, true);
}

void copyToClipboard(bool fullPath)
{
    scene::Graph& g = scene::Graph::current();
    QStringList lines;
    for (scene::NodeId id : g.selection())
        if (scene::Node* n = g.node(id))
            lines << QString::fromStdString(fullPath ? dagPath(g, id) : n->name());
    if (!lines.isEmpty())
        QApplication::clipboard()->setText(lines.join('\n'));
}

void registerOutlinerActions()
{
    if (actions::find("outliner.revealSelected"))
        return;
    auto onOutliner = [](void (Outliner::*fn)()) {
        return [fn] {
            if (Outliner* o = activeOutliner())
                (o->*fn)();
        };
    };
    actions::add({"outliner.revealSelected", "Reveal Selected", "Outliner", onOutliner(&Outliner::revealSelected), nullptr,
                  "Expands the outliner down to the selected objects and scrolls to them (F over the outliner)."});
    actions::add({"outliner.rename", "Rename", "Outliner", onOutliner(&Outliner::renameSelected), nullptr,
                  "Edits the selected object's name in place in the outliner."});
    actions::add({"outliner.expandAll", "Expand All", "Outliner", onOutliner(&Outliner::expandEverything), nullptr,
                  "Opens every row of the outliner."});
    actions::add({"outliner.collapseAll", "Collapse All", "Outliner", onOutliner(&Outliner::collapseEverything), nullptr,
                  "Folds the outliner down to the top-level objects."});
    actions::add({.id = "outliner.toggleSets", .label = "Show Sets", .category = "Outliner",
                  .run =
                      [] {
                          PrefStore::get().set("general", kShowSetsPref, !showSets());
                          for (Outliner* o : gOutliners)
                              o->scheduleRebuild();
                      },
                  .doc = "Lists object sets after the hierarchy, each with its members under it.",
                  .python = "lila.prefs.set(\"general\", \"/outliner/showSets\", False)",
                  .checked = [] { return showSets(); }});
    actions::add({"outliner.copyName", "Copy Name", "Outliner", [] { copyToClipboard(false); }, nullptr,
                  "Puts the selected objects' names on the clipboard, one per line."});
    actions::add({"outliner.copyPath", "Copy Path", "Outliner", [] { copyToClipboard(true); }, nullptr,
                  "Puts the selected objects' full DAG paths (|group|object) on the clipboard, one per line."});
    actions::add({"select.children", "Select Children", "Select", [] {
                      scene::Graph& g = scene::Graph::current();
                      std::vector<scene::NodeId> kids;
                      for (scene::NodeId id : g.selection())
                          for (scene::NodeId c : g.children(id))
                              if (std::find(kids.begin(), kids.end(), c) == kids.end())
                                  kids.push_back(c);
                      if (!kids.empty())
                          runCommand("select", {{"args", namesOf(kids)}});
                  }, nullptr, "Replaces the selection with the direct children of the selected objects.",
                  "select(listRelatives(children=True))"});

    using iconBars::Kind;
    const std::string bar = iconBars::kOutliner;
    iconBars::add({.id = "outliner/rename", .bar = bar, .action = "window.rename", .icon = "forms", .label = "Batch Rename"});
    iconBars::add({.id = "outliner/naming", .bar = bar, .kind = Kind::Menu, .icon = "text-spellcheck", .label = "Naming Conventions",
                   .menuActions = {"edit.checkNames", "edit.conformNames", "", "naming.toggleEnforce"}});
    iconBars::add({.id = "outliner/-view", .bar = bar, .kind = Kind::Separator});
    iconBars::add({.id = "outliner/reveal", .bar = bar, .action = "outliner.revealSelected", .icon = "focus-2"});
    iconBars::add({.id = "outliner/expand", .bar = bar, .action = "outliner.expandAll", .icon = "fold-down"});
    iconBars::add({.id = "outliner/collapse", .bar = bar, .action = "outliner.collapseAll", .icon = "fold-up"});
    iconBars::add({.id = "outliner/show", .bar = bar, .kind = Kind::Menu, .icon = "eye", .label = "Show",
                   .menuActions = {"outliner.toggleSets"}});
    iconBars::add({.id = "outliner/-copy", .bar = bar, .kind = Kind::Separator});
    iconBars::add({.id = "outliner/copy", .bar = bar, .kind = Kind::Menu, .icon = "copy", .label = "Copy",
                   .menuActions = {"outliner.copyName", "outliner.copyPath"}});
}

/** Adds an action's menu item, labelled as the action (or label) with its hotkey, running it through run. */
QAction* addItem(QMenu* menu, const std::string& id, const std::function<void(const json&)>& run, const QString& label = {})
{
    const Action* a = actions::find(id);
    if (!a)
        return nullptr;
    QString text = label.isEmpty() ? QString::fromStdString(a->label) : label;
    QString key = Hotkeys::get().keyFor(id);
    QAction* item = menu->addAction(key.isEmpty() ? text : text + "\t" + key, [run, id] { run(json(id)); });
    item->setToolTip(QString::fromStdString(a->doc));
    return item;
}

/** The label that follows the cursor while rows are moved: the first name, and how many more. */
QPixmap movePixmap(const QStringList& names, const QFont& font, const QPalette& pal, qreal dpr)
{
    QString text = names.size() == 1 ? names.front() : QString("%1 +%2").arg(names.front()).arg(names.size() - 1);
    QFontMetrics fm(font);
    QSize size(fm.horizontalAdvance(text) + 12, fm.height() + 6);
    QPixmap pm(size * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(pal.color(QPalette::Highlight));
    p.drawRoundedRect(QRectF(QPointF(0, 0), QSizeF(size)), 3, 3);
    p.setFont(font);
    p.setPen(pal.color(QPalette::HighlightedText));
    p.drawText(QRect(QPoint(0, 0), size), Qt::AlignCenter, text);
    return pm;
}

const io::Reference* referenceById(uint32_t id)
{
    for (const io::Reference& r : io::refs::all())
        if (r.id == id)
            return &r;
    return nullptr;
}

void addReferenceItems(QMenu* menu, const std::string& ns, bool loaded)
{
    json args = {{"args", {ns}}};
    auto add = [&](const char* label, const char* command) {
        menu->addAction(label, [command, args] { actions::run(json{{"command", command}, {"args", args}}); });
    };
    if (loaded)
    {
        add("Reload Reference", "reloadReference");
        add("Unload Reference", "unloadReference");
        add("Import Objects from Reference", "importReference");
    }
    else
        add("Load Reference", "loadReference");
    menu->addSeparator();
    if (const Action* a = actions::find("window.referenceEditor"))
        menu->addAction(QString::fromStdString(a->label), [] { actions::run(json("window.referenceEditor")); });
}
} // namespace

Outliner::Outliner(QWidget* parent) : QTreeWidget(parent)
{
    gOutliners.push_back(this);
    registerOutlinerActions();
    for (events::Event e : {events::Event::SceneNew, events::Event::SceneOpened})
        mEventSubs.push_back(events::subscribe(e, [this](const events::EventData&) { mExpanded.clear(); }));

    setHeaderHidden(true);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    setUniformRowHeights(true);
    setIconSize(QSize(16, 16));
    setProperty("pickWalk", true);
    setProperty("hotkeyContext", "outliner");
    setProperty("hotkeyHover", true);
    Hotkeys::passThrough(this);
    // Drops land through dropEvent; drags start only from MMB (startMove), so an LMB drag selects instead.
    setDragDropMode(QAbstractItemView::InternalMove);
    setDragEnabled(false);
    setDefaultDropAction(Qt::MoveAction);
    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &QWidget::customContextMenuRequested, this, &Outliner::showContextMenu);

    mListener = scene::Graph::current().addListener([this](scene::Change c, scene::NodeId id) {
        const scene::Node* n = scene::Graph::current().node(id);
        if (c == scene::Change::Value && n && scene::objectSets::isSet(*n))
            scheduleRebuild();
        else if (c == scene::Change::Value || c == scene::Change::Connection)
            scheduleRestyle();
        else if (c == scene::Change::Selection)
            syncSelectionFromScene();
        else
            scheduleRebuild();
    });

    connect(this, &QTreeWidget::itemSelectionChanged, this, &Outliner::pushSelectionToScene);
    connect(this, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int) {
        if (mSyncing)
            return;
        auto id = item->data(0, kIdRole).toULongLong();
        if (!id)
            id = item->data(0, kSetRole).toULongLong();
        scene::Node* n = scene::Graph::current().node(id);
        std::string to = item->text(0).trimmed().toStdString();
        if (!n || to == n->name())
            return;
        if (to.empty())
            return scheduleRebuild();
        try
        {
            ops::execute("rename", {{"args", {n->path(), to}}}, CallSource::UI);
        }
        catch (const CommandError& e)
        {
            log::error("{}", e.what());
            scheduleRebuild();
        }
    });
    connect(this, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem* item) {
        if (mSyncing)
            return;
        if (scene::NodeId set = item->data(0, kSetRole).toULongLong())
            mOpenSets.erase(set);
        else
            mExpanded.erase(item->data(0, kIdRole).toULongLong());
    });
    connect(this, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* item) {
        if (mSyncing)
            return;
        if (scene::NodeId set = item->data(0, kSetRole).toULongLong())
            mOpenSets.insert(set);
        else
            mExpanded.insert(item->data(0, kIdRole).toULongLong());
    });
    connect(this, &QTreeWidget::itemClicked, this, [](QTreeWidgetItem* item, int) {
        if (scene::Node* set = scene::Graph::current().node(item->data(0, kSetRole).toULongLong()))
            runCommand("selectSetMembers", {{"args", {set->path()}}});
    });

    rebuild();
}

Outliner::~Outliner()
{
    std::erase(gOutliners, this);
    for (events::SubscriptionId id : mEventSubs)
        events::unsubscribe(id);
    scene::Graph::current().removeListener(mListener);
}

void Outliner::dropEvent(QDropEvent* e)
{
    e->setDropAction(Qt::IgnoreAction);
    e->accept();
    scene::Graph& g = scene::Graph::current();
    std::vector<scene::NodeId> moved = mMoving;
    std::erase_if(moved, [&](scene::NodeId id) { return !g.node(id); });
    if (moved.empty())
        return;

    QTreeWidgetItem* target = itemAt(e->position().toPoint());
    if (target && target->data(0, kSetRole).isValid())
    {
        if (scene::Node* set = g.node(target->data(0, kSetRole).toULongLong()))
        {
            json args = namesOf(moved);
            args.insert(args.begin(), set->path());
            runCommand("addToSet", {{"args", args}});
        }
        return;
    }
    scene::NodeId targetId = target ? scene::NodeId(target->data(0, kIdRole).toULongLong()) : scene::kNoNode;
    DropIndicatorPosition where = targetId != scene::kNoNode ? dropIndicatorPosition() : OnViewport;
    scene::NodeId parent = scene::kNoNode;
    scene::NodeId anchor = scene::kNoNode; // the sibling the moved rows go in front of; none = the end
    bool place = true;
    if (where == OnItem)
    {
        if (std::find(moved.begin(), moved.end(), targetId) != moved.end())
            return;
        parent = targetId;
        place = false;
    }
    else if (where == AboveItem)
    {
        parent = g.parent(targetId);
        anchor = targetId;
    }
    else if (where == BelowItem && target->isExpanded() && !g.children(targetId).empty())
    {
        // The line sits between the row and its first child.
        parent = targetId;
        anchor = g.children(targetId).front();
    }
    else if (where == BelowItem)
    {
        parent = g.parent(targetId);
        const auto& sib = parent == scene::kNoNode ? g.roots() : g.children(parent);
        auto it = std::find(sib.begin(), sib.end(), targetId);
        if (it != sib.end() && it + 1 != sib.end())
            anchor = *(it + 1);
    }

    json call = {{"args", namesOf(moved)}};
    if (parent != scene::kNoNode)
        call["args"].push_back(g.node(parent)->path());
    else
        call["world"] = true;
    if (place)
        call["index"] = outliner::dropIndex(parent == scene::kNoNode ? g.roots() : g.children(parent), moved, anchor);
    runCommand("parent", call);
    scheduleRebuild();
}

void Outliner::keyPressEvent(QKeyEvent* e)
{
    QWidget* editor = QApplication::focusWidget();
    // Enter commits the edit rather than cancelling it.
    bool renameKey = (Hotkeys::get().isKeyFor(e, "outliner.rename") || Hotkeys::get().isKeyFor(e, "edit.rename")) &&
                     e->key() != Qt::Key_Return && e->key() != Qt::Key_Enter;
    if (state() == EditingState && renameKey && editor && viewport()->isAncestorOf(editor))
    {
        closeEditor(editor, QAbstractItemDelegate::RevertModelCache);
        setFocus();
        return;
    }
    QTreeWidget::keyPressEvent(e);
}

void Outliner::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::MiddleButton)
    {
        mMoving.clear();
        mMovePress = e->position().toPoint();
        if (QTreeWidgetItem* item = itemAt(mMovePress))
            if (scene::NodeId id = item->data(0, kIdRole).toULongLong())
                mMoving = outliner::menuTargets(id, selectedIds());
        e->accept();
        return;
    }
    QTreeWidget::mousePressEvent(e);
    mDragSelecting = e->button() == Qt::LeftButton;
}

void Outliner::mouseMoveEvent(QMouseEvent* e)
{
    if (!mMoving.empty() && (e->buttons() & Qt::MiddleButton))
    {
        if ((e->position().toPoint() - mMovePress).manhattanLength() >= QApplication::startDragDistance())
            startMove();
        e->accept();
        return;
    }
    QTreeWidget::mouseMoveEvent(e);
}

void Outliner::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() == Qt::MiddleButton)
    {
        mMoving.clear();
        e->accept();
        return;
    }
    QTreeWidget::mouseReleaseEvent(e);
    if (e->button() == Qt::LeftButton)
    {
        mDragSelecting = false;
        if (mSelectionPending)
        {
            mSelectionPending = false;
            pushSelectionToScene();
        }
    }
}

void Outliner::startMove()
{
    // Top-to-bottom row order, so the rows land in the order they were shown.
    std::vector<scene::NodeId> rows;
    QModelIndexList indexes;
    QStringList names;
    for (QTreeWidgetItemIterator it(this); *it; ++it)
        if (scene::NodeId id = (*it)->data(0, kIdRole).toULongLong();
            id && std::find(mMoving.begin(), mMoving.end(), id) != mMoving.end())
        {
            rows.push_back(id);
            indexes << indexFromItem(*it);
            names << (*it)->text(0);
        }
    mMoving = rows;
    if (rows.empty())
        return;
    auto* drag = new QDrag(this);
    QMimeData* mime = model()->mimeData(indexes);
    QStringList nodeNames;
    for (scene::NodeId id : rows)
        if (scene::Node* n = scene::Graph::current().node(id))
            nodeNames << QString::fromStdString(n->path());
    mime->setData(kNodeNamesMime, nodeNames.join(QChar('\n')).toUtf8());
    drag->setMimeData(mime);
    drag->setPixmap(movePixmap(names, font(), palette(), devicePixelRatioF()));
    drag->setHotSpot(QPoint(-12, -4));
    drag->exec(Qt::MoveAction | Qt::LinkAction, Qt::MoveAction);
    mMoving.clear();
}

void Outliner::scheduleRebuild()
{
    if (mQueued)
        return;
    mQueued = true;
    QTimer::singleShot(0, this, [this] {
        mQueued = false;
        if (height() <= 0)
            mStale = true;
        else
            rebuild();
    });
}

void Outliner::resizeEvent(QResizeEvent* e)
{
    QTreeWidget::resizeEvent(e);
    if (mStale && height() > 0)
    {
        mStale = false;
        rebuild();
    }
}

void Outliner::scheduleRestyle()
{
    if (mRestyleQueued || mQueued)
        return;
    mRestyleQueued = true;
    QTimer::singleShot(0, this, [this] {
        mRestyleQueued = false;
        restyle();
    });
}

void Outliner::rebuild()
{
    LILA_ZONE();

    mStale = false;
    mSyncing = true;
    scene::Graph& g = scene::Graph::current();
    clear();
    mItems.clear();
    std::function<void(scene::NodeId, QTreeWidgetItem*)> add = [&](scene::NodeId id, QTreeWidgetItem* parent) {
        scene::Node* n = g.node(id);
        auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(this);
        item->setText(0, QString::fromStdString(n->name()));
        item->setData(0, kIdRole, QVariant::fromValue<qulonglong>(id));
        item->setFlags(item->flags() | Qt::ItemIsEditable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
        // Referenced names can't be edited.
        if (n->referenceId())
            item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        const std::string& doc = n->type().doc;
        item->setToolTip(0, QString::fromStdString(g.fullPath(id) + "\n" + n->type().name + (doc.empty() ? "" : "\n" + doc)));
        mItems[id] = item;
        for (scene::NodeId c : g.children(id))
            add(c, item);
    };
    for (scene::NodeId id : g.roots())
        add(id, nullptr);
    if (showSets())
        addSetRows();
    // An unloaded reference has no nodes, so it gets a placeholder row to load it back from.
    for (const io::Reference& r : io::refs::all())
        if (!r.loaded)
        {
            auto* item = new QTreeWidgetItem(this);
            item->setText(0, QString::fromStdString(r.ns + " (unloaded)"));
            item->setData(0, kUnloadedRefRole, QString::fromStdString(r.ns));
            item->setFlags(Qt::ItemIsEnabled);
            item->setIcon(0, rowIcon("link-off", false, true));
            QFont f = item->font(0);
            f.setItalic(true);
            item->setFont(0, f);
            item->setForeground(0, themeShade(110));
            item->setToolTip(0, QString::fromStdString("Unloaded reference\n" + r.path.generic_string()));
        }
    collapseAll();
    std::erase_if(mExpanded, [&](scene::NodeId id) { return !mItems.contains(id); });
    for (scene::NodeId id : mExpanded)
        mItems[id]->setExpanded(true);
    for (int i = 0; i < topLevelItemCount(); ++i)
        if (QTreeWidgetItem* top = topLevelItem(i); top->data(0, kSetRole).isValid())
            top->setExpanded(mOpenSets.contains(top->data(0, kSetRole).toULongLong()));
    applyFilter();
    mSyncing = false;
    restyle();
    syncSelectionFromScene();
}

void Outliner::addSetRows()
{
    scene::Graph& g = scene::Graph::current();
    std::erase_if(mOpenSets, [&](scene::NodeId id) { return !g.node(id); });
    for (const scene::Node* s : scene::objectSets::all(g))
    {
        const bool referenced = s->referenceId() != 0;
        const bool quick = s->as<bool>(scene::ObjectSet::kQuickSelect);
        auto* item = new QTreeWidgetItem(this);
        item->setText(0, QString::fromStdString(s->name()));
        item->setData(0, kSetRole, QVariant::fromValue<qulonglong>(s->id()));
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsDropEnabled | (referenced ? Qt::ItemFlags() : Qt::ItemIsEditable));
        item->setIcon(0, rowIcon(quick ? "pointer" : "list-details", referenced, false));
        QFont f = item->font(0);
        f.setItalic(referenced);
        item->setFont(0, f);
        item->setToolTip(0, QString::fromStdString(std::string(quick ? "Quick select set" : "Object set") +
                                                   "\nClick to select its members; drop rows on it to add them."));
        for (const std::string& m : scene::objectSets::format(g, scene::objectSets::members(g, s->id())))
        {
            auto* row = new QTreeWidgetItem(item);
            row->setText(0, QString::fromStdString(m));
            row->setData(0, kSetMemberRole, QString::fromStdString(m));
            row->setFlags(Qt::ItemIsEnabled);
            row->setForeground(0, themeShade(150));
        }
    }
}

void Outliner::showSetMenu(QTreeWidgetItem* item, const QPoint& globalPos)
{
    scene::Graph& g = scene::Graph::current();
    QTreeWidgetItem* setItem = item->data(0, kSetRole).isValid() ? item : item->parent();
    scene::Node* set = setItem ? g.node(setItem->data(0, kSetRole).toULongLong()) : nullptr;
    if (!set)
        return;
    const std::string name = set->path();
    const bool editable = set->referenceId() == 0;
    QMenu menu(this);
    menu.setToolTipsVisible(true);
    if (item->data(0, kSetMemberRole).isValid())
    {
        const std::string member = item->data(0, kSetMemberRole).toString().toStdString();
        menu.addAction(icon("pointer"), "Select Member", [member] { runCommand("select", {{"args", {member.substr(0, member.find('.'))}}}); });
        menu.addAction(icon("minus"), "Remove from Set", [name, member] { runCommand("removeFromSet", {{"args", {name, member}}}); });
        menu.exec(globalPos);
        return;
    }
    menu.addAction(icon("pointer"), "Select Set Members", [name] { runCommand("selectSetMembers", {{"args", {name}}}); });
    menu.addAction("Add Selection to Set", [name] { runCommand("addToSet", {{"args", {name}}}); });
    menu.addAction("Remove Selection from Set", [name] { runCommand("removeFromSet", {{"args", {name}}}); });
    menu.addSeparator();
    QAction* quick = menu.addAction("Quick Select Set", [name, on = !set->as<bool>(scene::ObjectSet::kQuickSelect)] {
        runCommand("setAttr", {{"args", {name + ".quickSelect", on}}});
    });
    quick->setCheckable(true);
    quick->setChecked(set->as<bool>(scene::ObjectSet::kQuickSelect));
    quick->setToolTip("List it under Edit > Quick Select Sets.");
    menu.addAction(icon("pencil"), "Rename", [this, setItem] { editItem(setItem, 0); })->setEnabled(editable);
    menu.addAction(icon("trash"), "Delete Set", [name] { runCommand("deleteSet", {{"args", {name}}}); });
    menu.exec(globalPos);
}

void Outliner::restyle()
{
    LILA_ZONE();

    scene::Graph& g = scene::Graph::current();
    const QColor normal = palette().color(QPalette::Text);
    for (auto& [id, item] : mItems)
    {
        scene::Node* n = g.node(id);
        if (!n)
            continue;
        bool referenced = n->referenceId() != 0;
        bool hidden = outliner::hiddenInHierarchy(g, id);
        bool kept = mKept.contains(id);
        QString iconName = QString::fromStdString(outliner::iconName(*n));
        QString style = iconName + (referenced ? "/r" : "") + (hidden ? "/h" : "") + (kept ? "/k" : "");
        if (item->data(0, kStyleRole).toString() == style)
            continue;
        item->setData(0, kStyleRole, style);
        item->setIcon(0, rowIcon(iconName, referenced, hidden));
        QFont f = item->font(0);
        f.setItalic(referenced);
        item->setFont(0, f);
        QColor c = hidden || kept ? themeShade(100) : referenced ? themeShade(150) : normal;
        item->setForeground(0, c);
    }
}

void Outliner::setFilter(outliner::Filter filter)
{
    mFilter = std::move(filter);
    rebuild();
}

void Outliner::applyFilter()
{
    LILA_ZONE();

    mKept.clear();
    if (mFilter.empty() || !mFilter.valid())
        return;
    scene::Graph& g = scene::Graph::current();
    outliner::FilterRows rows = outliner::filterRows(g, mFilter);
    mKept = std::move(rows.kept);
    for (auto& [id, item] : mItems)
        item->setHidden(!rows.matched.contains(id) && !mKept.contains(id));
    // Opened without being remembered in mExpanded, so clearing the filter folds the rows back as they were.
    for (scene::NodeId id : rows.matched)
        for (scene::NodeId p = g.parent(id); p != scene::kNoNode; p = g.parent(p))
            if (auto it = mItems.find(p); it != mItems.end())
                it->second->setExpanded(true);
    for (int i = 0; i < topLevelItemCount(); ++i)
    {
        QTreeWidgetItem* top = topLevelItem(i);
        if (top->data(0, kSetRole).isValid())
        {
            const scene::Node* set = g.node(top->data(0, kSetRole).toULongLong());
            top->setHidden(!set || !mFilter.matches(set->name(), g.fullPath(set->id()), &set->type()));
        }
        else if (top->data(0, kUnloadedRefRole).isValid())
            top->setHidden(true);
    }
}

void Outliner::syncSelectionFromScene()
{
    mSyncing = true;
    const auto& sel = scene::Graph::current().selection();
    for (QTreeWidgetItemIterator it(this); *it; ++it)
    {
        auto id = (*it)->data(0, kIdRole).toULongLong();
        (*it)->setSelected(id && std::find(sel.begin(), sel.end(), id) != sel.end());
    }
    shadeAncestors();
    mSyncing = false;
}

void Outliner::shadeAncestors()
{
    scene::Graph& g = scene::Graph::current();
    const std::set<scene::NodeId> chain = outliner::selectionAncestors(g, g.selection());
    // A step above the background and well below the selection highlight.
    const QBrush shade(themeShade(56));
    for (auto& [id, item] : mItems)
    {
        bool on = chain.contains(id);
        if (item->data(0, Qt::BackgroundRole).isValid() != on)
            item->setData(0, Qt::BackgroundRole, on ? QVariant(shade) : QVariant());
    }
}

void Outliner::pushSelectionToScene()
{
    if (mSyncing)
        return;
    if (mDragSelecting)
    {
        mSelectionPending = true;
        return;
    }
    // Selection order matters (parent, parent chain, constraints). Qt's selectedItems() isn't click order, so keep
    // the scene's existing order and append whatever was newly selected.
    scene::Graph& g = scene::Graph::current();
    std::vector<scene::NodeId> picked;
    for (QTreeWidgetItem* item : selectedItems())
        if (scene::NodeId id = item->data(0, kIdRole).toULongLong())
            picked.push_back(id);
    std::vector<scene::NodeId> ordered;
    for (scene::NodeId id : g.selection())
        if (std::find(picked.begin(), picked.end(), id) != picked.end())
            ordered.push_back(id);
    for (scene::NodeId id : picked)
        if (std::find(ordered.begin(), ordered.end(), id) == ordered.end())
            ordered.push_back(id);
    json names = namesOf(ordered);
    try
    {
        if (names.empty())
            ops::execute("select", {{"clear", true}}, CallSource::UI);
        else
            ops::execute("select", {{"args", names}}, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

void Outliner::revealSelected()
{
    scene::Graph& g = scene::Graph::current();
    QTreeWidgetItem* first = nullptr;
    for (scene::NodeId id : g.selection())
    {
        auto it = mItems.find(id);
        if (it == mItems.end())
            continue;
        for (scene::NodeId p : outliner::ancestors(g, id))
            if (auto pit = mItems.find(p); pit != mItems.end())
            {
                pit->second->setExpanded(true);
                mExpanded.insert(p);
            }
        if (!first)
            first = it->second;
    }
    if (first)
        scrollToItem(first, QAbstractItemView::PositionAtCenter);
}

void Outliner::renameSelected()
{
    scene::Graph& g = scene::Graph::current();
    if (g.selection().empty())
        return;
    auto it = mItems.find(g.selection().front());
    if (it == mItems.end() || !(it->second->flags() & Qt::ItemIsEditable))
        return;
    revealSelected();
    setCurrentItem(it->second, 0, QItemSelectionModel::NoUpdate);
    editItem(it->second, 0);
}

void Outliner::expandEverything()
{
    for (auto& [id, item] : mItems)
        if (item->childCount())
            mExpanded.insert(id);
    expandAll();
}

void Outliner::collapseEverything()
{
    collapseAll();
    mExpanded.clear();
}

void Outliner::showContextMenu(const QPoint& pos)
{
    scene::Graph& g = scene::Graph::current();
    QTreeWidgetItem* clickedItem = itemAt(pos);
    QMenu menu(this);
    menu.setToolTipsVisible(true);

    if (clickedItem && (clickedItem->data(0, kSetRole).isValid() || clickedItem->data(0, kSetMemberRole).isValid()))
    {
        showSetMenu(clickedItem, viewport()->mapToGlobal(pos));
        return;
    }
    if (clickedItem && clickedItem->data(0, kUnloadedRefRole).isValid())
    {
        addReferenceItems(&menu, clickedItem->data(0, kUnloadedRefRole).toString().toStdString(), false);
        menu.exec(viewport()->mapToGlobal(pos));
        return;
    }

    scene::NodeId clicked = clickedItem ? scene::NodeId(clickedItem->data(0, kIdRole).toULongLong()) : scene::kNoNode;
    const std::vector<scene::NodeId> targets = outliner::menuTargets(clicked, selectedIds());
    // Actions work on the selection, so a click outside it selects the clicked row first, when the item runs.
    auto run = [targets](const json& target) {
        scene::Graph& g = scene::Graph::current();
        std::vector<scene::NodeId> sel = g.selection();
        std::vector<scene::NodeId> want = targets;
        std::sort(sel.begin(), sel.end());
        std::sort(want.begin(), want.end());
        if (!want.empty() && sel != want)
            runCommand("select", {{"args", namesOf(targets)}});
        actions::run(target);
    };

    bool any = !targets.empty();
    bool anyParented = false, anyGroup = false, anyTransform = false, anyMesh = false, anyChildren = false;
    std::vector<uint32_t> refIds;
    for (scene::NodeId id : targets)
    {
        scene::Node* n = g.node(id);
        if (!n)
            continue;
        anyParented |= g.parent(id) != scene::kNoNode;
        anyGroup |= n->type().name == "transform" && !g.children(id).empty();
        anyTransform |= n->type().isA("transform");
        anyMesh |= n->type().isA("mesh");
        anyChildren |= !g.children(id).empty();
        if (n->referenceId() && std::find(refIds.begin(), refIds.end(), n->referenceId()) == refIds.end())
            refIds.push_back(n->referenceId());
    }
    auto item = [&](const std::string& id, bool enabled = true, const QString& label = {}) {
        if (QAction* a = addItem(&menu, id, run, label))
            a->setEnabled(enabled);
    };

    item("select.hierarchy", any && anyChildren);
    item("select.children", any && anyChildren);
    item("viewport.frameSelected", any, "Frame in Viewport");
    item("outliner.revealSelected", any);
    menu.addSeparator();
    item("outliner.rename", targets.size() == 1 && clickedItem && (clickedItem->flags() & Qt::ItemIsEditable));
    item("edit.duplicate", any);
    if (QAction* del = menu.addAction("Delete", [run] { run(json{{"command", "delete"}}); }))
        del->setEnabled(any);
    menu.addSeparator();
    item("edit.group", any);
    item("edit.ungroup", anyGroup);
    item("edit.unparent", anyParented);
    QAction* parentTo = menu.addAction("Parent To...", [this, targets] {
        scene::Graph& g = scene::Graph::current();
        std::set<scene::NodeId> excluded(targets.begin(), targets.end());
        for (scene::NodeId t : targets)
            for (scene::NodeId d : g.descendants(t))
                excluded.insert(d);
        QStringList candidates;
        for (QTreeWidgetItemIterator it(this); *it; ++it)
            if (scene::NodeId id = (*it)->data(0, kIdRole).toULongLong(); id && !excluded.contains(id))
                candidates << (*it)->text(0);
        bool ok = false;
        QString parentName = QInputDialog::getItem(this, "Parent To", "Parent", candidates, 0, true, &ok);
        if (!ok || parentName.isEmpty())
            return;
        json args = namesOf(targets);
        args.push_back(parentName.toStdString());
        actions::run(json{{"command", "parent"}, {"args", {{"args", args}}}});
    });
    parentTo->setEnabled(any);
    menu.addSeparator();
    item("display.hide", any);
    item("display.show", any);
    item("display.toggleVisibility", any);
    item("viewport.isolate", any);
    menu.addSeparator();
    if (anyTransform)
    {
        item("modify.freezeTransforms");
        item("modify.centerPivot");
    }
    if (anyMesh)
    {
        item("mesh.bakeModifierStack");
        QMenu* mats = nullptr;
        for (scene::NodeId id : g.nodes())
            if (scene::Node* m = g.node(id); m && m->type().name == "material")
            {
                if (!mats)
                    mats = menu.addMenu("Assign Material");
                std::string name = m->path();
                mats->addAction(QString::fromStdString(m->name()), [run, name, targets] {
                    json args = namesOf(targets);
                    args.insert(args.begin(), name);
                    run(json{{"command", "assignMaterial"}, {"args", {{"args", args}}}});
                });
            }
    }
    if (anyTransform || anyMesh)
        menu.addSeparator();
    for (uint32_t refId : refIds)
        if (const io::Reference* r = referenceById(refId))
        {
            QMenu* sub = menu.addMenu(icon("link"), QString::fromStdString(refIds.size() == 1 ? "Reference" : "Reference " + r->ns));
            addReferenceItems(sub, r->ns, true);
        }
    if (!refIds.empty())
        menu.addSeparator();
    auto plain = [](const json& target) { actions::run(target); };
    addItem(&menu, "outliner.expandAll", plain);
    addItem(&menu, "outliner.collapseAll", plain);
    menu.addSeparator();
    item("outliner.copyName", any);
    item("outliner.copyPath", any);
    menu.exec(viewport()->mapToGlobal(pos));
}

void revealSelectionInOutliners()
{
    for (Outliner* o : gOutliners)
        if (o->isVisible())
            o->revealSelected();
}

namespace
{
constexpr const char* kSplitPref = "/outliner/split";

/** Clicking into it selects all of it, so typing replaces the filter; Esc clears it. */
class FilterEdit : public QLineEdit
{
public:
    using QLineEdit::QLineEdit;

protected:
    void focusInEvent(QFocusEvent* e) override
    {
        QLineEdit::focusInEvent(e);
        // After the click that focused it has placed the cursor.
        if (e->reason() == Qt::MouseFocusReason)
            QTimer::singleShot(0, this, [this] { selectAll(); });
    }
    void keyPressEvent(QKeyEvent* e) override
    {
        if (e->key() == Qt::Key_Escape && !text().isEmpty())
            return clear();
        QLineEdit::keyPressEvent(e);
    }
};

const char* kFilterSyntax = "Filter the outliner by name. Matches keep their parents, dimmed.\n"
                            "  pCube*  ?  [a-c]  [!0-9]   wildcards over the whole name\n"
                            "  cube sphere  or  cube, sphere   any of the terms\n"
                            "  !*Shape   leaves out what matches\n"
                            "  type:mesh   that type or one derived from it\n"
                            "  |rig|*   a term with | matches the full path\n"
                            "Case is ignored. Esc clears.";

/** An outliner under its filter box. */
QWidget* filteredOutliner(QWidget* parent)
{
    auto* pane = new QWidget(parent);
    auto* v = new QVBoxLayout(pane);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);
    auto* row = new QHBoxLayout();
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(2);
    auto* edit = new FilterEdit(pane);
    edit->setPlaceholderText("Filter");
    edit->setClearButtonEnabled(true);
    edit->addAction(icon("filter"), QLineEdit::LeadingPosition);
    edit->setToolTip(kFilterSyntax);
    auto* regex = new QToolButton(pane);
    regex->setCheckable(true);
    regex->setAutoRaise(true);
    regex->setIcon(icon("regex"));
    regex->setToolTip("Regular expressions: each space separated term is a regex searched in the name (in the full path "
                      "when it holds \\|). ! and type: still apply.");
    row->addWidget(edit, 1);
    row->addWidget(regex);
    v->addLayout(row);
    auto* tree = new Outliner(pane);
    v->addWidget(tree, 1);

    auto* debounce = new QTimer(pane);
    debounce->setSingleShot(true);
    debounce->setInterval(120);
    auto apply = [edit, regex, tree] {
        outliner::Filter f(edit->text().toStdString(), regex->isChecked());
        edit->setToolTip(f.valid() ? QString(kFilterSyntax) : QString::fromStdString(f.error()) + "\n\n" + kFilterSyntax);
        tree->setFilter(std::move(f));
    };
    QObject::connect(debounce, &QTimer::timeout, pane, apply);
    QObject::connect(edit, &QLineEdit::textChanged, debounce, qOverload<>(&QTimer::start));
    QObject::connect(regex, &QToolButton::toggled, pane, apply);
    return pane;
}

/** A splitter handle with a short grip line, so the collapsed second pane can still be found. */
class GripHandle : public QSplitterHandle
{
public:
    using QSplitterHandle::QSplitterHandle;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), themeShade(underMouse() ? 70 : 52));
        p.setPen(themeShade(120));
        int y = height() / 2, half = std::min(14, width() / 4);
        p.drawLine(width() / 2 - half, y, width() / 2 + half, y);
    }
    void enterEvent(QEnterEvent* e) override
    {
        QSplitterHandle::enterEvent(e);
        update();
    }
    void leaveEvent(QEvent* e) override
    {
        QSplitterHandle::leaveEvent(e);
        update();
    }
};

class GripSplitter : public QSplitter
{
public:
    using QSplitter::QSplitter;

protected:
    QSplitterHandle* createHandle() override { return new GripHandle(orientation(), this); }
};
} // namespace

OutlinerPanel::OutlinerPanel(QWidget* parent) : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* bar = new iconBars::IconBar(iconBars::kOutliner, Qt::Horizontal, 16, [](const std::string& id) { return viewportActionState(id, nullptr); }, this);
    bar->setFill(true);
    layout->addWidget(bar);
    auto* split = new GripSplitter(Qt::Vertical, this);
    split->setHandleWidth(5);
    split->addWidget(filteredOutliner(split));
    split->addWidget(filteredOutliner(split));
    split->setCollapsible(0, false);
    split->setCollapsible(1, true);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 0);
    split->handle(1)->setToolTip("Drag up for a second outliner");
    layout->addWidget(split);

    QList<int> sizes{1, 0};
    json saved = PrefStore::get().value("general", kSplitPref, json());
    if (saved.is_array() && saved.size() == 2 && saved[0].is_number_integer() && saved[1].is_number_integer())
        sizes = {std::max(1, saved[0].get<int>()), std::max(0, saved[1].get<int>())};
    split->setSizes(sizes);
    connect(split, &QSplitter::splitterMoved, this, [split] {
        QList<int> s = split->sizes();
        if (s.size() == 2)
            PrefStore::get().set("general", kSplitPref, json::array({s[0], s[1]}));
    });
}
} // namespace lila::ui
