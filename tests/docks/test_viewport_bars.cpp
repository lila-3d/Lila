#include <doctest/doctest.h>

#include "ui/ViewportHeader.h"
#include "ui/ViewportWidget.h"

#include <QMenu>
#include <QToolButton>

using namespace lila::ui;

namespace
{
QToolButton* chevronOf(QWidget& header)
{
    for (QToolButton* b : header.findChildren<QToolButton*>(Qt::FindDirectChildrenOnly))
        if (b->toolTip() == "More viewport options")
            return b;
    return nullptr;
}

QToolButton* buttonWithText(QWidget& header, const QString& prefix)
{
    for (QToolButton* b : header.findChildren<QToolButton*>())
        if (b->text().startsWith(prefix))
            return b;
    return nullptr;
}
} // namespace

TEST_CASE("viewport header drops labels, then folds items into the chevron as it narrows")
{
    ViewportWidget view;
    ViewportHeader header(&view);
    header.resize(1600, header.sizeHint().height());
    header.show();
    QToolButton* chevron = chevronOf(header);
    REQUIRE(chevron);
    QToolButton* snap = buttonWithText(header, "Snap");
    QToolButton* symmetry = buttonWithText(header, "Symmetry");
    REQUIRE(snap);
    REQUIRE(symmetry);
    CHECK_FALSE(chevron->isVisible());
    CHECK(snap->toolButtonStyle() == Qt::ToolButtonTextBesideIcon);

    header.resize(520, header.height());
    CHECK(snap->toolButtonStyle() == Qt::ToolButtonIconOnly);

    header.resize(260, header.height());
    CHECK(chevron->isVisible());
    Q_EMIT chevron->menu()->aboutToShow();
    CHECK_FALSE(chevron->menu()->actions().isEmpty());
}
