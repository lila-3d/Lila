#include "TestUtil.h"

#include "fbx/Fbx.h"
#include "scene/Pin.h"

#include <filesystem>
#include <fstream>
#include <iterator>

using namespace rigtest;
using namespace lila::scene;
using D3 = glm::dvec3;

TEST_CASE("bakePin records the socket joint on a pinned prop, and FBX exports write it")
{
    if (!ops::find("exportFbx"))
        fbx::registerCommands();
    run("newScene");
    const std::string board = run("polyPlane", {{"name", "Board"}, {"width", 10}, {"depth", 10}, {"subdivisionsX", 2}, {"subdivisionsZ", 2}})[0];
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "root"}});
    run("skinCluster", {{"args", {"root", board}}});
    const std::string sword = run("polyCube", {{"name", "Sword"}})[0];
    run("move", {{"args", {sword}}, {"by", v3(5, 1, 5)}});
    CHECK(pins::socketOf(node(sword)).empty());

    run("pin", {{"args", {sword, board}}, {"source", "surface"}});
    const json joints = run("bakePin", {{"args", {sword}}, {"start", 0.0}, {"end", 0.0}, {"fps", 30.0}});
    REQUIRE(joints.size() == 1);
    CHECK(node(joints[0]).name() == "Sword_socket");
    CHECK(pins::socketOf(node(sword)) == "Sword_socket");
    checkNear(pos("Sword_socket"), pos(sword), 1e-6);

    const auto file = std::filesystem::temp_directory_path() / "lila_rig_tests" / "pin_socket.fbx";
    std::filesystem::create_directories(file.parent_path());
    run("exportFbx", {{"args", {file.generic_string(), sword}}, {"kind", "static"}, {"compress", false}});
    std::ifstream in(file, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const size_t prop = bytes.find("UnrealSocket");
    REQUIRE(prop != std::string::npos);
    CHECK(bytes.find("Sword_socket", prop) != std::string::npos);

    // The record undoes with the bake.
    run("undo");
    CHECK(pins::socketOf(node(sword)).empty());
}
