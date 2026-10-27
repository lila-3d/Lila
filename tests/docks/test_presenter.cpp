#include <doctest/doctest.h>

#include "core/PrefStore.h"
#include "ops/Command.h"
#include "ui/Actions.h"
#include "ui/Presenter.h"

#include <QApplication>
#include <QWidget>

#include <algorithm>

using namespace lila;
using json = nlohmann::json;

namespace
{
bool hasCard(const QString& prefix)
{
    const std::vector<QString> cards = ui::presenter::cards();
    return std::ranges::any_of(cards, [&](const QString& c) { return c.startsWith(prefix); });
}
} // namespace

TEST_CASE("presenter: shows actions with their menu path, edits from the UI and agent commands, never selection clicks")
{
    auto* main = new QWidget; // lives as long as the presenter, which installs once
    main->resize(800, 600);
    main->show();
    ui::presenter::install(main);
    PrefStore::get().set("general", "/presenter/maxCards", 10);
    ui::presenter::setEnabled(true);

    ui::actions::add({"test.presenterBevel", "Bevel Test", "Test", [] {}, nullptr, "Does nothing; the presenter test runs it."});
    {
        ui::actions::ViaScope via(ui::actions::Via::Menu, "Edit Mesh > Bevel Test");
        ui::actions::run(json("test.presenterBevel"));
    }
    CHECK(hasCard("Bevel Test|Edit Mesh > Bevel Test|"));

    ops::registerCommand({.name = "presenterNudge", .doc = "Test command.", .flags = {{"by", "", ArgType::Any, nullptr, "Offset."}}, .run = [](const Args&) -> json { return nullptr; }});
    ops::registerCommand({.name = "selectPresenterThing", .doc = "Test command.", .run = [](const Args&) -> json { return nullptr; }});
    ops::execute("presenterNudge", {{"by", json::array({12.0, 0.0, 0.0})}}, CallSource::UI);
    CHECK(hasCard("Presenter Nudge|by 12, 0, 0|"));

    // A command the action runs is the action's, not a card of its own.
    ui::actions::add({"test.presenterWraps", "Wrapped Test", "Test", [] { ops::execute("presenterNudge", json::object(), CallSource::UI); }, nullptr,
                      "Runs a command; the presenter test checks only the action shows."});
    const size_t before = ui::presenter::cards().size();
    ui::actions::run(json("test.presenterWraps"));
    CHECK(hasCard("Wrapped Test|"));
    CHECK(ui::presenter::cards().size() <= before + 1);

    ops::execute("selectPresenterThing", json::object(), CallSource::UI);
    CHECK_FALSE(hasCard("Select Presenter Thing"));

    // Script commands show only when an agent asked for them.
    ops::execute("presenterNudge", {{"by", 3}}, CallSource::Script);
    CHECK_FALSE(hasCard("Presenter Nudge|by 3|"));
    {
        ops::AgentScope agent;
        ops::execute("presenterNudge", {{"by", 4}}, CallSource::Script);
    }
    CHECK(hasCard("Presenter Nudge|by 4|"));

    ui::presenter::setEditing(true);
    CHECK(ui::presenter::editing());
    ui::presenter::setEnabled(false);
    CHECK_FALSE(ui::presenter::editing());
    CHECK(ui::presenter::cards().empty());

    ui::actions::remove("test.presenterBevel");
    ui::actions::remove("test.presenterWraps");
    ops::unregisterCommand("presenterNudge");
    ops::unregisterCommand("selectPresenterThing");
}
