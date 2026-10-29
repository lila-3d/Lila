#include "ui/FixedRowLayout.h"

#include <QToolButton>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <vector>

namespace lila::ui
{
namespace
{
constexpr const char* kCompacted = "lilaCompacted";

/** The widget's labelled tool button that may drop to its icon: the widget itself, or the button an option box wraps. */
QToolButton* compactButton(QWidget* w)
{
    auto* b = qobject_cast<QToolButton*>(w);
    if (!b && w && w->layout() && w->layout()->count())
        b = qobject_cast<QToolButton*>(w->layout()->itemAt(0)->widget());
    if (!b || b->icon().isNull() || b->text().isEmpty())
        return nullptr;
    return b->property(kCompacted).toBool() || b->toolButtonStyle() == Qt::ToolButtonTextBesideIcon ? b : nullptr;
}

/** What the label adds to a tool button's width, as QToolButton::sizeHint counts it. */
int labelWidth(const QToolButton* b)
{
    QFontMetrics fm = b->fontMetrics();
    return 4 + fm.size(Qt::TextShowMnemonic, b->text()).width() + 2 * fm.horizontalAdvance(QLatin1Char(' '));
}

void setCompact(QToolButton* b, bool on)
{
    if (b->property(kCompacted).toBool() == on)
        return;
    b->setProperty(kCompacted, on);
    b->setToolButtonStyle(on ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon);
}

struct Widths
{
    int full = 0;
    int least = 0;
    QToolButton* button = nullptr;
};

// Read from the widget rather than the item, so a hidden widget still reserves its slot.
Widths widths(QLayoutItem* item)
{
    QWidget* w = item->widget();
    if (!w)
        return {item->sizeHint().width(), item->minimumSize().width(), nullptr};
    int hint = w->sizeHint().width();
    if (QToolButton* b = compactButton(w))
    {
        int label = labelWidth(b);
        return b->property(kCompacted).toBool() ? Widths{hint + label, hint, b} : Widths{hint, hint - label, b};
    }
    int full = std::clamp(hint, w->minimumWidth(), w->maximumWidth());
    int least = w->minimumWidth() > 0 ? w->minimumWidth() : std::max(0, w->minimumSizeHint().width());
    return {full, std::min(least, full), nullptr};
}

int hintHeight(QLayoutItem* item)
{
    QWidget* w = item->widget();
    return w ? std::clamp(w->sizeHint().height(), w->minimumHeight(), w->maximumHeight()) : item->sizeHint().height();
}

// Widgets take the whole slot whatever their size policy, so a cell's button fills the cell.
void place(QLayoutItem* item, const QRect& r)
{
    if (item->isEmpty())
        return;
    if (QWidget* w = item->widget())
        w->setGeometry(r);
    else
        item->setGeometry(r);
}

/** An option box's button grows into the space its wrapper is given. */
void fillWrapper(QWidget* w, QToolButton* b)
{
    if (b != w && b->sizePolicy().horizontalPolicy() != QSizePolicy::Preferred)
        b->setSizePolicy(QSizePolicy::Preferred, b->sizePolicy().verticalPolicy());
}
} // namespace

FixedRowLayout::FixedRowLayout(int columns, int spacing) : mColumns(columns)
{
    setSpacing(spacing);
    setContentsMargins(0, 0, 0, 0);
}

FixedRowLayout::~FixedRowLayout()
{
    while (QLayoutItem* item = takeAt(0))
        delete item;
}

void FixedRowLayout::endRow()
{
    mBreak = true;
}

void FixedRowLayout::addItem(QLayoutItem* item)
{
    int row = 0, column = 0;
    if (!mItems.isEmpty())
    {
        row = mItems.back().row;
        column = mItems.back().column + 1;
        if (mBreak || (mColumns > 0 && column >= mColumns))
        {
            ++row;
            column = 0;
        }
    }
    mBreak = false;
    mItems.append({item, row, column});
}

int FixedRowLayout::count() const
{
    return int(mItems.size());
}

QLayoutItem* FixedRowLayout::itemAt(int index) const
{
    return index >= 0 && index < mItems.size() ? mItems[index].item : nullptr;
}

QLayoutItem* FixedRowLayout::takeAt(int index)
{
    return index >= 0 && index < mItems.size() ? mItems.takeAt(index).item : nullptr;
}

Qt::Orientations FixedRowLayout::expandingDirections() const
{
    return Qt::Horizontal;
}

int FixedRowLayout::rowCount() const
{
    return mItems.isEmpty() ? 0 : mItems.back().row + 1;
}

int FixedRowLayout::rowHeight(int row) const
{
    int h = 0;
    for (const Entry& e : mItems)
        if (e.row == row)
            h = std::max(h, hintHeight(e.item));
    return h;
}

QSize FixedRowLayout::minimumSize() const
{
    int width = 0, height = 0;
    if (mColumns > 0)
    {
        int cell = 0;
        for (const Entry& e : mItems)
            cell = std::max(cell, widths(e.item).least);
        width = mColumns * cell + (mColumns - 1) * spacing();
    }
    for (int r = 0; r < rowCount(); ++r)
    {
        if (mColumns <= 0)
        {
            int w = -spacing();
            for (const Entry& e : mItems)
                if (e.row == r)
                    w += widths(e.item).least + spacing();
            width = std::max(width, w);
        }
        height += rowHeight(r) + (r ? spacing() : 0);
    }
    QMargins m = contentsMargins();
    return {width + m.left() + m.right(), height + m.top() + m.bottom()};
}

QSize FixedRowLayout::sizeHint() const
{
    int width = 0;
    if (mColumns > 0)
    {
        int cell = 0;
        for (const Entry& e : mItems)
            cell = std::max(cell, widths(e.item).full);
        width = mColumns * cell + (mColumns - 1) * spacing();
    }
    else
        for (int r = 0; r < rowCount(); ++r)
        {
            int w = -spacing();
            for (const Entry& e : mItems)
                if (e.row == r)
                    w += widths(e.item).full + spacing();
            width = std::max(width, w);
        }
    QMargins m = contentsMargins();
    return {width + m.left() + m.right(), minimumSize().height()};
}

void FixedRowLayout::setGeometry(const QRect& rect)
{
    QLayout::setGeometry(rect);
    QRect area = rect.marginsRemoved(contentsMargins());
    if (mColumns > 0)
    {
        arrangeGrid(area);
        return;
    }
    int y = area.y();
    for (int r = 0; r < rowCount(); ++r)
    {
        arrangeRow(r, y, area);
        y += rowHeight(r) + spacing();
    }
}

void FixedRowLayout::arrangeGrid(const QRect& area)
{
    int widest = 0;
    for (const Entry& e : mItems)
        widest = std::max(widest, widths(e.item).full);
    int cell = std::max(0, std::min(widest, (area.width() - (mColumns - 1) * spacing()) / mColumns));
    int y = area.y();
    for (int r = 0; r < rowCount(); ++r)
    {
        int h = rowHeight(r);
        for (const Entry& e : mItems)
        {
            if (e.row != r)
                continue;
            Widths w = widths(e.item);
            if (w.button)
            {
                setCompact(w.button, cell < w.full);
                fillWrapper(e.item->widget(), w.button);
            }
            int width = e.item->widget() ? std::min(cell, e.item->widget()->maximumWidth()) : cell;
            place(e.item, QRect(area.x() + e.column * (cell + spacing()), y, width, h));
        }
        y += h + spacing();
    }
}

void FixedRowLayout::arrangeRow(int row, int y, const QRect& area)
{
    std::vector<const Entry*> entries;
    std::vector<Widths> limits;
    for (const Entry& e : mItems)
        if (e.row == row)
        {
            entries.push_back(&e);
            limits.push_back(widths(e.item));
        }
    const size_t n = entries.size();
    const int avail = area.width() - spacing() * (int(n) - 1);
    std::vector<int> width(n);
    int total = 0;
    for (size_t i = 0; i < n; ++i)
        total += width[i] = limits[i].full;

    // Buttons drop to their icon from the right, so the leftmost keep their labels longest.
    for (size_t i = n; i-- > 0 && total > avail;)
        if (limits[i].button)
        {
            total -= width[i] - limits[i].least;
            width[i] = limits[i].least;
        }
    int slack = 0;
    for (size_t i = 0; i < n; ++i)
        if (!limits[i].button)
            slack += width[i] - limits[i].least;
    if (total > avail && slack > 0)
    {
        double share = std::min(1.0, double(total - avail) / slack);
        for (size_t i = 0; i < n; ++i)
            if (!limits[i].button)
                width[i] -= int(std::ceil((width[i] - limits[i].least) * share));
    }

    const int h = rowHeight(row);
    int x = area.x();
    for (size_t i = 0; i < n; ++i)
    {
        if (limits[i].button)
            setCompact(limits[i].button, width[i] < limits[i].full);
        place(entries[i]->item, QRect(x, y, width[i], h));
        x += width[i] + spacing();
    }
}
} // namespace lila::ui
