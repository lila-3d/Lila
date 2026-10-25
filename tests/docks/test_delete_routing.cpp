#include <doctest/doctest.h>

#include "ui/Deletion.h"

#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

#include <memory>

using namespace lila::ui;
using deletion::Target;

TEST_CASE("Delete goes to the focused panel's claim, then the hovered one, and to the scene only from a scene panel")
{
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* viewport = new QWidget(&window);
    auto* panel = new QWidget(&window);
    auto* rows = new QListWidget(panel);
    auto* field = new QLineEdit(panel);
    auto* button = new QPushButton(panel);
    auto* shelf = new QListWidget(panel);
    auto* stray = new QListWidget(&window);
    auto* other = new QPushButton(&window);
    for (QWidget* w : {viewport, panel, static_cast<QWidget*>(stray), static_cast<QWidget*>(other)})
        layout->addWidget(w);

    int removed = 0, sceneDeletes = 0;
    deletion::claimScene(viewport);
    deletion::claim(panel, [&] { ++removed; });
    deletion::claim(shelf, nullptr);
    auto scene = [&] { ++sceneDeletes; };

    SUBCASE("focus inside a claim takes it, whatever is under the cursor")
    {
        CHECK(deletion::route(viewport, panel).target == Target::Scene);
        deletion::Route r = deletion::route(rows, viewport);
        CHECK(r.target == Target::Panel);
        CHECK(r.owner == panel);
        CHECK(deletion::route(button, viewport).target == Target::Panel);
        deletion::run(rows, viewport, scene);
        CHECK(removed == 1);
        CHECK(sceneDeletes == 0);
    }

    SUBCASE("a child's own claim wins over its panel's, and an empty claim deletes nothing")
    {
        deletion::Route r = deletion::route(shelf, viewport);
        CHECK(r.target == Target::Nothing);
        CHECK(r.owner == shelf);
        deletion::run(shelf, viewport, scene);
        CHECK(removed == 0);
        CHECK(sceneDeletes == 0);
    }

    SUBCASE("a text field keeps Delete even inside a claimed panel")
    {
        CHECK(deletion::route(field, viewport).target == Target::Nothing);
        CHECK(deletion::route(nullptr, field).target == Target::Nothing);
    }

    SUBCASE("an unclaimed list never falls through to the panel under the cursor")
    {
        CHECK(deletion::route(stray, viewport).target == Target::Nothing);
        CHECK(deletion::route(stray, panel).target == Target::Nothing);
    }

    SUBCASE("with focus claiming nothing, the widget under the cursor decides")
    {
        CHECK(deletion::route(other, viewport).target == Target::Scene);
        CHECK(deletion::route(other, rows).target == Target::Panel);
        CHECK(deletion::route(nullptr, viewport).target == Target::Scene);
        CHECK(deletion::route(other, other).target == Target::Nothing);
        CHECK(deletion::route(nullptr, nullptr).target == Target::Nothing);
        deletion::run(nullptr, viewport, scene);
        CHECK(sceneDeletes == 1);
    }

    SUBCASE("claiming again replaces the claim, and a claim goes with its widget")
    {
        int second = 0;
        deletion::claim(panel, [&] { ++second; });
        deletion::run(rows, nullptr, scene);
        CHECK(removed == 0);
        CHECK(second == 1);
        deletion::claimScene(panel);
        CHECK(deletion::route(rows, nullptr).target == Target::Scene);

        auto gone = std::make_unique<QWidget>(&window);
        auto* inside = new QPushButton(gone.get());
        deletion::claim(gone.get(), [&] { ++removed; });
        CHECK(deletion::route(inside, nullptr).target == Target::Panel);
        gone.reset();
        CHECK(deletion::route(other, nullptr).target == Target::Nothing);
    }
}
