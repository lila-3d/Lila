#include <doctest/doctest.h>

#include "ui/PreferencesDialog.h"
#include "ui/WheelGuard.h"

#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QTabWidget>
#include <QWheelEvent>

using namespace lila;

namespace
{
void wheel(QWidget* w)
{
    QWidget* target = w;
    const QPointF pos = target->rect().center();
    QWheelEvent e(pos, target->mapToGlobal(pos), QPoint(), QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(target, &e);
}
} // namespace

TEST_CASE("preference fields ignore the wheel until clicked into")
{
    static QObject owner;
    static const bool installed = (ui::installWheelGuard(&owner), true);
    (void)installed;
    ui::PreferencesDialog dialog;
    dialog.show();
    auto* tabs = dialog.findChild<QTabWidget*>();
    REQUIRE(tabs);
    for (int i = 0; i < tabs->count(); ++i)
        if (tabs->tabText(i) == "Viewport")
            tabs->setCurrentIndex(i);
    QApplication::processEvents();

    QDoubleSpinBox* spin = nullptr;
    QComboBox* combo = nullptr;
    for (QDoubleSpinBox* s : tabs->currentWidget()->findChildren<QDoubleSpinBox*>())
        if (!spin && s->value() < s->maximum() && s->value() > s->minimum())
            spin = s;
    for (QComboBox* c : tabs->currentWidget()->findChildren<QComboBox*>())
        if (!combo && c->count() > 1 && c->currentIndex() == 0)
            combo = c;
    REQUIRE(spin);
    REQUIRE(combo);

    const double value = spin->value();
    const int index = combo->currentIndex();
    wheel(spin);
    wheel(combo);
    CHECK(spin->value() == value);
    CHECK(combo->currentIndex() == index);
    CHECK_FALSE(spin->hasFocus());
    CHECK_FALSE(combo->hasFocus());

    // A clicked-in field takes the wheel until the wheel turns somewhere else.
    dialog.activateWindow();
    spin->setFocus();
    REQUIRE(spin->hasFocus());
    wheel(spin);
    CHECK(spin->value() != value);
    const double changed = spin->value();
    wheel(tabs->currentWidget());
    CHECK_FALSE(spin->hasFocus());
    wheel(spin);
    CHECK(spin->value() == changed);
}
