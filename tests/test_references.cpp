#include <doctest/doctest.h>

#include "cmds/Command.h"

#include <filesystem>

using namespace lila;
using json = nlohmann::json;

namespace
{
json run(const char* name, json call = json::object()) { return cmds::execute(name, call); }
double get(const std::string& plug) { return run("getAttr", {{"args", {plug}}}).get<double>(); }

std::string tempPath(const char* name)
{
    auto dir = std::filesystem::temp_directory_path() / "lila_tests" / "refs";
    std::filesystem::create_directories(dir);
    return (dir / name).generic_string();
}
} // namespace

TEST_CASE("references load, keep edits across reload, and save")
{
    // The asset: a group with a locator, plus a connection inside it.
    run("newScene");
    run("group", {{"empty", true}, {"name", "Root"}});
    run("spaceLocator", {{"name", "Tip"}});
    run("parent", {{"args", {"Tip", "Root"}}});
    run("setAttr", {{"args", {"Tip.ty", 5}}});
    run("spaceLocator", {{"name", "Driven"}});
    run("connectAttr", {{"args", {"Tip.translate", "Driven.rotate"}}});
    std::string asset = tempPath("asset.lilaa");
    run("saveScene", {{"args", {asset}}});

    // The shot references it twice and edits one copy.
    run("newScene");
    CHECK(run("createReference", {{"args", {asset}}, {"namespace", "hero"}}) == "hero");
    CHECK(run("createReference", {{"args", {asset}}}) == "asset");
    CHECK(get("hero:Tip.ty") == doctest::Approx(5));
    CHECK(get("hero:Driven.ry") == doctest::Approx(5));
    run("setAttr", {{"args", {"hero:Tip.tx", 3}}});
    run("spaceLocator", {{"name", "Prop"}});
    run("parent", {{"args", {"Prop", "hero:Tip"}}});

    CHECK_THROWS_AS(run("delete", {{"args", {"hero:Tip"}}}), CommandError);
    CHECK_THROWS_AS(run("rename", {{"args", {"hero:Tip", "Other"}}}), CommandError);

    // Upstream change: the asset's Tip moves up.
    {
        run("saveScene", {{"args", {tempPath("shot.lila")}}});
        run("newScene");
        run("openScene", {{"args", {asset}}});
        run("setAttr", {{"args", {"Tip.ty", 7}}});
        run("saveScene", {{"args", {asset}}});
        run("newScene");
        run("openScene", {{"args", {tempPath("shot.lila")}}});
    }
    CHECK(get("hero:Tip.ty") == doctest::Approx(7)); // upstream
    CHECK(get("hero:Tip.tx") == doctest::Approx(3)); // local edit kept
    CHECK(get("asset:Tip.tx") == doctest::Approx(0));
    CHECK(run("listRelatives", {{"args", {"Prop"}}, {"parent", true}}) == json::array({"hero:Tip"}));

    run("unloadReference", {{"args", {"hero"}}});
    CHECK(!run("objExists", {{"args", {"hero:Tip"}}}).get<bool>());
    CHECK(run("objExists", {{"args", {"Prop"}}}).get<bool>());
    run("loadReference", {{"args", {"hero"}}});
    CHECK(get("hero:Tip.tx") == doctest::Approx(3));
    CHECK(run("listRelatives", {{"args", {"Prop"}}, {"parent", true}}) == json::array({"hero:Tip"}));

    run("reloadReference", {{"args", {"hero"}}});
    CHECK(get("hero:Tip.tx") == doctest::Approx(3));

    run("importReference", {{"args", {"asset"}}});
    run("rename", {{"args", {"asset:Tip", "Imported"}}});
    run("removeReference", {{"args", {"hero"}}});
    CHECK(!run("objExists", {{"args", {"hero:Root"}}}).get<bool>());
    CHECK(run("references").empty());
}

TEST_CASE("locks on referenced nodes are reference edits")
{
    auto locked = [](const std::string& plug) { return run("getAttr", {{"args", {plug}}, {"lock", true}}).get<bool>(); };

    run("newScene");
    run("spaceLocator", {{"name", "Tip"}});
    run("setAttr", {{"args", {"Tip.rx"}}, {"lock", true}});
    std::string asset = tempPath("lockAsset.lilaa");
    run("saveScene", {{"args", {asset}}});

    run("newScene");
    run("createReference", {{"args", {asset}}, {"namespace", "hero"}});
    CHECK(locked("hero:Tip.rx"));
    run("setAttr", {{"args", {"hero:Tip.ty"}}, {"lock", true}});
    run("setAttr", {{"args", {"hero:Tip.rx"}}, {"lock", false}});
    run("setAttr", {{"args", {"hero:Tip.rx", 30}}});

    std::string shot = tempPath("lockShot.lila");
    run("saveScene", {{"args", {shot}}});
    run("newScene");
    run("openScene", {{"args", {shot}}});
    CHECK(locked("hero:Tip.ty"));
    CHECK(!locked("hero:Tip.rx"));
    CHECK(get("hero:Tip.rx") == doctest::Approx(30));
    CHECK(!locked("hero:Tip.tx"));

    run("unloadReference", {{"args", {"hero"}}});
    run("loadReference", {{"args", {"hero"}}});
    CHECK(locked("hero:Tip.ty"));
    CHECK(!locked("hero:Tip.rx"));

    run("reloadReference", {{"args", {"hero"}}});
    CHECK(locked("hero:Tip.ty"));
    CHECK(!locked("hero:Tip.rx"));
    CHECK(get("hero:Tip.rx") == doctest::Approx(30));
}
