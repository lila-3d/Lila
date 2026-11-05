#include <doctest/doctest.h>

#include "ui/Actions.h"
#include "ui/Menus.h"
#include "ui/UserMenus.h"

using namespace lila::ui;
using json = nlohmann::json;

namespace
{
std::vector<std::string> rows(const std::string& path)
{
    std::vector<std::string> out;
    for (const menus::Item* i : menus::itemsIn(path))
        out.push_back(i->kind == menus::Kind::Action ? i->action : i->kind == menus::Kind::Section ? "#" + i->label : i->id);
    return out;
}

/** Puts the test's own menus in place and clears them again however the test ends. */
struct Scope
{
    explicit Scope(json menus)
    {
        actions::add({"test.um.a", "A", "Test", [] {}, nullptr, "a"});
        actions::add({"test.um.b", "B", "Test", [] {}, nullptr, "b"});
        menus::addMenu({.id = "UmHost", .title = "&Host"});
        menus::addActions("UmHost", {"#First", "test.um.a", "#Second", "test.um.b"});
        menus::addSubmenu("UmHost", "Deep");
        userMenus::setAll(std::move(menus));
    }
    ~Scope()
    {
        userMenus::setAll(json::array());
        menus::removeMenu("UmHost");
        actions::remove("test.um.a");
        actions::remove("test.um.b");
    }
};
} // namespace

TEST_CASE("a user menu on the menu bar registers its rows, wrapping rows with their own label or Python")
{
    Scope s(json::array({{{"title", "Kit"},
                          {"before", "UmHost"},
                          {"items",
                           {{{"action", "test.um.a"}},
                            {{"separator", true}},
                            {{"action", "test.um.b"}, {"label", "Bee"}},
                            {{"action", {{"python", "print(1)"}}}, {"label", "Say One"}},
                            {{"submenu", "More"}, {"items", {{{"action", "test.um.a"}}}}}}}}}));

    const menus::Menu* kit = menus::menuRegistry().find("user:Kit");
    REQUIRE(kit);
    CHECK(kit->title == "Kit");
    CHECK(kit->before == "UmHost");
    CHECK(*menus::menuRegistry().ownerOf("user:Kit") == userMenus::kOwner);

    std::vector<std::string> r = rows("user:Kit");
    REQUIRE(r.size() == 5);
    CHECK(r[0] == "test.um.a");
    CHECK(r[2] == "user.kit.bee");
    CHECK(r[3] == "user.kit.sayOne");
    CHECK(rows("user:Kit/More") == std::vector<std::string>{"test.um.a"});

    const Action* bee = actions::find("user.kit.bee");
    REQUIRE(bee);
    CHECK(bee->label == "Bee");
    CHECK(bee->category == "User Menus");
    CHECK(actions::find("user.kit.sayOne")->python == "print(1)");
}

TEST_CASE("a user menu goes into another menu as a submenu after an item, or inline as a titled section")
{
    Scope s(json::array({{{"title", "Sub"}, {"parent", "UmHost"}, {"after", "UmHost/test.um.a"}, {"items", {{{"action", "test.um.b"}}}}},
                         {{"title", "Inline"},
                          {"parent", "UmHost"},
                          {"as", "section"},
                          {"after", "UmHost/test.um.b"},
                          {"items", {{{"action", "test.um.a"}}, {{"action", "test.um.b"}}}}},
                         {{"title", "Nested"}, {"parent", "UmHost/Deep"}, {"items", {{{"action", "test.um.a"}}}}}}));

    CHECK(rows("UmHost") == std::vector<std::string>{"#First", "test.um.a", "user:Sub", "#Second", "test.um.b", "#Inline", "test.um.a",
                                                     "test.um.b", "UmHost/Deep/"});
    CHECK(rows("UmHost/Sub") == std::vector<std::string>{"test.um.b"});
    CHECK(rows("UmHost/Deep/Nested") == std::vector<std::string>{"test.um.a"});
    CHECK(userMenus::rowsPath(userMenus::all()[1]) == "UmHost");
}

TEST_CASE("saving user menus again replaces what they registered")
{
    Scope s(json::array({{{"title", "Kit"}, {"items", {{{"action", "test.um.b"}, {"label", "Bee"}}}}}}));
    REQUIRE(actions::find("user.kit.bee"));
    userMenus::setAll(json::array({{{"title", "Kat"}, {"items", json::array()}}}));
    CHECK_FALSE(menus::menuRegistry().contains("user:Kit"));
    CHECK_FALSE(actions::find("user.kit.bee"));
    CHECK(menus::menuRegistry().contains("user:Kat"));
}

TEST_CASE("a user menu that cannot be stored says why")
{
    CHECK(userMenus::problem({{"title", ""}}) == "a menu needs a title");
    CHECK_FALSE(userMenus::problem({{"title", "A/B"}}).empty());
    CHECK_FALSE(userMenus::problem({{"title", "Kit"}}, json::array({{{"title", "Kit"}}})).empty());
    CHECK_FALSE(userMenus::problem({{"title", "Kit"}, {"items", {{{"label", "nothing"}}}}}).empty());
    CHECK_FALSE(userMenus::problem({{"title", "Kit"}, {"items", {{{"submenu", "A"}}, {{"submenu", "A"}}}}}).empty());
    CHECK(userMenus::problem({{"title", "Kit"}, {"items", {{{"action", "edit.undo"}}, {{"submenu", "A"}, {"items", json::array()}}}}}).empty());

    menus::addMenu({.id = "UmTaken", .title = "UmTaken"});
    CHECK_FALSE(userMenus::problem({{"title", "UmTaken"}}).empty());
    CHECK(userMenus::problem({{"title", "UmTaken"}, {"parent", "Mesh"}}).empty());
    menus::removeMenu("UmTaken");
}
