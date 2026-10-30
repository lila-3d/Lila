#include <doctest/doctest.h>

#include "gn/Exec.h"
#include "gn/Gn.h"

using namespace lila;
using json = nlohmann::json;

namespace curveops_test
{
gn::GeometryPtr evalNodes(std::vector<gn::GNode> nodes, std::vector<gn::GLink> links)
{
    gn::Group g = gn::Group::makeDefault("T");
    g.links.clear();
    g.nodes.insert(g.nodes.end(), nodes.begin(), nodes.end());
    g.links = std::move(links);
    gn::PlanPtr plan = gn::compile(g);
    REQUIRE_MESSAGE(plan->error.empty(), plan->error);
    return gn::execute(*plan, {});
}

using N = gn::GNode;
using L = gn::GLink;

/** A line from the origin up 100 along Y, resampled to 11 points. */
std::vector<N> line11(std::vector<N> more)
{
    std::vector<N> n = {N{"Ln", "CurveLine", {}, {}}, N{"Rs", "ResampleCurve", {}, {{"count", 11}}}};
    n.insert(n.end(), more.begin(), more.end());
    return n;
}

TEST_CASE("trim, reverse and subdivide curves")
{
    auto trimmed = evalNodes(line11({N{"Tr", "TrimCurve", {}, {{"start", 0.25}, {"end", 0.75}}}}),
                             {L{"Ln", "curve", "Rs", "curve"}, L{"Rs", "curve", "Tr", "curve"}, L{"Tr", "curve", "GroupOutput", "geometry"}});
    REQUIRE(trimmed->curves);
    CHECK(trimmed->curves->numPoints() == 7);
    CHECK(trimmed->curves->positions.front().y == doctest::Approx(25.0));
    CHECK(trimmed->curves->positions.back().y == doctest::Approx(75.0));

    auto byLength = evalNodes({N{"Ln", "CurveLine", {}, {}}, N{"Tr", "TrimCurve", {}, {{"mode", "length"}, {"start", 10.0}, {"end", 35.0}}}},
                              {L{"Ln", "curve", "Tr", "curve"}, L{"Tr", "curve", "GroupOutput", "geometry"}});
    REQUIRE(byLength->curves->numPoints() == 2);
    CHECK(byLength->curves->positions[0].y == doctest::Approx(10.0));
    CHECK(byLength->curves->positions[1].y == doctest::Approx(35.0));

    auto rev = evalNodes({N{"Ln", "CurveLine", {}, {}}, N{"Rv", "ReverseCurve", {}, {}}},
                         {L{"Ln", "curve", "Rv", "curve"}, L{"Rv", "curve", "GroupOutput", "geometry"}});
    CHECK(rev->curves->positions[0].y == doctest::Approx(100.0));
    CHECK(rev->curves->positions[1].y == doctest::Approx(0.0));

    auto sub = evalNodes({N{"Ln", "CurveLine", {}, {}}, N{"Sd", "SubdivideCurve", {}, {{"cuts", 3}}}},
                         {L{"Ln", "curve", "Sd", "curve"}, L{"Sd", "curve", "GroupOutput", "geometry"}});
    REQUIRE(sub->curves->numPoints() == 5);
    CHECK(sub->curves->positions[1].y == doctest::Approx(25.0));

    // A Bezier keeps its kind and shape: the cut lands halfway along the straight segment.
    auto bez = evalNodes({N{"Ln", "CurveLine", {}, {}}, N{"Ty", "SetSplineType", {}, {{"type", "bezier"}}}, N{"Sd", "SubdivideCurve", {}, {}}},
                         {L{"Ln", "curve", "Ty", "curve"}, L{"Ty", "curve", "Sd", "curve"}, L{"Sd", "curve", "GroupOutput", "geometry"}});
    REQUIRE(bez->curves->numPoints() == 3);
    CHECK(bez->curves->positions[1].y == doctest::Approx(50.0));
    CHECK(bez->curves->curveAttrs.get<int32_t>("curve_type")[0] == 1);
}

TEST_CASE("curve primitives")
{
    auto quad = evalNodes({N{"Q", "CurveQuadrilateral", {}, {{"width", 40.0}, {"height", 20.0}}}}, {L{"Q", "curve", "GroupOutput", "geometry"}});
    REQUIRE(quad->curves->numPoints() == 4);
    CHECK(quad->curves->cyclic[0] == 1);
    CHECK(quad->curves->positions[0].x == doctest::Approx(20.0));
    CHECK(quad->curves->positions[0].z == doctest::Approx(-10.0));

    auto star = evalNodes({N{"S", "CurveStar", {}, {{"points", 5}}}}, {L{"S", "curve", "GroupOutput", "geometry"}});
    REQUIRE(star->curves->numPoints() == 10);
    CHECK(star->curves->positions[0].x == doctest::Approx(50.0));

    // Three points on a circle of radius 50 about the origin: the middle sample is the middle point.
    auto arc3 = evalNodes({N{"A", "CurveArc", {}, {{"mode", "points"}, {"resolution", 5}, {"start", json::array({-50, 0, 0})},
                                                   {"middle", json::array({0, 0, -50})}, {"end", json::array({50, 0, 0})}}}},
                          {L{"A", "curve", "GroupOutput", "geometry"}});
    REQUIRE(arc3->curves->numPoints() == 5);
    CHECK(arc3->curves->positions[2].x == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(arc3->curves->positions[2].z == doctest::Approx(-50.0));
    CHECK(arc3->curves->positions[4].x == doctest::Approx(50.0));

    auto half = evalNodes({N{"A", "CurveArc", {}, {{"resolution", 3}, {"radius", 10.0}, {"sweepAngle", 3.14159265358979}}}},
                          {L{"A", "curve", "GroupOutput", "geometry"}});
    REQUIRE(half->curves->numPoints() == 3);
    CHECK(half->curves->positions[1].z == doctest::Approx(-10.0));
    CHECK(half->curves->positions[2].x == doctest::Approx(-10.0));
}

TEST_CASE("sampling and reading curves")
{
    auto pts = evalNodes({N{"Ln", "CurveLine", {}, {}}, N{"Cp", "CurveToPoints", {}, {{"count", 5}}}},
                         {L{"Ln", "curve", "Cp", "curve"}, L{"Cp", "points", "GroupOutput", "geometry"}});
    REQUIRE(pts->points);
    REQUIRE(pts->points->numPoints() == 5);
    CHECK(pts->points->position(1).y == doctest::Approx(25.0));

    auto sampled = evalNodes({N{"Ln", "CurveLine", {}, {}}, N{"Sc", "SampleCurve", {}, {{"factor", 0.5}}}, N{"P", "Points", {}, {}},
                              N{"Sp", "SetPosition", {}, {}}},
                             {L{"Ln", "curve", "Sc", "curves"}, L{"P", "points", "Sp", "geometry"}, L{"Sc", "position", "Sp", "position"},
                              L{"Sp", "geometry", "GroupOutput", "geometry"}});
    REQUIRE(sampled->points);
    CHECK(sampled->points->position(0).y == doctest::Approx(50.0));

    auto ends = evalNodes(line11({N{"Ep", "EndpointSelection", {}, {}}, N{"D", "DeleteGeometry", {}, {}}}),
                          {L{"Ln", "curve", "Rs", "curve"}, L{"Rs", "curve", "D", "geometry"}, L{"Ep", "selection", "D", "selection"},
                           L{"D", "geometry", "GroupOutput", "geometry"}});
    REQUIRE(ends->curves->numPoints() == 9);
    CHECK(ends->curves->positions[0].y == doctest::Approx(10.0));

    auto param = evalNodes(line11({N{"Pa", "CurveParameter", {}, {}}, N{"St", "StoreNamedAttribute", {}, {{"name", "f"}}}}),
                           {L{"Ln", "curve", "Rs", "curve"}, L{"Rs", "curve", "St", "geometry"}, L{"Pa", "factor", "St", "value"},
                            L{"St", "geometry", "GroupOutput", "geometry"}});
    CHECK(param->curves->pointAttrs.get<float>("f")[5] == doctest::Approx(0.5));

    auto len = evalNodes({N{"Ln", "CurveLine", {}, {}}, N{"Sl", "SplineLength", {}, {}}, N{"St", "StoreNamedAttribute", {}, {{"name", "len"}, {"domain", "curve"}}}},
                         {L{"Ln", "curve", "St", "geometry"}, L{"Sl", "length", "St", "value"}, L{"St", "geometry", "GroupOutput", "geometry"}});
    CHECK(len->curves->curveAttrs.get<float>("len")[0] == doctest::Approx(100.0));

    auto tilt = evalNodes({N{"Ln", "CurveLine", {}, {}}, N{"T", "SetCurveTilt", {}, {{"tilt", 0.5}}}},
                          {L{"Ln", "curve", "T", "curve"}, L{"T", "curve", "GroupOutput", "geometry"}});
    CHECK(tilt->curves->pointAttrs.get<float>("tilt")[1] == doctest::Approx(0.5));

    // One guide straight up from the origin; a root 10 to the side grows the same curve there.
    auto grown = evalNodes({N{"Ln", "CurveLine", {}, {}}, N{"P", "Points", {}, {{"position", json::array({10, 0, 0})}}}, N{"I", "InterpolateCurves", {}, {}}},
                           {L{"Ln", "curve", "I", "guideCurves"}, L{"P", "points", "I", "points"}, L{"I", "curves", "GroupOutput", "geometry"}});
    REQUIRE(grown->curves->numCurves() == 1);
    CHECK(grown->curves->positions.back().x == doctest::Approx(10.0));
    CHECK(grown->curves->positions.back().y == doctest::Approx(100.0));
}

TEST_CASE("instance transform nodes")
{
    // Move 10 along X, then turn 90 degrees about world Z: the instance ends up 10 along Y.
    auto moved = evalNodes({N{"C", "MeshCube", {}, {}}, N{"G", "GeometryToInstance", {}, {}},
                            N{"Tr", "TranslateInstances", {}, {{"translation", json::array({10, 0, 0})}, {"local", false}}},
                            N{"Ro", "RotateInstances", {}, {{"rotation", json::array({0, 0, 90})}, {"local", false}}}, N{"Ip", "InstancesToPoints", {}, {}}},
                           {L{"C", "mesh", "G", "geometry"}, L{"G", "instances", "Tr", "instances"}, L{"Tr", "instances", "Ro", "instances"},
                            L{"Ro", "instances", "Ip", "instances"}, L{"Ip", "points", "GroupOutput", "geometry"}});
    REQUIRE(moved->points);
    REQUIRE(moved->points->numPoints() == 1);
    CHECK(moved->points->position(0).x == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(moved->points->position(0).y == doctest::Approx(10.0));

    auto scaled = evalNodes({N{"C", "MeshCube", {}, {}}, N{"G", "GeometryToInstance", {}, {}}, N{"Sc", "ScaleInstances", {}, {{"scale", json::array({2, 2, 2})}}}},
                            {L{"C", "mesh", "G", "geometry"}, L{"G", "instances", "Sc", "instances"}, L{"Sc", "instances", "GroupOutput", "geometry"}});
    REQUIRE(scaled->instances);
    CHECK(glm::length(glm::dvec3(scaled->instances->transforms[0][0])) == doctest::Approx(2.0));

    auto pts = evalNodes({N{"I", "Index", {}, {}}, N{"X", "CombineXYZ", {}, {}}, N{"P", "Points", {}, {{"count", 4}}}},
                         {L{"I", "value", "X", "x"}, L{"X", "vector", "P", "position"}, L{"P", "points", "GroupOutput", "geometry"}});
    REQUIRE(pts->points->numPoints() == 4);
    CHECK(pts->points->position(3).x == doctest::Approx(3.0));
}
} // namespace curveops_test
