#include <doctest/doctest.h>

#include "cmds/Command.h"

#include <filesystem>

using namespace lila;
using json = nlohmann::json;

namespace
{
json run(const char* name, json call = json::object()) { return cmds::execute(name, call); }
int faces(const std::string& m) { return run("meshInfo", {{"args", {m}}})["faces"].get<int>(); }
} // namespace

TEST_CASE("modifier stack is live and edits go under it")
{
    run("newScene");
    std::string cube = run("polyCube")[0];
    std::string sub = run("addModifier", {{"args", {cube}}, {"kind", "subdivision"}});
    CHECK(faces(cube) == 24);
    std::string mirror = run("addModifier", {{"args", {cube}}, {"kind", "array"}, {"params", {{"count", 2}}}});
    CHECK(faces(cube) == 48);
    CHECK(run("modifiers", {{"args", {cube}}}) == json::array({sub, mirror}));

    // Turning a modifier off passes the mesh through.
    run("setAttr", {{"args", {sub + ".enabled", false}}});
    CHECK(faces(cube) == 12);
    run("setAttr", {{"args", {sub + ".enabled", true}}});

    // Component edits land on the cage (6 faces), and the stack re-runs on top.
    run("selectComponents", {{"args", {cube}}, {"faces", {0}}});
    run("polyExtrude", {{"thickness", 10}});
    CHECK(faces(cube) == 10 * 4 * 2);
    CHECK(run("modifiers", {{"args", {cube}}}).size() == 2);

    run("moveModifier", {{"args", {sub}}, {"by", 1}});
    CHECK(run("modifiers", {{"args", {cube}}}) == json::array({mirror, sub}));
    CHECK(faces(cube) == 80);

    run("applyModifier", {{"args", {mirror}}});
    CHECK(run("modifiers", {{"args", {cube}}}) == json::array({sub}));
    CHECK(faces(cube) == 80);
    run("removeModifier", {{"args", {sub}}});
    CHECK(faces(cube) == 20);
    CHECK(run("modifiers", {{"args", {cube}}}).empty());

    run("undo");
    CHECK(faces(cube) == 80);
}

TEST_CASE("every modifier kind evaluates")
{
    run("newScene");
    for (const char* kind : {"mirror", "subdivision", "bevel", "array", "triangulate", "weightedNormals", "solidify", "decimate"})
    {
        std::string m = run("polyCube", {{"name", std::string("M") + kind}})[0];
        run("addModifier", {{"args", {m}}, {"kind", kind}});
        CHECK_MESSAGE(faces(m) > 0, kind);
    }
}

TEST_CASE("modifier stacks save and load")
{
    run("newScene");
    std::string cube = run("polyCube", {{"name", "Stacked"}})[0];
    run("addModifier", {{"args", {cube}}, {"kind", "bevel"}, {"params", {{"segments", 3}}}});
    run("addModifier", {{"args", {cube}}, {"kind", "mirror"}});
    int before = faces(cube);
    auto dir = std::filesystem::temp_directory_path() / "lila_tests";
    for (const char* ext : {".lila", ".lilaa"})
    {
        auto path = (dir / (std::string("stack") + ext)).string();
        run("saveScene", {{"args", {path}}});
        run("newScene");
        run("openScene", {{"args", {path}}});
        CHECK(faces(cube) == before);
        CHECK(run("modifiers", {{"args", {cube}}}).size() == 2);
    }
}

TEST_CASE("triangulating a mesh with modifiers triangulates the result")
{
    run("newScene");
    std::string cube = run("polyCube")[0];
    run("addModifier", {{"args", {cube}}, {"kind", "bevel"}, {"params", {{"segments", 2}}}});
    const json before = run("meshInfo", {{"args", {cube}}});
    run("select", {{"args", {cube}}});
    run("polyTriangulate");
    const json after = run("meshInfo", {{"args", {cube}}});
    CHECK(after["points"] == before["points"]);
    CHECK(after["faces"] == before["triangles"]);
    CHECK(after["triangles"] == before["triangles"]);
    const json stack = run("modifiers", {{"args", {cube}}});
    REQUIRE(stack.size() == 2);
    // Again is a no-op rather than a second modifier.
    run("selectMode", {{"args", {"object"}}});
    run("select", {{"args", {cube}}});
    run("polyTriangulate");
    CHECK(run("modifiers", {{"args", {cube}}}).size() == 2);
    run("undo");
    CHECK(run("modifiers", {{"args", {cube}}}).size() == 1);
    CHECK(run("meshInfo", {{"args", {cube}}})["faces"] == before["faces"]);
}
