#include <doctest/doctest.h>

#include "TestUtil.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "io/SceneFile.h"
#include "scene/Camera.h"
#include "scene/Graph.h"
#include "ui/Camera.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <filesystem>

using namespace lila;
using namespace lila::scene;
using namespace testutil;

namespace
{
Node& newCamera()
{
    run("newScene");
    std::string name = run("createNode", {{"args", {"camera"}}}).get<std::string>();
    return *Graph::current().find(name);
}

void set(Node& n, int attr, PlugValue v) { Graph::current().setValue({n.id(), attr, -1}, std::move(v)); }

double deg(double tanHalf) { return glm::degrees(2.0 * std::atan(tanHalf)); }

glm::dvec3 ndc(const glm::dmat4& proj, const glm::dvec3& viewPos)
{
    glm::dvec4 c = proj * glm::dvec4(viewPos, 1.0);
    return glm::dvec3(c) / c.w;
}

glm::dmat4 worldOfNode(const Node& n)
{
    Graph::current().evaluate();
    return n.as<glm::dmat4>(Transform::kWorldMatrix);
}
} // namespace

TEST_CASE("camera lens: angle of view per film fit")
{
    Node& cam = newCamera();
    // Maya's default 36 x 24 mm film back at 35 mm.
    const double halfW = 1.41732 * 25.4 * 0.5 / 35.0, halfH = 0.94488 * 25.4 * 0.5 / 35.0;
    const double filmAspect = halfW / halfH;

    SUBCASE("horizontal keeps the horizontal angle")
    {
        set(cam, Camera::kFilmFit, int64_t(1));
        for (double aspect : {1.0, filmAspect, 2.0})
        {
            glm::dvec2 a = camera::angleOfView(cam, aspect);
            CHECK(a.x == doctest::Approx(deg(halfW)));
            CHECK(a.y == doctest::Approx(deg(halfW / aspect)));
        }
        CHECK(camera::angleOfView(cam, filmAspect).x == doctest::Approx(54.4322).epsilon(1e-4));
    }
    SUBCASE("vertical keeps the vertical angle")
    {
        set(cam, Camera::kFilmFit, int64_t(2));
        glm::dvec2 a = camera::angleOfView(cam, 2.0);
        CHECK(a.y == doctest::Approx(deg(halfH)));
        CHECK(a.x == doctest::Approx(deg(halfH * 2.0)));
    }
    SUBCASE("fill keeps the view inside the film back")
    {
        set(cam, Camera::kFilmFit, int64_t(0));
        CHECK(camera::angleOfView(cam, 2.0).x == doctest::Approx(deg(halfW)));
        CHECK(camera::angleOfView(cam, 1.0).y == doctest::Approx(deg(halfH)));
    }
    SUBCASE("overscan keeps the film back inside the view")
    {
        set(cam, Camera::kFilmFit, int64_t(3));
        CHECK(camera::angleOfView(cam, 2.0).y == doctest::Approx(deg(halfH)));
        CHECK(camera::angleOfView(cam, 1.0).x == doctest::Approx(deg(halfW)));
    }
    SUBCASE("focal length")
    {
        set(cam, Camera::kFilmFit, int64_t(1));
        set(cam, Camera::kFocalLength, 50.0);
        CHECK(camera::angleOfView(cam, 1.5).x == doctest::Approx(deg(1.41732 * 25.4 * 0.5 / 50.0)));
    }
}

TEST_CASE("camera projection")
{
    Node& cam = newCamera();
    set(cam, Camera::kFilmFit, int64_t(1));

    SUBCASE("perspective clips at the near and far planes, reversed")
    {
        set(cam, Camera::kNearClipPlane, 0.5);
        set(cam, Camera::kFarClipPlane, 2000.0);
        glm::dmat4 p = camera::projection(cam, 1.5);
        CHECK(ndc(p, {0, 0, -0.5}).z == doctest::Approx(1.0));
        CHECK(ndc(p, {0, 0, -2000.0}).z == doctest::Approx(0.0).epsilon(1e-9));
        // The edge of the horizontal angle lands on the edge of the view; +Y is up on screen (Vulkan y down).
        glm::dvec2 ext = camera::viewExtents(cam, 1.5);
        CHECK(ndc(p, {ext.x * 100.0, 0, -100.0}).x == doctest::Approx(1.0));
        CHECK(ndc(p, {0, ext.y * 100.0, -100.0}).y == doctest::Approx(-1.0));
    }
    SUBCASE("no far clip is an infinite far plane")
    {
        glm::dmat4 p = camera::projection({0.5, 0.5}, false, 0.1, 0.0);
        CHECK(ndc(p, {0, 0, -0.1}).z == doctest::Approx(1.0));
        CHECK(ndc(p, {0, 0, -1e9}).z == doctest::Approx(0.0).epsilon(1e-6));
    }
    SUBCASE("orthographic shows the orthographic width")
    {
        set(cam, Camera::kOrthographic, true);
        set(cam, Camera::kOrthographicWidth, 40.0);
        set(cam, Camera::kNearClipPlane, 1.0);
        set(cam, Camera::kFarClipPlane, 101.0);
        glm::dmat4 p = camera::projection(cam, 2.0);
        CHECK(p[3][3] == 1.0);
        CHECK(ndc(p, {20.0, 0, -50}).x == doctest::Approx(1.0));
        CHECK(ndc(p, {0, 10.0, -50}).y == doctest::Approx(-1.0));
        CHECK(ndc(p, {0, 0, -1.0}).z == doctest::Approx(1.0));
        CHECK(ndc(p, {0, 0, -101.0}).z == doctest::Approx(0.0));
        CHECK(camera::filmExtents(cam).x == doctest::Approx(20.0));
    }
}

TEST_CASE("view matrix of a camera node")
{
    glm::dmat4 world = glm::translate(glm::dmat4(1.0), glm::dvec3(1, 2, 3)) * glm::rotate(glm::dmat4(1.0), glm::radians(90.0), glm::dvec3(0, 1, 0)) *
                       glm::scale(glm::dmat4(1.0), glm::dvec3(2.0));
    glm::dmat4 v = camera::viewMatrix(world);
    CHECK(near(glm::dvec3(v * glm::dvec4(1, 2, 3, 1)), {0, 0, 0}));
    // Rotated 90 degrees around Y, the camera looks down world -X.
    CHECK(near(glm::dvec3(v * glm::dvec4(-9, 2, 3, 1)), {0, 0, -10}));
}

TEST_CASE("viewPlace poses a camera node")
{
    Node& cam = newCamera();
    std::string name = cam.name();
    Graph& g = Graph::current();

    run("viewPlace", {{"args", {name}}, {"eye", {100, 0, 0}}, {"lookAt", {0, 0, 0}}, {"up", {0, 1, 0}}});
    glm::dmat4 w = worldOfNode(cam);
    CHECK(near(glm::dvec3(w[3]), {100, 0, 0}));
    CHECK(near(-glm::normalize(glm::dvec3(w[2])), {-1, 0, 0}));
    CHECK(near(glm::normalize(glm::dvec3(w[1])), {0, 1, 0}));

    SUBCASE("one undo step")
    {
        run("viewPlace", {{"args", {name}}, {"eye", {0, 50, 50}}, {"lookAt", {0, 0, 0}}, {"orthographicWidth", 12.0}});
        CHECK(cam.as<double>(Camera::kOrthographicWidth) == doctest::Approx(12.0));
        UndoStack::get().undo();
        CHECK(near(glm::dvec3(worldOfNode(cam)[3]), {100, 0, 0}));
        CHECK(cam.as<double>(Camera::kOrthographicWidth) == doctest::Approx(30.0));
    }
    SUBCASE("eye alone moves without turning")
    {
        run("viewPlace", {{"args", {name}}, {"eye", {100, 10, 0}}});
        glm::dmat4 m = worldOfNode(cam);
        CHECK(near(glm::dvec3(m[3]), {100, 10, 0}));
        CHECK(near(-glm::normalize(glm::dvec3(m[2])), {-1, 0, 0}));
    }
    SUBCASE("in world space under a moved parent, keeping its scale")
    {
        std::string grp = run("group", {{"empty", true}}).get<std::string>();
        Node* p = g.find(grp);
        g.setValue({p->id(), Transform::kTranslate, -1}, glm::dvec3(10, 0, 0));
        g.setValue({p->id(), Transform::kRotate, -1}, glm::dvec3(0, 45, 0));
        run("parent", {{"args", {name, grp}}});
        g.setValue({cam.id(), Transform::kScale, -1}, glm::dvec3(2.0));
        run("viewPlace", {{"args", {name}}, {"eye", {0, 0, 300}}, {"lookAt", {0, 0, 0}}, {"up", {0, 1, 0}}});
        glm::dmat4 m = worldOfNode(cam);
        CHECK(near(glm::dvec3(m[3]), {0, 0, 300}, 1e-6));
        CHECK(near(-glm::normalize(glm::dvec3(m[2])), {0, 0, -1}));
        CHECK(glm::length(glm::dvec3(m[0])) == doctest::Approx(2.0));
    }
    SUBCASE("locked cameras refuse")
    {
        set(cam, Camera::kLocked, true);
        CHECK_THROWS_AS(run("viewPlace", {{"args", {name}}, {"eye", {0, 0, 0}}}), CommandError);
        CHECK(near(glm::dvec3(worldOfNode(cam)[3]), {100, 0, 0}));
    }
    CHECK_THROWS_AS(run("viewPlace", {{"args", {"nothing"}}, {"eye", {0, 0, 0}}}), CommandError);
}

TEST_CASE("camera nodes and the ui section survive both scene formats")
{
    static json saved;
    static json loaded = "unset";
    static bool provide = false;
    io::registerUiSection(
        "testCameras", [] { return provide ? saved : json(); }, [](const json& v) { loaded = v; });

    auto dir = std::filesystem::temp_directory_path() / "lila_tests";
    std::filesystem::create_directories(dir);
    for (const char* ext : {".lila", ".lilaa"})
    {
        CAPTURE(ext);
        Node& cam = newCamera();
        std::string name = cam.name();
        set(cam, Camera::kFocalLength, 85.0);
        set(cam, Camera::kHorizontalFilmAperture, 0.98);
        set(cam, Camera::kVerticalFilmAperture, 0.735);
        set(cam, Camera::kFilmFit, int64_t(2));
        set(cam, Camera::kNearClipPlane, 2.5);
        set(cam, Camera::kFarClipPlane, 50000.0);
        set(cam, Camera::kOrthographic, true);
        set(cam, Camera::kOrthographicWidth, 123.0);
        set(cam, Camera::kLocked, true);
        Graph::current().setValue({cam.id(), Transform::kTranslate, -1}, glm::dvec3(1, 2, 3));
        provide = true;
        saved = {{"lookThrough", {"", name, "", ""}}};

        auto path = (dir / (std::string("camera") + ext)).string();
        run("saveScene", {{"args", {path}}});
        run("newScene");
        CHECK(!run("objExists", {{"args", {name}}}).get<bool>());
        provide = false;
        loaded = "unset";
        run("openScene", {{"args", {path}}});

        Node* back = Graph::current().find(name);
        REQUIRE(back);
        CHECK(back->type().name == "camera");
        CHECK(back->as<double>(Camera::kFocalLength) == doctest::Approx(85.0));
        CHECK(back->as<double>(Camera::kHorizontalFilmAperture) == doctest::Approx(0.98));
        CHECK(back->as<double>(Camera::kVerticalFilmAperture) == doctest::Approx(0.735));
        CHECK(back->as<int64_t>(Camera::kFilmFit) == 2);
        CHECK(back->as<double>(Camera::kNearClipPlane) == doctest::Approx(2.5));
        CHECK(back->as<double>(Camera::kFarClipPlane) == doctest::Approx(50000.0));
        CHECK(back->as<bool>(Camera::kOrthographic));
        CHECK(back->as<double>(Camera::kOrthographicWidth) == doctest::Approx(123.0));
        CHECK(back->as<bool>(Camera::kLocked));
        CHECK(near(back->as<glm::dvec3>(Transform::kTranslate), {1, 2, 3}));
        CHECK(loaded == json{{"lookThrough", {"", name, "", ""}}});
    }
}

TEST_CASE("viewport camera poses survive both scene formats")
{
    static ui::Camera saved;
    static ui::Camera loaded;
    static bool provide = false;
    io::registerUiSection(
        "testViewCameras", [] { return provide ? saved.poseJson() : json(); }, [](const json& v) { loaded.setPose(v); });
    saved.target = {10, -20, 30.5};
    saved.distance = 1234.5;
    saved.yaw = -135.0;
    saved.pitch = 12.25;
    saved.orthoWidth = 77.0;
    saved.focalLength = 50.0;

    auto dir = std::filesystem::temp_directory_path() / "lila_tests";
    std::filesystem::create_directories(dir);
    for (const char* ext : {".lila", ".lilaa"})
    {
        CAPTURE(ext);
        run("newScene");
        provide = true;
        auto path = (dir / (std::string("viewCameras") + ext)).string();
        run("saveScene", {{"args", {path}}});
        provide = false;
        loaded = ui::Camera{};
        run("openScene", {{"args", {path}}});
        CHECK(near(loaded.target, saved.target));
        CHECK(loaded.distance == doctest::Approx(saved.distance));
        CHECK(loaded.yaw == doctest::Approx(saved.yaw));
        CHECK(loaded.pitch == doctest::Approx(saved.pitch));
        CHECK(loaded.orthoWidth == doctest::Approx(saved.orthoWidth));
        CHECK(loaded.focalLength == doctest::Approx(saved.focalLength));
    }
    io::unregisterUiSection("testViewCameras");

    ui::Camera kept;
    kept.setPose(json{{"distance", "far"}, {"target", {1, 2}}, {"pitch", 400.0}});
    CHECK(kept.distance == doctest::Approx(ui::Camera{}.distance));
    CHECK(near(kept.target, glm::dvec3(0.0)));
    CHECK(kept.pitch == doctest::Approx(89.9));
}

TEST_CASE("ui sections nobody registered are written back")
{
    run("newScene");
    json doc = io::serialize();
    doc["ui"] = {{"someoneElse", {{"k", 1}}}};
    io::deserialize(doc);
    CHECK(io::serialize().value(json::json_pointer("/ui/someoneElse/k"), 0) == 1);
    run("newScene");
    CHECK(!io::serialize().contains("ui"));
}

TEST_CASE("image planes attach to camera nodes")
{
    Node& cam = newCamera();
    std::string plane = run("imagePlane", {{"camera", cam.name()}}).get<std::string>();
    CHECK(Graph::current().find(plane)->as<std::string>(ImagePlane::kCamera) == cam.name());
    CHECK(imagePlane::cameraName("SIDE") == "right");
    CHECK(imagePlane::cameraName("Camera1") == "Camera1");
}

TEST_CASE("camera node gate settings live on the node")
{
    Node& cam = newCamera();
    std::string name = cam.name();
    run("cameraSettings", {{"args", {name}}, {"gate", true}, {"overscan", 1.3}, {"filmFit", "vertical"}, {"maskOpacity", 0.25}});
    CHECK(cam.as<bool>(Camera::kDisplayResolution));
    CHECK(cam.as<double>(Camera::kOverscan) == doctest::Approx(1.3));
    CHECK(cam.as<int64_t>(Camera::kFilmFit) == 2);
    CHECK(renderSettings().cameras.count(name) == 0);

    run("rename", {{"args", {name, "shotCam"}}});
    CameraSettings c = renderSettings().camera("shotCam");
    CHECK(c.resolutionGate);
    CHECK(c.overscan == doctest::Approx(1.3));
    CHECK(c.filmFit == FilmFit::Vertical);
    CHECK(c.gateMaskOpacity == doctest::Approx(0.25));
    CHECK(!renderSettings().camera(name).resolutionGate);

    SUBCASE("built-in views keep theirs by name")
    {
        run("cameraSettings", {{"args", {"persp"}}, {"filmGate", true}});
        CHECK(renderSettings().cameras.at("persp").filmGate);
    }
    SUBCASE("older scenes keyed by name move onto the node")
    {
        json doc = io::serialize();
        doc["renderSettings"]["cameras"]["shotCam"] = {{"filmGate", true}, {"overscan", 2.0}};
        io::deserialize(doc);
        Node* back = Graph::current().find("shotCam");
        REQUIRE(back);
        CHECK(back->as<bool>(Camera::kDisplayFilmGate));
        CHECK(back->as<double>(Camera::kOverscan) == doctest::Approx(2.0));
        CHECK(renderSettings().cameras.count("shotCam") == 0);
    }
}

TEST_CASE("moving a saved view marks the scene modified until it moves back")
{
    run("newScene");
    double pose = 0.0;
    io::registerUiSection("testView", [&] { return json(pose); }, [&](const json& j) { pose = j.is_number() ? j.get<double>() : 0.0; });
    io::markClean();
    pose = 5.0;
    io::uiChanged("testView");
    CHECK(io::modified());
    pose = 0.0;
    io::uiChanged("testView");
    CHECK_FALSE(io::modified());

    pose = 7.0;
    io::acceptUi("testView");
    CHECK_FALSE(io::modified());
    io::unregisterUiSection("testView");
}

TEST_CASE("near clip: auto on the built-in views, manual override, camera nodes always manual")
{
    Node& cam = newCamera();
    CHECK(renderSettings().camera("persp").nearClip == 0.0);

    ui::Camera view;
    view.autoNear = true;
    view.distance = 2.0;
    CHECK(view.nearPlane() == doctest::Approx(0.002));
    view.distance = 120.0;
    CHECK(view.nearPlane() == doctest::Approx(0.12));
    view.ortho = true;
    CHECK(view.nearPlane() == view.nearClip);
    view.ortho = false;
    view.autoNear = false;
    view.nearClip = 0.5;
    CHECK(view.nearPlane() == 0.5);
    CHECK(ndc(view.proj(1.5), {0, 0, -0.5}).z == doctest::Approx(1.0));

    run("cameraSettings", {{"args", {"persp"}}, {"nearClip", 0.25}});
    CHECK(renderSettings().camera("persp").nearClip == doctest::Approx(0.25));
    run("cameraSettings", {{"args", {"persp"}}, {"nearClip", 0.0}});
    CHECK(renderSettings().camera("persp").nearClip == 0.0);
    run("cameraSettings", {{"args", {"persp"}}, {"nearClip", -1.0}});
    CHECK(renderSettings().camera("persp").nearClip == 0.0);

    run("cameraSettings", {{"args", {cam.name()}}, {"nearClip", 0.03}});
    CHECK(cam.as<double>(Camera::kNearClipPlane) == doctest::Approx(0.03));
    CHECK(renderSettings().camera(cam.name()).nearClip == doctest::Approx(0.03));
    CHECK_THROWS_AS(run("cameraSettings", {{"args", {cam.name()}}, {"nearClip", 0.0}}), CommandError);
}
