#include <doctest/doctest.h>

#include "ops/Command.h"

using namespace lila;
using json = nlohmann::json;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }
int faces(const std::string& m) { return run("meshInfo", {{"args", {m}}})["faces"].get<int>(); }
} // namespace

TEST_CASE("polyKnife: loops and paths are one undo step each")
{
    run("newScene");
    std::string cube = run("polyCube")[0];
    json loop = run("polyKnife", {{"mesh", cube}, {"loopEdge", 0}, {"fraction", 0.3}});
    CHECK(loop.size() == 4);
    CHECK(faces(cube) == 10);
    CHECK(run("selectMode") == "edge");
    CHECK(run("componentSelection")[cube]["edges"].size() == 4);
    run("undo");
    CHECK(faces(cube) == 6);

    std::string plane = run("polyPlane")[0];
    json cut = run("polyKnife", {{"mesh", plane}, {"points", {{{"vertex", 0}}, {{"vertex", 3}}}}, {"direction", {0, -1, 0}}});
    CHECK(cut.size() == 1);
    CHECK(faces(plane) == 2);
    CHECK_THROWS(run("polyKnife", {{"mesh", plane}, {"points", {{{"vertex", 0}}}}}));
}

TEST_CASE("polyKnife: symmetry cuts both sides")
{
    run("newScene");
    std::string plane = run("polyPlane", {{"subdivisionsX", 4}, {"subdivisionsZ", 1}})[0];
    run("setSymmetry", {{"args", {plane}}, {"axis", "x"}});
    // A loop across the outermost column on one side also lands on the other.
    run("polyKnife", {{"mesh", plane}, {"loopEdge", 0}, {"fraction", 0.5}});
    CHECK(faces(plane) == 6);
    CHECK(run("symmetryCheck", {{"args", {plane}}})[plane]["unmatched"].get<int>() == 0);
}

TEST_CASE("polyKnife: deleteEdge joins its two faces in one undo step")
{
    run("newScene");
    std::string cube = run("polyCube")[0];
    run("polyKnife", {{"mesh", cube}, {"deleteEdge", 0}});
    CHECK(faces(cube) == 5);
    run("undo");
    CHECK(faces(cube) == 6);
    CHECK_THROWS(run("polyKnife", {{"mesh", cube}, {"deleteEdge", 999}}));
}
