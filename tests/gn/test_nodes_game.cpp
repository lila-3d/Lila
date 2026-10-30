#include <doctest/doctest.h>

#include "gn/Exec.h"
#include "gn/Gn.h"
#include "geo/Mesh.h"

#include <filesystem>
#include <fstream>

using namespace lila;
using json = nlohmann::json;

namespace game_tests
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

size_t triangles(const geo::Mesh& m)
{
    size_t n = 0;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
            n += m.faceSize(f) - 2;
    return n;
}

bool uvsInUnitSquare(const geo::Mesh& m, std::string_view set)
{
    auto uv = m.cornerAttrs().get<glm::vec2>(set);
    if (uv.empty())
        return false;
    for (geo::Index c = 0; c < uv.size(); ++c)
        if (m.cornerValid(c) && (uv[c].x < -1e-4f || uv[c].y < -1e-4f || uv[c].x > 1.0001f || uv[c].y > 1.0001f))
            return false;
    return true;
}

std::string writeTemp(const std::string& name, const std::string& text)
{
    auto dir = std::filesystem::temp_directory_path() / "lila_gn_tests";
    std::filesystem::create_directories(dir);
    auto path = dir / name;
    std::ofstream(path, std::ios::binary) << text;
    return path.string();
}

/** Two 100 cubes, the second 300 along X. */
std::vector<N> twoCubes()
{
    return {N{"C", "MeshCube", {}, {}}, N{"Tr", "TransformGeometry", {}, {{"translation", json::array({300, 0, 0})}}}, N{"J", "JoinGeometry", {}, {}}};
}
std::vector<L> twoCubeLinks(const std::string& to, const std::string& socket)
{
    return {L{"C", "mesh", "Tr", "geometry"}, L{"C", "mesh", "J", "geometry"}, L{"Tr", "geometry", "J", "geometry"}, L{"J", "geometry", to, socket}};
}

TEST_CASE("lod chain")
{
    // LOD1 keeps about half of the sphere's triangles.
    auto lods = evalNodes({N{"S", "MeshUVSphere", {}, {}}, N{"L", "LodChain", {}, {{"levels", 3}}}},
                          {L{"S", "mesh", "L", "mesh"}, L{"L", "lods", "GroupOutput", "geometry"}});
    REQUIRE(lods->instances);
    CHECK(lods->instances->size() == 4);
    CHECK(lods->instances->attrs.has("lod"));
    CHECK(lods->instances->attrs.has("screenSize"));
    const size_t lod0 = triangles(*lods->instances->references[0]->mesh);
    CHECK(lod0 > 500);

    auto lod1 = evalNodes({N{"S", "MeshUVSphere", {}, {}}, N{"L", "LodChain", {}, {{"level", 1}}}},
                          {L{"S", "mesh", "L", "mesh"}, L{"L", "mesh", "GroupOutput", "geometry"}});
    REQUIRE(lod1->mesh);
    CHECK(triangles(*lod1->mesh) > 0);
    CHECK(double(triangles(*lod1->mesh)) < 0.6 * double(lod0));
}

TEST_CASE("collision")
{
    auto box = evalNodes({N{"C", "MeshCube", {}, {}}, N{"K", "Collision", {}, {{"shape", "box"}}}},
                         {L{"C", "mesh", "K", "mesh"}, L{"K", "collision", "GroupOutput", "geometry"}});
    REQUIRE(box->mesh);
    CHECK(box->mesh->numFaces() == 6);
    CHECK(box->mesh->bounds().max.x == doctest::Approx(50.0));
    auto shape = box->mesh->faceAttrs().get<int32_t>("collisionShape");
    REQUIRE(!shape.empty());
    CHECK(shape[0] == 1);

    auto hull = evalNodes({N{"C", "MeshCube", {}, {}}, N{"K", "Collision", {}, {}}},
                          {L{"C", "mesh", "K", "mesh"}, L{"K", "collision", "GroupOutput", "geometry"}});
    CHECK(hull->mesh->numPoints() == 8);
    CHECK(hull->mesh->faceAttrs().get<int32_t>("collisionShape")[0] == 0);

    // One hull per island.
    auto nodes = twoCubes();
    nodes.push_back(N{"K", "Collision", {}, {}});
    auto links = twoCubeLinks("K", "mesh");
    links.push_back(L{"K", "collision", "GroupOutput", "geometry"});
    auto two = evalNodes(nodes, links);
    CHECK(two->mesh->numPoints() == 16);
    auto piece = two->mesh->faceAttrs().get<int32_t>("collision");
    REQUIRE(!piece.empty());
    CHECK(*std::max_element(piece.begin(), piece.end()) == 1);
    CHECK(two->mesh->bounds().max.x == doctest::Approx(350.0));

    auto sphere = evalNodes({N{"C", "MeshCube", {}, {}}, N{"K", "Collision", {}, {{"shape", "sphere"}}}},
                            {L{"C", "mesh", "K", "mesh"}, L{"K", "collision", "GroupOutput", "geometry"}});
    CHECK(sphere->mesh->faceAttrs().get<int32_t>("collisionShape")[0] == 2);
    // The cube's corners are sqrt(3) * 50 from its centre.
    CHECK(sphere->mesh->bounds().max.y == doctest::Approx(86.6).epsilon(0.01));
}

TEST_CASE("walkable surface")
{
    auto top = evalNodes({N{"C", "MeshCube", {}, {}}, N{"W", "WalkableSurface", {}, {{"mode", "separate"}}}},
                         {L{"C", "mesh", "W", "mesh"}, L{"W", "mesh", "GroupOutput", "geometry"}});
    CHECK(top->mesh->numFaces() == 1);

    // A 150 step height takes in the cube's sides beside its top, never the bottom.
    auto steps = evalNodes({N{"C", "MeshCube", {}, {}}, N{"W", "WalkableSurface", {}, {{"mode", "separate"}, {"stepHeight", 150.0}}}},
                           {L{"C", "mesh", "W", "mesh"}, L{"W", "mesh", "GroupOutput", "geometry"}});
    CHECK(steps->mesh->numFaces() == 5);

    auto open = evalNodes({N{"G", "MeshGrid", {}, {}}, N{"W", "WalkableSurface", {}, {{"mode", "separate"}}}},
                          {L{"G", "mesh", "W", "mesh"}, L{"W", "mesh", "GroupOutput", "geometry"}});
    CHECK(open->mesh->numFaces() == 4);

    // A cube hanging 50 above the grid blocks every grid face; only its own top stays.
    auto covered = evalNodes({N{"G", "MeshGrid", {}, {}}, N{"C", "MeshCube", {}, {}}, N{"Tr", "TransformGeometry", {}, {{"translation", json::array({0, 100, 0})}}},
                              N{"J", "JoinGeometry", {}, {}}, N{"W", "WalkableSurface", {}, {{"mode", "separate"}}}},
                             {L{"C", "mesh", "Tr", "geometry"}, L{"G", "mesh", "J", "geometry"}, L{"Tr", "geometry", "J", "geometry"}, L{"J", "geometry", "W", "mesh"},
                              L{"W", "mesh", "GroupOutput", "geometry"}});
    REQUIRE(covered->mesh->numFaces() == 1);
    CHECK(covered->mesh->bounds().min.y == doctest::Approx(150.0));
}

TEST_CASE("materials")
{
    auto two = evalNodes({N{"C", "MeshCube", {}, {}}, N{"A", "SetMaterial", {}, {{"material", "A"}}}, N{"B", "SetMaterial", {}, {{"material", "B"}}},
                          N{"Nm", "Normal", {}, {}}, N{"X", "SeparateXYZ", {}, {}}, N{"Cmp", "Compare", {}, {{"b", 0.5}}}},
                         {L{"C", "mesh", "A", "geometry"}, L{"A", "geometry", "B", "geometry"}, L{"Nm", "value", "X", "vector"}, L{"X", "y", "Cmp", "a"},
                          L{"Cmp", "result", "B", "selection"}, L{"B", "geometry", "GroupOutput", "geometry"}});
    REQUIRE(two->mesh);
    CHECK(two->mesh->materialNames() == std::vector<std::string>{"A", "B"});

    auto onlyB = evalNodes({N{"C", "MeshCube", {}, {}}, N{"A", "SetMaterial", {}, {{"material", "A"}}}, N{"B", "SetMaterial", {}, {{"material", "B"}}},
                            N{"Nm", "Normal", {}, {}}, N{"X", "SeparateXYZ", {}, {}}, N{"Cmp", "Compare", {}, {{"b", 0.5}}},
                            N{"MS", "MaterialSelection", {}, {{"material", "B"}}}, N{"S", "SeparateGeometry", {}, {{"domain", "face"}}}},
                           {L{"C", "mesh", "A", "geometry"}, L{"A", "geometry", "B", "geometry"}, L{"Nm", "value", "X", "vector"}, L{"X", "y", "Cmp", "a"},
                            L{"Cmp", "result", "B", "selection"}, L{"B", "geometry", "S", "geometry"}, L{"MS", "selection", "S", "selection"},
                            L{"S", "selection", "GroupOutput", "geometry"}});
    CHECK(onlyB->mesh->numFaces() == 1);

    // Assigning to some faces of a mesh without slots keeps the rest on an empty default slot.
    auto partial = evalNodes({N{"C", "MeshCube", {}, {}}, N{"B", "SetMaterial", {}, {{"material", "B"}}}, N{"Nm", "Normal", {}, {}},
                              N{"X", "SeparateXYZ", {}, {}}, N{"Cmp", "Compare", {}, {{"b", 0.5}}}, N{"MI", "MaterialIndex", {}, {}},
                              N{"St", "StoreNamedAttribute", {}, {{"dataType", "int"}, {"domain", "face"}, {"name", "slot"}}}},
                             {L{"C", "mesh", "B", "geometry"}, L{"Nm", "value", "X", "vector"}, L{"X", "y", "Cmp", "a"}, L{"Cmp", "result", "B", "selection"},
                              L{"B", "geometry", "St", "geometry"}, L{"MI", "materialIndex", "St", "value"}, L{"St", "geometry", "GroupOutput", "geometry"}});
    CHECK(partial->mesh->materialNames() == std::vector<std::string>{"", "B"});
    auto slot = partial->mesh->faceAttrs().get<int32_t>("slot");
    REQUIRE(slot.size() == 6);
    CHECK(std::count(slot.begin(), slot.end(), 1) == 1);
}

TEST_CASE("snap to grid")
{
    auto snapped = evalNodes({N{"C", "MeshCube", {}, {{"size", json::array({110, 110, 110})}}}, N{"S", "SnapToGrid", {}, {}}},
                             {L{"C", "mesh", "S", "geometry"}, L{"S", "geometry", "GroupOutput", "geometry"}});
    CHECK(snapped->mesh->bounds().max.x == doctest::Approx(50.0));
    CHECK(snapped->mesh->bounds().min.z == doctest::Approx(-50.0));
}

TEST_CASE("uv nodes")
{
    auto unwrapped = evalNodes({N{"C", "MeshCube", {}, {}}, N{"U", "UVUnwrap", {}, {{"seams", "auto"}}}},
                               {L{"C", "mesh", "U", "mesh"}, L{"U", "mesh", "GroupOutput", "geometry"}});
    CHECK(uvsInUnitSquare(*unwrapped->mesh, "uv0"));

    auto lightmap = evalNodes({N{"C", "MeshCube", {}, {}}, N{"Lm", "LightmapUV", {}, {}}},
                              {L{"C", "mesh", "Lm", "mesh"}, L{"Lm", "mesh", "GroupOutput", "geometry"}});
    CHECK(uvsInUnitSquare(*lightmap->mesh, "uv1"));

    auto packed = evalNodes({N{"C", "MeshCube", {}, {}}, N{"P", "PackUVIslands", {}, {}}},
                            {L{"C", "mesh", "P", "mesh"}, L{"P", "mesh", "GroupOutput", "geometry"}});
    CHECK(uvsInUnitSquare(*packed->mesh, "uv0"));

    auto density = evalNodes({N{"C", "MeshCube", {}, {}}, N{"T", "TexelDensity", {}, {{"density", 2.0}}}},
                             {L{"C", "mesh", "T", "mesh"}, L{"T", "mesh", "GroupOutput", "geometry"}});
    auto measured = density->mesh->faceAttrs().get<float>("__T_density");
    REQUIRE(measured.size() == 6);
    for (float d : measured)
        CHECK(d == doctest::Approx(2.0).epsilon(0.02));
}

TEST_CASE("pivot painter")
{
    auto nodes = twoCubes();
    nodes.push_back(N{"P", "PivotPainter", {}, {}});
    auto links = twoCubeLinks("P", "mesh");
    links.push_back(L{"P", "mesh", "GroupOutput", "geometry"});
    auto painted = evalNodes(nodes, links);
    const geo::Mesh& m = *painted->mesh;
    auto uvA = m.cornerAttrs().get<glm::vec2>("uv2");
    auto uvB = m.cornerAttrs().get<glm::vec2>("uv3");
    REQUIRE(!uvA.empty());
    REQUIRE(!uvB.empty());
    // Bounds-bottom pivots (0, -50, 0) and (300, -50, 0) in Unreal's axes are (0, 0, -50) and (300, 0, -50).
    bool first = false, second = false;
    for (geo::Index c = 0; c < m.cornerCapacity(); ++c)
    {
        if (!m.cornerValid(c))
            continue;
        CHECK(uvA[c].y == doctest::Approx(0.0));
        CHECK(uvB[c].x == doctest::Approx(-50.0));
        first = first || (uvA[c].x == doctest::Approx(0.0) && uvB[c].y == doctest::Approx(0.0));
        second = second || (uvA[c].x == doctest::Approx(300.0) && uvB[c].y == doctest::Approx(1.0));
    }
    CHECK(first);
    CHECK(second);
    CHECK(m.cornerAttrs().has("color:pivotAxis"));
}

TEST_CASE("sockets and occluder")
{
    auto sockets = evalNodes({N{"C", "MeshCube", {}, {}}, N{"S", "SocketPoints", {}, {}}},
                             {L{"C", "mesh", "S", "geometry"}, L{"S", "sockets", "GroupOutput", "geometry"}});
    REQUIRE(sockets->points);
    CHECK(sockets->points->numPoints() == 6);
    CHECK(sockets->points->pointAttrs().has("socket"));

    auto occ = evalNodes({N{"S", "MeshUVSphere", {}, {}}, N{"O", "OccluderMesh", {}, {}}},
                         {L{"S", "mesh", "O", "mesh"}, L{"O", "mesh", "GroupOutput", "geometry"}});
    REQUIRE(occ->mesh);
    CHECK(occ->mesh->numFaces() > 0);
    CHECK(triangles(*occ->mesh) < 400);
    CHECK(occ->mesh->bounds().max.x < 50.0);
}

TEST_CASE("data nodes")
{
    std::string csv = writeTemp("table.csv", "x,y,z,name,count,weight,dir_x,dir_y,dir_z\n"
                                             "1,2,3,oak,4,0.5,0,1,0\n"
                                             "4,5,6,\"pine, tall\",7,1.5,1,0,0\n"
                                             "7,8,9,oak,1,2,0,0,1\n");
    auto rows = evalNodes({N{"R", "ReadDataTable", {}, {{"path", csv}}}}, {L{"R", "points", "GroupOutput", "geometry"}});
    REQUIRE(rows->points);
    const geo::Mesh& p = *rows->points;
    REQUIRE(p.numPoints() == 3);
    CHECK(p.position(1).x == doctest::Approx(4.0));
    CHECK(p.position(2).z == doctest::Approx(9.0));
    auto name = p.pointAttrs().get<int32_t>("name");
    REQUIRE(name.size() == 3);
    CHECK(name[0] == 0);
    CHECK(name[1] == 1);
    CHECK(name[2] == 0);
    CHECK(p.pointAttrs().get<int32_t>("count")[1] == 7);
    CHECK(p.pointAttrs().get<float>("weight")[1] == doctest::Approx(1.5));
    auto dir = p.pointAttrs().get<glm::vec3>("dir");
    REQUIRE(dir.size() == 3);
    CHECK(dir[1].x == doctest::Approx(1.0));

    std::string js = writeTemp("table.json", R"([{"position": [1, 2, 3], "hp": 10, "alive": true}, {"position": [4, 5, 6], "hp": 2.5, "alive": false}])");
    auto jrows = evalNodes({N{"R", "ReadDataTable", {}, {{"path", js}}}}, {L{"R", "points", "GroupOutput", "geometry"}});
    REQUIRE(jrows->points);
    CHECK(jrows->points->numPoints() == 2);
    CHECK(jrows->points->position(1).y == doctest::Approx(5.0));
    CHECK(jrows->points->pointAttrs().get<float>("hp")[1] == doctest::Approx(2.5));
    CHECK(jrows->points->pointAttrs().get<bool>("alive")[0]);

    std::string obj = writeTemp("quad.obj", "v 0 0 0\nv 1 0 0\nv 1 0 1\nv 0 0 1\nf 1 2 3 4\n");
    auto quad = evalNodes({N{"I", "ImportMesh", {}, {{"path", obj}, {"scale", 100.0}}}}, {L{"I", "geometry", "GroupOutput", "geometry"}});
    REQUIRE(quad->mesh);
    CHECK(quad->mesh->numFaces() == 1);
    CHECK(quad->mesh->bounds().max.x == doctest::Approx(100.0));
}
} // namespace game_tests
