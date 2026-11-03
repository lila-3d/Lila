#include <doctest/doctest.h>

#include "TestUtil.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/ImagePlane.h"

#include <glm/gtc/matrix_transform.hpp>

using namespace lila;
using namespace lila::scene;
using namespace testutil;
namespace ip = lila::scene::imagePlane;

namespace
{
bool nearTight(const glm::dvec4& a, const glm::dvec4& b) { return glm::length(a - b) < 1e-9; }
} // namespace

TEST_CASE("image plane fit")
{
    const glm::dvec2 box(200.0, 100.0); // 2:1 frame

    SUBCASE("best fits inside")
    {
        CHECK(ip::fit(ip::Fit::Best, box, 4.0).size == glm::dvec2(200.0, 50.0));
        CHECK(ip::fit(ip::Fit::Best, box, 1.0).size == glm::dvec2(100.0, 100.0));
        CHECK(nearTight(ip::fit(ip::Fit::Best, box, 1.0).uv, {0, 0, 1, 1}));
    }
    SUBCASE("fill covers and crops")
    {
        ip::FitResult wide = ip::fit(ip::Fit::Fill, box, 4.0);
        CHECK(wide.size == box);
        CHECK(nearTight(wide.uv, {0.25, 0.0, 0.75, 1.0}));
        ip::FitResult square = ip::fit(ip::Fit::Fill, box, 1.0);
        CHECK(nearTight(square.uv, {0.0, 0.25, 1.0, 0.75}));
        // The shown part keeps the image's aspect.
        double shownAspect = (square.uv.z - square.uv.x) * 1.0 / (square.uv.w - square.uv.y);
        CHECK(shownAspect == doctest::Approx(2.0));
    }
    SUBCASE("horizontal, vertical, to size")
    {
        CHECK(ip::fit(ip::Fit::Horizontal, box, 1.0).size == glm::dvec2(200.0, 200.0));
        CHECK(ip::fit(ip::Fit::Vertical, box, 4.0).size == glm::dvec2(400.0, 100.0));
        ip::FitResult stretched = ip::fit(ip::Fit::ToSize, box, 0.5);
        CHECK(stretched.size == box);
        CHECK(nearTight(stretched.uv, {0, 0, 1, 1}));
    }
    SUBCASE("degenerate input")
    {
        CHECK(ip::fit(ip::Fit::Best, {0.0, 0.0}, 2.0).size == glm::dvec2(0.0));
        CHECK(ip::fit(ip::Fit::Horizontal, box, 0.0).size == glm::dvec2(200.0, 200.0));
    }
}

TEST_CASE("image plane layout")
{
    ip::Settings s;
    s.width = 300.0;
    s.maintainRatio = true;

    SUBCASE("free plane follows its world matrix")
    {
        s.offset = {10.0, 0.0};
        glm::dmat4 world = glm::translate(glm::dmat4(1.0), glm::dvec3(0, 50, 0)) * glm::rotate(glm::dmat4(1.0), glm::radians(90.0), {0, 1, 0});
        ip::Quad q = ip::layoutFree(s, world, 1.5);
        // Local +X maps to world -Z after the turn around Y.
        CHECK(nearTight(q.center, {0, 50, -10}));
        CHECK(nearTight(q.halfU, {0, 0, -150}));
        CHECK(nearTight(q.halfV, {0, 100, 0}));
        CHECK(nearTight(q.normal(), {1, 0, 0}));
        auto c = q.corners();
        CHECK(nearTight(c[0], {0, 150, 140}));
        CHECK(nearTight(c[2], {0, -50, -160}));
    }
    SUBCASE("maintain ratio off uses the height")
    {
        s.maintainRatio = false;
        s.height = 50.0;
        s.fit = ip::Fit::ToSize;
        ip::Quad q = ip::layoutFree(s, glm::dmat4(1.0), 1.5);
        CHECK(nearTight(q.halfV, {0, 25, 0}));
    }
    SUBCASE("orthographic camera keeps the plane fixed in the world")
    {
        ip::CameraFrame front;
        front.ortho = true;
        front.eye = {12, 34, 5000}; // panning the view moves only the eye
        front.forward = {0, 0, -1};
        front.right = {1, 0, 0};
        front.up = {0, 1, 0};
        s.depth = 1000.0;
        s.offset = {0.0, 75.0};
        ip::Quad q = ip::layoutAttached(s, front, 2.0);
        CHECK(nearTight(q.center, {0, 75, -1000}));
        CHECK(nearTight(q.halfU, {150, 0, 0}));
        CHECK(nearTight(q.halfV, {0, 75, 0}));
        front.eye = {-500, 0, 20};
        CHECK(nearTight(ip::layoutAttached(s, front, 2.0).center, q.center));

        ip::CameraFrame side = front;
        side.forward = {-1, 0, 0};
        side.right = {0, 0, -1};
        ip::Quad r = ip::layoutAttached(s, side, 2.0);
        CHECK(nearTight(r.center, {-1000, 75, 0}));
        CHECK(nearTight(r.normal(), {1, 0, 0})); // facing the camera
    }
    SUBCASE("perspective camera fills its film gate at depth")
    {
        ip::CameraFrame cam;
        cam.eye = {0, 0, 100};
        cam.film = {0.5, 0.25}; // tangents: a 2:1 gate
        s.depth = 200.0;
        s.fit = ip::Fit::Fill;
        ip::Quad q = ip::layoutAttached(s, cam, 1.0);
        CHECK(nearTight(q.center, {0, 0, -100}));
        CHECK(nearTight(q.halfU, {100, 0, 0}));
        CHECK(nearTight(q.halfV, {0, 50, 0}));
        CHECK(nearTight(q.uv, {0.0, 0.25, 1.0, 0.75}));

        s.fit = ip::Fit::Best;
        CHECK(nearTight(ip::layoutAttached(s, cam, 1.0).halfU, {50, 0, 0}));
        // A plane further away covers the same part of the view.
        s.depth = 400.0;
        ip::Quad far = ip::layoutAttached(s, cam, 1.0);
        CHECK(nearTight(far.halfU, {100, 0, 0}));
        CHECK(nearTight(far.center, {0, 0, -300}));
    }
    SUBCASE("rotation turns the image in its plane")
    {
        ip::CameraFrame front;
        front.ortho = true;
        s.rotation = 90.0;
        s.depth = 0.0;
        ip::Quad q = ip::layoutAttached(s, front, 1.0);
        CHECK(nearTight(q.halfU, {0, 150, 0}));
        CHECK(nearTight(q.halfV, {-150, 0, 0}));
        CHECK(nearTight(q.normal(), {0, 0, 1}));
    }
}

TEST_CASE("image sequence file names")
{
    CHECK(ip::frameFile("ref/shot.0001.png", 42) == "ref/shot.0042.png");
    CHECK(ip::frameFile("ref/shot_7.jpg", 123) == "ref/shot_123.jpg");
    CHECK(ip::frameFile("ref/shot.####.png", 5) == "ref/shot.0005.png");
    CHECK(ip::frameFile("ref/shot.#.png", 12345) == "ref/shot.12345.png");
    CHECK(ip::frameFile("ref/shot.0010.png", -3) == "ref/shot.-0003.png");
    CHECK(ip::frameFile("v2/blueprint.png", 9) == "v2/blueprint.png");
    CHECK(ip::frameFile("C:\\take01\\plate.tga", 9) == "C:\\take01\\plate.tga");
    CHECK(ip::frameFile("C:\\take01\\plate100.tga", 101) == "C:\\take01\\plate101.tga");
}

TEST_CASE("imagePlane command")
{
    run("newScene");
    Graph& g = Graph::current();

    std::string name = run("imagePlane", {{"fileName", "refs/front.png"}, {"camera", "side"}, {"fit", "fill"}, {"showInAllViews", true},
                                          {"alphaGain", 0.5}, {"placement", "inFront"}, {"locked", true}})
                           .get<std::string>();
    Node* n = g.find(name);
    REQUIRE(n);
    CHECK(n->type().isA("imagePlane"));
    CHECK(n->type().isA("transform"));
    CHECK(n->as<std::string>(ImagePlane::kImageName) == "refs/front.png");
    CHECK(n->as<std::string>(ImagePlane::kCamera) == "right");
    CHECK(ip::Fit(n->as<int64_t>(ImagePlane::kFit)) == ip::Fit::Fill);
    CHECK(ip::DisplayMode(n->as<int64_t>(ImagePlane::kDisplayMode)) == ip::DisplayMode::AllViews);
    CHECK(ip::Placement(n->as<int64_t>(ImagePlane::kPlacement)) == ip::Placement::InFront);
    CHECK(n->as<double>(ImagePlane::kAlphaGain) == 0.5);
    CHECK(n->as<bool>(ImagePlane::kLocked));
    CHECK(g.isSelected(n->id()));
    CHECK((n->type().attrs[ImagePlane::kImageName].flags & AttrFile));

    run("imagePlane", {{"args", {name}}, {"depth", 250.0}, {"camera", ""}});
    CHECK(n->as<double>(ImagePlane::kDepth) == 250.0);
    CHECK(n->as<std::string>(ImagePlane::kCamera).empty());

    size_t before = g.nodes().size();
    CHECK_THROWS_AS(run("imagePlane", {{"camera", "nowhere"}}), CommandError);
    CHECK_THROWS_AS(run("imagePlane", {{"fit", "sideways"}}), CommandError);
    CHECK_THROWS_AS(run("imagePlane", {{"args", {"nothing"}}}), CommandError);
    CHECK(g.nodes().size() == before);

    // One command, one undo step: creating then undoing leaves no plane behind.
    std::string second = run("imagePlane", {{"fileName", "a.png"}}).get<std::string>();
    REQUIRE(g.find(second));
    run("undo");
    CHECK(g.find(second) == nullptr);
    CHECK(g.find(name));
}
