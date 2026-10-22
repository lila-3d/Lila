#include <doctest/doctest.h>

#include "cmds/Command.h"

using namespace lila;
using json = nlohmann::json;

namespace
{
json run(const char* name, json call = json::object()) { return cmds::execute(name, call); }

void faceMode(const std::string& mesh)
{
    run("selectComponents", {{"args", {mesh}}, {"faces", {0}}});
    REQUIRE(run("selectMode") == "face");
}
} // namespace

TEST_CASE("convertSelection in object mode takes every component of the selected meshes")
{
    run("newScene");
    std::string cube = run("polyCube")[0];
    run("selectMode", {{"args", {"object"}}});
    run("select", {{"args", {cube}}});

    run("convertSelection", {{"args", {"edge"}}});
    CHECK(run("selectMode") == "edge");
    CHECK(run("componentSelection")[cube]["edges"].size() == 12);

    run("undo");
    CHECK(run("selectMode") == "object");
    CHECK_FALSE(run("componentSelection").contains(cube));
    run("redo");
    CHECK(run("selectMode") == "edge");
    CHECK(run("componentSelection")[cube]["edges"].size() == 12);

    run("selectMode", {{"args", {"object"}}});
    run("convertSelection", {{"args", {"vertex"}}});
    CHECK(run("selectMode") == "vertex");
    CHECK(run("componentSelection")[cube]["vertices"].size() == 8);

    run("selectMode", {{"args", {"object"}}});
    run("select", {{"clear", true}});
    CHECK_THROWS_AS(run("convertSelection", {{"args", {"face"}}}), CommandError);
    CHECK(run("selectMode") == "object");
}

TEST_CASE("creating a node lands in object mode with the new node selected")
{
    run("newScene");
    std::string cube = run("polyCube")[0];

    faceMode(cube);
    std::string loc = run("spaceLocator");
    CHECK(run("selectMode") == "object");
    CHECK(run("ls", {{"selection", true}}) == json::array({loc}));

    faceMode(cube);
    std::string group = run("group", {{"empty", true}});
    CHECK(run("selectMode") == "object");
    CHECK(run("ls", {{"selection", true}}) == json::array({group}));

    faceMode(cube);
    std::string sphere = run("polySphere")[0];
    CHECK(run("selectMode") == "object");
    CHECK(run("ls", {{"selection", true}}) == json::array({sphere}));

    faceMode(cube);
    std::string node = run("createNode", {{"args", {"transform"}}});
    CHECK(run("selectMode") == "object");
    CHECK(run("ls", {{"selection", true}}) == json::array({node}));

    if (cmds::find("joint"))
    {
        faceMode(cube);
        std::string joint = run("joint");
        CHECK(run("selectMode") == "object");
        CHECK(run("ls", {{"selection", true}}) == json::array({joint}));
    }

    // Edits that make no new selected node keep the component mode.
    faceMode(cube);
    run("polyExtrude", {{"thickness", 1}});
    CHECK(run("selectMode") == "face");
    faceMode(cube);
    run("spaceLocator", {{"skipSelect", true}});
    CHECK(run("selectMode") == "face");
    run("selectMode", {{"args", {"object"}}});
}
