#include <doctest/doctest.h>

#include "ops/Command.h"
#include "geo/Attribute.h"
#include "geo/Mesh.h"
#include "scene/Graph.h"
#include "scene/Isolation.h"
#include "scene/MeshNodes.h"

using namespace lila;
using json = nlohmann::json;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }
int faces(const std::string& m) { return run("meshInfo", {{"args", {m}}})["faces"].get<int>(); }

size_t hardEdges(const std::string& m)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    const geo::Mesh& mesh = *g.find(m)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    std::span<const bool> hard = mesh.edgeAttrs().get<bool>(geo::attr::kHard);
    size_t n = 0;
    for (geo::Index e = 0; e < hard.size(); ++e)
        n += mesh.edgeValid(e) && hard[e] ? 1 : 0;
    return n;
}
} // namespace

TEST_CASE("polyInsertLoop follows symmetry")
{
    run("newScene");
    std::string plane = run("polyPlane", {{"width", 100.0}, {"depth", 100.0}, {"subdivisionsX", 4}, {"subdivisionsZ", 1}})[0];
    run("setSymmetry", {{"args", {plane}}, {"axis", "x"}});
    run("selectComponents", {{"args", {plane}}, {"edges", {0}}});
    run("polyInsertLoop");
    CHECK(faces(plane) == 6);
    CHECK(run("symmetryCheck", {{"args", {plane}}})[plane]["unmatched"].get<int>() == 0);
    run("undo");
    run("selectComponents", {{"args", {plane}}, {"edges", {0}}});
    run("polyInsertLoop", {{"count", 2}});
    CHECK(faces(plane) == 8);
}

TEST_CASE("polyDuplicateFaces copies faces into the same mesh")
{
    run("newScene");
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
    run("selectComponents", {{"args", {cube}}, {"faces", {0}}});
    run("polyDuplicateFaces");
    CHECK(faces(cube) == 7);
    CHECK(run("componentSelection")[cube]["faces"].size() == 1);
    run("undo");
    CHECK(faces(cube) == 6);
}

TEST_CASE("component edits in object mode take the whole mesh")
{
    run("newScene");
    std::string plane = run("polyPlane", {{"width", 100.0}, {"depth", 100.0}})[0];
    const int before = faces(plane);
    run("select", {{"args", {plane}}});
    run("selectMode", {{"args", {"object"}}});
    run("polyExtrude", {{"thickness", 1.0}});
    CHECK(faces(plane) == before + 4);
    CHECK(run("selectMode") == "face");
    run("undo");
    CHECK(faces(plane) == before);
    CHECK(run("selectMode") == "object");

    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
    run("select", {{"args", {cube}}});

    run("polySoftEdge", {{"angle", 0.0}});
    CHECK(hardEdges(cube) == 12);
    CHECK(run("selectMode") == "object");
    run("polySoftEdge", {{"angle", 180.0}});
    CHECK(hardEdges(cube) == 0);
    run("polySoftEdge", {{"angle", 30.0}});
    CHECK(hardEdges(cube) == 12);
}

TEST_CASE("polyMirror flip keeps nothing of the original")
{
    run("newScene");
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
    run("setAttr", {{"args", {cube + ".translate", {20.0, 0.0, 0.0}}}});
    run("polyMirror", {{"args", {cube}}, {"space", "world"}, {"flip", true}});
    CHECK(faces(cube) == 6);
    double least = 1e9, most = -1e9;
    const json data = run("meshData", {{"args", {cube}}});
    for (const json& p : data["points"])
    {
        least = std::min(least, p[0].get<double>());
        most = std::max(most, p[0].get<double>());
    }
    CHECK(least == doctest::Approx(-90.0));
    CHECK(most == doctest::Approx(10.0));
}

TEST_CASE("showLastHidden shows what the last hide hid")
{
    run("newScene");
    std::string a = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
    std::string b = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
    run("hide", {{"args", {a}}});
    run("hide", {{"args", {b}}});
    json shown = run("showLastHidden");
    REQUIRE(shown.size() == 1);
    CHECK(run("getAttr", {{"args", {b + ".visibility"}}}) == true);
    CHECK(run("getAttr", {{"args", {a + ".visibility"}}}) == false);
}

TEST_CASE("isolation sets take in objects made from isolated ones")
{
    using scene::isolation::Set;
    Set set;
    CHECK_FALSE(scene::isolation::inheritInto(set, {7}, {1}));
    set.nodes = {1, 2};
    CHECK_FALSE(scene::isolation::inheritInto(set, {7}, {3}));
    CHECK(scene::isolation::inheritInto(set, {7, 8}, {3, 2}));
    CHECK(set.nodes.contains(7));
    CHECK(set.nodes.contains(8));
}

TEST_CASE("merging exactly two vertices ignores the distance")
{
    run("newScene");
    std::string plane = run("polyPlane", {{"width", 100.0}, {"depth", 100.0}, {"subdivisionsX", 1}, {"subdivisionsZ", 1}})[0];
    run("selectComponents", {{"args", {plane}}, {"vertices", {0, 1}}});
    run("polyMerge");
    CHECK(run("meshInfo", {{"args", {plane}}})["points"].get<int>() == 3);
    run("undo");
    run("selectComponents", {{"args", {plane}}, {"vertices", {0, 1, 2}}});
    run("polyMerge");
    CHECK(run("meshInfo", {{"args", {plane}}})["points"].get<int>() == 4);
}
