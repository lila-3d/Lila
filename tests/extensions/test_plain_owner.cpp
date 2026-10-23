#include "Isolate.h"

#include "ops/Command.h"
#include "core/Registry.h"
#include "extensions/ExtensionManager.h"
#include "ui/Actions.h"
#include "ui/MarkingMenu.h"
#include "ui/Menus.h"
#include "ui/Modes.h"
#include "ui/OptionBox.h"
#include "ui/Panels.h"

#include <doctest/doctest.h>

using namespace lila;
using namespace lila::ui;

namespace
{
std::vector<size_t> sizes()
{
    return {actions::registry().size(), menus::itemRegistry().size(), panels::registry().size(), markingMenus::registry().size(),
            modes::registry().size(), optionBox::customRegistry().size(), ops::registry().size()};
}

const std::string* ownerOf(const Registry<Action>& r, const char* id)
{
    return r.ownerOf(id);
}
} // namespace

TEST_CASE("registrations made during an extension's init belong to it, whatever owner they name, and go before its library")
{
    isolate();
    modes::registerBuiltins();
    modes::activate(modes::kDefaultMode, true);
    std::vector<size_t> before = sizes();

    auto& mgr = extensions::ExtensionManager::get();
    REQUIRE(mgr.load(LILA_TEST_PLAIN_OWNER_PACKAGE) == "plainOwner");
    CHECK(sizes() != before);
    REQUIRE(ownerOf(actions::registry(), "plainOwner.go"));
    CHECK(*ownerOf(actions::registry(), "plainOwner.go") == "plainOwner");
    CHECK(*ownerOf(actions::registry(), "plainOwner.claimsLila") == "plainOwner");
    CHECK(*actions::registry().ownerOf("mode.plainOwnerMode") == "plainOwner");
    CHECK(*panels::registry().ownerOf("plainOwnerPanel") == "plainOwner");
    CHECK(*modes::registry().ownerOf("plainOwnerMode") == "plainOwner");
    REQUIRE(ops::find("plainOwnerCommand"));
    CHECK(ops::find("plainOwnerCommand")->owner == "plainOwner");
    CHECK(ops::execute("plainOwnerCommand", json::object()) == 42);
    CHECK(actions::run(json("plainOwner.go")));
    CHECK(modes::activate("plainOwnerMode"));

    mgr.unload("plainOwner");
    CHECK(modes::current() == modes::kDefaultMode);
    CHECK(sizes() == before);
    CHECK_FALSE(actions::find("plainOwner.go"));
    CHECK_FALSE(ops::find("plainOwnerCommand"));

    // Loads again cleanly: nothing of the first load was left behind to clash with.
    REQUIRE(mgr.load(LILA_TEST_PLAIN_OWNER_PACKAGE) == "plainOwner");
    mgr.unload("plainOwner");
    CHECK(sizes() == before);
}

TEST_CASE("an empty owner scope keeps Lila's own registrations Lila's")
{
    Registry<int> r;
    {
        owners::OwnerScope ext("someExtension");
        r.put("a", 1, "lila");
        {
            owners::OwnerScope lila({});
            r.put("b", 2, "lila");
        }
        r.put("c", 3);
    }
    r.put("d", 4, "lila");
    CHECK(*r.ownerOf("a") == "someExtension");
    CHECK(*r.ownerOf("b") == "lila");
    CHECK(*r.ownerOf("c") == "someExtension");
    CHECK(*r.ownerOf("d") == "lila");
    CHECK(owners::removeOwned("someExtension") == 2);
    CHECK(r.size() == 2);
}
