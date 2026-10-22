#include <doctest/doctest.h>

#include "ui/TearOffLogic.h"

#include <algorithm>

namespace tearoff = lila::ui::tearoff;
using json = nlohmann::json;

TEST_CASE("tear-off labels drop mnemonics and the shortcut column")
{
    CHECK(tearoff::stripMnemonic("&Mesh") == "Mesh");
    CHECK(tearoff::stripMnemonic("M&odify") == "Modify");
    CHECK(tearoff::stripMnemonic("Save && Close") == "Save & Close");
    CHECK(tearoff::stripMnemonic("Trailing&") == "Trailing");
    CHECK(tearoff::itemLabel("&Extrude\tCtrl+E\xE2\x80\x83") == "Extrude");
    CHECK(tearoff::itemLabel("  Bevel...  ") == "Bevel...");
    CHECK(tearoff::itemLabel("") == "");
}

TEST_CASE("tear-off dock names round trip a menu path")
{
    CHECK(tearoff::dockName({"Mesh"}) == "tearoff:Mesh");
    CHECK(tearoff::dockName({"Mesh", "Symmetry"}) == "tearoff:Mesh/Symmetry");
    for (std::vector<std::string> path : {std::vector<std::string>{"UV", "Pack"}, {"File", "Import/Export", "100%"}, {"A & B"}})
    {
        auto parsed = tearoff::parseDockName(tearoff::dockName(path));
        REQUIRE(parsed);
        CHECK(*parsed == path);
    }
    CHECK(tearoff::dockName({"File", "Import/Export"}) == "tearoff:File/Import%2FExport");
    CHECK_FALSE(tearoff::parseDockName("outliner"));
    CHECK_FALSE(tearoff::parseDockName("tearoff:"));
    CHECK_FALSE(tearoff::parseDockName("tearoff:Mesh//Pack"));
    CHECK_FALSE(tearoff::parseDockName("py_tearoff:Mesh"));
}

TEST_CASE("a layout recreates only the torn-off docks it shows open")
{
    json docks = {{"outliner", {{"open", true}}},
                  {"tearoff:Mesh", {{"open", true}, {"floating", true}}},
                  {"tearoff:UV/Pack", {{"open", false}}},
                  {"tearoff:Rig", {{"open", true}, {"collapsed", true}}},
                  {"tearoff:", {{"open", true}}}};
    std::vector<std::string> open = tearoff::openDocks(docks);
    std::sort(open.begin(), open.end());
    CHECK(open == std::vector<std::string>{"tearoff:Mesh", "tearoff:Rig"});
    CHECK(tearoff::openDocks(json::array()).empty());
    CHECK(tearoff::openDocks(json()).empty());
}
