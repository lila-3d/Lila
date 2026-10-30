#include <doctest/doctest.h>

#include "geo/Mesh.h"
#include "gn/Exec.h"
#include "gn/Gn.h"

using namespace lila;
using json = nlohmann::json;

namespace meshops_tests
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

/** Source node `src` (output `srcOut`) into op node `op` (input `opIn`, output `opOut`), into the group output. */
gn::GeometryPtr chain(gn::GNode src, const char* srcOut, gn::GNode op, const char* opIn, const char* opOut)
{
    std::string s = src.name, o = op.name;
    return evalNodes({std::move(src), std::move(op)}, {gn::GLink{s, srcOut, o, opIn}, gn::GLink{o, opOut, "GroupOutput", "geometry"}});
}

gn::GNode node(const char* name, const char* type, json values = json::object()) { return gn::GNode{name, type, {}, std::move(values)}; }

gn::GeometryPtr onCube(gn::GNode op, const char* opIn = "mesh", const char* opOut = "mesh") { return chain(node("C", "MeshCube"), "mesh", std::move(op), opIn, opOut); }

TEST_CASE("mesh op nodes: topology")
{
    // Every edge of a cube bevelled once: 6 shrunk faces, 12 edge strips, 8 corner triangles.
    auto bevel = onCube(node("B", "Bevel"));
    CHECK(bevel->mesh->numFaces() == 26);

    auto inset = onCube(node("I", "InsetFaces", {{"individual", true}}));
    CHECK(inset->mesh->numFaces() == 30);

    // A 2x2 grid solidified inward: both shells and a quad on each of the 8 border edges.
    auto solid = chain(node("G", "MeshGrid"), "mesh", node("S", "Solidify"), "mesh", "mesh");
    CHECK(solid->mesh->numFaces() == 16);
    CHECK(solid->mesh->numPoints() == 18);
    CHECK(solid->mesh->bounds().min.y == doctest::Approx(-5.0));
    CHECK(solid->mesh->bounds().max.y == doctest::Approx(0.0));

    auto sub = onCube(node("S", "SubdivideMesh"));
    CHECK(sub->mesh->numFaces() == 24);
    CHECK(sub->mesh->numPoints() == 26);
    CHECK(sub->mesh->bounds().max.x == doctest::Approx(50.0));

    auto dual = onCube(node("D", "DualMesh"));
    CHECK(dual->mesh->numPoints() == 6);
    CHECK(dual->mesh->numFaces() == 8);

    auto split = onCube(node("S", "SplitEdges"));
    CHECK(split->mesh->numFaces() == 6);
    CHECK(split->mesh->numPoints() == 24);

    // Keep the +X half of the default 3x3 grid and mirror it back, welded on the plane.
    auto mirror = chain(node("G", "MeshGrid"), "mesh", node("M", "MirrorMesh", {{"direction", "positive"}}), "mesh", "mesh");
    CHECK(mirror->mesh->numFaces() == 4);
    CHECK(mirror->mesh->numPoints() == 9);

    // An open 8-sided tube: its two end loops bridged, or both filled.
    auto tube = [] { return node("Cy", "MeshCylinder", {{"vertices", 8}, {"fillType", "none"}}); };
    auto bridged = chain(tube(), "mesh", node("B", "BridgeEdgeLoops"), "mesh", "mesh");
    CHECK(bridged->mesh->numFaces() == 16);
    auto filled = chain(tube(), "mesh", node("F", "GridFill"), "mesh", "mesh");
    CHECK(filled->mesh->numFaces() > 8);

    auto grid = node("G", "MeshGrid", {{"verticesX", 11}, {"verticesY", 11}});
    auto reduced = chain(grid, "mesh", node("D", "Decimate", {{"ratio", 0.25}}), "mesh", "mesh");
    CHECK(reduced->mesh->numFaces() > 0);
    CHECK(reduced->mesh->numFaces() < 200);

    auto voxel = onCube(node("V", "VoxelRemesh", {{"resolution", 16}}));
    CHECK(voxel->mesh->numFaces() > 6);
    CHECK(voxel->mesh->bounds().max.x == doctest::Approx(50.0).epsilon(0.1));
}

TEST_CASE("mesh op nodes: shaping")
{
    // All six faces form one group centred on the origin: the whole cube halves.
    auto scaled = onCube(node("S", "ScaleElements", {{"scale", 0.5}}));
    CHECK(scaled->mesh->bounds().max.x == doctest::Approx(25.0));
    CHECK(scaled->mesh->bounds().min.y == doctest::Approx(-25.0));

    auto smoothed = onCube(node("S", "SmoothMesh", {{"mode", "laplacian"}, {"iterations", 3}}));
    CHECK(smoothed->mesh->bounds().max.x < 49.0);

    // Cube corners pulled onto a sphere of radius 30.
    auto wrapped = evalNodes({node("C", "MeshCube"), node("Sp", "MeshUVSphere", {{"radius", 30.0}}), node("W", "Shrinkwrap")},
                             {gn::GLink{"C", "mesh", "W", "mesh"}, gn::GLink{"Sp", "mesh", "W", "target"}, gn::GLink{"W", "mesh", "GroupOutput", "geometry"}});
    for (geo::Index p = 0; p < wrapped->mesh->pointCapacity(); ++p)
    {
        double r = glm::length(wrapped->mesh->position(p));
        CHECK(r > 28.0);
        CHECK(r < 30.01);
    }

    // Taper along Y: the top of the cube at half scale, the bottom untouched.
    auto tapered = chain(node("C", "MeshCube"), "mesh", node("T", "Taper"), "geometry", "geometry");
    for (geo::Index p = 0; p < tapered->mesh->pointCapacity(); ++p)
    {
        const glm::dvec3& at = tapered->mesh->position(p);
        CHECK(std::abs(at.x) == doctest::Approx(at.y > 0.0 ? 25.0 : 50.0));
    }

    // A quarter turn at the top only: the top corners swap x and z magnitudes, which are equal on a cube.
    auto twisted = chain(node("C", "MeshCube"), "mesh", node("T", "Twist"), "geometry", "geometry");
    CHECK(twisted->mesh->bounds().max.y == doctest::Approx(50.0));
    CHECK(twisted->mesh->bounds().max.x == doctest::Approx(50.0));

    auto bent = chain(node("C", "MeshCube"), "mesh", node("B", "Bend", {{"angle", 0.0}}), "geometry", "geometry");
    CHECK(bent->mesh->bounds().max.x == doctest::Approx(50.0));
}

TEST_CASE("mesh op nodes: hulls and bounds")
{
    auto hull = chain(node("C", "MeshCube"), "mesh", node("H", "ConvexHull"), "geometry", "convexHull");
    REQUIRE(hull->mesh);
    CHECK(hull->mesh->numPoints() == 8);
    CHECK(hull->mesh->bounds().max.z == doctest::Approx(50.0));

    for (const char* shape : {"box", "orientedBox"})
    {
        auto box = chain(node("C", "MeshCube"), "mesh", node("B", "BoundingShape", {{"shape", shape}}), "geometry", "mesh");
        REQUIRE(box->mesh);
        CHECK(box->mesh->numFaces() == 6);
        CHECK(box->mesh->bounds().max.x == doctest::Approx(50.0));
        CHECK(box->mesh->bounds().min.y == doctest::Approx(-50.0));
    }

    auto sphere = chain(node("C", "MeshCube"), "mesh", node("B", "BoundingShape", {{"shape", "sphere"}}), "geometry", "mesh");
    CHECK(sphere->mesh->bounds().max.y == doctest::Approx(50.0 * std::sqrt(3.0)).epsilon(0.01));
}
} // namespace meshops_tests
