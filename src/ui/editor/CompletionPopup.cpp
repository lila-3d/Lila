#include "ui/editor/CompletionPopup.h"

#include "core/Profile.h"
#include "ui/editor/DocFormat.h"
#include "ui/editor/EditorPalette.h"
#include "ui/editor/TextUtil.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QListWidget>
#include <QPainter>
#include <QScreen>
#include <QStyledItemDelegate>
#include <QTextBrowser>

#include <algorithm>

namespace lila::ui
{
namespace
{
constexpr int kEntryRole = Qt::UserRole + 1;
constexpr int kMaxRows = 12;

QString kindTag(CompletionKind k)
{
    switch (k)
    {
    case CompletionKind::Method:
    case CompletionKind::Function:
    case CompletionKind::Constructor: return "fn";
    case CompletionKind::Class:
    case CompletionKind::Interface: return "cl";
    case CompletionKind::Module: return "mod";
    case CompletionKind::Property:
    case CompletionKind::Field: return "pr";
    case CompletionKind::Keyword: return "kw";
    case CompletionKind::Constant: return "co";
    case CompletionKind::Variable: return "v";
    default: return "";
    }
}

class EntryDelegate : public QStyledItemDelegate
{
public:
    EntryDelegate(CompletionPopup* popup) : QStyledItemDelegate(popup), mPopup(popup) {}

    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& index) const override
    {
        int e = index.data(kEntryRole).toInt();
        const CompletionEntry& entry = mPopup->entries()[e];
        p->save();
        if (opt.state & QStyle::State_Selected)
            p->fillRect(opt.rect, palette::gray(72));

        QFont base = opt.font;
        QFontMetrics fm(base);
        QRect r = opt.rect.adjusted(6, 0, -6, 0);

        QFont small = base;
        small.setPointSizeF(base.pointSizeF() * 0.85);
        p->setFont(small);
        p->setPen(palette::gray(118));
        int tagW = fm.horizontalAdvance("mod") + 6;
        p->drawText(QRect(r.left(), r.top(), tagW, r.height()), Qt::AlignVCenter | Qt::AlignLeft, kindTag(entry.kind));

        // Label, with the characters that matched the typed prefix in bold.
        int x = r.left() + tagW;
        const QString& typed = mPopup->typed();
        bool prefix = entry.label.startsWith(typed, Qt::CaseInsensitive);
        QFont bold = base;
        bold.setBold(true);
        int labelEnd = x;
        if (prefix && !typed.isEmpty())
        {
            p->setFont(bold);
            p->setPen(palette::gray(255));
            QString head = entry.label.left(typed.size());
            p->drawText(QRect(x, r.top(), r.width(), r.height()), Qt::AlignVCenter | Qt::AlignLeft, head);
            x += QFontMetrics(bold).horizontalAdvance(head);
            p->setFont(base);
            p->setPen(palette::gray(212));
            QString tail = entry.label.mid(typed.size());
            p->drawText(QRect(x, r.top(), r.width(), r.height()), Qt::AlignVCenter | Qt::AlignLeft, tail);
            labelEnd = x + fm.horizontalAdvance(tail);
        }
        else
        {
            p->setFont(base);
            p->setPen(palette::gray(212));
            p->drawText(QRect(x, r.top(), r.width(), r.height()), Qt::AlignVCenter | Qt::AlignLeft, entry.label);
            labelEnd = x + fm.horizontalAdvance(entry.label);
        }

        QString detail = entry.detail.simplified();
        if (!detail.isEmpty())
        {
            p->setFont(small);
            p->setPen(palette::gray(125));
            int avail = r.right() - labelEnd - 16;
            if (avail > 30)
            {
                QString el = QFontMetrics(small).elidedText(detail, Qt::ElideRight, avail);
                p->drawText(QRect(labelEnd + 16, r.top(), avail, r.height()), Qt::AlignVCenter | Qt::AlignRight, el);
            }
        }
        p->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem& opt, const QModelIndex&) const override
    {
        return {200, QFontMetrics(opt.font).height() + 4};
    }

private:
    CompletionPopup* mPopup;
};
} // namespace

CompletionPopup::CompletionPopup(QWidget* owner) : QFrame(owner, Qt::ToolTip | Qt::FramelessWindowHint)
{
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFocusPolicy(Qt::NoFocus);
    setFrameShape(QFrame::Box);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);

    mList = new QListWidget(this);
    mList->setFocusPolicy(Qt::NoFocus);
    mList->setItemDelegate(new EntryDelegate(this));
    mList->setUniformItemSizes(true);
    mList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    mList->setFrameShape(QFrame::NoFrame);
    mList->setFixedWidth(440);
    layout->addWidget(mList);

    mDoc = new QTextBrowser(this);
    mDoc->setFocusPolicy(Qt::NoFocus);
    mDoc->setFrameShape(QFrame::NoFrame);
    mDoc->setFixedWidth(380);
    mDoc->setOpenLinks(false);
    layout->addWidget(mDoc);

    auto applyLook = [this] {
        using palette::gray;
        const doc::Theme theme = doc::currentTheme(mList->font());
        setStyleSheet(QString("lila--ui--CompletionPopup { border: 1px solid %1; background: %2; }").arg(theme.border.name(), gray(36).name()));
        mList->setStyleSheet(QString("QListWidget { background: %1; }").arg(gray(36).name()));
        mDoc->setStyleSheet(QString("QTextBrowser { background: %1; color: %2; border-left: 1px solid %3; }")
                                .arg(theme.background.name(), theme.text.name(), theme.border.name()));
        refreshDocumentation();
    };
    palette::onLookChanged(this, applyLook);
    applyLook();

    connect(mList, &QListWidget::currentRowChanged, this, [this](int) {
        refreshDocumentation();
        Q_EMIT currentEntryChanged(currentEntry());
    });
    connect(mList, &QListWidget::itemDoubleClicked, this, [this] { Q_EMIT activated(); });
}

void CompletionPopup::setEditorFont(const QFont& font)
{
    mList->setFont(font);
}

void CompletionPopup::setEntries(QList<CompletionEntry> entries)
{
    mEntries = std::move(entries);
}

int CompletionPopup::filter(const QString& typed)
{
    LILA_ZONE();

    mTyped = typed;
    struct Ranked
    {
        int score;
        int entry;
    };
    std::vector<Ranked> ranked;
    ranked.reserve(size_t(mEntries.size()));
    for (int i = 0; i < mEntries.size(); ++i)
    {
        const CompletionEntry& e = mEntries[i];
        // Private names only once the user is typing one.
        if (e.label.startsWith('_') && !typed.startsWith('_'))
            continue;
        int s = text::matchScore(e.label, typed);
        if (s >= 0)
            ranked.push_back({s, i});
    }
    std::stable_sort(ranked.begin(), ranked.end(), [this](const Ranked& a, const Ranked& b) {
        // Prefix-matched tiers first; within a tier keep the server's own ordering.
        int ta = a.score / 1000, tb = b.score / 1000;
        if (ta != tb)
            return ta < tb;
        if (ta >= 2 && a.score != b.score)
            return a.score < b.score;
        return mEntries[a.entry].sortText < mEntries[b.entry].sortText;
    });

    mList->setUpdatesEnabled(false);
    mList->clear();
    for (const Ranked& r : ranked)
    {
        auto* item = new QListWidgetItem(mEntries[r.entry].label);
        item->setData(kEntryRole, r.entry);
        mList->addItem(item);
    }
    if (mList->count() > 0)
        mList->setCurrentRow(0);
    mList->setUpdatesEnabled(true);

    int rows = std::min(kMaxRows, mList->count());
    int rowH = mList->sizeHintForRow(0) > 0 ? mList->sizeHintForRow(0) : QFontMetrics(mList->font()).height() + 4;
    setFixedHeight(std::max(1, rows) * rowH + 4);
    return mList->count();
}

int CompletionPopup::currentEntry() const
{
    QListWidgetItem* it = mList->currentItem();
    return it ? it->data(kEntryRole).toInt() : -1;
}

void CompletionPopup::moveSelection(int delta)
{
    int n = mList->count();
    if (n == 0)
        return;
    int row = mList->currentRow() + delta;
    if (std::abs(delta) == 1)
        row = (row + n) % n;
    else
        row = std::clamp(row, 0, n - 1);
    mList->setCurrentRow(row);
}

void CompletionPopup::refreshDocumentation()
{
    int e = currentEntry();
    if (e < 0)
    {
        mDoc->hide();
        return;
    }
    const CompletionEntry& entry = mEntries[e];
    const QString docs = entry.documentation.trimmed();
    const QString detail = entry.detail.trimmed();
    const doc::Theme theme = doc::currentTheme(mList->font());
    QString html;
    if (!detail.isEmpty())
        html += doc::codeBlockHtml(detail, theme);
    if (!docs.isEmpty())
        html += doc::docstringHtml(docs, theme, doc::parameterAnnotations(detail));
    mDoc->setHtml(html);
    mDoc->setVisible(!html.isEmpty());
    adjustSize();
}

void CompletionPopup::showBelow(const QRect& cursorRect)
{
    adjustSize();
    QPoint pos = cursorRect.bottomLeft() + QPoint(0, 2);
    QScreen* screen = QApplication::screenAt(cursorRect.center());
    if (screen)
    {
        QRect avail = screen->availableGeometry();
        // Width with the doc pane, so the popup is placed once and does not jump as docs come and go.
        int fullWidth = mList->width() + 380 + 2;
        if (pos.x() + fullWidth > avail.right())
            pos.setX(std::max(avail.left(), avail.right() - fullWidth));
        if (pos.y() + height() > avail.bottom())
            pos.setY(cursorRect.top() - height() - 2);
    }
    move(pos);
    show();
    raise();
}
} // namespace lila::ui
