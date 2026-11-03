#include <doctest/doctest.h>

#include "ui/SpinScrub.h"

#include <QApplication>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QWidget>

#include <utility>

namespace
{
void widgetMouse(QWidget* w, QEvent::Type type, QPoint at, Qt::MouseButton button, Qt::MouseButtons buttons)
{
    QMouseEvent e(type, at, w->mapToGlobal(at), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(w, &e);
}

void dragScrub(QWidget* w, Qt::MouseButton button, int dx)
{
    const QPoint at(4, 4);
    widgetMouse(w, QEvent::MouseButtonPress, at, button, button);
    for (int x = 6; x <= dx; x += 6)
        widgetMouse(w, QEvent::MouseMove, at + QPoint(x, 0), Qt::NoButton, button);
    widgetMouse(w, QEvent::MouseButtonRelease, at + QPoint(dx, 0), button, Qt::NoButton);
}
} // namespace

TEST_CASE("a spin box's form label scrubs it with the left button")
{
    static bool installed = false;
    if (!std::exchange(installed, true))
        lila::ui::installSpinScrub(qApp);

    QWidget panel;
    auto* form = new QFormLayout(&panel);
    auto* box = new QDoubleSpinBox(&panel);
    box->setRange(0.0, 10.0);
    box->setSingleStep(0.5);
    form->addRow("Strength", box);
    auto* label = qobject_cast<QLabel*>(form->labelForField(box));
    REQUIRE(label);

    // Six pixels a step.
    dragScrub(label, Qt::LeftButton, 36);
    CHECK(box->value() == doctest::Approx(3.0));
    CHECK(label->cursor().shape() == Qt::SizeHorCursor);
    dragScrub(label, Qt::MiddleButton, 12);
    CHECK(box->value() == doctest::Approx(4.0));

    // The field itself keeps its left button for editing.
    dragScrub(box, Qt::LeftButton, 36);
    CHECK(box->value() == doctest::Approx(4.0));

    // A label that scrubs by itself is left to do so.
    QLabel own("Own", &panel);
    own.setCursor(Qt::SizeHorCursor);
    own.setBuddy(box);
    dragScrub(&own, Qt::LeftButton, 36);
    CHECK(box->value() == doctest::Approx(4.0));
}
