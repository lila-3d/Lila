#include <doctest/doctest.h>

#include "TestUtil.h"
#include "ops/Command.h"
#include "fbx/Fbx.h"
#include "geo/Primitives.h"
#include "math/MathCore.h"
#include "scene/Anim.h"
#include "scene/Light.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <ufbx.h>

#include <glm/gtc/matrix_transform.hpp>

#include <filesystem>
#include <map>

using namespace lila;
using namespace lila::scene;
using namespace fbxtest;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace
{
/** A spot under a turned group with a child, an area, a dome and a warm point light. */
void lightScene()
{
    run("newScene");
    run("createNode", {{"args", {"transform"}}, {"name", "rig"}});
    run("setAttr", {{"args", {"rig.rotate", 0, 30, 0}}});
    Graph& g = Graph::current();
    lights::Settings spot;
    spot.kind = lights::Kind::Spot;
    spot.color = {1.0, 0.5, 0.25};
    spot.intensity = 3.0;
    spot.innerConeAngle = 20.0;
    spot.outerConeAngle = 35.0;
    spot.range = 800.0;
    spot.castShadows = false;
    Node& key = lights::create(g, "key", g.find("rig")->id(), spot);
    Transform::setLocalMatrix(g, key.id(), glm::translate(glm::dmat4(1.0), {5, 60, 200}) * glm::dmat4(scene::eulerToMatrix({-15, 20, 5}, RotateOrder::XYZ)));
    run("createNode", {{"args", {"transform"}}, {"name", "kid"}, {"parent", "key"}});
    run("setAttr", {{"args", {"kid.translate", 10, 20, -30}}});
    lights::Settings area;
    area.kind = lights::Kind::Area;
    area.width = 120.0;
    area.height = 40.0;
    area.units = lights::Units::Nits;
    lights::create(g, "panel", kNoNode, area);
    lights::Settings dome;
    dome.kind = lights::Kind::Dome;
    dome.texture = "sky.exr";
    lights::create(g, "sky", kNoNode, dome);
    lights::Settings warm;
    warm.useTemperature = true;
    warm.temperature = 3000.0;
    warm.sourceRadius = 5.0;
    warm.extra = R"({"decayRate":1})";
    lights::create(g, "bulb", kNoNode, warm);
    g.evaluate();
}
} // namespace

TEST_CASE("lights export as FBX lights shining down -Y in every profile")
{
    for (math::UpAxis up : {math::UpAxis::Y, math::UpAxis::Z})
        for (fbx::AxisProfile p : {fbx::AxisProfile::Ue5, fbx::AxisProfile::Ue6, fbx::AxisProfile::Generic})
        {
            CAPTURE(int(up));
            CAPTURE(fbx::profileName(p));
            math::setUpAxis(up);
            lightScene();
            const glm::dmat4 w = worldOf("key");
            const glm::dmat3 basis = fbx::profileBasis(p, up);
            fs::path path = tempFile(std::string("light_axes_") + fbx::profileName(p) + (up == math::UpAxis::Z ? "_z" : "_y") + ".fbx");
            run("exportFbx", {{"args", {path.string(), "rig", "panel", "sky", "bulb"}}, {"profile", fbx::profileName(p)}, {"kind", "static"}});
            UfbxScene s(path);
            REQUIRE(s.scene);
            const ufbx_node* n = nodeNamed(*s.scene, "key");
            REQUIRE(n);
            REQUIRE(n->light);
            const ufbx_light& l = *n->light;
            const glm::dvec3 dir = glm::normalize(vec(ufbx_transform_direction(&n->node_to_world, l.local_direction)));
            CHECK(near(dir, basis * glm::normalize(-glm::dvec3(w[2]))));
            CHECK(l.type == UFBX_LIGHT_SPOT);
            CHECK(near(vec(l.color), {1.0, 0.5, 0.25}));
            CHECK(l.intensity == doctest::Approx(3.0));
            CHECK(l.inner_angle == doctest::Approx(40.0));
            CHECK(l.outer_angle == doctest::Approx(70.0));
            CHECK_FALSE(l.cast_shadows);
            CHECK(l.decay == UFBX_LIGHT_DECAY_QUADRATIC);
            REQUIRE(nodeNamed(*s.scene, "panel"));
            CHECK(nodeNamed(*s.scene, "panel")->light->type == UFBX_LIGHT_AREA);
            CHECK(nodeNamed(*s.scene, "bulb")->light->decay == UFBX_LIGHT_DECAY_LINEAR);
        }
    math::setUpAxis(math::UpAxis::Y);
}

TEST_CASE("lights round trip through FBX with their settings, placement and children")
{
    for (const char* profile : {"ue5", "ue6", "generic"})
    {
        CAPTURE(profile);
        lightScene();
        Graph& g = Graph::current();
        std::map<std::string, std::pair<lights::Settings, glm::dmat4>> before;
        for (const char* name : {"key", "panel", "sky", "bulb"})
            before[name] = {lights::settings(*g.find(name)), worldOf(name)};
        const glm::dmat4 kidWorld = worldOf("kid");
        fs::path path = tempFile(std::string("light_trip_") + profile + ".fbx");
        run("exportFbx", {{"args", {path.string(), "rig", "panel", "sky", "bulb"}}, {"profile", profile}, {"kind", "static"}});
        run("newScene");
        run("importFbx", {{"args", {path.string()}}});
        g.evaluate();
        for (auto& [name, was] : before)
        {
            CAPTURE(name);
            const Node* n = g.find(name);
            REQUIRE(n);
            CHECK(n->type().isA("light"));
            lights::Settings now = lights::settings(*n);
            CHECK(now.sourceFormat.empty());
            now.sourceFormat = was.first.sourceFormat;
            CHECK(now.kind == was.first.kind);
            CHECK(near(now.color, was.first.color));
            CHECK(now.intensity == doctest::Approx(was.first.intensity));
            CHECK(now.units == was.first.units);
            CHECK(now.width == doctest::Approx(was.first.width));
            CHECK(now.height == doctest::Approx(was.first.height));
            CHECK(now.range == doctest::Approx(was.first.range));
            CHECK(now.innerConeAngle == doctest::Approx(was.first.innerConeAngle));
            CHECK(now.outerConeAngle == doctest::Approx(was.first.outerConeAngle));
            CHECK(now.useTemperature == was.first.useTemperature);
            CHECK(now.temperature == doctest::Approx(was.first.temperature));
            CHECK(now.sourceRadius == doctest::Approx(was.first.sourceRadius));
            CHECK(now.castShadows == was.first.castShadows);
            CHECK(now.texture == was.first.texture);
            CHECK(now.extra == was.first.extra);
            CHECK(maxDiff(worldOf(name.c_str()), was.second) < 1e-6);
        }
        CHECK(maxDiff(worldOf("kid"), kidWorld) < 1e-6);
    }
}

TEST_CASE("added attributes export as FBX user properties named Component.attr")
{
    run("newScene");
    run("polyCube", {{"name", "Crate"}});
    run("addAttr", {{"args", {"Crate"}}, {"longName", "style"}, {"dataType", "string"}, {"category", "Prop"}});
    run("setAttr", {{"args", {"Crate.style", "whale-architecture"}}});
    run("addAttr", {{"args", {"Crate"}}, {"longName", "hp"}, {"defaultValue", 150.5}, {"category", "Health"}});
    run("addAttr", {{"args", {"Crate"}}, {"longName", "lives"}, {"attributeType", "long"}, {"defaultValue", 3}});
    run("addAttr", {{"args", {"Crate"}}, {"longName", "breakable"}, {"attributeType", "bool"}, {"defaultValue", true}});
    run("addAttr", {{"args", {"Crate"}}, {"longName", "tint"}, {"attributeType", "double3"}, {"category", "Look"}});
    run("setAttr", {{"args", {"Crate.tint", 0.25, 0.5, 1.0}}});
    run("addAttr", {{"args", {"Crate"}}, {"longName", "tags"}, {"dataType", "stringArray"}, {"category", "Look"}});
    run("setAttr", {{"args", {"Crate.tags", json::array({"metal", "heavy"})}}});
    run("addAttr", {{"args", {"Crate"}}, {"longName", "secret"}, {"hidden", true}});
    const fs::path file = tempFile("props.fbx");
    run("exportFbx", {{"args", {file.generic_string(), "Crate"}}, {"kind", "static"}});

    UfbxScene s(file);
    REQUIRE(s.scene);
    const ufbx_node* n = nodeNamed(*s.scene, "Crate");
    REQUIRE(n);
    auto prop = [&](const char* name) { return ufbx_find_prop(&n->props, name); };
    REQUIRE(prop("Prop.style"));
    CHECK(std::string(prop("Prop.style")->value_str.data) == "whale-architecture");
    REQUIRE(prop("Health.hp"));
    CHECK(prop("Health.hp")->value_real == doctest::Approx(150.5));
    REQUIRE(prop("lives"));
    CHECK(prop("lives")->value_int == 3);
    REQUIRE(prop("breakable"));
    CHECK(prop("breakable")->value_int == 1);
    REQUIRE(prop("Look.tint"));
    CHECK(near(vec(prop("Look.tint")->value_vec3), {0.25, 0.5, 1.0}));
    REQUIRE(prop("Look.tags"));
    CHECK(std::string(prop("Look.tags")->value_str.data) == "metal,heavy");
    CHECK_FALSE(prop("secret"));
}
