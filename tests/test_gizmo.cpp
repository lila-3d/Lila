#include <doctest/doctest.h>

#include "TestUtil.h"

#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/TransformOps.h"
#include "ui/GizmoMath.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <iterator>

using namespace lila::ui;
using doctest::Approx;
using namespace testutil;

TEST_CASE("increment snapping rounds to the nearest step, negatives included")
{
    CHECK(gizmo::snapValue(12.4, 5.0) == 10.0);
    CHECK(gizmo::snapValue(12.6, 5.0) == 15.0);
    CHECK(gizmo::snapValue(-12.6, 5.0) == -15.0);
    CHECK(gizmo::snapValue(-12.4, 5.0) == -10.0);
    CHECK(gizmo::snapValue(0.26, 0.1) == Approx(0.3));
    CHECK(gizmo::snapValue(7.3, 0.0) == 7.3);
    CHECK(gizmo::snapValue(7.3, -1.0) == 7.3);
    // No negative zero in readouts.
    CHECK(std::signbit(gizmo::snapValue(-0.4, 5.0)) == false);
}

TEST_CASE("relative move snap steps from the drag start along the frame's masked axes")
{
    glm::dmat3 world(1.0);
    // An X axis drag: only X snaps, and Y/Z (zero anyway for an axis drag) pass through.
    CHECK(nearTight(gizmo::snapRelative({13.0, 0.0, 0.0}, world, {1, 0, 0}, 10.0), {10.0, 0.0, 0.0}));
    CHECK(nearTight(gizmo::snapRelative({-16.0, 0.0, 0.0}, world, {1, 0, 0}, 10.0), {-20.0, 0.0, 0.0}));
    // A YZ plane drag snaps both plane axes and leaves the isolated one alone.
    CHECK(nearTight(gizmo::snapRelative({0.3, 4.0, -6.0}, world, {0, 1, 1}, 5.0), {0.3, 5.0, -5.0}));
    // A rotated frame snaps along its own axes: 13 along a 45 degree X lands on 10 along it.
    glm::dmat3 frame = glm::dmat3(glm::rotate(glm::dmat4(1.0), glm::radians(45.0), glm::dvec3(0, 0, 1)));
    glm::dvec3 snapped = gizmo::snapRelative(frame[0] * 13.0, frame, {1, 0, 0}, 10.0);
    CHECK(nearTight(snapped, frame[0] * 10.0));
}

TEST_CASE("absolute move snap lands the moved point on the grid")
{
    glm::dmat3 world(1.0);
    glm::dvec3 start(3.0, 7.0, -2.0);
    // Start at x=3, drag 13: 16 rounds to 20 on a 10 grid, so the delta is 17.
    CHECK(nearTight(gizmo::snapAbsolute(start, {13.0, 0.0, 0.0}, world, {1, 0, 0}, 10.0), {17.0, 0.0, 0.0}));
    // Negative side: 3 - 9 = -6 rounds to -10.
    CHECK(nearTight(gizmo::snapAbsolute(start, {-9.0, 0.0, 0.0}, world, {1, 0, 0}, 10.0), {-13.0, 0.0, 0.0}));
    // Unmasked axes keep the start coordinate even though it is off grid.
    glm::dvec3 d = gizmo::snapAbsolute(start, {1.0, 1.0, 1.0}, world, {1, 0, 1}, 5.0);
    CHECK(nearTight(start + d, {5.0, 8.0, 0.0}));
}

TEST_CASE("rotate snap keeps counting past a half turn")
{
    CHECK(gizmo::snapAngle(182.4, 5.0) == 180.0);
    CHECK(gizmo::snapAngle(187.6, 5.0) == 190.0);
    CHECK(gizmo::snapAngle(-187.6, 5.0) == -190.0);
    CHECK(gizmo::snapAngle(367.0, 15.0) == 360.0);
    CHECK(gizmo::snapAngle(-7.4, 15.0) == -0.0);
    CHECK(gizmo::snapAngle(-8.0, 15.0) == -15.0);
}

TEST_CASE("scale snap measures from the start factor of 1")
{
    CHECK(gizmo::snapScale(1.26, 0.25) == Approx(1.25));
    CHECK(gizmo::snapScale(0.6, 0.25) == Approx(0.5));
    CHECK(gizmo::snapScale(1.04, 0.1) == Approx(1.0));
    CHECK(gizmo::snapScale(-0.2, 0.5) == Approx(0.0));
}

TEST_CASE("plane constraint drops the isolated axis")
{
    glm::dvec3 x(1, 0, 0);
    CHECK(nearTight(gizmo::constrainToPlane({3, 4, 5}, x), {0, 4, 5}));
    glm::dvec3 diag = glm::normalize(glm::dvec3(1, 1, 0));
    glm::dvec3 d = gizmo::constrainToPlane({2, 0, 1}, diag);
    CHECK(glm::dot(d, diag) == Approx(0.0));
    CHECK(nearTight(d, {1, -1, 1}));
}

TEST_CASE("a ray hits the drag plane, or reports it parallel")
{
    glm::dvec3 hit;
    // Looking down -Y at the XZ plane (Ctrl+Y on the move gizmo) through the point (2, 10, 3).
    REQUIRE(gizmo::rayPlane({2, 10, 3}, {0, -1, 0}, {0, 0, 0}, {0, 1, 0}, hit));
    CHECK(nearTight(hit, {2, 0, 3}));
    // Oblique ray at a plane offset from the origin.
    REQUIRE(gizmo::rayPlane({0, 0, 0}, glm::normalize(glm::dvec3(1, 1, 0)), {0, 5, 0}, {0, 1, 0}, hit));
    CHECK(nearTight(hit, {5, 5, 0}));
    CHECK_FALSE(gizmo::rayPlane({0, 1, 0}, {1, 0, 0}, {0, 0, 0}, {0, 1, 0}, hit));
}

TEST_CASE("snap defaults and the rotate presets")
{
    CHECK(gizmo::kDefaultMoveStep == 1.0);
    CHECK(gizmo::kDefaultRotateStep == 2.5);
    CHECK(gizmo::kDefaultScaleStep == 0.1);
    CHECK(std::find(std::begin(gizmo::kRotateSteps), std::end(gizmo::kRotateSteps), 2.5) != std::end(gizmo::kRotateSteps));
    CHECK(gizmo::turnDivision(128) == 2.8125);
    CHECK(gizmo::turnDivision(64) == 5.625);
    CHECK(gizmo::turnDivision(32) == 11.25);
    CHECK(gizmo::turnDivision(16) == 22.5);
    // A turn division snaps exactly, so 128 steps make a full turn with no drift.
    CHECK(gizmo::snapAngle(2.9, gizmo::turnDivision(128)) == 2.8125);
    CHECK(gizmo::snapAngle(360.1, gizmo::turnDivision(128)) == 360.0);
    CHECK(gizmo::snapAngle(3.6, 2.5) == 2.5);
    CHECK(gizmo::snapAngle(3.8, 2.5) == 5.0);
    CHECK(gizmo::snapValue(12.4, gizmo::kDefaultMoveStep) == 12.0);
    CHECK(gizmo::snapScale(1.26, gizmo::kDefaultScaleStep) == Approx(1.3));
}

TEST_CASE("Shift+MMB picks the axis whose screen direction matches the drag")
{
    // Screen axes (y down): X right, Y up, Z toward the lower left, as in a default perspective view.
    glm::dvec2 axes[3] = {{100, 0}, {0, -100}, {-60, 50}};
    CHECK(gizmo::axisAlongDrag(axes, {20, 2}) == 0);
    CHECK(gizmo::axisAlongDrag(axes, {-20, -1}) == 0); // either way along it
    CHECK(gizmo::axisAlongDrag(axes, {1, 15}) == 1);
    CHECK(gizmo::axisAlongDrag(axes, {-12, 9}) == 2);
    CHECK(gizmo::axisAlongDrag(axes, {12, -11}) == 2);
    CHECK(gizmo::axisAlongDrag(axes, {0, 0}) == -1);
    // Looking straight down Y: its arrow is a dot and can't be picked even by a vertical drag.
    glm::dvec2 top[3] = {{100, 0}, {0, 0.5}, {0, 100}};
    CHECK(gizmo::axisAlongDrag(top, {0, -30}, 12.0) == 2);
    glm::dvec2 none[3] = {{0, 0}, {0, 0}, {0, 0}};
    CHECK(gizmo::axisAlongDrag(none, {5, 5}, 12.0) == -1);
}

TEST_CASE("Shift+MMB rotate picks the ring a trackball drag would turn about")
{
    glm::dvec3 right(1, 0, 0), up(0, 1, 0);
    glm::dmat3 world(1.0);
    // Sideways drags spin about the camera's up, vertical drags about its right.
    CHECK(gizmo::axisNearest(world, gizmo::trackballAxis({30, 2}, right, up)) == 1);
    CHECK(gizmo::axisNearest(world, gizmo::trackballAxis({-2, 30}, right, up)) == 0);
    // A tilted camera: right is X, up leans toward Z. A sideways drag still picks the world axis nearest its up.
    glm::dvec3 tiltedUp = glm::normalize(glm::dvec3(0, 0.4, 1));
    CHECK(gizmo::axisNearest(world, gizmo::trackballAxis({30, 0}, right, tiltedUp)) == 2);
    // Negative directions pick the same axis.
    CHECK(gizmo::axisNearest(world, glm::dvec3(0, -1, 0.2)) == 1);
}

TEST_CASE("closest point parameter along a line")
{
    // A ray straight down at x=7 crossing the X axis line.
    CHECK(gizmo::lineParam({0, 0, 0}, {1, 0, 0}, {7, 10, 0}, {0, -1, 0}) == Approx(7.0));
    // Parallel: no unique answer.
    CHECK(gizmo::lineParam({0, 0, 0}, {1, 0, 0}, {0, 5, 0}, {1, 0, 0}) == 0.0);
}

TEST_CASE("orienting the pivot turns the manipulator's axes, not the object")
{
    using namespace lila;
    ops::execute("newScene", nlohmann::json::object());
    std::string cube = ops::execute("polyCube", nlohmann::json::object())[0];
    ops::execute("rotate", {{"args", {cube}}, {"by", {0, 90, 0}}});
    scene::Graph& g = scene::Graph::current();
    scene::NodeId id = g.find(cube)->id();
    g.evaluate();
    glm::dmat4 before = g.node(id)->as<glm::dmat4>(scene::Transform::kWorldMatrix);
    glm::dmat3 object = scene::xformops::frame(g, id, scene::Space::Object);
    CHECK(nearTight(scene::xformops::pivotFrame(g, id)[0], object[0]));

    // Turn the pivot 30 degrees about world Y, as the rotate tool does in pivot edit mode.
    glm::dmat3 turn(glm::rotate(glm::dmat4(1.0), glm::radians(30.0), glm::dvec3(0, 1, 0)));
    scene::xformops::setWorldPivotFrame(g, id, turn * object);
    g.evaluate();
    CHECK(nearTight(scene::xformops::pivotFrame(g, id)[0], turn * object[0]));
    CHECK(g.node(id)->as<glm::dvec3>(scene::Transform::kPivotOrient).y == Approx(30.0));
    CHECK(g.node(id)->as<glm::dmat4>(scene::Transform::kWorldMatrix) == before);

    // The same through the command, then reset.
    ops::execute("setPivot", {{"args", {cube}}, {"orient", {0, 0, 45}}});
    CHECK(g.node(id)->as<glm::dvec3>(scene::Transform::kPivotOrient).z == Approx(45.0));
    ops::execute("undo", nlohmann::json::object());
    CHECK(g.node(id)->as<glm::dvec3>(scene::Transform::kPivotOrient).y == Approx(30.0));
    ops::execute("resetPivot", {{"args", {cube}}});
    CHECK(nearTight(g.node(id)->as<glm::dvec3>(scene::Transform::kPivotOrient), glm::dvec3(0.0)));
    CHECK(g.node(id)->as<glm::dmat4>(scene::Transform::kWorldMatrix) == before);
}

TEST_CASE("cursor wrap: moves queued before a warp carry on from the far side instead of jumping back")
{
    using gizmo::CursorWrap;
    CHECK(CursorWrap::target({400.0, 300.0}, 800.0, 600.0) == glm::dvec2(400.0, 300.0));
    CHECK(CursorWrap::target({1.0, 300.0}, 800.0, 600.0) == glm::dvec2(796.0, 300.0));
    CHECK(CursorWrap::target({799.0, 599.0}, 800.0, 600.0) == glm::dvec2(4.0, 4.0));

    // A drag running off the right edge: the cursor warps from x 799 to x 4.
    CursorWrap wrap;
    double virtualX = 799.0, lastX = 799.0;
    auto move = [&](double x) {
        glm::dvec2 p = wrap.map({x, 300.0});
        virtualX += p.x - lastX;
        lastX = p.x;
    };
    wrap.warped({799.0, 300.0}, {4.0, 300.0});
    lastX += 4.0 - 799.0;
    move(801.0); // queued before the warp, still on the old side
    move(803.0);
    CHECK(virtualX == Approx(803.0));
    move(9.0); // the first move after it
    CHECK(virtualX == Approx(804.0));
    move(12.0);
    CHECK(virtualX == Approx(807.0));
    // Once the cursor is past the warp, positions near the old side are real again.
    wrap.reset();
    CHECK(wrap.map({790.0, 300.0}) == glm::dvec2(790.0, 300.0));
}
