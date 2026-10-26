#include <doctest/doctest.h>

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
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }
json v3(double x, double y, double z) { return json::array({x, y, z}); }

fs::path tempFile(const std::string& name)
{
    fs::path dir = fs::temp_directory_path() / "lila_fbx_tests";
    fs::create_directories(dir);
    return dir / name;
}

struct UfbxScene
{
    ufbx_scene* scene = nullptr;
    explicit UfbxScene(const fs::path& p)
    {
        ufbx_load_opts opts{};
        ufbx_error err{};
        std::string s = p.string();
        scene = ufbx_load_file(s.c_str(), &opts, &err);
        if (!scene)
            MESSAGE(std::string(err.description.data, err.description.length));
    }
    ~UfbxScene() { ufbx_free_scene(scene); }
};

glm::dvec3 vec(const ufbx_vec3& v) { return {v.x, v.y, v.z}; }
bool near(const glm::dvec3& a, const glm::dvec3& b, double eps = 1e-6) { return glm::length(a - b) < eps; }

const ufbx_node* nodeNamed(const ufbx_scene& s, const char* name)
{
    for (size_t i = 0; i < s.nodes.count; ++i)
        if (std::string(s.nodes.data[i]->name.data) == name)
            return s.nodes.data[i];
    return nullptr;
}

double maxDiff(const glm::dmat4& a, const glm::dmat4& b)
{
    double d = 0.0;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            d = std::max(d, std::abs(a[c][r] - b[c][r]));
    return d;
}

glm::dmat4 worldOf(const char* name) { return Graph::current().find(name)->as<glm::dmat4>(Transform::kWorldMatrix); }

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
