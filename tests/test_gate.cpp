#include <doctest/doctest.h>

#include "cmds/Command.h"
#include "io/SceneFormats.h"
#include "scene/RenderSettings.h"

using namespace lila;
using namespace lila::scene;

namespace
{
// Maya's default 36x24 mm film back at 35 mm, as tangents.
const glm::dvec2 kFilm(18.0 / 35.0, 12.0 / 35.0);

double aspectOf(glm::dvec2 e) { return e.x / e.y; }
} // namespace

TEST_CASE("gate fit against the film back")
{
    const double hd = 16.0 / 9.0;
    SUBCASE("horizontal keeps the film width")
    {
        glm::dvec2 g = fitGate(kFilm, hd, FilmFit::Horizontal);
        CHECK(g.x == doctest::Approx(kFilm.x));
        CHECK(aspectOf(g) == doctest::Approx(hd));
        CHECK(fitGate(kFilm, 9.0 / 16.0, FilmFit::Horizontal).x == doctest::Approx(kFilm.x));
    }
    SUBCASE("vertical keeps the film height")
    {
        CHECK(fitGate(kFilm, hd, FilmFit::Vertical).y == doctest::Approx(kFilm.y));
        CHECK(fitGate(kFilm, 0.5, FilmFit::Vertical).y == doctest::Approx(kFilm.y));
    }
    SUBCASE("fill puts the gate inside the film, overscan the film inside the gate")
    {
        for (double a : {0.5, 1.0, 1.5, hd, 2.39})
        {
            glm::dvec2 fill = fitGate(kFilm, a, FilmFit::Fill), over = fitGate(kFilm, a, FilmFit::Overscan);
            CHECK(aspectOf(fill) == doctest::Approx(a));
            CHECK(fill.x <= kFilm.x + 1e-12);
            CHECK(fill.y <= kFilm.y + 1e-12);
            CHECK(aspectOf(over) == doctest::Approx(a));
            CHECK(over.x >= kFilm.x - 1e-12);
            CHECK(over.y >= kFilm.y - 1e-12);
        }
    }
}

TEST_CASE("the gate's contents don't depend on the viewport's shape")
{
    // Quad panes, a tall strip and a wide strip.
    for (FilmFit fit : {FilmFit::Fill, FilmFit::Horizontal, FilmFit::Vertical, FilmFit::Overscan})
    {
        glm::dvec2 gate = fitGate(kFilm, 16.0 / 9.0, fit);
        for (double viewAspect : {0.4, 1.0, 1.3, 16.0 / 9.0, 3.5})
        {
            glm::dvec2 view = fitView(kFilm, viewAspect, fit, 1.0, 16.0 / 9.0);
            CHECK(aspectOf(view) == doctest::Approx(viewAspect));
            // The gate fits whole and touches the view in one dimension.
            CHECK(view.x >= gate.x - 1e-12);
            CHECK(view.y >= gate.y - 1e-12);
            CHECK(std::min(view.x / gate.x, view.y / gate.y) == doctest::Approx(1.0));
            glm::dvec2 over = fitView(kFilm, viewAspect, fit, 1.25, 16.0 / 9.0);
            CHECK(over.x == doctest::Approx(view.x * 1.25));
        }
    }
}

TEST_CASE("without a gate the viewport is the gate")
{
    SUBCASE("horizontal keeps the horizontal FOV across panel shapes")
    {
        for (double a : {0.5, 1.0, 2.0})
            CHECK(fitView(kFilm, a, FilmFit::Horizontal, 1.0, std::nullopt).x == doctest::Approx(kFilm.x));
    }
    SUBCASE("vertical keeps the vertical FOV")
    {
        for (double a : {0.5, 1.0, 2.0})
            CHECK(fitView(kFilm, a, FilmFit::Vertical, 1.0, std::nullopt).y == doctest::Approx(kFilm.y));
    }
    SUBCASE("fill: a wide panel matches widths, a narrow one heights")
    {
        CHECK(fitView(kFilm, 2.0, FilmFit::Fill, 1.0, std::nullopt).x == doctest::Approx(kFilm.x));
        CHECK(fitView(kFilm, 1.0, FilmFit::Fill, 1.0, std::nullopt).y == doctest::Approx(kFilm.y));
    }
    SUBCASE("ortho extents fit the same way")
    {
        glm::dvec2 film(400.0, 400.0 * 24.0 / 36.0);
        glm::dvec2 v = fitView(film, 1.0, FilmFit::Overscan, 1.0, 1.0);
        CHECK(v.x == doctest::Approx(400.0));
        CHECK(v.y == doctest::Approx(400.0));
    }
}

TEST_CASE("render settings commands and scene round trip")
{
    renderSettings().fromJson({});

    json r = cmds::execute("setRenderResolution", {{"preset", "Portrait 1080x1920"}});
    CHECK(r["width"] == 1080);
    CHECK(r["height"] == 1920);
    CHECK(renderSettings().deviceAspect() == doctest::Approx(1080.0 / 1920.0));
    r = cmds::execute("setRenderResolution", {{"width", 2000}, {"height", 1000}, {"deviceAspect", 2.4}});
    CHECK(r["pixelAspect"].get<double>() == doctest::Approx(1.2));
    CHECK_THROWS_AS(cmds::execute("setRenderResolution", {{"preset", "nope"}}), CommandError);

    json c = cmds::execute("cameraSettings", {{"args", {"persp"}}, {"filmFit", "Horizontal"}, {"overscan", 1.3}, {"gate", true}});
    CHECK(c["filmFit"] == "horizontal");
    CHECK(renderSettings().camera("persp").resolutionGate);
    CHECK_FALSE(renderSettings().camera("top").resolutionGate);
    CHECK_THROWS_AS(cmds::execute("cameraSettings", {{"filmFit", "stretch"}}), CommandError);

    json saved = renderSettings().toJson();
    json doc = {{"lila", 2}, {"renderSettings", saved}, {"nodes", json::array()}};
    std::vector<std::string> warnings;
    json back = io::decodeAscii(io::encodeAscii(doc), &warnings);
    CHECK(back["renderSettings"] == saved);

    renderSettings().fromJson({});
    CHECK(renderSettings().width == 1920);
    CHECK(renderSettings().cameras.empty());
    renderSettings().fromJson(back["renderSettings"]);
    CHECK(renderSettings().width == 2000);
    CHECK(renderSettings().camera("persp").overscan == doctest::Approx(1.3));
    renderSettings().fromJson({});
}
