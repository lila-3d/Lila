#include <doctest/doctest.h>

#include "TestUtil.h"

#include "curve/RegionSplit.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "rigmath/MathNodes.h"
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

TEST_CASE("imagePlaneCut")
{
    rigmath::registerTypes();
    if (!ops::find("expression"))
        rigmath::registerCommands();
    run("newScene");
    Graph& g = Graph::current();
    // A 200 cm square picture centred on the origin, facing +Z, with "arms" either side.
    std::string body = run("imagePlane", {{"fileName", "a.png"}, {"fit", "toSize"}, {"width", 200.0}, {"height", 200.0}, {"maintainRatio", false}}).get<std::string>();
    Node* plane = g.find(body);
    REQUIRE(plane);
    CHECK(ip::region(*plane).empty());

    // Shoulder at x = 40, cut from y = 50 to y = 30. The ends run on fanned 30 degrees out from straight across, so the
    // arm is the wedge right of x = 40 widening away from the body, and nothing below the armpit goes with it.
    json out = run("imagePlaneCut", {{"points", {{40.0, 50.0, 0.0}, {40.0, 30.0, 0.0}}}, {"symmetry", "x"}, {"aspect", 1.0}});
    REQUIRE(out.size() == 2);
    Node* arm = g.find(out[0].get<std::string>());
    Node* other = g.find(out[1].get<std::string>());
    REQUIRE(arm);
    REQUIRE(other);
    CHECK(g.parent(arm->id()) == plane->id());
    CHECK(g.parent(other->id()) == plane->id());
    CHECK(arm->as<glm::dvec3>(Transform::kRotatePivot).x == doctest::Approx(40.0));
    CHECK(arm->as<glm::dvec3>(Transform::kRotatePivot).y == doctest::Approx(40.0));
    CHECK(other->as<glm::dvec3>(Transform::kRotatePivot).x == doctest::Approx(-40.0));
    CHECK(arm->as<std::string>(ImagePlane::kImageName) == "a.png");
    CHECK(arm->as<bool>(ImagePlane::kKeyBackground));
    CHECK(plane->as<bool>(ImagePlane::kKeyBackground));

    auto contains = [](const Node& n, glm::dvec2 p) { return curve::regionContains(ip::region(n), p); };
    CHECK(contains(*arm, {80.0, 40.0}));
    CHECK_FALSE(contains(*arm, {0.0, 0.0}));
    CHECK_FALSE(contains(*arm, {60.0, -40.0})); // beside the hip
    CHECK_FALSE(contains(*arm, {-80.0, 40.0}));
    CHECK(contains(*other, {-80.0, 40.0}));
    CHECK(contains(*plane, {0.0, 0.0}));
    CHECK(contains(*plane, {60.0, -40.0}));
    CHECK_FALSE(contains(*plane, {80.0, 40.0}));
    CHECK_FALSE(contains(*plane, {-80.0, 40.0}));
    // The picture's own edge bounds the pieces.
    CHECK_FALSE(contains(*arm, {150.0, 40.0}));

    // Elbow: cuts each arm, not the body, and chains under it on both sides.
    json elbow = run("imagePlaneCut", {{"points", {{70.0, 50.0, 0.0}, {70.0, 30.0, 0.0}}}, {"symmetry", "x"}});
    REQUIRE(elbow.size() == 2);
    Node* forearm = g.find(elbow[0].get<std::string>());
    Node* otherForearm = g.find(elbow[1].get<std::string>());
    REQUIRE(forearm);
    REQUIRE(otherForearm);
    CHECK(g.parent(forearm->id()) == arm->id());
    CHECK(g.parent(otherForearm->id()) == other->id());
    CHECK(contains(*forearm, {90.0, 40.0}));
    CHECK(contains(*arm, {55.0, 40.0}));
    CHECK(contains(*plane, {0.0, 0.0}));

    // Wrist as a lasso: across the wrist, then around the hand. Only what the lasso holds goes; the forearm keeps the rest.
    json wrist = run("imagePlaneCut", {{"points", {{85.0, 45.0, 0.0}, {85.0, 35.0, 0.0}, {98.0, 30.0, 0.0}, {98.0, 50.0, 0.0}}}});
    REQUIRE(wrist.size() == 1);
    Node* hand = g.find(wrist[0].get<std::string>());
    REQUIRE(hand);
    CHECK(g.parent(hand->id()) == forearm->id());
    CHECK(contains(*hand, {92.0, 40.0}));
    CHECK_FALSE(contains(*hand, {92.0, 60.0}));
    CHECK(contains(*forearm, {92.0, 60.0}));
    CHECK(hand->as<glm::dvec3>(Transform::kRotatePivot).x == doctest::Approx(85.0));

    // A lasso wholly inside a piece leaves no hole behind: the piece stays one loop with the lasso's part gone.
    json patch = run("imagePlaneCut", {{"points", {{-10.0, -60.0, 0.0}, {10.0, -60.0, 0.0}, {10.0, -80.0, 0.0}, {-10.0, -80.0, 0.0}}}});
    REQUIRE(patch.size() == 1);
    CHECK(ip::region(*plane).size() == 1);
    CHECK_FALSE(contains(*plane, {0.0, -70.0}));
    CHECK(contains(*plane, {0.0, 0.0}));
    CHECK(contains(*g.find(patch[0].get<std::string>()), {0.0, -70.0}));
    run("undo");

    // The mirrored arm follows the first, turning the other way.
    run("setAttr", {{"args", {arm->name() + ".rz", 30.0}}});
    CHECK(get(other->name() + ".rz") == doctest::Approx(-30.0));
    // A piece draws at its own order plus its plane's, so moving the plane moves the figure.
    run("setAttr", {{"args", {plane->name() + ".drawOrder", 5}}});
    run("setAttr", {{"args", {forearm->name() + ".drawOrder", 1}}});
    CHECK(arm->as<int64_t>(ImagePlane::kDrawOrder) == 0);
    CHECK(ip::drawOrder(g, *arm) == 5);
    CHECK(ip::drawOrder(g, *forearm) == 6);
    run("undo");
    run("undo");
    // Moving the pivot or the piece mirrors too.
    run("setAttr", {{"args", {arm->name() + ".rpx", 45.0}}});
    run("setAttr", {{"args", {arm->name() + ".rpy", 42.0}}});
    run("setAttr", {{"args", {arm->name() + ".tx", 3.0}}});
    CHECK(get(other->name() + ".rpx") == doctest::Approx(-45.0));
    CHECK(get(other->name() + ".rpy") == doctest::Approx(42.0));
    CHECK(get(other->name() + ".tx") == doctest::Approx(-3.0));
    CHECK(get(other->name() + ".rx") == doctest::Approx(0.0));
    run("undo");
    run("undo");
    run("undo");

    CHECK_THROWS_AS(run("imagePlaneCut", {{"points", {{0.0, 0.0, 500.0}, {0.0, 10.0, 500.0}}}}), CommandError);

    // One undo step takes the whole cut back.
    run("undo"); // the rotation
    CHECK(g.find(wrist[0].get<std::string>()));
    run("undo");
    CHECK(g.find(wrist[0].get<std::string>()) == nullptr);
    run("undo");
    CHECK(g.find(elbow[0].get<std::string>()) == nullptr);
    CHECK(g.find(elbow[1].get<std::string>()) == nullptr);
}

TEST_CASE("imagePlaneCut mirror follows down the chain")
{
    rigmath::registerTypes();
    if (!ops::find("expression"))
        rigmath::registerCommands();
    run("newScene");
    Graph& g = Graph::current();
    std::string body = run("imagePlane", {{"fileName", "a.png"}, {"fit", "toSize"}, {"width", 200.0}, {"height", 200.0}, {"maintainRatio", false}}).get<std::string>();
    json shoulder = run("imagePlaneCut", {{"points", {{40.0, 50.0, 0.0}, {40.0, 30.0, 0.0}}}, {"symmetry", "x"}, {"aspect", 1.0}});
    REQUIRE(shoulder.size() == 2);
    const std::string arm = shoulder[0].get<std::string>(), other = shoulder[1].get<std::string>();
    run("setAttr", {{"args", {arm + ".rz", 30.0}}});

    // The elbow is cut where it has swung to, so its plane frame is turned.
    auto swing = [](glm::dvec2 p) {
        const glm::dvec2 c(40.0, 40.0), d = p - c;
        const double a = glm::radians(30.0);
        return json::array({c.x + d.x * std::cos(a) - d.y * std::sin(a), c.y + d.x * std::sin(a) + d.y * std::cos(a), 0.0});
    };
    json elbow = run("imagePlaneCut", {{"points", {swing({70.0, 50.0}), swing({70.0, 30.0})}}, {"symmetry", "x"}, {"aspect", 1.0}});
    REQUIRE(elbow.size() == 2);
    const std::string forearm = elbow[0].get<std::string>(), otherForearm = elbow[1].get<std::string>();
    CHECK(g.parent(g.find(otherForearm)->id()) == g.find(other)->id());

    auto worldPivot = [&](const std::string& n) {
        g.evaluate();
        const Node* node = g.find(n);
        return glm::dvec3(node->as<glm::dmat4>(Transform::kWorldMatrix) * glm::dvec4(node->as<glm::dvec3>(Transform::kRotatePivot), 1.0));
    };
    run("setPivot", {{"args", {forearm}}, {"position", {80.0, 40.0, 0.0}}});
    run("setAttr", {{"args", {forearm + ".rz", 15.0}}});
    const glm::dvec3 p = worldPivot(forearm), q = worldPivot(otherForearm);
    CHECK(p.x == doctest::Approx(80.0));
    CHECK(q.x == doctest::Approx(-80.0));
    CHECK(q.y == doctest::Approx(p.y));
    CHECK(get(otherForearm + ".rz") == doctest::Approx(-15.0));
    g.evaluate();
    const glm::dmat4 a = g.find(forearm)->as<glm::dmat4>(Transform::kWorldMatrix), b = g.find(otherForearm)->as<glm::dmat4>(Transform::kWorldMatrix);
    CHECK(b[3].x == doctest::Approx(-a[3].x));
    CHECK(b[3].y == doctest::Approx(a[3].y));
    (void)body;
}

TEST_CASE("imagePlaneCut mirror off the plane's origin")
{
    rigmath::registerTypes();
    if (!ops::find("expression"))
        rigmath::registerCommands();
    run("newScene");
    Graph& g = Graph::current();
    // The plane's origin sits 10 cm right of the mirror plane.
    std::string body = run("imagePlane", {{"fileName", "a.png"}, {"fit", "toSize"}, {"width", 200.0}, {"height", 200.0}, {"maintainRatio", false}}).get<std::string>();
    run("xform", {{"args", {body}}, {"worldSpace", true}, {"translation", {10.0, 0.0, 0.0}}});
    json shoulder = run("imagePlaneCut", {{"points", {{40.0, 50.0, 0.0}, {40.0, 30.0, 0.0}}}, {"symmetry", "x"}, {"aspect", 1.0}});
    REQUIRE(shoulder.size() == 2);
    const std::string arm = shoulder[0].get<std::string>(), other = shoulder[1].get<std::string>();
    run("setPivot", {{"args", {arm}}, {"position", {45.0, 38.0, 0.0}}});
    g.evaluate();
    const Node* o = g.find(other);
    const glm::dvec3 q(o->as<glm::dmat4>(Transform::kWorldMatrix) * glm::dvec4(o->as<glm::dvec3>(Transform::kRotatePivot), 1.0));
    CHECK(q.x == doctest::Approx(-45.0));
    CHECK(q.y == doctest::Approx(38.0));
}

TEST_CASE("imagePlaneGuide")
{
    run("newScene");
    Graph& g = Graph::current();
    std::string body = run("imagePlane", {{"fileName", "a.png"}, {"fit", "toSize"}, {"width", 200.0}, {"height", 200.0}, {"maintainRatio", false}}).get<std::string>();
    CHECK(run("imagePlaneGuide", {{"args", {body}}, {"lines", {{15.0, 47.0, 50.0, 12.0}, {-15.0, 47.0, -50.0, 12.0}}}}) == 2);
    REQUIRE(ip::guides(*g.find(body)).size() == 2);
    CHECK(ip::guides(*g.find(body))[1] == glm::dvec4(-15.0, 47.0, -50.0, 12.0));
    CHECK(run("imagePlaneGuide", {{"args", {body}}, {"remove", 0}}) == 1);
    CHECK(ip::guides(*g.find(body))[0].x == -15.0);
    CHECK_THROWS_AS(run("imagePlaneGuide", {{"args", {body}}, {"remove", 4}}), CommandError);

    // Guides stay on the plane the pieces come from.
    json piece = run("imagePlaneCut", {{"points", {{40.0, 50.0, 0.0}, {40.0, 30.0, 0.0}}}, {"aspect", 1.0}});
    CHECK(ip::guides(*g.find(piece[0].get<std::string>())).empty());
    CHECK(ip::guides(*g.find(body)).size() == 1);

    CHECK(run("imagePlaneGuide", {{"args", {body}}, {"clear", true}}) == 0);
    run("undo");
    CHECK(ip::guides(*g.find(body)).size() == 1);
}
