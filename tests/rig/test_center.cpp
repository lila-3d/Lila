#include "TestUtil.h"

#include "scene/CenterInMesh.h"
#include "scene/DominantInfluence.h"
#include "scene/Rig.h"
#include "scene/Skin.h"

#include <filesystem>

using namespace rigtest;
using namespace lila::scene;

namespace
{
/** A 100 cm tube along Y with a chain up it whose middle joint sits off the axis, bound with default weights. */
std::string offAxisChain()
{
    run("newScene");
    std::string mesh = run("polyCylinder", {{"name", "Tube"}, {"radius", 10}, {"height", 100}, {"subdivisionsAxis", 24},
                                            {"subdivisionsHeight", 10}, {"caps", false}})[0];
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(0, -50, 0)}, {"name", "bone_a"}});
    run("joint", {{"position", v3(3, 0, 4)}, {"name", "bone_b"}});
    run("joint", {{"position", v3(0, 50, 0)}, {"name", "bone_c"}});
    run("skinCluster", {{"args", {"bone_a", mesh}}});
    return mesh;
}
} // namespace

TEST_CASE("centerJoint moves a joint onto the axis of the mesh it is bound to")
{
    std::string mesh = offAxisChain();
    CHECK(rig::centerMeshes(graph(), node("bone_b").id()) == std::vector<NodeId>{node(mesh).id()});

    json moved = run("centerJoint", {{"args", {"bone_b"}}, {"keepChildren", true}});
    CHECK(moved == json::array({"bone_b"}));
    glm::dvec3 p = pos("bone_b");
    CHECK(std::abs(p.x) < 1e-6);
    CHECK(std::abs(p.z) < 1e-6);
    CHECK(std::abs(p.y) < 5.0);
    checkNear(pos("bone_c"), {0, 50, 0});

    run("undo");
    checkNear(pos("bone_b"), {3, 0, 4});

    // Without keepChildren the child rides along.
    run("centerJoint", {{"args", {"bone_b"}}});
    CHECK(glm::length(pos("bone_c") - glm::dvec3(0, 50, 0)) > 1.0);
}

TEST_CASE("centerMeshes binds unskinned meshes and centring picks the limb under the joint")
{
    run("newScene");
    run("polyCylinder", {{"name", "LegL"}, {"radius", 6}, {"height", 80}, {"subdivisionsAxis", 16}, {"caps", true}});
    run("setAttr", {{"args", {"LegL.translate", v3(-15, 0, 0)}}});
    run("polyCylinder", {{"name", "LegR"}, {"radius", 6}, {"height", 80}, {"subdivisionsAxis", 16}, {"caps", true}});
    run("setAttr", {{"args", {"LegR.translate", v3(15, 0, 0)}}});
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(13, -40, 2)}, {"name", "hip"}});
    run("joint", {{"position", v3(13, -10, 2)}, {"name", "knee"}});

    CHECK_THROWS(run("centerMeshes", {{"args", {"knee"}}, {"meshes", {"hip"}}}));
    json bound = run("centerMeshes", {{"args", {"knee"}}, {"meshes", {"LegL", "LegR"}}, {"centerInMesh", true}});
    CHECK(bound["knee"] == json::array({"LegL", "LegR"}));
    CHECK(rig::centerEnabled(node("knee")));

    // The binding follows renames and survives a save and reopen under the new name.
    run("rename", {{"args", {"LegR", "Shin"}}});
    CHECK(run("centerMeshes", {{"args", {"knee"}}})["knee"] == json::array({"LegL", "Shin"}));
    auto path = (std::filesystem::temp_directory_path() / "lila_rig_tests" / "center.lilaa").string();
    run("saveScene", {{"args", {path}}});
    run("openScene", {{"args", {path}}});
    CHECK(run("centerMeshes", {{"args", {"knee"}}})["knee"] == json::array({"LegL", "Shin"}));

    run("centerJoint", {{"args", {"knee"}}});
    checkNear(pos("knee"), {15, -10, 0});

    // A space-separated name string, as older files wrote it, still loads.
    const AttrDef& def = node("knee").type().attrs[Joint::kCenterMeshes];
    graph().setValue({node("knee").id(), Joint::kCenterMeshes, -1}, fromJson(def, json("Shin LegL")));
    CHECK(rig::centerMeshes(graph(), node("knee").id()) == std::vector<NodeId>{node("Shin").id(), node("LegL").id()});

    // Unbound and unskinned: nothing to centre in, so it stays.
    run("centerMeshes", {{"args", {"hip"}}, {"meshes", json::array()}});
    CHECK(run("centerJoint", {{"args", {"hip"}}}).empty());
}

TEST_CASE("snapToComponentCenter moves a joint to the selected vertices, children kept")
{
    run("newScene");
    run("polyCube", {{"name", "Box"}, {"width", 10}, {"height", 10}, {"depth", 10}});
    run("setAttr", {{"args", {"Box.translate", v3(100, 0, 0)}}});
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "root"}});
    run("joint", {{"position", v3(0, 20, 0)}, {"name", "tip"}});

    MeshPtr m = outMesh("Box");
    std::vector<uint32_t> top;
    for (geo::Index p = 0; p < m->pointCapacity(); ++p)
        if (m->pointValid(p) && m->position(p).y > 4.0)
            top.push_back(p);
    REQUIRE(top.size() == 4);
    // One extra vertex pulls the average off the bounding-box middle.
    std::vector<uint32_t> pick = top;
    for (geo::Index p = 0; p < m->pointCapacity(); ++p)
        if (m->pointValid(p) && m->position(p).y < -4.0)
        {
            pick.push_back(p);
            break;
        }
    run("select", {{"args", {"Box"}}});
    run("selectComponents", {{"args", {"Box"}}, {"vertices", pick}});

    CHECK_THROWS(run("snapToComponentCenter", {{"args", json::array()}}));
    run("snapToComponentCenter", {{"args", {"root"}}});
    checkNear(pos("root"), {100, 0, 0});
    checkNear(pos("tip"), {0, 20, 0});

    run("snapToComponentCenter", {{"args", {"root"}}, {"center", "average"}, {"keepChildren", false}});
    glm::dvec3 avg = pos("root");
    CHECK(avg.y == doctest::Approx(3.0));
    CHECK(pos("tip").y == doctest::Approx(23.0));
    run("undo");
    checkNear(pos("root"), {100, 0, 0});
}

TEST_CASE("dominant influence per face follows the weights")
{
    run("newScene");
    std::string mesh = run("polyCylinder", {{"name", "Tube"}, {"radius", 10}, {"height", 100}, {"subdivisionsHeight", 10}, {"caps", false}})[0];
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(0, -50, 0)}, {"name", "bone_a"}});
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "bone_b"}});
    run("joint", {{"position", v3(0, 50, 0)}, {"name", "bone_c"}});
    run("skinCluster", {{"args", {"bone_a", mesh}}});
    Graph& g = graph();
    NodeId tube = node(mesh).id();

    MeshPtr m = outMesh(mesh);
    geo::Index low = geo::kInvalid, high = geo::kInvalid;
    for (geo::Index f = 0; f < m->faceCapacity(); ++f)
        if (m->faceValid(f))
        {
            double y = m->faceCentroid(f).y;
            if (low == geo::kInvalid || y < m->faceCentroid(low).y)
                low = f;
            if (high == geo::kInvalid || y > m->faceCentroid(high).y)
                high = f;
        }
    CHECK(rig::dominantJoint(g, tube, low) == node("bone_a").id());
    CHECK(rig::dominantJoint(g, tube, high) != node("bone_a").id());
    CHECK(run("dominantInfluence", {{"args", {mesh}}, {"face", low}}) == "bone_a");

    size_t total = 0;
    for (const char* j : {"bone_a", "bone_b", "bone_c"})
        total += rig::dominatedFaces(g, tube, node(j).id()).size();
    CHECK(total == m->numFaces());

    std::vector<float> wash = rig::influenceWeights(g, tube, node("bone_a").id());
    REQUIRE(wash.size() == m->pointCapacity());
    CHECK(*std::max_element(wash.begin(), wash.end()) > 0.9f);
    CHECK(rig::influenceWeights(g, tube, tube).empty());

    // Repainting every point to bone_c rebuilds the cached map.
    auto before = rig::dominantInfluences(g, skin::find(g, tube));
    std::vector<uint32_t> all;
    for (geo::Index p = 0; p < m->pointCapacity(); ++p)
        all.push_back(p);
    run("skinPercent", {{"args", {mesh}}, {"vertices", all}, {"transformValue", json::array({json::array({"bone_c", 1.0})})}});
    CHECK(rig::dominantInfluences(g, skin::find(g, tube)) != before);
    CHECK(rig::dominantJoint(g, tube, low) == node("bone_c").id());
    run("undo");
    CHECK(rig::dominantJoint(g, tube, low) == node("bone_a").id());
}
