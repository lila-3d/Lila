#pragma once

#include "core/Export.h"

#include <QLayout>
#include <QList>

namespace lila::ui
{
/**
 * Lays widgets in authored rows that never rewrap, so a button keeps its place whatever the panel width. A narrower panel
 * shrinks items instead: labelled tool buttons drop to their icon (rightmost first), spin boxes and combos shrink to their
 * minimum. Hidden items keep their slot.
 *
 * With columns set, every item sits in a uniform cell and rows wrap at that count, so buttons line up across rows; the
 * cells stretch with the panel up to the widest button's natural width.
 */
class LILA_UI_API FixedRowLayout : public QLayout
{
public:
    explicit FixedRowLayout(int columns = 0, int spacing = 4);
    ~FixedRowLayout() override;

    /** The next item starts a new row. */
    void endRow();

    void addItem(QLayoutItem* item) override;
    int count() const override;
    QLayoutItem* itemAt(int index) const override;
    QLayoutItem* takeAt(int index) override;
    Qt::Orientations expandingDirections() const override;
    QSize minimumSize() const override;
    QSize sizeHint() const override;
    void setGeometry(const QRect& rect) override;

private:
    struct Entry
    {
        QLayoutItem* item;
        int row;
        int column;
    };
    int rowCount() const;
    int rowHeight(int row) const;
    void arrangeGrid(const QRect& area);
    void arrangeRow(int row, int y, const QRect& area);

    QList<Entry> mItems;
    int mColumns;
    bool mBreak = false;
};
} // namespace lila::ui
