#include <doctest/doctest.h>

#include "ops/Command.h"
#include "fbx/Fbx.h"
#include "geo/Primitives.h"
#include "math/MathCore.h"
#include "scene/Anim.h"
#include "scene/Camera.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <ufbx.h>

#include <filesystem>

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

/** A camera on frames 0, 12 and 24 at 24 fps: moving, turning, zooming and changing its clipping. */
void keyedCamera(const char* name)
{
    run("playbackOptions", {{"fps", 24.0}, {"minTime", 0.0}, {"maxTime", 1.0}, {"animationStartTime", 0.0}, {"animationEndTime", 1.0}});
    run("createNode", {{"args", {"camera"}}, {"name", name}});
    const std::string cam = name;
    struct K
    {
        double t;
        glm::dvec3 tr, rot;
        double fl, ncp, fcp;
    };
    for (const K& k : {K{0.0, {0, 100, 500}, {-10, 0, 0}, 35.0, 1.0, 5000.0}, K{0.5, {200, 150, 300}, {-20, 45, 5}, 50.0, 2.0, 8000.0},
                       K{1.0, {400, 120, 0}, {-5, 90, 0}, 85.0, 1.0, 5000.0}})
    {
        run("currentTime", {{"args", {k.t}}});
        run("setAttr", {{"args", {cam + ".translate", k.tr.x, k.tr.y, k.tr.z}}});
        run("setAttr", {{"args", {cam + ".rotate", k.rot.x, k.rot.y, k.rot.z}}});
        run("setAttr", {{"args", {cam + ".focalLength", k.fl}}});
        run("setAttr", {{"args", {cam + ".nearClipPlane", k.ncp}}});
        run("setAttr", {{"args", {cam + ".farClipPlane", k.fcp}}});
        run("setKeyframe", {{"args", {cam}}, {"attribute", {"translate", "rotate", "focalLength", "nearClipPlane", "farClipPlane"}}});
    }
    run("setAttr", {{"args", {cam + ".horizontalFilmAperture", 1.247}}});
    run("setAttr", {{"args", {cam + ".verticalFilmAperture", 0.702}}});
}

struct Sample
{
    glm::dmat4 world;
    double focal, nearClip, farClip;
};

std::vector<Sample> sampleCamera(const char* name)
{
    Graph& g = Graph::current();
    std::vector<Sample> out;
    for (int f = 0; f <= 24; f += 3)
    {
        g.setTime(f / 24.0);
        g.evaluate();
        const Node* n = g.find(name);
        out.push_back({n->as<glm::dmat4>(Transform::kWorldMatrix), n->as<double>(Camera::kFocalLength), n->as<double>(Camera::kNearClipPlane),
                       n->as<double>(Camera::kFarClipPlane)});
    }
    return out;
}
} // namespace

TEST_CASE("cameras export as FBX cameras looking down +X with +Y up in every profile")
{
    for (math::UpAxis up : {math::UpAxis::Y, math::UpAxis::Z})
        for (fbx::AxisProfile p : {fbx::AxisProfile::Ue5, fbx::AxisProfile::Ue6, fbx::AxisProfile::Generic})
        {
            CAPTURE(int(up));
            CAPTURE(fbx::profileName(p));
            math::setUpAxis(up);
            run("newScene");
            run("createNode", {{"args", {"camera"}}, {"name", "cam"}});
            run("setAttr", {{"args", {"cam.translate", 10, 20, 30}}});
            run("setAttr", {{"args", {"cam.rotate", 25, -40, 15}}});
            run("setAttr", {{"args", {"cam.focalLength", 50.0}}});
            run("setAttr", {{"args", {"cam.nearClipPlane", 5.0}}});
            run("setAttr", {{"args", {"cam.farClipPlane", 20000.0}}});
            Graph::current().evaluate();
            const glm::dmat4 w = worldOf("cam");
            const glm::dmat3 basis = fbx::profileBasis(p, up);

            fs::path path = tempFile(std::string("camera_axes_") + fbx::profileName(p) + (up == math::UpAxis::Z ? "_z" : "_y") + ".fbx");
            run("exportFbx", {{"args", {path.string(), "cam"}}, {"profile", fbx::profileName(p)}, {"kind", "static"}});
            UfbxScene s(path);
            REQUIRE(s.scene);
            const ufbx_node* n = nodeNamed(*s.scene, "cam");
            REQUIRE(n);
            REQUIRE(n->camera);
            const ufbx_matrix& m = n->node_to_world;
            const glm::dvec3 look = glm::normalize(vec(m.cols[0])), camUp = glm::normalize(vec(m.cols[1]));
            CHECK(near(look, basis * glm::normalize(-glm::dvec3(w[2]))));
            CHECK(near(camUp, basis * glm::normalize(glm::dvec3(w[1]))));
            CHECK(near(vec(m.cols[3]), basis * glm::dvec3(w[3])));

            const ufbx_camera& c = *n->camera;
            CHECK(c.aperture_mode == UFBX_APERTURE_MODE_FOCAL_LENGTH);
            CHECK(c.projection_mode == UFBX_PROJECTION_MODE_PERSPECTIVE);
            CHECK(c.focal_length_mm == doctest::Approx(50.0));
            CHECK(c.film_size_inch.x == doctest::Approx(1.41732));
            CHECK(c.film_size_inch.y == doctest::Approx(0.94488));
            CHECK(c.near_plane == doctest::Approx(5.0));
            CHECK(c.far_plane == doctest::Approx(20000.0));
            CHECK(c.gate_fit == UFBX_GATE_FIT_HORIZONTAL);
            CHECK(n->children.count == 0);
        }
    math::setUpAxis(math::UpAxis::Y);
}

TEST_CASE("an animated camera round trips with its focal length and clipping curves")
{
    for (const char* units : {"cm", "m"})
        for (const char* profile : {"ue5", "ue6"})
        {
            CAPTURE(units);
            CAPTURE(profile);
            run("newScene");
            keyedCamera("shotCam");
            Graph& g = Graph::current();
            NodeId cube = g.createNode(*types::find("mesh"), "prop", kNoNode, false)->id();
            g.setValue({cube, MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(geo::makeCube())));
            const std::vector<Sample> before = sampleCamera("shotCam");

            fs::path withProp = tempFile(std::string("camera_sequence_") + profile + "_" + units + ".fbx");
            run("exportFbx", {{"args", {withProp.string(), "shotCam", "prop"}}, {"profile", profile}, {"units", units}, {"kind", "sequence"}});
            {
                UfbxScene s(withProp);
                REQUIRE(s.scene);
                CHECK(s.scene->meshes.count == 0);
                CHECK(nodeNamed(*s.scene, "prop"));
                CHECK(s.scene->anim_stacks.count == 1);
            }

            fs::path path = tempFile(std::string("camera_anim_") + profile + "_" + units + ".fbx");
            // Only cameras given: kind auto picks sequence.
            run("exportFbx", {{"args", {path.string(), "shotCam"}}, {"profile", profile}, {"units", units}});
            {
                UfbxScene s(path);
                REQUIRE(s.scene);
                const ufbx_node* n = nodeNamed(*s.scene, "shotCam");
                REQUIRE(n);
                REQUIRE(n->camera);
                REQUIRE(s.scene->anim_stacks.count == 1);
                const ufbx_anim* anim = s.scene->anim_stacks.data[0]->anim;
                bool focalAnimated = false;
                for (size_t i = 0; i < s.scene->anim_layers.data[0]->anim_props.count; ++i)
                {
                    const ufbx_anim_prop& ap = s.scene->anim_layers.data[0]->anim_props.data[i];
                    focalAnimated = focalAnimated || (ap.element == &n->camera->element && std::string(ap.prop_name.data) == "FocalLength");
                }
                CHECK(focalAnimated);
                CHECK(ufbx_evaluate_prop(anim, &n->camera->element, "FocalLength", 0.5).value_real == doctest::Approx(50.0));
                const double perUnit = std::string(units) == "m" ? 0.01 : 1.0;
                CHECK(ufbx_evaluate_prop(anim, &n->camera->element, "FarPlane", 0.5).value_real == doctest::Approx(8000.0 * perUnit));
            }

            run("rename", {{"args", {"shotCam", "origCam"}}});
            json rep = run("importFbx", {{"args", {path.string()}}, {"report", true}});
            REQUIRE(rep["cameras"].size() == 1);
            CHECK(rep["cameras"][0] == "shotCam");
            REQUIRE(g.find("shotCam")->type().isA("camera"));
            CHECK(g.find("shotCam")->as<double>(Camera::kHorizontalFilmAperture) == doctest::Approx(1.247));
            CHECK(g.find("shotCam")->as<double>(Camera::kVerticalFilmAperture) == doctest::Approx(0.702));
            const std::vector<Sample> after = sampleCamera("shotCam");
            REQUIRE(after.size() == before.size());
            for (size_t f = 0; f < before.size(); ++f)
            {
                CAPTURE(f);
                CHECK(maxDiff(before[f].world, after[f].world) < 1e-3);
                CHECK(after[f].focal == doctest::Approx(before[f].focal).epsilon(1e-5));
                CHECK(after[f].nearClip == doctest::Approx(before[f].nearClip).epsilon(1e-5));
                CHECK(after[f].farClip == doctest::Approx(before[f].farClip).epsilon(1e-5));
            }
        }
}

TEST_CASE("children of a camera keep their world placement through the camera's turn")
{
    for (const char* profile : {"ue5", "ue6", "generic"})
    {
        CAPTURE(profile);
        run("newScene");
        run("createNode", {{"args", {"transform"}}, {"name", "rig"}});
        run("setAttr", {{"args", {"rig.rotate", 0, 30, 0}}});
        run("createNode", {{"args", {"camera"}}, {"name", "cam"}, {"parent", "rig"}});
        run("setAttr", {{"args", {"cam.translate", 5, 60, 200}}});
        run("setAttr", {{"args", {"cam.rotate", -15, 20, 5}}});
        run("createNode", {{"args", {"transform"}}, {"name", "kid"}, {"parent", "cam"}});
        run("setAttr", {{"args", {"kid.translate", 10, 20, -30}}});
        run("setAttr", {{"args", {"kid.rotate", 15, 25, 35}}});
        Graph::current().evaluate();
        const glm::dmat4 kidWorld = worldOf("kid"), camWorld = worldOf("cam");
        fbx::AxisProfile p = fbx::AxisProfile::Ue5;
        REQUIRE(fbx::parseProfile(profile, &p));
        const glm::dmat4 basis(fbx::profileBasis(p, math::upAxis()));

        fs::path path = tempFile(std::string("camera_child_") + profile + ".fbx");
        run("exportFbx", {{"args", {path.string(), "rig"}}, {"profile", profile}, {"kind", "static"}});
        {
            UfbxScene s(path);
            REQUIRE(s.scene);
            const ufbx_node* kid = nodeNamed(*s.scene, "kid");
            REQUIRE(kid);
            // A plain node's world in the file is its Lila world in file axes, whatever its camera parent's turn.
            const glm::dmat4 expected = basis * kidWorld * glm::transpose(basis);
            const ufbx_matrix& m = kid->node_to_world;
            for (int c = 0; c < 4; ++c)
                CHECK(near(vec(m.cols[c]), glm::dvec3(expected[c]), 1e-5));
        }
        run("newScene");
        run("importFbx", {{"args", {path.string()}}});
        Graph::current().evaluate();
        CHECK(maxDiff(worldOf("cam"), camWorld) < 1e-6);
        CHECK(maxDiff(worldOf("kid"), kidWorld) < 1e-6);
    }
}
