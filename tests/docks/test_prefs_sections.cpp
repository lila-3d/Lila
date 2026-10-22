#include <doctest/doctest.h>

#include "core/PrefStore.h"
#include "ui/PreferencesDialog.h"

#include <QFormLayout>
#include <QTabWidget>
#include <QToolButton>

using namespace lila;

TEST_CASE("preference sections collapse their rows and remember it")
{
    const char* key = "/preferences/collapsed/Interface/Floating Shelf";
    PrefStore::get().set("general", key, false);
    auto* dialog = new ui::PreferencesDialog;
    QToolButton* header = nullptr;
    for (QToolButton* b : dialog->findChildren<QToolButton*>())
        if (b->text() == "Floating Shelf")
            header = b;
    REQUIRE(header);
    auto* form = qobject_cast<QFormLayout*>(header->parentWidget()->layout());
    REQUIRE(form);
    int headerRow = -1;
    form->getWidgetPosition(header, &headerRow, nullptr);
    CHECK(form->isRowVisible(headerRow + 1));

    header->click();
    CHECK_FALSE(form->isRowVisible(headerRow + 1));
    CHECK(form->isRowVisible(headerRow));
    CHECK(PrefStore::get().value<bool>("general", key, false));
    delete dialog;

    dialog = new ui::PreferencesDialog;
    for (QToolButton* b : dialog->findChildren<QToolButton*>())
        if (b->text() == "Floating Shelf")
            CHECK_FALSE(b->isChecked());
    PrefStore::get().set("general", key, false);
    delete dialog;
}

TEST_CASE("the Interface page follows General and holds the chrome toggles")
{
    ui::PreferencesDialog dialog;
    auto* tabs = dialog.findChild<QTabWidget*>();
    REQUIRE(tabs);
    REQUIRE(tabs->count() > 1);
    CHECK(tabs->tabText(0) == "General");
    CHECK(tabs->tabText(1) == "Interface");
    CHECK(dialog.width() >= 840);
    bool chrome = false;
    for (QToolButton* b : tabs->widget(1)->findChildren<QToolButton*>())
        chrome |= b->text() == "Chrome";
    CHECK(chrome);
}
