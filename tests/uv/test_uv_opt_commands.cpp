#include "cmds/Command.h"

#include <doctest/doctest.h>

using namespace lila;

namespace
{
json run(const char* name, json call = json::object())
{
    return cmds::execute(name, call);
}
} // namespace

TEST_CASE("uv optimise, bff, optcuts, symmetry and align commands")
{
    run("newScene");
    std::string sphere = run("polySphere", {{"subdivisionsAxis", 16}, {"subdivisionsHeight", 10}})[0];
    run("select", {{"args", {sphere}}});

    json seams = run("uvAutoSeams", {{"method", "optcuts"}, {"distortionBound", 0.06}, {"visibility", false}});
    CHECK(seams["islands"].get<int>() >= 1);
    run("uvUnwrap", {{"method", "bff"}});
    CHECK(run("uvLayout")["foldedFaces"] == 0);
    run("undo");
    run("redo");

    json bff = run("uvBff", {{"boundary", "disk"}});
    CHECK(bff["failed"] == 0);
    CHECK(run("uvBff", {{"cones", 2}})["failed"] == 0);
    CHECK(run("uvBff", {{"boundary", "rectangle"}})["failed"] == 0);

    json opt = run("uvOptimize", {{"mode", "authalic"}, {"iterations", 10}});
    CHECK(opt["failed"] == 0);
    CHECK(opt["areaDistortion"][1].get<double>() <= opt["areaDistortion"][0].get<double>() + 1e-9);
    run("uvOptimize", {{"mode", "blend"}, {"mix", 0.3}});
    run("uvOptimize", {{"mode", "isometric"}});
    CHECK_THROWS(run("uvOptimize", {{"mode", "sideways"}}));
    json mosaic = run("uvMosaic", {{"threshold", 0.02}, {"visibility", false}});
    CHECK(mosaic["islandsAfter"].get<int>() >= mosaic["islandsBefore"].get<int>());

    json sym = run("uvUnwrapSymmetric", {{"axis", "x"}});
    CHECK(sym["unpaired"] == 0);
    CHECK(sym["axis"] == "x");
    CHECK(run("uvUnwrapSymmetric", {{"mode", "stack"}})["unpaired"] == 0);

    // Edge tools need edges.
    run("selectComponents", {{"args", {sphere}}, {"edges", {0, 1, 2, 3}}});
    CHECK(run("uvStraightenEdges")["chains"].get<int>() >= 1);
    CHECK(run("uvAlignToEdge", {{"axis", "u"}})["islands"].get<int>() >= 1);
    run("uvAlignToEdge");
    run("undo");
}
