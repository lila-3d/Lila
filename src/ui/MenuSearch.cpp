#include "ui/MenuSearch.h"

#include "ui/Hotkeys.h"

#include <QApplication>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPointer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidgetAction>

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

QString& lastQuery()
{
    static QString q;
    return q;
}

class SearchBox : public QFrame
{
public:
    SearchBox(QMenuBar* bar, QWidget* over) : QFrame(over, Qt::Popup)
    {
        setAttribute(Qt::WA_DeleteOnClose);
        setFrameShape(QFrame::StyledPanel);
        for (QAction* a : bar->actions())
            if (QMenu* m = a->menu(); m && a->isVisible())
                collect(m, plain(a), mItems, 0);

        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(6, 6, 6, 6);
        lay->setSpacing(4);
        mEdit = new QLineEdit(this);
        mEdit->setPlaceholderText("Search menus");
        mEdit->setClearButtonEnabled(true);
        mList = new QTreeWidget(this);
        mList->setColumnCount(3);
        mList->setHeaderHidden(true);
        mList->setRootIsDecorated(false);
        mList->setUniformRowHeights(true);
        mList->setFocusPolicy(Qt::NoFocus);
        mList->header()->setStretchLastSection(false);
        mList->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        mList->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        mList->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        lay->addWidget(mEdit);
        lay->addWidget(mList);
        mEdit->installEventFilter(this);
        connect(mEdit, &QLineEdit::textChanged, this, [this] { refill(); });
        connect(mList, &QTreeWidget::itemClicked, this, [this] { run(); });
        mEdit->setText(lastQuery());
        mEdit->selectAll();
        refill();

        int w = std::min(760, over->width() - 40);
        resize(w, 420);
        QPoint top = over->mapToGlobal(QPoint((over->width() - w) / 2, 60));
        move(top);
    }

    ~SearchBox() override { lastQuery() = mEdit->text(); }

    void start()
    {
        show();
        mEdit->setFocus();
    }

protected:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        if (obj != mEdit || e->type() != QEvent::KeyPress)
            return false;
        auto* k = static_cast<QKeyEvent*>(e);
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
        case Qt::Key_Enter: run(); return true;
        case Qt::Key_Escape: close(); return true;
        default: return false;
        }
    }

private:
    void refill()
    {
        QString q = mEdit->text().trimmed();
        std::vector<std::pair<int, const Item*>> hits;
        for (const Item& it : mItems)
            if (it.action)
                if (int s = score(it, q))
                    hits.emplace_back(-s, &it);
        std::stable_sort(hits.begin(), hits.end(), [](auto& a, auto& b) { return a.first < b.first; });
        mList->clear();
        for (auto& [s, it] : hits)
        {
            QString key;
            if (QString id = it->action->data().toString(); !id.isEmpty())
                key = Hotkeys::get().keyFor(id.toStdString());
            auto* row = new QTreeWidgetItem(mList, {it->label, it->path, key});
            row->setData(0, Qt::UserRole, QVariant::fromValue(static_cast<QObject*>(it->action.data())));
            row->setDisabled(!it->action->isEnabled());
            row->setForeground(1, palette().brush(QPalette::Disabled, QPalette::Text));
            row->setForeground(2, palette().brush(QPalette::Disabled, QPalette::Text));
        }
        if (mList->topLevelItemCount())
            mList->setCurrentItem(mList->topLevelItem(0));
    }

    void run()
    {
        QTreeWidgetItem* row = mList->currentItem();
        QPointer<QAction> a = row ? qobject_cast<QAction*>(row->data(0, Qt::UserRole).value<QObject*>()) : nullptr;
        close();
        if (a && a->isEnabled())
            a->trigger();
    }

    std::vector<Item> mItems;
    QLineEdit* mEdit = nullptr;
    QTreeWidget* mList = nullptr;
};
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
    (new SearchBox(bar, over))->start();
}
} // namespace lila::ui::menuSearch
