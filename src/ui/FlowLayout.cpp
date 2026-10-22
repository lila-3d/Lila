#include "ui/FlowLayout.h"

#include <QWidget>

#include <algorithm>

namespace lila::ui
{
FlowLayout::FlowLayout(int spacing)
{
    setSpacing(spacing);
    setContentsMargins(0, 0, 0, 0);
}

FlowLayout::~FlowLayout()
{
    while (QLayoutItem* item = takeAt(0))
        delete item;
}

void FlowLayout::addItem(QLayoutItem* item)
{
    mItems.append(item);
}

int FlowLayout::count() const
{
    return int(mItems.size());
}

QLayoutItem* FlowLayout::itemAt(int index) const
{
    return mItems.value(index);
}

QLayoutItem* FlowLayout::takeAt(int index)
{
    return index >= 0 && index < mItems.size() ? mItems.takeAt(index) : nullptr;
}

Qt::Orientations FlowLayout::expandingDirections() const
{
    return {};
}

bool FlowLayout::hasHeightForWidth() const
{
    return true;
}

int FlowLayout::heightForWidth(int width) const
{
    return arrange(QRect(0, 0, width, 0), false);
}

QSize FlowLayout::minimumSize() const
{
    QSize size;
    for (QLayoutItem* item : mItems)
        size = size.expandedTo(item->minimumSize());
    QMargins m = contentsMargins();
    return size + QSize(m.left() + m.right(), m.top() + m.bottom());
}

QSize FlowLayout::sizeHint() const
{
    int width = 0, height = 0;
    for (QLayoutItem* item : mItems)
    {
        QSize s = item->sizeHint();
        width += s.width() + spacing();
        height = std::max(height, s.height());
    }
    return {width, height};
}

void FlowLayout::setGeometry(const QRect& rect)
{
    QLayout::setGeometry(rect);
    arrange(rect, true);
}

int FlowLayout::arrange(const QRect& rect, bool apply) const
{
    QRect area = rect.marginsRemoved(contentsMargins());
    int x = area.x(), y = area.y(), lineHeight = 0;
    for (QLayoutItem* item : mItems)
    {
        if (item->isEmpty())
            continue;
        QSize s = item->sizeHint();
        if (x + s.width() > area.right() + 1 && lineHeight > 0)
        {
            x = area.x();
            y += lineHeight + spacing();
            lineHeight = 0;
        }
        if (apply)
            item->setGeometry(QRect(QPoint(x, y), s));
        x += s.width() + spacing();
        lineHeight = std::max(lineHeight, s.height());
    }
    return y + lineHeight - rect.y() + contentsMargins().bottom();
}
} // namespace lila::ui
