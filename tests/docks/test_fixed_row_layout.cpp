#include <doctest/doctest.h>

#include "ui/FixedRowLayout.h"
#include "ui/UvEditor.h"

#include <QApplication>
#include <QIcon>
#include <QPixmap>
#include <QSpinBox>
#include <QToolButton>
#include <QWidget>

#include <map>

using namespace lila::ui;

namespace
{
QToolButton* labelled(const QString& text, QWidget* parent)
{
    auto* b = new QToolButton(parent);
    QPixmap px(16, 16);
    px.fill(Qt::gray);
    b->setIcon(QIcon(px));
    b->setText(text);
    b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    return b;
}

void layoutAt(QWidget& w, int width)
{
    w.resize(width, w.height());
    QApplication::processEvents();
    w.layout()->activate();
}

/** Each widget's row (top edge) and the order of the widgets within it. */
std::map<QWidget*, std::pair<int, int>> places(QWidget& w)
{
    std::map<QWidget*, std::pair<int, int>> out;
    std::map<int, int> next;
    QList<QWidget*> kids = w.findChildren<QWidget*>(Qt::FindDirectChildrenOnly);
    std::sort(kids.begin(), kids.end(), [](QWidget* a, QWidget* b) { return a->y() != b->y() ? a->y() < b->y() : a->x() < b->x(); });
    for (QWidget* k : kids)
        out[k] = {k->y(), next[k->y()]++};
    return out;
}
} // namespace

TEST_CASE("fixed rows keep every widget on its row and in its order at any width")
{
    QWidget w;
    auto* rows = new FixedRowLayout();
    w.setLayout(rows);
    for (const char* t : {"Extrude", "Bevel", "Bridge", "Inset"})
        rows->addWidget(labelled(t, &w));
    rows->endRow();
    rows->addWidget(labelled("Merge", &w));
    rows->addWidget(new QSpinBox(&w));
    w.show();

    layoutAt(w, 900);
    auto wide = places(w);
    for (int width : {600, 300, w.minimumSizeHint().width()})
    {
        layoutAt(w, width);
        CAPTURE(width);
        auto now = places(w);
        for (auto& [k, place] : wide)
            CHECK(now[k].second == place.second);
        std::map<int, int> rowOf;
        for (auto& [k, place] : wide)
            rowOf[place.first] = now[k].first;
        for (auto& [k, place] : wide)
            CHECK(now[k].first == rowOf[place.first]);
    }
}

TEST_CASE("a narrow row drops labels from the right and keeps them when there is room")
{
    QWidget w;
    auto* rows = new FixedRowLayout();
    w.setLayout(rows);
    QToolButton* first = labelled("Quadrangulate", &w);
    QToolButton* last = labelled("Triangulate", &w);
    rows->addWidget(first);
    rows->addWidget(last);
    w.show();

    layoutAt(w, 600);
    CHECK(first->toolButtonStyle() == Qt::ToolButtonTextBesideIcon);
    CHECK(last->toolButtonStyle() == Qt::ToolButtonTextBesideIcon);

    layoutAt(w, first->sizeHint().width() + 40);
    CHECK(first->toolButtonStyle() == Qt::ToolButtonTextBesideIcon);
    CHECK(last->toolButtonStyle() == Qt::ToolButtonIconOnly);
    CHECK(last->text() == "Triangulate");

    layoutAt(w, w.minimumSizeHint().width());
    CHECK(first->toolButtonStyle() == Qt::ToolButtonIconOnly);

    layoutAt(w, 600);
    CHECK(first->toolButtonStyle() == Qt::ToolButtonTextBesideIcon);
    CHECK(last->toolButtonStyle() == Qt::ToolButtonTextBesideIcon);
}

TEST_CASE("grid columns line up across rows and a hidden button keeps its cell")
{
    QWidget w;
    auto* grid = new FixedRowLayout(3);
    w.setLayout(grid);
    std::vector<QToolButton*> b;
    for (const char* t : {"Grow", "Shrink", "Loop", "Ring", "Border"})
    {
        b.push_back(labelled(t, &w));
        grid->addWidget(b.back());
    }
    w.show();
    layoutAt(w, 400);
    CHECK(b[3]->y() > b[0]->y());
    CHECK(b[3]->x() == b[0]->x());
    CHECK(b[4]->x() == b[1]->x());

    const int before = b[2]->x();
    b[1]->hide();
    layoutAt(w, 400);
    CHECK(b[2]->x() == before);
}

TEST_CASE("a narrow grid keeps every button in its cell and drops labels to icons")
{
    QWidget w;
    auto* grid = new FixedRowLayout(4);
    w.setLayout(grid);
    std::vector<QToolButton*> b;
    for (const char* t : {"Extrude", "Bevel", "Bridge", "Inset", "Merge"})
    {
        b.push_back(labelled(t, &w));
        grid->addWidget(b.back());
    }
    w.show();
    for (int width : {grid->sizeHint().width() * 2, grid->sizeHint().width(), grid->sizeHint().width() * 3 / 4, w.minimumSizeHint().width()})
    {
        layoutAt(w, width);
        CAPTURE(width);
        CHECK(b[3]->y() == b[0]->y());
        CHECK(b[4]->y() > b[0]->y());
        CHECK(b[4]->x() == b[0]->x());
    }
    CHECK(b[0]->toolButtonStyle() == Qt::ToolButtonIconOnly);
    layoutAt(w, grid->sizeHint().width());
    for (QToolButton* button : b)
        CHECK(button->toolButtonStyle() == Qt::ToolButtonTextBesideIcon);
}

TEST_CASE("the UV editor's buttons keep their rows from wide to its narrowest")
{
    UvEditor editor;
    editor.resize(1200, 800);
    editor.show();
    QApplication::processEvents();
    std::map<QToolButton*, QPoint> wide;
    for (QToolButton* b : editor.findChildren<QToolButton*>())
        if (b->isVisible())
            wide[b] = b->mapTo(&editor, QPoint(0, 0));

    editor.resize(editor.minimumSizeHint().width(), 800);
    QApplication::processEvents();
    for (auto& [b, at] : wide)
    {
        CAPTURE(b->text().toStdString());
        CHECK(b->mapTo(&editor, QPoint(0, 0)).y() == at.y());
    }
}

