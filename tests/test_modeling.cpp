#include <doctest/doctest.h>

#include "ops/Command.h"
#include "geo/Mesh.h"
#include "geo/Primitives.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

using namespace lila;
using json = nlohmann::json;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }
int faces(const std::string& m) { return run("meshInfo", {{"args", {m}}})["faces"].get<int>(); }
} // namespace

TEST_CASE("component selection and modeling ops")
{
    run("newScene");
    std::string cube = run("polyCube")[0];
    run("selectComponents", {{"args", {cube}}, {"faces", {0}}});
    CHECK(run("selectMode") == "face");
    CHECK(run("componentSelection")[cube]["faces"] == json::array({0}));

    run("polyExtrude", {{"thickness", 20}});
    CHECK(faces(cube) == 10);
    // History was baked by the edit; the generator is gone.
    CHECK(run("ls", {{"type", "polyCube"}}).empty());
    run("undo");
    CHECK(faces(cube) == 6);
    run("redo");
    CHECK(faces(cube) == 10);

    run("polyInset", {{"thickness", 5}});
    CHECK(faces(cube) == 14);

    run("convertSelection", {{"args", {"edge"}}});
    auto sel = run("componentSelection");
    CHECK(sel[cube]["edges"].size() == 4); // the inset face's border
    run("polyBevel", {{"offset", 1}});
    CHECK(faces(cube) > 14);

    run("selectComponents", {{"args", {cube}}, {"faces", {0, 1}}});
    run("deleteComponents");
    CHECK(run("meshInfo", {{"args", {cube}}})["faces"].get<int>() > 0);
    run("polyFillHole");

    run("newScene");
    std::string plane = run("polyPlane", {{"subdivisionsX", 4}, {"subdivisionsZ", 1}})[0];
    run("setSymmetry", {{"args", {plane}}, {"axis", "x"}});
    run("selectComponents", {{"args", {plane}}, {"vertices", {0}}});
    json before = run("meshInfo", {{"args", {plane}}});
    run("moveComponents", {{"by", {0, 10, 0}}});
    run("rotateComponents", {{"by", {0, 0, 0}}});
    run("scaleComponents", {{"by", {1, 1, 1}}});
    json after = run("meshInfo", {{"args", {plane}}});
    CHECK(after["boundsMax"][1].get<double>() == doctest::Approx(10));

    run("newScene");
    std::string a = run("polyCube", {{"position", {-100, 0, 0}}})[0];
    std::string b = run("polyCube", {{"position", {100, 0, 0}}})[0];
    std::string c = run("polyCombine", {{"args", {a, b}}, {"name", "Both"}});
    CHECK(faces(c) == 12);
    json parts = run("polySeparate", {{"args", {c}}});
    CHECK(parts.size() == 2);
}

TEST_CASE("duplicate, freeze, center pivot")
{
    run("newScene");
    std::string cube = run("polyCube", {{"name", "Box"}, {"position", {10, 20, 30}}})[0];
    run("addModifier", {{"args", {cube}}, {"kind", "subdivision"}});
    json dup = run("duplicate", {{"args", {cube}}});
    REQUIRE(dup.size() == 1);
    std::string copy = dup[0];
    CHECK(copy != cube);
    CHECK(faces(copy) == faces(cube));
    // The copy has its own live stack: changing the original's generator leaves the copy alone.
    CHECK(run("modifiers", {{"args", {copy}}}).size() == 1);
    run("setAttr", {{"args", {"PolyCube.subdivisionsX", 3}}});
    CHECK(faces(cube) != faces(copy));

    run("rotate", {{"args", {copy}}, {"by", {0, 45, 0}}});
    run("scale", {{"args", {copy}}, {"by", {2, 1, 1}}});
    json before = run("meshData", {{"args", {copy}}, {"worldSpace", true}});
    run("freezeTransforms", {{"args", {copy}}});
    CHECK(run("getAttr", {{"args", {copy + ".ry"}}}).get<double>() == doctest::Approx(0));
    CHECK(run("getAttr", {{"args", {copy + ".sx"}}}).get<double>() == doctest::Approx(1));
    json after = run("meshData", {{"args", {copy}}, {"worldSpace", true}});
    REQUIRE(before["points"].size() == after["points"].size());
    for (size_t i = 0; i < before["points"].size(); ++i)
        for (int k = 0; k < 3; ++k)
            CHECK(after["points"][i][k].get<double>() == doctest::Approx(before["points"][i][k].get<double>()).epsilon(1e-9));

    run("centerPivot", {{"args", {copy}}});
    run("undo");
}

TEST_CASE("bounding mesh: fitted collision named and parented for Unreal")
{
    run("newScene");
    std::string cube = run("polyCube", {{"name", "Crate"}, {"width", 200}, {"height", 40}, {"depth", 60}, {"position", {10, 20, 30}}})[0];
    run("rotate", {{"args", {cube}}, {"by", {0, 30, 10}}});
    auto size = [](const std::string& m) {
        json info = run("meshInfo", {{"args", {m}}});
        std::vector<double> s;
        for (int i = 0; i < 3; ++i)
            s.push_back(info["boundsMax"][i].get<double>() - info["boundsMin"][i].get<double>());
        std::sort(s.begin(), s.end());
        return s;
    };

    json made = run("boundingMesh", {{"args", {cube}}, {"offset", 5}});
    REQUIRE(made.size() == 1);
    std::string box = made[0];
    CHECK(box == "UBX_Crate");
    CHECK(run("listRelatives", {{"args", {box}}, {"parent", true}}) == json::array({cube}));
    std::vector<double> s = size(box);
    CHECK(s[0] == doctest::Approx(50.0));
    CHECK(s[1] == doctest::Approx(70.0));
    CHECK(s[2] == doctest::Approx(210.0));

    // The existing collision child is not wrapped again.
    CHECK(run("boundingMesh", {{"args", {cube}}}) == json::array({"UBX_Crate_01"}));
    CHECK(size("UBX_Crate_01")[2] == doctest::Approx(200.0));

    CHECK(run("boundingMesh", {{"args", {cube}}, {"shape", "hull"}})[0] == "UCX_Crate");
    CHECK(run("boundingMesh", {{"args", {cube}}, {"shape", "sphere"}})[0] == "USP_Crate");
    std::string capsule = run("boundingMesh", {{"args", {cube}}, {"shape", "capsule"}, {"generator", true}})[0];
    CHECK(capsule == "UCP_Crate");
    CHECK(run("ls", {{"type", "polyCapsule"}}).size() == 1);
    CHECK(size(capsule)[2] >= 200.0);

    run("undo");
    CHECK(run("ls", {{"type", "polyCapsule"}}).empty());
}

namespace
{
geo::Mesh meshOf(const std::string& name)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    return *g.find(name)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
}

} // namespace

TEST_CASE("component normals are area weighted and selection aware")
{
    geo::Mesh cube = geo::makeCube({});
    scene::components::Ids all;
    for (geo::Index f = 0; f < cube.faceCapacity(); ++f)
        all.push_back(f);
    auto n = scene::componentNormals(cube, scene::ComponentMode::Face, all);
    for (geo::Index p = 0; p < cube.pointCapacity(); ++p)
    {
        glm::dvec3 corner = glm::sign(cube.position(p)) / std::sqrt(3.0);
        CHECK(glm::length(n[p] - corner) < 1e-9);
    }
    // One selected face: its corners take only that face's normal.
    auto one = scene::componentNormals(cube, scene::ComponentMode::Face, {0});
    glm::dvec3 face = cube.faceNormal(0);
    for (geo::Index p : cube.facePoints(0))
        CHECK(glm::length(one[p] - face) < 1e-9);
}

TEST_CASE("moveComponents normal= offsets each point along its own normal")
{
    run("newScene");
    std::string cube = run("polyCube")[0];
    run("selectComponents", {{"args", {cube}}, {"faces", {0, 1, 2, 3, 4, 5}}});
    run("moveComponents", {{"normal", 10}});
    json info = run("meshInfo", {{"args", {cube}}});
    // Every corner leaves along its diagonal, so all six faces move out by the same amount.
    const double out = 50.0 + 10.0 / std::sqrt(3.0);
    for (int i = 0; i < 3; ++i)
    {
        CHECK(info["boundsMax"][i].get<double>() == doctest::Approx(out));
        CHECK(info["boundsMin"][i].get<double>() == doctest::Approx(-out));
    }
    run("undo");
    CHECK(run("meshInfo", {{"args", {cube}}})["boundsMax"][0].get<double>() == doctest::Approx(50.0));

    // A single face moves out by the full distance and leaves the rest alone.
    run("selectComponents", {{"args", {cube}}, {"faces", {0}}});
    geo::Mesh before = meshOf(cube);
    glm::dvec3 normal = before.faceNormal(0);
    run("moveComponents", {{"normal", 7}});
    geo::Mesh after = meshOf(cube);
    auto facePts = before.facePoints(0);
    for (geo::Index p = 0; p < before.pointCapacity(); ++p)
    {
        bool onFace = std::find(facePts.begin(), facePts.end(), p) != facePts.end();
        glm::dvec3 expect = before.position(p) + (onFace ? normal * 7.0 : glm::dvec3(0.0));
        CHECK(glm::length(after.position(p) - expect) < 1e-9);
    }
    CHECK_THROWS(run("moveComponents"));
    CHECK_THROWS(run("moveComponents", {{"by", {1, 0, 0}}, {"normal", 1}}));
}

TEST_CASE("moveComponents normal= keeps symmetry")
{
    run("newScene");
    std::string cube = run("polyCube", {{"subdivisionsX", 4}, {"subdivisionsY", 2}})[0];
    run("setSymmetry", {{"args", {cube}}, {"axis", "x"}});
    geo::Mesh m = meshOf(cube);
    // The +X half of the top, so the mirror side follows from symmetry alone.
    scene::components::Ids pts;
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.position(p).y > 49.0 && m.position(p).x > 1.0)
            pts.push_back(p);
    run("selectComponents", {{"args", {cube}}, {"vertices", pts}});
    run("moveComponents", {{"normal", 6}});
    geo::Mesh after = meshOf(cube);
    int moved = 0;
    for (geo::Index p = 0; p < after.pointCapacity(); ++p)
    {
        glm::dvec3 at = after.position(p);
        moved += glm::length(at - m.position(p)) > 1e-6;
        double best = 1e30;
        for (geo::Index q = 0; q < after.pointCapacity(); ++q)
            best = std::min(best, glm::length(after.position(q) - glm::dvec3(-at.x, at.y, at.z)));
        CHECK(best < 1e-9);
    }
    CHECK(moved == 2 * int(pts.size()));
}
