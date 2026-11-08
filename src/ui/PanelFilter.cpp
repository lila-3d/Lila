#include "ui/PanelFilter.h"

#include "ui/Icons.h"
#include "ui/Theme.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QComboBox>
#include <QDockWidget>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QPainter>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QTableView>
#include <QTextDocumentFragment>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>

#include <algorithm>
#include <vector>

namespace lila::ui::panelFilter
{
namespace
{
constexpr int kRefreshMs = 120;
constexpr int kMaxViewRows = 400;
constexpr int kMaxViewColumns = 8;

struct Match
{
    QPointer<QWidget> target; // the shown widget the highlight is drawn on
    QRect rect;               // in target's coordinates
    QPointer<QWidget> source; // the matching widget, which may be hidden in a collapsed section or another tab
    QPersistentModelIndex index;
    std::vector<QPointer<QToolButton>> headers; // collapsed sections to open, outermost first
    QPointer<QTabWidget> tabs;
    int tab = -1;
};

QString plain(const QString& text)
{
    QString t = Qt::mightBeRichText(text) ? QTextDocumentFragment::fromHtml(text).toPlainText() : text;
    return t.remove('&');
}

/** The name a tooltip opens with ("Pack: ..." gives "Pack"), or its first line. */
QString tipName(const QString& tip)
{
    const QString t = plain(tip).section('\n', 0, 0);
    const int colon = t.indexOf(':');
    return colon > 0 && colon < 48 ? t.left(colon) : t.left(64);
}

Match matchAt(QWidget* target, QRect rect, QWidget* source, const QModelIndex& index = {})
{
    Match m;
    m.target = target;
    m.rect = rect;
    m.source = source;
    m.index = index;
    return m;
}

bool menuMatches(const QMenu* menu, const QString& needle, int depth = 0)
{
    if (!menu || depth > 3)
        return false;
    for (const QAction* a : menu->actions())
        if (plain(a->text()).contains(needle, Qt::CaseInsensitive) || menuMatches(a->menu(), needle, depth + 1))
            return true;
    return false;
}

bool widgetMatches(QWidget* w, const QString& needle)
{
    auto has = [&](const QString& s) { return !s.isEmpty() && plain(s).contains(needle, Qt::CaseInsensitive); };
    if (auto* l = qobject_cast<QLabel*>(w))
        return has(l->text());
    if (auto* b = qobject_cast<QAbstractButton*>(w))
    {
        if (has(b->text()) || has(b->accessibleName()) || has(tipName(b->toolTip())))
            return true;
        auto* tb = qobject_cast<QToolButton*>(b);
        return tb && menuMatches(tb->menu(), needle);
    }
    if (auto* c = qobject_cast<QComboBox*>(w))
    {
        for (int i = 0; i < c->count(); ++i)
            if (has(c->itemText(i)))
                return true;
        return false;
    }
    if (auto* g = qobject_cast<QGroupBox*>(w))
        return has(g->title());
    return false;
}

/**
 * Where a hidden match shows: the outermost collapsed section header or tab hiding it, recorded in m. False when it is
 * hidden for some other reason (a control the panel keeps out of sight), which is not a match worth showing.
 */
bool placeHidden(QWidget* w, QWidget* root, Match& m)
{
    QWidget* shown = nullptr;
    QRect shownRect;
    for (QWidget* p = w; p && p != root; p = p->parentWidget())
    {
        QWidget* parent = p->parentWidget();
        if (!p->isHidden() || !parent)
            continue;
        if (parent->property("role").toString() == "section")
        {
            QToolButton* header = nullptr;
            for (QToolButton* t : parent->findChildren<QToolButton*>(Qt::FindDirectChildrenOnly))
                if (t->property("role").toString() == "sectionHeader")
                    header = t;
            if (!header)
                return false;
            m.headers.insert(m.headers.begin(), header);
            shown = header;
            shownRect = header->rect();
        }
        else if (auto* stack = qobject_cast<QStackedWidget*>(parent); stack && qobject_cast<QTabWidget*>(stack->parentWidget()))
        {
            auto* tabs = static_cast<QTabWidget*>(stack->parentWidget());
            m.tabs = tabs;
            m.tab = tabs->indexOf(p);
            m.headers.clear();
            shown = tabs->tabBar();
            shownRect = tabs->tabBar()->tabRect(m.tab);
        }
        else
            return false;
    }
    if (!shown || !shown->isVisible())
        return false;
    m.target = shown;
    m.rect = shownRect;
    return true;
}

void viewMatches(QAbstractItemView* v, const QString& needle, std::vector<Match>& out)
{
    QAbstractItemModel* model = v->model();
    if (!model)
        return;
    auto* tree = qobject_cast<QTreeView*>(v);
    auto* table = qobject_cast<QTableView*>(v);
    auto* list = qobject_cast<QListView*>(v);
    const int height = v->viewport()->height();
    QModelIndex row = v->indexAt(QPoint(2, 2));
    for (int n = 0; row.isValid() && n < kMaxViewRows; ++n)
    {
        if (v->visualRect(row).top() > height)
            break;
        const int columns = list ? 1 : std::min(model->columnCount(row.parent()), kMaxViewColumns);
        for (int c = 0; c < columns; ++c)
        {
            const QModelIndex cell = list ? row : model->index(row.row(), c, row.parent());
            if ((tree && tree->isColumnHidden(c)) || (table && table->isColumnHidden(c)))
                continue;
            if (cell.data(Qt::DisplayRole).toString().contains(needle, Qt::CaseInsensitive))
                out.push_back(matchAt(v->viewport(), v->visualRect(cell), v, cell));
        }
        row = tree ? tree->indexBelow(row) : model->index(row.row() + 1, row.column(), row.parent());
    }
}

std::vector<Match> collect(QWidget* root, const QString& needle, const QWidget* skip)
{
    std::vector<Match> out;
    for (QWidget* w : root->findChildren<QWidget*>())
    {
        if (w->window() != root->window() || w == skip || skip->isAncestorOf(w))
            continue;
        if (auto* v = qobject_cast<QAbstractItemView*>(w))
        {
            if (v->isVisible())
                viewMatches(v, needle, out);
            continue;
        }
        if (auto* bar = qobject_cast<QTabBar*>(w))
        {
            if (bar->isVisible())
                for (int i = 0; i < bar->count(); ++i)
                    if (plain(bar->tabText(i)).contains(needle, Qt::CaseInsensitive))
                        out.push_back(matchAt(bar, bar->tabRect(i), bar));
            continue;
        }
        if (!widgetMatches(w, needle))
            continue;
        Match m = matchAt(w, w->rect(), w);
        if (!w->isVisible() && !placeHidden(w, root, m))
            continue;
        out.push_back(std::move(m));
    }
    // Several hidden matches behind one header show as one.
    std::vector<Match> unique;
    for (Match& m : out)
        if (std::none_of(unique.begin(), unique.end(), [&](const Match& u) { return u.target == m.target && u.rect == m.rect; }))
            unique.push_back(std::move(m));
    std::stable_sort(unique.begin(), unique.end(), [](const Match& a, const Match& b) {
        const QPoint pa = a.target->mapToGlobal(a.rect.topLeft()), pb = b.target->mapToGlobal(b.rect.topLeft());
        return pa.y() != pb.y() ? pa.y() < pb.y() : pa.x() < pb.x();
    });
    return unique;
}

/** The part of rect (in w's coordinates) its ancestors up to top leave showing, in top's coordinates. */
QRect shownPart(QWidget* w, QRect rect, QWidget* top)
{
    QRect r = rect.intersected(w->rect());
    for (QWidget* p = w; p && p != top; p = p->parentWidget())
    {
        QWidget* parent = p->parentWidget();
        if (!parent)
            return {};
        r = r.translated(p->pos()).intersected(parent->rect());
        if (r.isEmpty())
            return {};
    }
    return r;
}

class Highlights : public QWidget
{
public:
    explicit Highlights(QWidget* parent) : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
    }

    void setRects(std::vector<QRect> rects, int current)
    {
        if (rects == mRects && current == mCurrent)
            return;
        mRects = std::move(rects);
        mCurrent = current;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QColor accent = themeColor("accent");
        QColor fill = accent;
        fill.setAlpha(46);
        for (int i = 0; i < int(mRects.size()); ++i)
        {
            const bool cur = i == mCurrent;
            QColor line = accent;
            line.setAlpha(cur ? 255 : 170);
            p.setPen(QPen(line, cur ? 2.0 : 1.0));
            p.setBrush(fill);
            p.drawRoundedRect(QRectF(mRects[size_t(i)]).adjusted(1, 1, -1, -1), 3, 3);
        }
    }

private:
    std::vector<QRect> mRects;
    int mCurrent = -1;
};

class FindBar : public QWidget
{
public:
    explicit FindBar(QDockWidget* dock) : QWidget(dock), mDock(dock)
    {
        setObjectName("panelFind");
        mHighlights = new Highlights(dock);
        auto* h = new QHBoxLayout(this);
        h->setContentsMargins(6, 4, 4, 4);
        h->setSpacing(2);
        auto* glyph = new QLabel(this);
        glyph->setPixmap(icon("search").pixmap(14, 14));
        h->addWidget(glyph);
        mEdit = new QLineEdit(this);
        mEdit->setPlaceholderText("Find in panel");
        mEdit->setClearButtonEnabled(true);
        mEdit->setFrame(false);
        h->addWidget(mEdit, 1);
        mCount = new QLabel(this);
        mCount->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        h->addWidget(mCount);
        auto button = [this, h](const char* ic, const QString& tip) {
            auto* b = new QToolButton(this);
            b->setIcon(icon(ic));
            b->setIconSize(QSize(14, 14));
            b->setAutoRaise(true);
            b->setToolTip(tip);
            b->setFocusPolicy(Qt::NoFocus);
            h->addWidget(b);
            return b;
        };
        QToolButton* prev = button("chevron-up", "Previous match (Shift+Enter)");
        QToolButton* next = button("chevron-down", "Next match (Enter)");
        QToolButton* close = button("x", "Close (Esc)");
        connect(prev, &QToolButton::clicked, this, [this] { step(-1); });
        connect(next, &QToolButton::clicked, this, [this] { step(1); });
        connect(close, &QToolButton::clicked, this, &FindBar::finish);
        connect(mEdit, &QLineEdit::textChanged, this, [this] {
            mCurrent = -1;
            mCurrentSource = nullptr;
            refresh();
        });
        mEdit->installEventFilter(this);
        mTimer = new QTimer(this);
        mTimer->setInterval(kRefreshMs);
        connect(mTimer, &QTimer::timeout, this, &FindBar::refresh);
        if (QWidget* content = dock->widget())
            content->installEventFilter(this);
    }

    void begin()
    {
        show();
        mHighlights->show();
        place();
        raise();
        mEdit->setFocus(Qt::ShortcutFocusReason);
        mEdit->selectAll();
        mTimer->start();
        refresh();
    }

protected:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        if (obj == mEdit && e->type() == QEvent::KeyPress)
        {
            auto* k = static_cast<QKeyEvent*>(e);
            if (k->key() == Qt::Key_Escape)
            {
                finish();
                return true;
            }
            if (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter)
            {
                step(k->modifiers() & Qt::ShiftModifier ? -1 : 1);
                return true;
            }
        }
        else if (obj == mDock->widget() && (e->type() == QEvent::Resize || e->type() == QEvent::Move))
            place();
        return false;
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(themeColor("border"));
        p.setBrush(themeColor("window"));
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
    }

private:
    void place()
    {
        QWidget* content = mDock->widget();
        if (!content)
            return;
        const QRect area = content->geometry();
        mHighlights->setGeometry(area);
        const int w = std::min(300, std::max(160, area.width() - 8));
        const int h = sizeHint().height();
        setGeometry(area.right() - w - 3, area.top() + 4, w, h);
        mHighlights->raise();
        raise();
    }

    void refresh()
    {
        QWidget* content = mDock->widget();
        const QString needle = mEdit->text().trimmed();
        if (!content || needle.isEmpty() || !mDock->isVisible())
        {
            mMatches.clear();
            mHighlights->setRects({}, -1);
            mCount->clear();
            return;
        }
        mMatches = collect(content, needle, this);
        // The current match stays current while the list around it changes (a section opened, a scroll).
        mCurrent = -1;
        for (int i = 0; i < int(mMatches.size()); ++i)
            if (mCurrentSource && mMatches[size_t(i)].source == mCurrentSource &&
                (!mCurrentIndex.isValid() || mMatches[size_t(i)].index == mCurrentIndex))
                mCurrent = i;
        std::vector<QRect> rects;
        for (const Match& m : mMatches)
            rects.push_back(m.target ? shownPart(m.target, m.rect, content) : QRect());
        mHighlights->setRects(std::move(rects), mCurrent);
        mCount->setText(mMatches.empty() ? QString("None")
                        : mCurrent >= 0  ? QString("%1/%2").arg(mCurrent + 1).arg(mMatches.size())
                                         : QString::number(mMatches.size()));
    }

    void step(int by)
    {
        refresh();
        if (mMatches.empty())
            return;
        const int n = int(mMatches.size());
        const int i = mCurrent < 0 ? (by > 0 ? 0 : n - 1) : ((mCurrent + by) % n + n) % n;
        const Match m = mMatches[size_t(i)];
        mCurrentSource = m.source;
        mCurrentIndex = m.index;
        for (const QPointer<QToolButton>& header : m.headers)
            if (header && !header->isChecked())
                header->setChecked(true);
        if (m.tabs && m.tab >= 0)
            m.tabs->setCurrentIndex(m.tab);
        // After the opened sections lay out, so the scroll lands on the match's real place.
        QTimer::singleShot(0, this, [this, m] {
            if (auto* view = qobject_cast<QAbstractItemView*>(m.source.data()); view && m.index.isValid())
                view->scrollTo(m.index);
            else if (m.source)
                for (QWidget* p = m.source->parentWidget(); p; p = p->parentWidget())
                    if (auto* area = qobject_cast<QScrollArea*>(p); area && area->widget() && area->widget()->isAncestorOf(m.source))
                    {
                        area->ensureWidgetVisible(m.source, 8, 24);
                        break;
                    }
            refresh();
        });
        refresh();
    }

    void finish()
    {
        mTimer->stop();
        mEdit->clear();
        mHighlights->hide();
        hide();
        if (QWidget* content = mDock->widget())
            content->setFocus(Qt::OtherFocusReason);
    }

    QDockWidget* mDock;
    Highlights* mHighlights;
    QLineEdit* mEdit;
    QLabel* mCount;
    QTimer* mTimer;
    std::vector<Match> mMatches;
    int mCurrent = -1;
    QPointer<QWidget> mCurrentSource;
    QPersistentModelIndex mCurrentIndex;
};
} // namespace

void open(QDockWidget* dock)
{
    if (!dock || !dock->widget())
        return;
    // FindBar has no meta-object of its own, so it is found by name.
    auto* bar = static_cast<FindBar*>(dock->findChild<QWidget*>("panelFind", Qt::FindDirectChildrenOnly));
    if (!bar)
        bar = new FindBar(dock);
    bar->begin();
}

QDockWidget* dockOf(QWidget* w)
{
    for (QWidget* p = w; p; p = p->parentWidget())
        if (auto* d = qobject_cast<QDockWidget*>(p))
            return d;
    return nullptr;
}
} // namespace lila::ui::panelFilter
