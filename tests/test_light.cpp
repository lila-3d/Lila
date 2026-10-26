#include <doctest/doctest.h>

#include "core/ExtensionContext.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Light.h"

#include <filesystem>

using namespace lila;
using namespace lila::scene;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }

Node& lightNamed(const json& name) { return *Graph::current().find(name.get<std::string>()); }
} // namespace

TEST_CASE("light command sets what it is given")
{
    run("newScene");
    Node& n = lightNamed(run("light", {{"type", "spot"}, {"intensity", 800.0}, {"units", "candela"}, {"outerConeAngle", 30.0},
                                       {"temperature", 3200.0}, {"position", {10.0, 20.0, 30.0}}}));
    CHECK(n.type().name == "light");
    lights::Settings s = lights::settings(n);
    CHECK(s.kind == lights::Kind::Spot);
    CHECK(s.intensity == doctest::Approx(800.0));
    CHECK(s.units == lights::Units::Candela);
    CHECK(s.outerConeAngle == doctest::Approx(30.0));
    CHECK(s.useTemperature);
    CHECK(n.as<glm::dvec3>(Transform::kTranslate) == glm::dvec3(10, 20, 30));
    CHECK(Graph::current().selection() == std::vector<NodeId>{n.id()});

    CHECK_THROWS(run("light", {{"type", "laser"}}));
    CHECK_THROWS(run("light", {{"units", "furlongs"}}));
}

TEST_CASE("light settings round trip through both scene formats")
{
    auto dir = std::filesystem::temp_directory_path() / "lila_tests";
    std::filesystem::create_directories(dir);
    lights::Settings s;
    s.kind = lights::Kind::Area;
    s.color = {1.0, 0.5, 0.25};
    s.intensity = 12.5;
    s.units = lights::Units::Nits;
    s.range = 900.0;
    s.width = 40.0;
    s.height = 20.0;
    s.castShadows = false;
    s.texture = "C:/ies/strip.ies";
    s.provider = "someRenderer";
    s.sourceFormat = "fbx:Area";
    s.extra = R"({"decay":2})";
    for (const char* ext : {".lila", ".lilaa"})
    {
        CAPTURE(ext);
        run("newScene");
        Node& n = lights::create(Graph::current(), "key", kNoNode, s);
        std::string name = n.name();
        auto path = (dir / (std::string("light") + ext)).string();
        run("saveScene", {{"args", {path}}});
        run("newScene");
        run("openScene", {{"args", {path}}});
        Node* back = Graph::current().find(name);
        REQUIRE(back);
        CHECK(lights::settings(*back) == s);
    }
}

TEST_CASE("light providers: a renderer wins over the placeholder")
{
    run("newScene");
    Node& point = lightNamed(run("light", {{"type", "point"}}));
    Node& dome = lightNamed(run("light", {{"type", "dome"}}));
    CHECK(lights::providerFor(point) == nullptr);
    CHECK(lights::isPlaceholder(point));

    ext::Context placeholder("PlaceholderTest");
    lights::registerProvider(placeholder, {.id = "test.placeholder", .name = "Placeholder", .placeholder = true});
    CHECK(lights::providerFor(point)->id == "test.placeholder");
    CHECK(lights::isPlaceholder(point));

    {
        ext::Context renderer("RendererTest");
        lights::registerProvider(renderer, {.id = "test.renderer", .name = "Renderer", .handles = [](const Node& n) {
                                                return lights::settings(n).kind != lights::Kind::Dome;
                                            }});
        CHECK(lights::providerFor(point)->id == "test.renderer");
        CHECK(!lights::isPlaceholder(point));
        CHECK(lights::providerFor(dome)->id == "test.placeholder");
        CHECK(lights::isPlaceholder(dome));
        renderer.unloadAll();
    }
    CHECK(lights::isPlaceholder(point));

    Graph::current().setValue({point.id(), Light::kProvider, -1}, std::string("test.placeholder"));
    CHECK(lights::providerFor(point)->id == "test.placeholder");
    placeholder.unloadAll();
    CHECK(lights::providerFor(point) == nullptr);
}

TEST_CASE("kelvin to linear")
{
    glm::dvec3 d65 = lights::kelvinToLinear(6500.0);
    CHECK(d65.r == doctest::Approx(1.0).epsilon(0.02));
    CHECK(d65.b == doctest::Approx(1.0).epsilon(0.05));
    glm::dvec3 warm = lights::kelvinToLinear(2700.0);
    CHECK(warm.r == doctest::Approx(1.0));
    CHECK(warm.b < warm.g);
    CHECK(warm.g < warm.r);
}
