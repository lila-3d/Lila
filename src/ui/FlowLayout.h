#pragma once

#include <QLayout>
#include <QList>

namespace lila::ui
{
/** Lays items left to right and wraps onto new lines, so a row of tool buttons never forces its panel wider. */
class FlowLayout : public QLayout
{
public:
    explicit FlowLayout(int spacing = 4);
    ~FlowLayout() override;

    void addItem(QLayoutItem* item) override;
    int count() const override;
    QLayoutItem* itemAt(int index) const override;
    QLayoutItem* takeAt(int index) override;
    Qt::Orientations expandingDirections() const override;
    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;
    QSize minimumSize() const override;
    QSize sizeHint() const override;
    void setGeometry(const QRect& rect) override;

private:
    int arrange(const QRect& rect, bool apply) const;

    QList<QLayoutItem*> mItems;
};
} // namespace lila::ui
