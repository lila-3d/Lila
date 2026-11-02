#include <doctest/doctest.h>

#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

using namespace lila;
using json = nlohmann::json;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }

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

    if (ops::find("joint"))
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

TEST_CASE("picking another object from the UI leaves component mode")
{
    run("newScene");
    std::string a = run("polyCube")[0];
    std::string b = run("polySphere")[0];
    run("select", {{"args", {a}}});
    auto pick = [](json call) { ops::execute("select", call, CallSource::UI); };

    faceMode(a);
    pick({{"args", {b}}});
    CHECK(run("selectMode") == "object");
    CHECK(run("ls", {{"selection", true}}) == json::array({b}));

    // Reselecting what is already selected, or deselecting, keeps the mode.
    run("select", {{"args", {a, b}}});
    faceMode(a);
    pick({{"args", {a}}});
    CHECK(run("selectMode") == "face");
    pick({{"args", {a}}, {"deselect", true}});
    CHECK(run("selectMode") == "face");

    // Scripts select without changing the mode.
    run("select", {{"args", {a}}});
    faceMode(a);
    run("select", {{"args", {b}}});
    CHECK(run("selectMode") == "face");
    run("selectMode", {{"args", {"object"}}});
}

TEST_CASE("createMesh puts each face corner's uv on that corner")
{
    json made = run("createMesh", {{"points", {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {2, 0, 0}}},
                                   {"faces", {{0, 1, 2, 3}, {1, 4, 2}}},
                                   {"uvs", {{0, 0}, {1, 0}, {1, 1}, {0, 1}, {0.5, 0}, {1, 0.5}, {0.5, 0.5}}},
                                   {"name", "UvQuad"}});
    scene::Graph& g = scene::Graph::current();
    scene::MeshPtr m = std::get<scene::MeshPtr>(g.storedValue(g.plug(made.get<std::string>() + ".inMesh")));
    REQUIRE(m);
    std::span<const glm::vec2> uv = m->cornerAttrs().get<glm::vec2>(geo::attr::uvName(0));
    REQUIRE(uv.size() >= 7);
    const std::vector<std::vector<int>> faces{{0, 1, 2, 3}, {1, 4, 2}};
    const std::vector<glm::vec2> want{{0, 0}, {1, 0}, {1, 1}, {0, 1}, {0.5f, 0}, {1, 0.5f}, {0.5f, 0.5f}};
    size_t at = 0;
    for (geo::Index f = 0; f < m->numFaces(); ++f)
    {
        geo::Index c = m->faceCorner(f);
        for (int point : faces[f])
        {
            CHECK(m->cornerPoint(c) == geo::Index(point));
            CHECK(uv[c] == want[at++]);
            c = m->cornerNext(c);
        }
    }

    CHECK_THROWS(run("createMesh", {{"points", {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}}}, {"faces", {{0, 1, 2}}}, {"uvs", {{0, 0}}}}));
}

TEST_CASE("smoothPreview on a group reaches every mesh below it")
{
    run("newScene");
    std::string a = run("polyCube")[0], b = run("polyCube")[0];
    std::string inner = run("group", {{"args", {b}}});
    std::string outer = run("group", {{"args", {a, inner}}});
    run("smoothPreview", {{"args", {outer}}, {"level", 2}});
    const json meshes = run("ls", {{"type", "mesh"}});
    REQUIRE(meshes.size() == 2);
    for (const json& mesh : meshes)
        CHECK(run("getAttr", {{"args", {mesh.get<std::string>() + ".smoothPreview"}}}).get<int>() == 2);
}
