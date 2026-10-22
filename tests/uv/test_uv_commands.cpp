#include "cmds/Command.h"

#include <doctest/doctest.h>

using namespace lila;

namespace
{
json run(const char* name, json call = json::object()) { return cmds::execute(name, call); }
} // namespace

TEST_CASE("uv commands on a cube")
{
    run("newScene");
    std::string cube = run("polyCube")[0];
    run("select", {{"args", {cube}}});

    json r = run("uvAuto");
    CHECK(r["ok"] == true);
    CHECK(r["islands"].get<int>() <= 6);
    json info = run("uvLayout");
    CHECK(info["overlappingFaces"] == 0);
    CHECK(info["foldedFaces"] == 0);
    CHECK(info["islands"].get<int>() == r["islands"].get<int>());
    run("undo");
    run("redo");

    run("uvProject", {{"type", "box"}});
    CHECK(run("uvLayout")["islands"] == 6);
    run("uvPack", {{"resolution", 512}, {"padding", 8}});
    CHECK(run("uvLayout")["overlappingFaces"] == 0);
    run("uvOrient");
    run("uvStraighten");

    double td = run("uvTexelDensity", {{"resolution", 1024}}).get<double>();
    CHECK(td > 0.0);
    run("uvTexelDensity", {{"resolution", 1024}, {"density", 2.0}});
    CHECK(run("uvTexelDensity", {{"resolution", 1024}}).get<double>() == doctest::Approx(2.0).epsilon(1e-3));

    json lm = run("uvLightmap", {{"resolution", 64}});
    CHECK(lm["ok"] == true);
    CHECK(run("uvLayout", {{"uvSet", "uv1"}})["overlappingFaces"] == 0);
    CHECK(run("uvSetCurrent") == "uv0");

    // Component edits: cut then sew one edge of a face, move one face's uvs.
    run("selectComponents", {{"args", {cube}}, {"edges", {0}}});
    CHECK(run("uvCutEdges")["cut"] == 1);
    CHECK(run("uvSewEdges")["sewn"].get<int>() <= 1);
    run("selectComponents", {{"args", {cube}}, {"faces", {0}}});
    CHECK(run("uvTransform", {{"translate", {0.1, 0.0}}, {"rotate", 15.0}})["corners"].get<int>() >= 4);
    run("uvUnwrap");
    run("uvAutoSeams");
}

TEST_CASE("uvPack packs every selected mesh into one layout and stacks")
{
    run("newScene");
    std::string a = run("polyCube")[0], b = run("polyCube")[0];
    run("select", {{"args", {a, b}}});
    run("uvProject", {{"type", "box"}});
    json r = run("uvPack", {{"resolution", 512}, {"padding", 4}, {"effort", 2}});
    CHECK(r["ok"] == true);
    CHECK(r["density"].get<double>() > 30.0);
    CHECK(r["stacked"] == 0);
    CHECK(run("uvStacks")[a].empty());

    json s = run("uvPack", {{"stack", true}, {"rotateStep", 0}, {"angles", {0, 90}}, {"layout", "fill"}, {"keep", "none"}});
    CHECK(s["stacked"] == 11);
    json stacks = run("uvStacks", {{"select", true}});
    CHECK(stacks[a].size() == 1);
    CHECK(stacks[a][0].size() == 6);

    run("select", {{"args", {a, b}}});
    json g = run("uvPack", {{"layout", "groupTiles"}, {"groupBy", "object"}, {"scaleMode", "keep"}, {"region", {0.0, 0.0, 0.5, 1.0}}});
    CHECK(g["tiles"] == 2);
    run("undo");
    CHECK_THROWS(run("uvPack", {{"scaleMode", "lock"}, {"tilesU", 1}, {"padding", 600}}));
}
