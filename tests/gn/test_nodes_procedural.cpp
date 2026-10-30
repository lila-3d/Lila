#include <doctest/doctest.h>

#include "gn/Exec.h"
#include "gn/Gn.h"
#include "geo/Mesh.h"

#include <algorithm>

using namespace lila;
using json = nlohmann::json;

namespace procedural_tests
{
using N = gn::GNode;
using L = gn::GLink;

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

std::vector<glm::dvec3> pointsOf(const geo::Mesh& m)
{
    std::vector<glm::dvec3> out;
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            out.push_back(m.position(p));
    return out;
}

double minSpacing(const std::vector<glm::dvec3>& pts)
{
    double best = 1e30;
    for (size_t a = 0; a < pts.size(); ++a)
        for (size_t b = a + 1; b < pts.size(); ++b)
            best = std::min(best, glm::distance(pts[a], pts[b]));
    return best;
}

TEST_CASE("poisson disk points")
{
    json box = {{"mode", "box"}, {"radius", 100.0}};
    auto a = evalNodes({N{"P", "PoissonDiskPoints", {}, box}}, {L{"P", "points", "GroupOutput", "geometry"}});
    REQUIRE(a->points);
    auto pts = pointsOf(*a->points);
    CHECK(pts.size() > 40);
    CHECK(pts.size() < 140);
    CHECK(minSpacing(pts) >= 100.0 - 1e-9);
    for (const auto& p : pts)
    {
        CHECK(p.y == doctest::Approx(0.0));
        CHECK(std::abs(p.x) <= 500.0);
        CHECK(std::abs(p.z) <= 500.0);
    }

    // Same seed, same points.
    auto again = evalNodes({N{"P", "PoissonDiskPoints", {}, box}}, {L{"P", "points", "GroupOutput", "geometry"}});
    CHECK(pointsOf(*again->points) == pts);

    // An avoid point keeps everything 300 away.
    json avoid = box;
    avoid["avoidRadius"] = 300.0;
    auto kept = evalNodes({N{"A", "MeshLine", {}, {{"count", 1}}}, N{"P", "PoissonDiskPoints", {}, avoid}},
                          {L{"A", "mesh", "P", "avoid"}, L{"P", "points", "GroupOutput", "geometry"}});
    for (const auto& p : pointsOf(*kept->points))
        CHECK(glm::length(p) >= 300.0);

    // Classes: every point has one, and pairs keep the mean of their radii apart.
    json cls = box;
    cls["classes"] = "200, 50";
    auto classed = evalNodes({N{"P", "PoissonDiskPoints", {}, cls}}, {L{"P", "points", "GroupOutput", "geometry"}});
    auto cpts = pointsOf(*classed->points);
    auto classes = classed->points->pointAttrs().get<int32_t>("class");
    auto radii = classed->points->pointAttrs().get<float>("radius");
    REQUIRE(classes.size() == cpts.size());
    REQUIRE(radii.size() == cpts.size());
    bool big = false, small = false;
    for (size_t k = 0; k < cpts.size(); ++k)
    {
        big = big || classes[k] == 0;
        small = small || classes[k] == 1;
        CHECK(radii[k] == doctest::Approx(classes[k] == 0 ? 200.0 : 50.0));
        for (size_t j = k + 1; j < cpts.size(); ++j)
            CHECK(glm::distance(cpts[k], cpts[j]) >= 0.5 * (radii[k] + radii[j]) - 1e-3);
    }
    CHECK(big);
    CHECK(small);

    // On a surface: a flat 1000 cm grid.
    auto surf = evalNodes({N{"G", "MeshGrid", {}, {{"sizeX", 1000.0}, {"sizeY", 1000.0}, {"verticesX", 2}, {"verticesY", 2}}},
                           N{"P", "PoissonDiskPoints", {}, {{"radius", 100.0}}}},
                          {L{"G", "mesh", "P", "mesh"}, L{"P", "points", "GroupOutput", "geometry"}});
    auto spts = pointsOf(*surf->points);
    CHECK(spts.size() > 40);
    CHECK(minSpacing(spts) >= 100.0 - 1e-9);
}

TEST_CASE("wave function collapse")
{
    // Labels force tiles 0 and 1 to alternate along X.
    json labels = {{"sizeX", 6}, {"sizeY", 1}, {"sizeZ", 1}, {"rules", "0: +x=a -x=b; 1: +x=b -x=a"}};
    auto alt = evalNodes({N{"A", "MeshCube", {}, {}}, N{"B", "MeshCube", {}, {}}, N{"W", "WaveFunctionCollapse", {}, labels}},
                         {L{"A", "mesh", "W", "tiles"}, L{"B", "mesh", "W", "tiles"}, L{"W", "instances", "GroupOutput", "geometry"}});
    REQUIRE(alt->instances);
    REQUIRE(alt->instances->size() == 6);
    for (size_t k = 0; k < 6; ++k)
    {
        CHECK(alt->instances->transforms[k][3].x == doctest::Approx(100.0 * double(k)));
        if (k > 0)
            CHECK(alt->instances->reference[k] != alt->instances->reference[k - 1]);
    }

    // A tile that can't sit next to itself has no solution.
    json none = {{"sizeX", 2}, {"sizeY", 1}, {"sizeZ", 1}, {"rules", "0: +x=a -x=b"}, {"attempts", 3}};
    auto failed = evalNodes({N{"A", "MeshCube", {}, {}}, N{"W", "WaveFunctionCollapse", {}, none}},
                            {L{"A", "mesh", "W", "tiles"}, L{"W", "instances", "GroupOutput", "geometry"}});
    CHECK((!failed->instances || failed->instances->size() == 0));

    // Profiles: a cell-sized cube meets only itself, a half-size cube only itself, so the grid is all one kind.
    json prof = {{"adjacency", "profiles"}, {"sizeX", 2}, {"sizeY", 1}, {"sizeZ", 2}, {"seed", 5}};
    auto same = evalNodes({N{"A", "MeshCube", {}, {}}, N{"B", "MeshCube", {}, {{"size", json::array({50, 50, 50})}}},
                           N{"W", "WaveFunctionCollapse", {}, prof}},
                          {L{"A", "mesh", "W", "tiles"}, L{"B", "mesh", "W", "tiles"}, L{"W", "instances", "GroupOutput", "geometry"}});
    REQUIRE(same->instances);
    REQUIRE(same->instances->size() == 4);
    for (size_t k = 1; k < 4; ++k)
        CHECK(same->instances->reference[k] == same->instances->reference[0]);
}

TEST_CASE("split grammar")
{
    auto run = [](const char* rules, json extra, const char* source) {
        json v = extra;
        v["rules"] = rules;
        std::vector<N> nodes = {N{"S", "SplitBox", {}, v}};
        if (std::string(source) == "cube")
            nodes.push_back(N{"M", "MeshCube", {}, {}});
        else
            nodes.push_back(N{"M", "MeshGrid", {}, {{"verticesX", 2}, {"verticesY", 2}}});
        return evalNodes(nodes, {L{"M", "mesh", "S", "mesh"}, L{"S", "mesh", "GroupOutput", "geometry"}});
    };

    // Four sides split into a fixed and a floating bay, plus the roof; the bottom is dropped.
    auto split = run("Lot -> comp(f) { side : Facade | top : Roof }\nFacade -> split(x) { 50 : A | ~1 : B }", {{"scope", "box"}}, "cube");
    REQUIRE(split->mesh);
    CHECK(split->mesh->numFaces() == 9);
    auto rule = split->mesh->faceAttrs().get<int32_t>("rule");
    REQUIRE(rule.size() == 9);
    CHECK(std::count(rule.begin(), rule.end(), 0) == 4);
    CHECK(std::count(rule.begin(), rule.end(), 2) == 1);
    for (geo::Index f = 0; f < split->mesh->faceCapacity(); ++f)
        CHECK(split->mesh->faceArea(f) == doctest::Approx(rule[f] == 2 ? 10000.0 : 5000.0));

    // 100 cm sides repeat into three floors of about 33 cm.
    auto floors = run("Lot -> comp(f) { side : Facade | top : Roof }\nFacade -> repeat(y, 30) Floor", {{"scope", "box"}}, "cube");
    CHECK(floors->mesh->numFaces() == 13);

    // A lot face extruded up 300 cm into four walls and a roof.
    auto mass = run("Lot -> extrude(300) M\nM -> comp(f) { side : W | top : R | bottom : NIL }", json::object(), "grid");
    REQUIRE(mass->mesh);
    CHECK(mass->mesh->numFaces() == 5);
    CHECK(mass->mesh->bounds().max.y == doctest::Approx(300.0));
    CHECK(mass->mesh->bounds().min.y == doctest::Approx(0.0));
    for (geo::Index f = 0; f < mass->mesh->faceCapacity(); ++f)
        CHECK(glm::dot(mass->mesh->faceNormal(f), mass->mesh->faceCentroid(f) - glm::dvec3(0, 150, 0)) > 0.0);
}

TEST_CASE("l-system")
{
    json v = {{"axiom", "F"}, {"rules", "F=F[+F]F"}, {"iterations", 1}, {"angle", 90.0}, {"step", 10.0}};
    auto g = evalNodes({N{"L", "LSystem", {}, v}}, {L{"L", "curve", "GroupOutput", "geometry"}});
    REQUIRE(g->curves);
    CHECK(g->curves->numCurves() == 2);
    CHECK(g->curves->numPoints() == 5);
    double maxY = -1e30, minX = 1e30;
    for (const auto& p : g->curves->positions)
    {
        maxY = std::max(maxY, p.y);
        minX = std::min(minX, p.x);
    }
    CHECK(maxY == doctest::Approx(20.0));
    CHECK(minX == doctest::Approx(-10.0));
    // The branch is thinner than the trunk.
    auto radius = g->curves->pointAttrs.get<float>("radius");
    REQUIRE(radius.size() == 5);
    CHECK(radius[0] < radius[2]);
}

TEST_CASE("space colonization")
{
    // A column of attractors straight above one root grows a single straight trunk.
    auto g = evalNodes({N{"R", "MeshLine", {}, {{"count", 1}}},
                        N{"A", "MeshLine", {}, {{"count", 11}, {"start", json::array({0, 100, 0})}, {"offset", json::array({0, 10, 0})}}},
                        N{"S", "SpaceColonization", {}, {{"step", 10.0}, {"influence", 1000.0}, {"killDistance", 15.0}}}},
                       {L{"R", "mesh", "S", "roots"}, L{"A", "mesh", "S", "attractors"}, L{"S", "curve", "GroupOutput", "geometry"}});
    REQUIRE(g->curves);
    CHECK(g->curves->numCurves() == 1);
    CHECK(g->curves->numPoints() == 20);
    CHECK(g->curves->positions.back().y == doctest::Approx(190.0));
    CHECK(g->curves->positions.back().x == doctest::Approx(0.0));

    // Two attractors off to either side: the trunk stalls between them, then branches out to both.
    auto fork = evalNodes({N{"R", "MeshLine", {}, {{"count", 1}}},
                           N{"A", "MeshLine", {}, {{"count", 2}, {"start", json::array({-100, 100, 0})}, {"offset", json::array({200, 0, 0})}}},
                           N{"S", "SpaceColonization", {}, {{"step", 10.0}, {"influence", 1000.0}, {"killDistance", 15.0}}}},
                          {L{"R", "mesh", "S", "roots"}, L{"A", "mesh", "S", "attractors"}, L{"S", "curve", "GroupOutput", "geometry"}});
    REQUIRE(fork->curves);
    CHECK(fork->curves->numCurves() >= 2);
    double minX = 1e30, maxX = -1e30;
    for (const auto& p : fork->curves->positions)
    {
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
    }
    CHECK(minX < -80.0);
    CHECK(maxX > 80.0);
}

TEST_CASE("spline network")
{
    // Two roads crossing square: four strips ending on a square junction, neighbours sharing its corners.
    auto net = evalNodes({N{"A", "CurveLine", {}, {{"start", json::array({-500, 0, 0})}, {"end", json::array({500, 0, 0})}}},
                          N{"B", "CurveLine", {}, {{"start", json::array({0, 0, -500})}, {"end", json::array({0, 0, 500})}}},
                          N{"J", "JoinGeometry", {}, {}}, N{"S", "SplineNetwork", {}, {{"width", 100.0}}}},
                         {L{"A", "curve", "J", "geometry"}, L{"B", "curve", "J", "geometry"}, L{"J", "geometry", "S", "curves"},
                          L{"S", "mesh", "GroupOutput", "geometry"}});
    REQUIRE(net->mesh);
    CHECK(net->mesh->numFaces() == 5);
    CHECK(net->mesh->numPoints() == 12);
    for (geo::Index f = 0; f < net->mesh->faceCapacity(); ++f)
        CHECK(net->mesh->faceNormal(f).y > 0.99);
    auto junction = net->mesh->faceAttrs().get<bool>("junction");
    REQUIRE(junction.size() == 5);
    CHECK(std::count(junction.begin(), junction.end(), true) == 1);

    auto points = evalNodes({N{"A", "CurveLine", {}, {{"start", json::array({-500, 0, 0})}, {"end", json::array({500, 0, 0})}}},
                             N{"B", "CurveLine", {}, {{"start", json::array({0, 0, -500})}, {"end", json::array({0, 0, 500})}}},
                             N{"J", "JoinGeometry", {}, {}}, N{"S", "SplineNetwork", {}, {{"width", 100.0}}}},
                            {L{"A", "curve", "J", "geometry"}, L{"B", "curve", "J", "geometry"}, L{"J", "geometry", "S", "curves"},
                             L{"S", "junctions", "GroupOutput", "geometry"}});
    REQUIRE(points->points);
    CHECK(points->points->numPoints() == 1);

    // Two lines meeting end to end become one bent strip with no junction.
    auto bend = evalNodes({N{"A", "CurveLine", {}, {{"start", json::array({-500, 0, 0})}, {"end", json::array({0, 0, 0})}}},
                           N{"B", "CurveLine", {}, {{"start", json::array({0, 0, 0})}, {"end", json::array({0, 0, 500})}}},
                           N{"J", "JoinGeometry", {}, {}}, N{"S", "SplineNetwork", {}, {{"width", 100.0}, {"capEnds", true}}}},
                          {L{"A", "curve", "J", "geometry"}, L{"B", "curve", "J", "geometry"}, L{"J", "geometry", "S", "curves"},
                           L{"S", "mesh", "GroupOutput", "geometry"}});
    REQUIRE(bend->mesh);
    // Two quads and two end caps.
    CHECK(bend->mesh->numFaces() == 4);
    auto bendJunction = bend->mesh->faceAttrs().get<bool>("junction");
    CHECK(std::count(bendJunction.begin(), bendJunction.end(), true) == 0);
}
} // namespace procedural_tests
