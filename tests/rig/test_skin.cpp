#include "TestUtil.h"

#include "scene/Skin.h"

#include <filesystem>

using namespace rigtest;
using namespace lila::scene;

namespace
{
/** A 100 cm cylinder along Y (y in [-50, 50]) with a three-joint chain up its middle, bound with default weights. */
std::string skinnedCylinder()
{
    run("newScene");
    std::string mesh = run("polyCylinder", {{"name", "Tube"}, {"radius", 10}, {"height", 100}, {"subdivisionsHeight", 10}, {"caps", false}})[0];
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(0, -50, 0)}, {"name", "bone_a"}});
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "bone_b"}});
    run("joint", {{"position", v3(0, 50, 0)}, {"name", "bone_c"}});
    run("skinCluster", {{"args", {"bone_a", mesh}}});
    return mesh;
}

std::vector<glm::dvec3> points(const std::string& mesh)
{
    MeshPtr m = outMesh(mesh);
    REQUIRE(m);
    return {m->positions().begin(), m->positions().end()};
}

double maxY(const std::vector<glm::dvec3>& pts)
{
    double y = -1e300;
    for (auto& p : pts)
        y = std::max(y, p.y);
    return y;
}
} // namespace

TEST_CASE("skinning a cylinder with a joint chain bends it and undo restores")
{
    std::string mesh = skinnedCylinder();
    json info = run("skinInfo", {{"args", {mesh}}});
    CHECK(info["influences"] == json::array({"bone_a", "bone_b", "bone_c"}));
    CHECK(info["unnormalized"] == 0);
    CHECK(info["unweighted"] == 0);
    CHECK(info["maxUsed"].get<int>() <= 4);

    std::vector<glm::dvec3> rest = points(mesh);
    CHECK(maxY(rest) == doctest::Approx(50));

    // Bend the middle joint 90 degrees about Z: the top half swings over to -X.
    run("setAttr", {{"args", {"bone_b.rotate", v3(0, 0, 90)}}});
    std::vector<glm::dvec3> bent = points(mesh);
    REQUIRE(bent.size() == rest.size());
    double topX = 0.0;
    int bottomStill = 0, bottom = 0;
    for (size_t i = 0; i < rest.size(); ++i)
    {
        if (rest[i].y > 49.0)
            topX = std::min(topX, bent[i].x);
        if (rest[i].y < -49.0)
        {
            ++bottom;
            // Closest-distance binding leaves a sliver of the far bone here, as Maya's does.
            bottomStill += glm::length(bent[i] - rest[i]) < 0.5;
        }
    }
    CHECK(topX < -40.0);
    CHECK(maxY(bent) < 20.0);
    CHECK(bottom > 0);
    CHECK(bottomStill == bottom);

    run("undo");
    std::vector<glm::dvec3> back = points(mesh);
    for (size_t i = 0; i < rest.size(); ++i)
        CHECK(glm::length(back[i] - rest[i]) < 1e-9);

    // Undoing the bind itself leaves the plain mesh and no skin.
    run("undo");
    CHECK(skin::find(graph(), node(mesh).id()) == kNoNode);
    CHECK(points(mesh).size() == rest.size());
    run("redo");
    CHECK(skin::find(graph(), node(mesh).id()) != kNoNode);
}

TEST_CASE("dual quaternion and envelope")
{
    std::string mesh = skinnedCylinder();
    NodeId skin = skin::find(graph(), node(mesh).id());
    std::string sc = graph().node(skin)->name();
    std::vector<glm::dvec3> rest = points(mesh);
    run("setAttr", {{"args", {"bone_b.rotate", v3(0, 0, 90)}}});
    run("setAttr", {{"args", {sc + ".envelope", 0.0}}});
    std::vector<glm::dvec3> off = points(mesh);
    for (size_t i = 0; i < rest.size(); ++i)
        CHECK(glm::length(off[i] - rest[i]) < 1e-9);
    run("setAttr", {{"args", {sc + ".envelope", 1.0}}});
    run("setAttr", {{"args", {sc + ".skinningMethod", 1}}});
    CHECK(maxY(points(mesh)) < 20.0);
}

TEST_CASE("skinned meshes save and load in both formats")
{
    std::string mesh = skinnedCylinder();
    run("setAttr", {{"args", {"bone_b.rotate", v3(0, 0, 45)}}});
    std::vector<glm::dvec3> before = points(mesh);
    json weightsBefore = run("skinPercent", {{"args", {mesh}}, {"vertices", {0, 5, 17}}});
    auto dir = std::filesystem::temp_directory_path() / "lila_rig_tests";
    for (const char* ext : {".lila", ".lilaa"})
    {
        auto path = (dir / (std::string("skinned") + ext)).string();
        run("saveScene", {{"args", {path}}});
        run("newScene");
        run("openScene", {{"args", {path}}});
        std::vector<glm::dvec3> after = points(mesh);
        REQUIRE(after.size() == before.size());
        for (size_t i = 0; i < before.size(); ++i)
            CHECK(glm::length(after[i] - before[i]) < 1e-4);
        CHECK(run("skinPercent", {{"args", {mesh}}, {"vertices", {0, 5, 17}}}) == weightsBefore);
        CHECK(run("skinInfo", {{"args", {mesh}}})["influences"].size() == 3);
        // Still live after loading.
        run("setAttr", {{"args", {"bone_b.rotate", v3(0, 0, 90)}}});
        CHECK(maxY(points(mesh)) < 20.0);
        run("setAttr", {{"args", {"bone_b.rotate", v3(0, 0, 45)}}});
    }
}

TEST_CASE("weights normalize, prune and set exactly")
{
    std::string mesh = skinnedCylinder();
    run("skinPercent", {{"args", {mesh}}, {"vertices", {0}}, {"transformValue", {{"bone_c", 0.25}}}});
    json w = run("skinPercent", {{"args", {mesh}}, {"vertices", {0}}})[0]["weights"];
    CHECK(w["bone_c"].get<double>() == doctest::Approx(0.25));
    double sum = 0.0;
    for (auto& [k, v] : w.items())
        sum += v.get<double>();
    CHECK(sum == doctest::Approx(1.0));

    // Unnormalized input, then normalizeWeights fixes it.
    run("skinPercent", {{"args", {mesh}}, {"vertices", {1}}, {"transformValue", {{"bone_a", 0.5}, {"bone_b", 0.2}}}, {"normalize", false}});
    json raw = run("skinPercent", {{"args", {mesh}}, {"vertices", {1}}})[0]["weights"];
    run("normalizeWeights", {{"args", {mesh}}});
    json fixed = run("skinPercent", {{"args", {mesh}}, {"vertices", {1}}})[0]["weights"];
    sum = 0.0;
    for (auto& [k, v] : fixed.items())
        sum += v.get<double>();
    CHECK(sum == doctest::Approx(1.0));
    CHECK(run("skinInfo", {{"args", {mesh}}})["unnormalized"] == 0);

    run("pruneWeights", {{"args", {mesh}}, {"threshold", 0.3}});
    for (const json& p : run("skinPercent", {{"args", {mesh}}}))
    {
        double s = 0.0;
        for (auto& [k, v] : p["weights"].items())
        {
            CHECK(v.get<double>() >= 0.3 - 1e-6);
            s += v.get<double>();
        }
        CHECK(s == doctest::Approx(1.0));
    }
}

TEST_CASE("copy and mirror skin weights")
{
    std::string mesh = skinnedCylinder();
    std::string other = run("polyCylinder", {{"name", "Copy"}, {"radius", 10}, {"height", 100}, {"subdivisionsHeight", 10}, {"caps", false}})[0];
    run("skinCluster", {{"args", {"bone_a", other}}, {"bindTo", "selected"}});
    CHECK(run("skinInfo", {{"args", {other}}})["influences"].size() == 1);
    run("copySkinWeights", {{"args", {mesh, other}}});
    CHECK(run("skinInfo", {{"args", {other}}})["influences"].size() == 3);
    json a = run("skinPercent", {{"args", {mesh}}, {"vertices", {3, 40}}});
    json b = run("skinPercent", {{"args", {other}}, {"vertices", {3, 40}}});
    for (int k = 0; k < 2; ++k)
        for (auto& [j, v] : a[k]["weights"].items())
            CHECK(b[k]["weights"].value(j, 0.0) == doctest::Approx(v.get<double>()).epsilon(1e-3));

    // Mirror: paint one side, mirror, the other side matches.
    run("skinPercent", {{"args", {mesh}}, {"vertices", {0}}, {"transformValue", {{"bone_c", 1.0}}}});
    MeshPtr m = outMesh(mesh);
    glm::dvec3 p0 = m->position(0);
    int partner = -1;
    for (geo::Index p = 0; p < m->pointCapacity(); ++p)
        if (glm::length(m->position(p) - glm::dvec3(-p0.x, p0.y, p0.z)) < 1e-6)
            partner = int(p);
    REQUIRE(partner >= 0);
    REQUIRE(p0.x > 0.0);
    run("mirrorSkinWeights", {{"args", {mesh}}});
    json mirrored = run("skinPercent", {{"args", {mesh}}, {"vertices", {partner}}})[0]["weights"];
    CHECK(mirrored.value("bone_c", 0.0) == doctest::Approx(1.0));
}

TEST_CASE("heatmap bind, unbind and bind pose")
{
    run("newScene");
    std::string mesh = run("polyCylinder", {{"name", "Tube"}, {"radius", 10}, {"height", 100}, {"subdivisionsHeight", 10}})[0];
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(0, -50, 0)}, {"name", "bone_a"}});
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "bone_b"}});
    run("joint", {{"position", v3(0, 50, 0)}, {"name", "bone_c"}});
    run("skinCluster", {{"args", {"bone_a", mesh}}, {"method", "heatmap"}, {"maxInfluences", 2}});
    json info = run("skinInfo", {{"args", {mesh}}});
    CHECK(info["unnormalized"] == 0);
    CHECK(info["maxUsed"].get<int>() <= 2);

    std::vector<glm::dvec3> rest = points(mesh);
    run("setAttr", {{"args", {"bone_b.rotate", v3(0, 0, 60)}}});
    run("goToBindPose", {{"args", {mesh}}});
    std::vector<glm::dvec3> bound = points(mesh);
    for (size_t i = 0; i < rest.size(); ++i)
        CHECK(glm::length(bound[i] - rest[i]) < 1e-6);

    run("setAttr", {{"args", {"bone_b.rotate", v3(0, 0, 60)}}});
    run("unbindSkin", {{"args", {mesh}}});
    CHECK(skin::find(graph(), node(mesh).id()) == kNoNode);
    std::vector<glm::dvec3> plain = points(mesh);
    for (size_t i = 0; i < rest.size(); ++i)
        CHECK(glm::length(plain[i] - rest[i]) < 1e-9);
    for (NodeId id : graph().nodes())
        CHECK(graph().node(id)->type().name != "skinInfluence");
}
