#include <doctest/doctest.h>

#include "ui/ViewportToolbar.h"

#include <QMenu>
#include <QToolButton>

using namespace lila::ui;

namespace
{
QToolButton* chevronOf(QWidget& bar)
{
    for (QToolButton* b : bar.findChildren<QToolButton*>(Qt::FindDirectChildrenOnly))
        if (b->toolTip() == "More viewport options")
            return b;
    return nullptr;
}

/** The bar's direct child rows (each group is one), visible ones only. */
int visibleRows(QWidget& bar)
{
    int n = 0;
    for (QWidget* w : bar.findChildren<QWidget*>(Qt::FindDirectChildrenOnly))
        if (w->isVisible() && !qobject_cast<QToolButton*>(w) && w->objectName() != "toolbarSeparator")
            ++n;
    return n;
}
} // namespace

TEST_CASE("viewport bottom bar keeps the snaps and folds the rest into the chevron as it narrows")
{
    ViewportBottomBar bar(nullptr);
    bar.resize(1400, bar.sizeHint().height());
    bar.show();
    QToolButton* chevron = chevronOf(bar);
    REQUIRE(chevron);
    // Highlight, masks, symmetry and snaps; Retopo Draw's row only shows while that tool runs.
    CHECK(visibleRows(bar) == 4);
    CHECK_FALSE(chevron->isVisible());

    bar.resize(360, bar.height());
    CHECK(chevron->isVisible());
    CHECK(visibleRows(bar) >= 1);
    CHECK(visibleRows(bar) < 4);
    emit chevron->menu()->aboutToShow();
    CHECK_FALSE(chevron->menu()->actions().isEmpty());
}
