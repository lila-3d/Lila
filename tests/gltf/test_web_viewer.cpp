#include "Helpers.h"

#include "geo/Primitives.h"

using namespace gltftest;

TEST_CASE("exportWebViewer writes model.glb and a model-viewer page")
{
    run("newScene");
    Graph& g = graph();
    addMesh(g, "Crate", geo::makeCube({.width = 50, .height = 50, .depth = 50}));
    g.select({node("Crate").id()});

    const fs::path dir = tempDir("web_viewer");
    fs::remove_all(dir);
    json r = run("exportWebViewer", {{"args", {dir.generic_string()}},
                                     {"title", "Crate <A&B>"},
                                     {"author", "Someone"},
                                     {"exposure", 1.0},
                                     {"tonemapper", "agx"},
                                     {"background", {0.0, 0.5, 1.0}},
                                     {"environment", false}});
    CHECK(r.get<std::string>() == dir.generic_string());
    REQUIRE(fs::is_regular_file(dir / "model.glb"));
    CHECK(readAll(dir / "model.glb").substr(0, 4) == "glTF");
    REQUIRE(fs::is_regular_file(dir / "index.html"));
    CHECK_FALSE(fs::exists(dir / "environment.hdr"));

    const std::string html = readAll(dir / "index.html");
    CHECK(html.find("src=\"model.glb\"") != std::string::npos);
    CHECK(html.find("https://cdn.jsdelivr.net/npm/@google/model-viewer@3.5.0/dist/model-viewer.min.js") != std::string::npos);
    CHECK(html.find("Crate &lt;A&amp;B&gt;") != std::string::npos);
    CHECK(html.find("Someone") != std::string::npos);
    CHECK(html.find("exposure=\"2\"") != std::string::npos);
    CHECK(html.find("tone-mapping=\"agx\"") != std::string::npos);
    CHECK(html.find("environment-image=\"neutral\"") != std::string::npos);
    CHECK(html.find("#0080ff") != std::string::npos);
    CHECK(html.find(" auto-rotate ") != std::string::npos);
}

TEST_CASE("exportWebViewer exports the whole scene without a selection")
{
    run("newScene");
    addMesh(graph(), "Crate", geo::makeCube({.width = 50, .height = 50, .depth = 50}));
    graph().select({});

    const fs::path dir = tempDir("web_viewer_scene");
    fs::remove_all(dir);
    run("exportWebViewer", {{"args", {dir.generic_string()}}, {"autoRotate", false}, {"environment", false}});
    REQUIRE(fs::is_regular_file(dir / "model.glb"));
    const std::string html = readAll(dir / "index.html");
    CHECK(html.find(" auto-rotate ") == std::string::npos);
    CHECK(html.find("tone-mapping=\"neutral\"") != std::string::npos);

    CHECK_THROWS_AS(run("exportWebViewer", {{"args", {dir.generic_string()}}, {"selection", true}}), CommandError);
}
