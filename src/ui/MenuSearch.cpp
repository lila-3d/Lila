#include "ui/MenuSearch.h"

#include "core/Log.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "ui/Hotkeys.h"
#include "ui/Icons.h"
#include "ui/OptionBox.h"
#include "ui/Outliner.h"
#include "ui/OutlinerFilter.h"
#include "ui/OutlinerLogic.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QPointer>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <algorithm>
#include <map>
#include <vector>

namespace lila::ui::menuSearch
{
namespace
{
QString plain(const QAction* a)
{
    // Menus append the shortcut after a tab; the search box shows it in its own column.
    QString t = a->text().section('\t', 0, 0);
    t.remove('&');
    return t;
}

bool matches(const QAction* a, const QString& text, int depth = 0)
{
    if (plain(a).contains(text, Qt::CaseInsensitive))
        return true;
    if (QMenu* sub = a->menu(); sub && depth < 8)
        for (const QAction* c : sub->actions())
            if (c->isVisible() && !c->isSeparator() && matches(c, text, depth + 1))
                return true;
    return false;
}

struct Filter
{
    QString text;
    QPointer<QWidgetAction> header;
    QPointer<QLabel> label;
    std::vector<QPointer<QAction>> hidden;
};

class TypeToFilter : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        auto* menu = qobject_cast<QMenu*>(obj);
        if (!menu)
            return false;
        switch (e->type())
        {
        case QEvent::KeyPress: return key(menu, static_cast<QKeyEvent*>(e));
        case QEvent::Show: inherit(menu); return false;
        case QEvent::Hide:
            if (auto it = mFilters.find(menu); it != mFilters.end())
            {
                restore(menu, it->second);
                mFilters.erase(it);
            }
            return false;
        default: return false;
        }
    }

private:
    bool key(QMenu* menu, QKeyEvent* e)
    {
        if (e->modifiers() & ~(Qt::ShiftModifier | Qt::KeypadModifier))
            return false;
        Filter& f = mFilters[menu];
        QString typed = e->text();
        if (e->key() == Qt::Key_Backspace || e->key() == Qt::Key_Escape)
        {
            if (f.text.isEmpty())
                return false;
            f.text = e->key() == Qt::Key_Escape ? QString() : f.text.chopped(1);
        }
        // Space on an empty search keeps its usual job of running the highlighted row.
        else if (typed.size() == 1 && typed[0].isPrint() && (typed != " " || !f.text.isEmpty()))
            f.text += typed;
        else
            return false;
        apply(menu, f);
        return true;
    }

    /** A submenu opened from a filtered menu keeps filtering by the same text, unless its own name matched. */
    void inherit(QMenu* menu)
    {
        for (QObject* o : menu->menuAction()->associatedObjects())
            if (auto it = mFilters.find(qobject_cast<QMenu*>(o)); it != mFilters.end() && !it->second.text.isEmpty())
            {
                if (plain(menu->menuAction()).contains(it->second.text, Qt::CaseInsensitive))
                    return;
                Filter& f = mFilters[menu];
                f.text = it->second.text;
                apply(menu, f);
                return;
            }
    }

    static void restore(QMenu* menu, Filter& f)
    {
        for (QAction* a : f.hidden)
            if (a)
                a->setVisible(true);
        f.hidden.clear();
        if (f.header)
        {
            menu->removeAction(f.header);
            f.header->deleteLater();
        }
    }

    static void apply(QMenu* menu, Filter& f)
    {
        restore(menu, f);
        if (f.text.isEmpty())
        {
            menu->setActiveAction(nullptr);
            return;
        }
        f.label = new QLabel(menu);
        f.label->setContentsMargins(10, 4, 10, 4);
        f.label->setEnabled(false);
        f.header = new QWidgetAction(menu);
        f.header->setDefaultWidget(f.label);
        QList<QAction*> rows = menu->actions();
        menu->insertAction(rows.value(0), f.header);

        QAction* first = nullptr;
        int shown = 0;
        for (QAction* a : rows)
        {
            if (!a->isVisible())
                continue;
            if (a->isSeparator() || !matches(a, f.text))
            {
                a->setVisible(false);
                f.hidden.emplace_back(a);
                continue;
            }
            ++shown;
            // Highlighting a submenu row would pop it open, so Enter goes to the first plain row.
            if (!first && a->isEnabled() && !a->menu())
                first = a;
        }
        f.label->setText(shown ? QString("Search: %1").arg(f.text) : QString("No match: %1").arg(f.text));
        menu->setActiveAction(first);
    }

    std::map<QMenu*, Filter> mFilters;
};

struct Item
{
    QPointer<QAction> action;
    QString label;
    QString path;
};

void collect(QMenu* menu, const QString& path, std::vector<Item>& out, int depth)
{
    if (depth > 8)
        return;
    // Menus that fill themselves when opened (recent files, cameras) are filled now so their rows can be found.
    emit menu->aboutToShow();
    for (QAction* a : menu->actions())
    {
        if (a->isSeparator() || !a->isVisible() || qobject_cast<QWidgetAction*>(a))
            continue;
        QString label = plain(a);
        if (label.isEmpty())
            continue;
        if (QMenu* sub = a->menu())
            collect(sub, path + " > " + label, out, depth + 1);
        else
            out.push_back({a, label, path});
    }
}

/** Every word must appear in the label or the path; rows whose label starts with the query come first. */
int score(const Item& it, const QString& query)
{
    if (query.isEmpty())
        return 1;
    QString hay = it.label + ' ' + it.path;
    for (const QString& w : query.split(' ', Qt::SkipEmptyParts))
        if (!hay.contains(w, Qt::CaseInsensitive))
            return 0;
    if (it.label.startsWith(query, Qt::CaseInsensitive))
        return 3;
    return it.label.contains(query, Qt::CaseInsensitive) ? 2 : 1;
}

enum class Mode
{
    Menus,
    Scene,
};

QString& lastQuery(Mode m)
{
    static QString q[2];
    return q[int(m)];
}

constexpr int kActionRole = Qt::UserRole;
constexpr int kNodeRole = Qt::UserRole + 1;
constexpr int kParentPathRole = Qt::UserRole + 2; // "|group|sub|" in front of the row's name

enum Column
{
    kLabel,
    kWhere,
    kKey,
    kOptions,
    kColumns,
};

/** The scene finder's path cell: the ancestors dim, the node's own name bright, eliding the ancestors first. */
class PathDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        QString name = opt.text;
        opt.text.clear();
        const QWidget* w = opt.widget;
        QStyle* style = w ? w->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, p, w);
        QRect r = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, w).adjusted(2, 0, -2, 0);
        p->save();
        p->setFont(opt.font);
        const QFontMetrics fm = p->fontMetrics();
        QString shownName = fm.elidedText(name, Qt::ElideRight, r.width());
        int nameW = fm.horizontalAdvance(shownName);
        QString head = fm.elidedText(index.data(kParentPathRole).toString(), Qt::ElideLeft, std::max(0, r.width() - nameW));
        QRect headRect;
        p->setPen(themeColor("dimText"));
        p->drawText(r, Qt::AlignVCenter | Qt::AlignLeft, head, &headRect);
        p->setPen(themeColor("brightText"));
        // Placed after the rect the ancestors actually drew into, which can be wider than their measured advance.
        p->drawText(r.adjusted(head.isEmpty() ? 0 : headRect.width(), 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, shownName);
        p->restore();
    }
};

struct SceneRow
{
    scene::NodeId id;
    QString name;
    QString parentPath;
    std::string fullPath;
};

class SearchBox;
QPointer<SearchBox>& openBoxInstance()
{
    static QPointer<SearchBox> box;
    return box;
}

/**
 * The Ctrl+T / Ctrl+F box. Menus mode lists every menu item and runs it, or opens its option box; Scene mode lists
 * every node the outliner shows and selects it. The other mode's hotkey switches in place.
 */
class SearchBox : public QFrame
{
public:
    SearchBox(QMenuBar* bar, QWidget* over) : QFrame(over, Qt::Popup), mBar(bar)
    {
        setAttribute(Qt::WA_DeleteOnClose);
        setFrameShape(QFrame::StyledPanel);

        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(6, 6, 6, 6);
        lay->setSpacing(4);
        mEdit = new QLineEdit(this);
        mEdit->setClearButtonEnabled(true);
        mList = new QTreeWidget(this);
        mList->setColumnCount(kColumns);
        mList->setHeaderHidden(true);
        mList->setRootIsDecorated(false);
        mList->setUniformRowHeights(true);
        mList->setFocusPolicy(Qt::NoFocus);
        mList->setIconSize(QSize(16, 16));
        mList->setMouseTracking(true);
        mList->header()->setStretchLastSection(false);
        mList->header()->setSectionResizeMode(kLabel, QHeaderView::Stretch);
        for (int c : {kWhere, kKey, kOptions})
            mList->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
        mHint = new QLabel(this);
        mHint->setContentsMargins(4, 0, 4, 0);
        mHint->setStyleSheet("color: #969696;");
        lay->addWidget(mEdit);
        lay->addWidget(mList);
        lay->addWidget(mHint);
        mPathDelegate = new PathDelegate(mList);
        mEdit->installEventFilter(this);
        connect(mEdit, &QLineEdit::textChanged, this, [this] { refill(); });
        connect(mList, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* row, int column) {
            mList->setCurrentItem(row);
            Qt::KeyboardModifiers mods = QApplication::keyboardModifiers();
            if (mMode == Mode::Scene)
                pick(mods);
            else if (column == kOptions || (mods & Qt::ShiftModifier))
                openOptions();
            else
                run();
        });

        int w = std::min(760, over->width() - 40);
        resize(w, 420);
        move(over->mapToGlobal(QPoint((over->width() - w) / 2, 60)));
    }

    ~SearchBox() override { lastQuery(mMode) = mEdit->text(); }

    Mode mode() const { return mMode; }

    void start(Mode m)
    {
        setMode(m);
        show();
        mEdit->setFocus();
    }

    void setMode(Mode m)
    {
        if (mStarted && m == mMode)
            return;
        if (mStarted)
            lastQuery(mMode) = mEdit->text();
        mStarted = true;
        mMode = m;
        bool scene = m == Mode::Scene;
        if (scene)
            collectScene();
        else if (mItems.empty() && mBar)
            for (QAction* a : mBar->actions())
                if (QMenu* menu = a->menu(); menu && a->isVisible())
                    collect(menu, plain(a), mItems, 0);
        mList->setItemDelegateForColumn(kLabel, scene ? mPathDelegate : nullptr);
        mList->setColumnHidden(kKey, scene);
        mList->setColumnHidden(kOptions, scene);
        mEdit->setPlaceholderText(scene ? "Search the scene: names, wildcards (pCube*), type:mesh, |group|path, !exclude"
                                        : "Search menus");
        auto key = [](const char* id, const char* fallback) {
            QString k = Hotkeys::get().keyFor(id);
            return k.isEmpty() ? QString(fallback) : k;
        };
        mHint->setText(scene ? QString("Enter selects    Shift+Enter adds    Ctrl+Enter toggles    %1 searches menus")
                                   .arg(key("window.searchMenus", "Ctrl+T"))
                             : QString("Enter runs    Shift+Enter or the square opens the options    %1 searches the scene")
                                   .arg(key("window.searchScene", "Ctrl+F")));
        QSignalBlocker block(mEdit);
        mEdit->setText(lastQuery(m));
        mEdit->selectAll();
        refill();
    }

protected:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        if (obj != mEdit || e->type() != QEvent::KeyPress)
            return false;
        auto* k = static_cast<QKeyEvent*>(e);
        if (Hotkeys::get().isKeyFor(k, "window.searchMenus") || Hotkeys::get().isKeyFor(k, "window.searchScene"))
        {
            Mode want = Hotkeys::get().isKeyFor(k, "window.searchScene") ? Mode::Scene : Mode::Menus;
            if (want == mMode)
                close();
            else
                setMode(want);
            return true;
        }
        int n = mList->topLevelItemCount();
        int row = mList->currentItem() ? mList->indexOfTopLevelItem(mList->currentItem()) : -1;
        switch (k->key())
        {
        case Qt::Key_Down:
        case Qt::Key_Up:
        case Qt::Key_PageDown:
        case Qt::Key_PageUp:
        {
            if (!n)
                return true;
            int step = k->key() == Qt::Key_Down ? 1 : k->key() == Qt::Key_Up ? -1 : k->key() == Qt::Key_PageDown ? 10 : -10;
            mList->setCurrentItem(mList->topLevelItem(std::clamp(row + step, 0, n - 1)));
            return true;
        }
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (mMode == Mode::Scene)
                pick(k->modifiers());
            else if (k->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier))
                openOptions();
            else
                run();
            return true;
        case Qt::Key_Escape: close(); return true;
        default: return false;
        }
    }

private:
    void refill()
    {
        mList->clear();
        if (mMode == Mode::Scene)
            fillScene();
        else
            fillMenus();
        if (mList->topLevelItemCount())
            mList->setCurrentItem(mList->topLevelItem(0));
    }

    void fillMenus()
    {
        QString q = mEdit->text().trimmed();
        std::vector<std::pair<int, const Item*>> hits;
        for (const Item& it : mItems)
            if (it.action)
                if (int s = score(it, q))
                    hits.emplace_back(-s, &it);
        std::stable_sort(hits.begin(), hits.end(), [](auto& a, auto& b) { return a.first < b.first; });
        QBrush dim = palette().brush(QPalette::Disabled, QPalette::Text);
        for (auto& [s, it] : hits)
        {
            std::string id = it->action->data().toString().toStdString();
            QString key = id.empty() ? QString() : Hotkeys::get().keyFor(id);
            auto* row = new QTreeWidgetItem(mList, {it->label, it->path, key, QString()});
            row->setData(kLabel, kActionRole, QVariant::fromValue(static_cast<QObject*>(it->action.data())));
            row->setDisabled(!it->action->isEnabled());
            row->setForeground(kWhere, dim);
            row->setForeground(kKey, dim);
            if (!id.empty() && optionBox::available(id))
            {
                row->setIcon(kOptions, icon("app-window"));
                row->setToolTip(kOptions, "Options (Shift+Enter, or Shift+click the row)");
            }
        }
    }

    /** Snapshot of the hierarchy when the scene mode opens; the scene does not change while the box has focus. */
    void collectScene()
    {
        mScene.clear();
        scene::Graph& g = scene::Graph::current();
        for (scene::NodeId id : outliner::hierarchyNodes(g))
        {
            const scene::Node* n = g.node(id);
            std::string path = g.fullPath(id);
            QString full = QString::fromStdString(path);
            QString name = QString::fromStdString(n->name());
            mScene.push_back({id, name, full.left(full.size() - name.size()), std::move(path)});
        }
    }

    void fillScene()
    {
        constexpr size_t kMaxRows = 500;
        scene::Graph& g = scene::Graph::current();
        outliner::Filter f(mEdit->text().toStdString());
        f.setFuzzy(true);
        std::vector<std::pair<int, const SceneRow*>> hits;
        for (const SceneRow& r : mScene)
            if (const scene::Node* n = g.node(r.id))
                if (int s = f.score(n->name(), r.fullPath, &n->type()))
                    hits.emplace_back(-s, &r);
        std::stable_sort(hits.begin(), hits.end(), [](auto& a, auto& b) { return a.first < b.first; });
        if (hits.size() > kMaxRows)
            hits.resize(kMaxRows);
        const std::vector<scene::NodeId>& sel = g.selection();
        QBrush dim = palette().brush(QPalette::Disabled, QPalette::Text);
        for (auto& [s, r] : hits)
        {
            const scene::Node* n = g.node(r->id);
            auto* row = new QTreeWidgetItem(mList, {r->name, QString::fromStdString(n->type().name)});
            row->setData(kLabel, kNodeRole, QVariant::fromValue<qulonglong>(r->id));
            row->setData(kLabel, kParentPathRole, r->parentPath);
            row->setIcon(kLabel, icon(QString::fromStdString(outliner::iconName(*n))));
            row->setToolTip(kLabel, QString::fromStdString(r->fullPath));
            row->setForeground(kWhere, dim);
            if (std::find(sel.begin(), sel.end(), r->id) != sel.end())
            {
                QFont bold = row->font(kLabel);
                bold.setBold(true);
                row->setFont(kLabel, bold);
            }
        }
    }

    /** Selects the highlighted node: replacing the selection and closing, or with Shift adding, with Ctrl toggling. */
    void pick(Qt::KeyboardModifiers mods)
    {
        QTreeWidgetItem* row = mList->currentItem();
        scene::Node* n = row ? scene::Graph::current().node(row->data(kLabel, kNodeRole).toULongLong()) : nullptr;
        if (!n)
            return;
        json call = {{"args", {n->path()}}};
        bool keepOpen = mods & (Qt::ShiftModifier | Qt::ControlModifier);
        if (mods & Qt::ControlModifier)
            call["toggle"] = true;
        else if (mods & Qt::ShiftModifier)
            call["add"] = true;
        try
        {
            ops::execute("select", call, CallSource::UI);
        }
        catch (const CommandError& e)
        {
            log::error("{}", e.what());
        }
        revealSelectionInOutliners();
        if (!keepOpen)
        {
            close();
            return;
        }
        int current = mList->indexOfTopLevelItem(row);
        refill();
        if (QTreeWidgetItem* again = mList->topLevelItem(current))
            mList->setCurrentItem(again);
    }

    QPointer<QAction> currentAction() const
    {
        QTreeWidgetItem* row = mList->currentItem();
        return row ? qobject_cast<QAction*>(row->data(kLabel, kActionRole).value<QObject*>()) : nullptr;
    }

    void run()
    {
        QPointer<QAction> a = currentAction();
        close();
        if (a && a->isEnabled())
            a->trigger();
    }

    /** The highlighted item's option box; a row without one just runs. */
    void openOptions()
    {
        QPointer<QAction> a = currentAction();
        std::string id = a ? a->data().toString().toStdString() : std::string();
        if (id.empty() || !optionBox::available(id))
            return run();
        close();
        QTimer::singleShot(0, [id] { optionBox::open(id); });
    }

    QPointer<QMenuBar> mBar;
    Mode mMode = Mode::Menus;
    bool mStarted = false;
    std::vector<Item> mItems;
    std::vector<SceneRow> mScene;
    QLineEdit* mEdit = nullptr;
    QTreeWidget* mList = nullptr;
    QLabel* mHint = nullptr;
    PathDelegate* mPathDelegate = nullptr;
};

void openIn(Mode m, QMenuBar* bar, QWidget* over)
{
    QPointer<SearchBox>& box = openBoxInstance();
    if (box && box->isVisible())
    {
        if (box->mode() == m)
            box->close();
        else
            box->setMode(m);
        return;
    }
    box = new SearchBox(bar, over);
    box->start(m);
}
} // namespace

void install()
{
    static QPointer<TypeToFilter> f;
    if (!f)
    {
        f = new TypeToFilter(qApp);
        qApp->installEventFilter(f);
    }
}

void openBox(QMenuBar* bar, QWidget* over)
{
    openIn(Mode::Menus, bar, over);
}

void openSceneFinder(QMenuBar* bar, QWidget* over)
{
    openIn(Mode::Scene, bar, over);
}
} // namespace lila::ui::menuSearch
