#include <doctest/doctest.h>

#include "cmds/Command.h"
#include "cmds/Undo.h"
#include "io/SceneFormats.h"
#include "scene/SceneScripts.h"

#include <filesystem>

using namespace lila;
using json = nlohmann::json;
namespace scripts = lila::scene::scripts;

namespace
{
// Blank lines, trailing whitespace, quotes, a pipe, a tab and no final newline.
const std::string kAwkward = "import lila\n\n  x = \"a | b\"   \n\tprint(x)\n# done |";
} // namespace

TEST_CASE("scene scripts survive both file formats")
{
    json doc = {{"lila", 1},
                {"nodes", json::array()},
                {"connections", json::array()},
                {"selection", json::array()},
                {"scripts", json::array({{{"name", "setup rig"}, {"text", kAwkward}, {"runOnOpen", true}},
                                          {{"name", "empty"}, {"text", ""}},
                                          {{"name", "trailing"}, {"text", "a\n\n"}}})}};

    std::string ascii = io::encodeAscii(doc);
    CHECK(ascii.find("script \"setup rig\" runOnOpen\n") != std::string::npos);
    CHECK(ascii.find("doc scripts") == std::string::npos);
    std::vector<std::string> warnings;
    json back = io::decodeAscii(ascii, &warnings);
    CHECK(warnings.empty());
    CHECK(back["scripts"] == doc["scripts"]);

    json fromBinary = io::decodeBinary(io::encodeBinary(doc), &warnings);
    CHECK(fromBinary["scripts"] == doc["scripts"]);
}

TEST_CASE("sceneScript command is undoable and saves with the scene")
{
    cmds::execute("newScene", json::object());
    CHECK(scripts::all().empty());
    cmds::execute("sceneScript", {{"args", {"tool"}}, {"text", kAwkward}});
    REQUIRE(scripts::find("tool"));
    CHECK(scripts::find("tool")->text == kAwkward);
    CHECK(!scripts::find("tool")->runOnOpen);

    cmds::execute("sceneScript", {{"args", {"tool"}}, {"runOnOpen", true}});
    CHECK(scripts::find("tool")->runOnOpen);
    UndoStack::get().undo();
    CHECK(!scripts::find("tool")->runOnOpen);
    UndoStack::get().redo();
    CHECK(scripts::find("tool")->runOnOpen);

    cmds::execute("sceneScript", {{"args", {"tool"}}, {"rename", "tool2"}});
    CHECK(!scripts::find("tool"));
    CHECK(scripts::find("tool2"));
    CHECK_THROWS(cmds::execute("sceneScript", {{"args", {"missing"}}, {"query", true}}));

    std::vector<scripts::Script> offered;
    scripts::setOpenHandler([&](const std::vector<scripts::Script>& s) { offered = s; });
    auto dir = std::filesystem::temp_directory_path() / "lila_tests";
    for (const char* ext : {".lila", ".lilaa"})
    {
        auto path = (dir / (std::string("scripts") + ext)).string();
        cmds::execute("saveScene", {{"args", {path}}});
        cmds::execute("newScene", json::object());
        CHECK(scripts::all().empty());
        offered.clear();
        cmds::execute("openScene", {{"args", {path}}});
        REQUIRE(scripts::find("tool2"));
        CHECK(scripts::find("tool2")->text == kAwkward);
        CHECK(scripts::find("tool2")->runOnOpen);
        REQUIRE(offered.size() == 1);
        CHECK(offered[0].name == "tool2");
    }
    scripts::setOpenHandler(nullptr);

    cmds::execute("sceneScript", {{"args", {"tool2"}}, {"remove", true}});
    CHECK(scripts::all().empty());
    UndoStack::get().undo();
    CHECK(scripts::find("tool2"));
    cmds::execute("newScene", json::object());
}
