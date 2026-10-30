#include <doctest/doctest.h>

#include "geo/Mesh.h"
#include "geo/Primitives.h"
#include "gn/Exec.h"
#include "gn/Gn.h"
#include "gn/Volume.h"

using namespace lila;
using json = nlohmann::json;

namespace volume_tests
{
gn::GeometryPtr evalNodes(std::vector<gn::GNode> nodes, std::vector<gn::GLink> links, const gn::ExecEnv& env = {})
{
    gn::Group g = gn::Group::makeDefault("T");
    g.links.clear();
    g.nodes.insert(g.nodes.end(), nodes.begin(), nodes.end());
    g.links = std::move(links);
    gn::PlanPtr plan = gn::compile(g);
    REQUIRE_MESSAGE(plan->error.empty(), plan->error);
    return gn::execute(*plan, env);
}

using N = gn::GNode;
using L = gn::GLink;

TEST_CASE("volume: mesh to sdf and back")
{
    // A 100 cube padded by band + 1 voxels each side: 140 / 5 + 1 voxels per axis, centres from -70.
    auto sdf = evalNodes({N{"C", "MeshCube", {}, {}}, N{"S", "MeshToSDF", {}, {}}}, {L{"C", "mesh", "S", "mesh"}, L{"S", "volume", "GroupOutput", "geometry"}});
    REQUIRE(sdf->volume);
    const gn::Volume& v = *sdf->volume;
    CHECK(v.isSdf());
    CHECK(v.dims == glm::ivec3(29, 29, 29));
    CHECK(v.voxelSize() == doctest::Approx(5.0));
    CHECK(v.bounds().min.x == doctest::Approx(-70.0));
    // Deep inside clamps to minus the band; 2 inside the top face reads -2; far away reads the background.
    CHECK(v.sample({0, 0, 0}) == doctest::Approx(-15.0));
    CHECK(v.sample({0, 0, 48}) == doctest::Approx(-2.0).epsilon(1e-4));
    CHECK(v.sample({0, 0, 200}) == doctest::Approx(15.0));
    CHECK(sdf->empty() == false);
    CHECK(gn::toOutputMesh(*sdf).numPoints() == 0);

    auto mesh = evalNodes({N{"C", "MeshCube", {}, {}}, N{"S", "MeshToSDF", {}, {}}, N{"M", "VolumeToMesh", {}, {{"threshold", 0.0}}}},
                          {L{"C", "mesh", "S", "mesh"}, L{"S", "volume", "M", "volume"}, L{"M", "mesh", "GroupOutput", "geometry"}});
    REQUIRE(mesh->mesh);
    CHECK(!mesh->volume);
    CHECK(mesh->mesh->numFaces() > 0);
    geo::Bounds b = mesh->mesh->bounds();
    CHECK(b.max.x == doctest::Approx(50.0).epsilon(0.01));
    CHECK(b.min.y == doctest::Approx(-50.0).epsilon(0.01));

    // Offset by one voxel grows the box by as much.
    auto grown = evalNodes({N{"C", "MeshCube", {}, {}}, N{"S", "MeshToSDF", {}, {}}, N{"O", "OffsetSDF", {}, {{"distance", 5.0}}},
                            N{"M", "VolumeToMesh", {}, {{"threshold", 0.0}}}},
                           {L{"C", "mesh", "S", "mesh"}, L{"S", "volume", "O", "volume"}, L{"O", "volume", "M", "volume"}, L{"M", "mesh", "GroupOutput", "geometry"}});
    CHECK(grown->mesh->bounds().max.z == doctest::Approx(55.0).epsilon(0.01));
}

TEST_CASE("volume: points to sdf and booleans")
{
    std::vector<glm::dvec3> centre = {glm::dvec3(0.0)};
    std::vector<double> radius = {20.0};
    gn::Volume sphere = gn::pointsToSdf(centre, radius, 2.0, 3);
    CHECK(sphere.sample({0, 0, 0}) == doctest::Approx(-6.0));
    CHECK(sphere.sample({100, 0, 0}) == doctest::Approx(6.0));
    geo::Bounds sb = gn::volumeToMesh(sphere, 0.0).bounds();
    CHECK(sb.max.x == doctest::Approx(20.0).epsilon(0.05));
    CHECK(sb.min.z == doctest::Approx(-20.0).epsilon(0.05));

    // The same sphere twice, 60 apart, unioned onto a grid covering both.
    gn::ExecEnv env;
    env.input = gn::volumeGeometry(sphere);
    auto unioned = evalNodes({N{"T", "TransformGeometry", {}, {{"translation", json::array({60, 0, 0})}}}, N{"B", "SDFBoolean", {}, {}},
                              N{"M", "VolumeToMesh", {}, {{"threshold", 0.0}}}},
                             {L{"GroupInput", "geometry", "T", "geometry"}, L{"GroupInput", "geometry", "B", "a"}, L{"T", "geometry", "B", "b"},
                              L{"B", "volume", "M", "volume"}, L{"M", "mesh", "GroupOutput", "geometry"}},
                             env);
    geo::Bounds ub = unioned->mesh->bounds();
    CHECK(ub.min.x == doctest::Approx(-20.0).epsilon(0.05));
    CHECK(ub.max.x == doctest::Approx(80.0).epsilon(0.05));

    // Cutting a sphere shifted by its radius leaves a lens-shaped bite: the rim, at x = 10, is now the furthest point.
    auto cut = evalNodes({N{"T", "TransformGeometry", {}, {{"translation", json::array({20, 0, 0})}}}, N{"B", "SDFBoolean", {}, {{"operation", "difference"}}},
                          N{"M", "VolumeToMesh", {}, {{"threshold", 0.0}}}},
                         {L{"GroupInput", "geometry", "T", "geometry"}, L{"GroupInput", "geometry", "B", "a"}, L{"T", "geometry", "B", "b"},
                          L{"B", "volume", "M", "volume"}, L{"M", "mesh", "GroupOutput", "geometry"}},
                         env);
    geo::Bounds cb = cut->mesh->bounds();
    CHECK(cb.min.x == doctest::Approx(-20.0).epsilon(0.05));
    CHECK(cb.max.x > 8.0);
    CHECK(cb.max.x < 12.0);

    // Densities can't take part.
    gn::Volume density = sphere;
    density.kind = gn::Volume::Kind::Density;
    gn::ExecEnv denv;
    denv.input = gn::volumeGeometry(density);
    gn::Group g = gn::Group::makeDefault("T");
    g.links.clear();
    g.nodes.push_back(N{"O", "OffsetSDF", {}, {}});
    g.links = {L{"GroupInput", "geometry", "O", "volume"}, L{"O", "volume", "GroupOutput", "geometry"}};
    gn::PlanPtr plan = gn::compile(g);
    CHECK_THROWS(gn::execute(*plan, denv));
}

TEST_CASE("volume: density cube, distribution and info")
{
    // Density 1 inside a 100 box of 32 voxels a side, 0 outside: the 0.1 crossing sits 0.9 of a voxel past the last centre.
    auto box = evalNodes({N{"V", "VolumeCube", {}, {}}, N{"M", "VolumeToMesh", {}, {}}},
                         {L{"V", "volume", "M", "volume"}, L{"M", "mesh", "GroupOutput", "geometry"}});
    CHECK(box->mesh->bounds().max.x == doctest::Approx(50.0 + 0.9 * 100.0 / 31.0));

    auto grid = evalNodes({N{"V", "VolumeCube", {}, {}}, N{"D", "DistributePointsInVolume", {}, {{"mode", "grid"}, {"spacing", json::array({9, 9, 9})}}}},
                          {L{"V", "volume", "D", "volume"}, L{"D", "points", "GroupOutput", "geometry"}});
    REQUIRE(grid->points);
    CHECK(grid->points->numPoints() == 12 * 12 * 12);

    // About 100 points per million cubic units, over (32 * 100 / 31)^3 of voxels.
    auto random = evalNodes({N{"V", "VolumeCube", {}, {}}, N{"D", "DistributePointsInVolume", {}, {{"density", 100.0}}}},
                            {L{"V", "volume", "D", "volume"}, L{"D", "points", "GroupOutput", "geometry"}});
    REQUIRE(random->points);
    CHECK(random->points->numPoints() > 60);
    CHECK(random->points->numPoints() < 180);

    // Density from Position: positive only in the upper half, so the surface stops near y = 0.
    auto half = evalNodes({N{"P", "Position", {}, {}}, N{"X", "SeparateXYZ", {}, {}}, N{"V", "VolumeCube", {}, {}}, N{"M", "VolumeToMesh", {}, {{"threshold", 0.0}}}},
                          {L{"P", "value", "X", "vector"}, L{"X", "y", "V", "density"}, L{"V", "volume", "M", "volume"}, L{"M", "mesh", "GroupOutput", "geometry"}});
    geo::Bounds hb = half->mesh->bounds();
    CHECK(hb.min.y == doctest::Approx(0.0).epsilon(0.01));
    CHECK(hb.max.y > 50.0);
}

TEST_CASE("volume: join and transform")
{
    gn::Volume a = gn::Volume::grid({glm::dvec3(0.0), glm::dvec3(10.0)}, 1.0, gn::Volume::Kind::Sdf, 3.0f);
    gn::Volume b = a;
    a.values[0] = -1.0f;
    b.values[0] = -2.0f;
    b.values[1] = -0.5f;
    gn::GeometryPtr parts[2] = {gn::volumeGeometry(a), gn::volumeGeometry(b)};
    gn::Geometry joined = gn::join(parts);
    REQUIRE(joined.volume);
    CHECK(joined.volume->values[0] == doctest::Approx(-2.0));
    CHECK(joined.volume->values[1] == doctest::Approx(-0.5));
    CHECK(joined.volume->values[2] == doctest::Approx(3.0));

    glm::dmat4 xf(2.0);
    xf[3] = glm::dvec4(5.0, 0.0, 0.0, 1.0);
    gn::Geometry moved = gn::transformed(*parts[0], xf);
    REQUIRE(moved.volume);
    CHECK(moved.volume->voxelSize() == doctest::Approx(2.0));
    CHECK(moved.volume->bounds().min.x == doctest::Approx(5.0));
    CHECK(moved.volume->values[0] == doctest::Approx(-2.0));
    CHECK(moved.volume->background == doctest::Approx(6.0));
}
} // namespace volume_tests
