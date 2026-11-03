#include <doctest/doctest.h>
#include "scene/TransformOps.h"

#include "ops/Command.h"
#include "geo/Mesh.h"
#include "geo/Primitives.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <algorithm>

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
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
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
    std::string plane = run("polyPlane", {{"width", 100.0}, {"depth", 100.0}, {"subdivisionsX", 4}, {"subdivisionsZ", 1}})[0];
    run("setSymmetry", {{"args", {plane}}, {"axis", "x"}});
    run("selectComponents", {{"args", {plane}}, {"vertices", {0}}});
    json before = run("meshInfo", {{"args", {plane}}});
    run("moveComponents", {{"by", {0, 10, 0}}});
    run("rotateComponents", {{"by", {0, 0, 0}}});
    run("scaleComponents", {{"by", {1, 1, 1}}});
    json after = run("meshInfo", {{"args", {plane}}});
    CHECK(after["boundsMax"][1].get<double>() == doctest::Approx(10));

    run("newScene");
    std::string a = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}, {"position", {-100, 0, 0}}})[0];
    std::string b = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}, {"position", {100, 0, 0}}})[0];
    std::string c = run("polyCombine", {{"args", {a, b}}, {"name", "Both"}});
    CHECK(faces(c) == 12);
    json parts = run("polySeparate", {{"args", {c}}});
    CHECK(parts.size() == 2);
}

TEST_CASE("whole-mesh commands take groups")
{
    run("newScene");
    std::string a = run("polyCube", {{"position", {-100, 0, 0}}})[0];
    std::string b = run("polyCube", {{"position", {100, 0, 0}}})[0];
    std::string c = run("polyCube", {{"position", {0, 100, 0}}})[0];
    std::string inner = run("group", {{"args", {b, c}}}).get<std::string>();
    std::string outer = run("group", {{"args", {a, inner}}}).get<std::string>();
    CHECK(faces(run("polyCombine", {{"args", {outer}}}).get<std::string>()) == 18);

    run("newScene");
    a = run("polyCube", {{"position", {-100, 0, 0}}})[0];
    b = run("polyCube", {{"position", {100, 0, 0}}})[0];
    std::string grp = run("group", {{"args", {a, b}}}).get<std::string>();
    std::string empty = run("group", {{"empty", true}}).get<std::string>();
    // A hierarchy selection: the group, its meshes again, and a group with no meshes.
    json selection = run("listRelatives", {{"args", {grp}}});
    selection.push_back(grp);
    selection.push_back(empty);
    run("select", {{"args", selection}});
    CHECK(faces(run("polyCombine").get<std::string>()) == 12);

    CHECK_THROWS(run("polySubdivide", {{"args", {empty}}}));
}

TEST_CASE("duplicate, freeze, center pivot")
{
    run("newScene");
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}, {"name", "Box"}, {"position", {10, 20, 30}}})[0];
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

TEST_CASE("freeze and center pivot on a group reach its children")
{
    run("newScene");
    std::string a = run("polyCube", {{"name", "A"}, {"position", {10, 0, 0}}})[0];
    std::string b = run("polyCube", {{"name", "B"}, {"position", {30, 0, 0}}})[0];
    std::string grp = run("group", {{"args", {a, b}}}).get<std::string>();
    run("move", {{"args", {grp}}, {"by", {0, 5, 0}}});
    run("rotate", {{"args", {a}}, {"by", {0, 30, 0}}});

    run("centerPivot", {{"args", {grp}}});
    json piv = run("pivot", {{"args", {grp}}});
    CHECK(piv[0].get<double>() == doctest::Approx(20).epsilon(0.05));
    CHECK(piv[1].get<double>() == doctest::Approx(5));

    json before = run("meshData", {{"args", {a}}, {"worldSpace", true}});
    run("freezeTransforms", {{"args", {grp}}});
    for (const std::string& n : {grp, a, b})
    {
        CHECK(run("getAttr", {{"args", {n + ".ty"}}}).get<double>() == doctest::Approx(0));
        CHECK(run("getAttr", {{"args", {n + ".tx"}}}).get<double>() == doctest::Approx(0));
    }
    CHECK(run("getAttr", {{"args", {a + ".ry"}}}).get<double>() == doctest::Approx(0));
    json after = run("meshData", {{"args", {a}}, {"worldSpace", true}});
    for (size_t i = 0; i < before["points"].size(); ++i)
        for (int k = 0; k < 3; ++k)
            CHECK(after["points"][i][k].get<double>() == doctest::Approx(before["points"][i][k].get<double>()).epsilon(1e-9));

    run("undo");
    run("freezeTransforms", {{"args", {grp}}, {"hierarchy", false}});
    CHECK(run("getAttr", {{"args", {grp + ".ty"}}}).get<double>() == doctest::Approx(0));
    CHECK(run("getAttr", {{"args", {a + ".ty"}}}).get<double>() == doctest::Approx(5));
}

TEST_CASE("bakePivot turns the object onto its pivot's axes without moving it")
{
    using namespace lila::scene;
    run("newScene");
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
    run("rotate", {{"args", {cube}}, {"by", {0, 20, 0}}});
    run("setPivot", {{"args", {cube}}, {"position", {10, 0, 0}}, {"orient", {0, 0, 30}}});
    Graph& g = Graph::current();
    const NodeId id = g.find(cube)->id();
    const glm::dmat3 axes = xformops::pivotFrame(g, id);
    json before = run("meshData", {{"args", {cube}}, {"worldSpace", true}});

    run("bakePivot", {{"args", {cube}}});
    const glm::dmat3 object = xformops::frame(g, id, Space::Object);
    for (int c = 0; c < 3; ++c)
        CHECK(glm::length(object[c] - axes[c]) < 1e-6);
    CHECK(glm::length(xformops::worldPivot(g, id) - glm::dvec3(10, 0, 0)) < 1e-6);
    CHECK(run("getAttr", {{"args", {cube + ".pivotOrient"}}})[2].get<double>() == doctest::Approx(0));
    json after = run("meshData", {{"args", {cube}}, {"worldSpace", true}});
    REQUIRE(before["points"].size() == after["points"].size());
    for (size_t i = 0; i < before["points"].size(); ++i)
        for (int k = 0; k < 3; ++k)
            CHECK(after["points"][i][k].get<double>() == doctest::Approx(before["points"][i][k].get<double>()).epsilon(1e-9));
}

TEST_CASE("freezing joints moves rotation into jointOrient and keeps translate")
{
    using namespace lila::scene;
    run("newScene");
    std::string root = run("joint", {{"name", "root"}, {"position", {0, 10, 0}}, {"parent", ""}}).get<std::string>();
    std::string tip = run("joint", {{"name", "tip"}, {"position", {0, 30, 0}}, {"parent", root}}).get<std::string>();
    run("setAttr", {{"args", {root + ".rotate", json::array({0, 0, 40})}}});
    run("setAttr", {{"args", {root + ".scale", json::array({2, 2, 2})}}});
    run("setAttr", {{"args", {tip + ".rotate", json::array({15, 0, 0})}}});
    Graph& g = Graph::current();
    g.evaluate();
    const glm::dmat4 rootBefore = g.find(root)->as<glm::dmat4>(Transform::kWorldMatrix);
    const glm::dmat4 tipBefore = g.find(tip)->as<glm::dmat4>(Transform::kWorldMatrix);

    run("freezeTransforms", {{"args", {root}}});
    g.evaluate();
    CHECK(run("getAttr", {{"args", {root + ".ty"}}}).get<double>() == doctest::Approx(10));
    CHECK(run("getAttr", {{"args", {root + ".rz"}}}).get<double>() == doctest::Approx(0));
    CHECK(run("getAttr", {{"args", {root + ".sx"}}}).get<double>() == doctest::Approx(1));
    CHECK(run("getAttr", {{"args", {root + ".jointOrient"}}})[2].get<double>() == doctest::Approx(40));
    CHECK(run("getAttr", {{"args", {tip + ".rx"}}}).get<double>() == doctest::Approx(0));
    const glm::dmat4 rootAfter = g.find(root)->as<glm::dmat4>(Transform::kWorldMatrix);
    const glm::dmat4 tipAfter = g.find(tip)->as<glm::dmat4>(Transform::kWorldMatrix);
    // Scale is gone, so compare positions and normalised axes.
    for (int c = 0; c < 3; ++c)
    {
        CHECK(glm::length(glm::normalize(glm::dvec3(rootAfter[c])) - glm::normalize(glm::dvec3(rootBefore[c]))) < 1e-6);
        CHECK(glm::length(glm::normalize(glm::dvec3(tipAfter[c])) - glm::normalize(glm::dvec3(tipBefore[c]))) < 1e-6);
    }
    CHECK(glm::length(glm::dvec3(rootAfter[3]) - glm::dvec3(rootBefore[3])) < 1e-6);
    CHECK(glm::length(glm::dvec3(tipAfter[3]) - glm::dvec3(tipBefore[3])) < 1e-6);
}

TEST_CASE("freezing joints keeps mirrors, segment scale compensate, and can zero orientation")
{
    using namespace lila::scene;
    run("newScene");
    std::string root = run("joint", {{"name", "root"}, {"position", {0, 10, 0}}, {"parent", ""}}).get<std::string>();
    std::string mid = run("joint", {{"name", "mid"}, {"position", {0, 30, 0}}, {"parent", root}, {"segmentScaleCompensate", true}}).get<std::string>();
    std::string tip = run("joint", {{"name", "tip"}, {"position", {10, 40, 0}}, {"parent", mid}}).get<std::string>();
    run("setAttr", {{"args", {root + ".rotate", json::array({0, 0, 40})}}});
    run("setAttr", {{"args", {root + ".scale", json::array({-1, 2, 1})}}});
    run("setAttr", {{"args", {mid + ".rotate", json::array({20, 10, 0})}}});
    Graph& g = Graph::current();
    auto world = [&](const std::string& n) {
        g.evaluate();
        return g.find(n)->as<glm::dmat4>(Transform::kWorldMatrix);
    };
    const glm::dmat4 midBefore = world(mid), tipBefore = world(tip);

    run("freezeTransforms", {{"args", {root}}, {"scale", false}});
    for (int c = 0; c < 4; ++c)
    {
        CHECK(glm::length(world(mid)[c] - midBefore[c]) < 1e-6);
        CHECK(glm::length(world(tip)[c] - tipBefore[c]) < 1e-6);
    }
    CHECK(run("getAttr", {{"args", {mid + ".rx"}}}).get<double>() == doctest::Approx(0));
    CHECK(run("getAttr", {{"args", {root + ".sx"}}}).get<double>() == doctest::Approx(-1));

    run("undo");
    run("freezeTransforms", {{"args", {root}}});
    CHECK(run("getAttr", {{"args", {root + ".sx"}}}).get<double>() == doctest::Approx(-1));
    CHECK(run("getAttr", {{"args", {root + ".sy"}}}).get<double>() == doctest::Approx(1));
    CHECK(glm::length(glm::dvec3(world(tip)[3]) - glm::dvec3(tipBefore[3])) < 1e-6);

    run("undo");
    run("freezeTransforms", {{"args", {root}}, {"jointOrient", true}});
    for (const std::string& n : {root, mid, tip})
    {
        const glm::dmat4 w = world(n);
        CHECK(glm::length(glm::normalize(glm::dvec3(w[1])) - glm::dvec3(0, 1, 0)) < 1e-6);
        CHECK(glm::length(glm::normalize(glm::dvec3(w[2])) - glm::dvec3(0, 0, 1)) < 1e-6);
    }
    CHECK(glm::length(glm::dvec3(world(tip)[3]) - glm::dvec3(tipBefore[3])) < 1e-6);
}

TEST_CASE("freeze without hierarchy leaves children's values")
{
    run("newScene");
    std::string a = run("polyCube", {{"name", "A"}})[0];
    std::string grp = run("group", {{"args", {a}}}).get<std::string>();
    run("move", {{"args", {grp}}, {"by", {0, 5, 0}}});
    run("freezeTransforms", {{"args", {grp}}, {"hierarchy", false}});
    CHECK(run("getAttr", {{"args", {grp + ".ty"}}}).get<double>() == doctest::Approx(0));
    CHECK(run("getAttr", {{"args", {a + ".ty"}}}).get<double>() == doctest::Approx(5));
}

TEST_CASE("multi-box bounds: one box per chunk, named, parented, undoable")
{
    run("newScene");
    std::string slab = run("polyCube", {{"name", "Slab"}, {"width", 200}, {"height", 20}, {"depth", 40}})[0];
    json preview = run("multiBoxBounds", {{"args", {slab}}, {"preview", true}});
    CHECK(preview["boxCount"] == 1);
    CHECK(preview["boxes"].size() == 1);
    CHECK(run("ls", {{"type", "mesh"}}).size() == 1);

    json made = run("multiBoxBounds", {{"args", {slab}}, {"padding", 1}});
    REQUIRE(made["meshes"].size() == 1);
    CHECK(made["meshes"][0] == "UCX_Slab_01");
    CHECK(made["averageFill"].get<double>() > 0.9);
    CHECK(made["coverage"].get<double>() > 0.9);
    CHECK(run("listRelatives", {{"args", {"UCX_Slab_01"}}, {"parent", true}}) == json::array({slab}));

    run("undo");
    CHECK(run("ls", {{"type", "mesh"}}).size() == 1);
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
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
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
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}, {"subdivisionsX", 4}, {"subdivisionsY", 2}})[0];
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

TEST_CASE("freeze keeps the pivot's world position and oriented axes")
{
    using namespace lila::scene;
    run("newScene");
    std::string a = run("polyCube", {{"name", "A"}, {"position", {10, 0, 0}}})[0];
    run("rotate", {{"args", {a}}, {"by", {0, 30, 0}}});
    run("scale", {{"args", {a}}, {"by", {2, 1, 1}}});
    run("setPivot", {{"args", {a}}, {"position", {13, 4, 0}}, {"orient", {0, 0, 20}}});
    Graph& g = Graph::current();
    const NodeId id = g.find(a)->id();
    const glm::dvec3 at = xformops::worldPivot(g, id);
    const glm::dmat3 axes = xformops::pivotFrame(g, id);
    run("freezeTransforms", {{"args", {a}}});
    CHECK(glm::length(xformops::worldPivot(g, id) - at) < 1e-6);
    const glm::dmat3 now = xformops::pivotFrame(g, id);
    for (int c = 0; c < 3; ++c)
        CHECK(glm::length(glm::normalize(now[c]) - glm::normalize(axes[c])) < 1e-6);
}

TEST_CASE("unfrozenTransforms lists non-identity objects and filters rotations by step")
{
    run("newScene");
    const std::string frozen = run("polyCube", {{"name", "Frozen"}})[0];
    const std::string moved = run("polyCube", {{"name", "Moved"}, {"position", {10, 0, 0}}})[0];
    const std::string quarter = run("polyCube", {{"name", "Quarter"}})[0];
    const std::string tilted = run("polyCube", {{"name", "Tilted"}})[0];
    const std::string scaled = run("polyCube", {{"name", "Scaled"}})[0];
    run("rotate", {{"args", {quarter}}, {"by", {0, 90, 0}}});
    run("rotate", {{"args", {tilted}}, {"by", {0, 37.5, 0}}});
    run("scale", {{"args", {scaled}}, {"by", {2, 2, 2}}});
    run("createNode", {{"args", {"camera"}}, {"name", "Cam"}});
    run("move", {{"args", {"Cam"}}, {"by", {0, 5, 0}}});

    auto names = [](const json& rows) {
        std::vector<std::string> out;
        for (const json& r : rows)
            out.push_back(r["name"]);
        std::sort(out.begin(), out.end());
        return out;
    };
    const json all = {{"args", {frozen, moved, quarter, tilted, scaled}}};
    CHECK(names(run("unfrozenTransforms", all)) == std::vector<std::string>{moved, quarter, scaled, tilted});

    json offStep = all;
    offStep["translate"] = false;
    offStep["scale"] = false;
    offStep["rotateStep"] = 45.0;
    CHECK(names(run("unfrozenTransforms", offStep)) == std::vector<std::string>{tilted});
    offStep["invert"] = true;
    CHECK(names(run("unfrozenTransforms", offStep)) == std::vector<std::string>{quarter});

    const json rows = run("unfrozenTransforms", {{"args", {tilted}}});
    REQUIRE(rows.size() == 1);
    CHECK(rows[0]["unfrozen"] == json::array({"rotate"}));
    CHECK(rows[0]["rotate"][1].get<double>() == doctest::Approx(37.5));

    // The scene check leaves out the camera, which its transform places.
    CHECK(names(run("unfrozenTransforms", {{"scene", true}})) == std::vector<std::string>{moved, quarter, scaled, tilted});

    run("freezeTransforms", {{"args", {moved, quarter, tilted, scaled}}});
    CHECK(run("unfrozenTransforms", {{"scene", true}}).empty());
}
