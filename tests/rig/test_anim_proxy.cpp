#include "TestUtil.h"

#include "fbx/Fbx.h"
#include "geo/Mesh.h"
#include "scene/LastOp.h"
#include "scene/LodGroup.h"
#include "scene/NodeBounds.h"
#include "scene/ProxySet.h"
#include "scene/Skin.h"
#include "scene/SplitMesh.h"

#include <filesystem>

using namespace rigtest;
using namespace lila::scene;

namespace
{
NodeId idOf(const std::string& name) { return node(name).id(); }

/** A closed tube along y skinned to clavicle_l > upperarm_l > lowerarm_l > hand_l. */
std::string proxyArm()
{
    run("newScene");
    std::string mesh = run("polyCylinder", {{"name", "Arm"}, {"radius", 8}, {"height", 120}, {"subdivisionsHeight", 24}})[0];
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(0, -60, 0)}, {"name", "clavicle_l"}});
    run("joint", {{"position", v3(0, -40, 0)}, {"name", "upperarm_l"}});
    run("joint", {{"position", v3(0, 10, 0)}, {"name", "lowerarm_l"}});
    run("joint", {{"position", v3(0, 45, 0)}, {"name", "hand_l"}});
    run("skinCluster", {{"args", {"clavicle_l", mesh}}});
    return mesh;
}

const json kArmPlanes = json::array({{{"joint", "upperarm_l"}}, {{"joint", "lowerarm_l"}}});

std::unordered_set<NodeId> hiddenAt(int level)
{
    graph().evaluate();
    lod::View v;
    v.mode = lod::Mode::Level;
    v.level = level;
    return lod::hidden(graph(), v);
}

json setInfo(const std::string& set) { return run("proxySetInfo", {{"args", {set}}})[0]; }

std::string pieceOf(const json& info, const std::string& joint, const std::string& kind = "proxy")
{
    for (const json& p : info["pieces"])
        if (p["joint"] == joint && p["kind"] == kind)
            return p["mesh"];
    FAIL("no " << kind << " piece for " << joint);
    return {};
}
} // namespace

TEST_CASE("split by two planes gives three closed pieces holding the source's volume")
{
    run("newScene");
    run("polyCylinder", {{"name", "Tube"}, {"radius", 8}, {"height", 120}, {"subdivisionsHeight", 24}});
    const geo::Mesh source = *outMesh("Tube");
    const split::Plane planes[] = {{{0, -15, 0}, {0, 1, 0}, 0.0}, {{0, 25, 0}, {0, 1, 0}, 0.0}};
    const split::Result r = split::splitByPlanes(source, planes);
    REQUIRE(r.pieces.size() == 3);
    double sum = 0.0;
    for (const geo::Mesh& p : r.pieces)
    {
        for (geo::Index e = 0; e < p.edgeCapacity(); ++e)
            if (p.edgeValid(e))
                CHECK_FALSE(p.isBoundaryEdge(e));
        const double v = split::enclosedVolume(p);
        CHECK(v > 0.0);
        sum += v;
    }
    CHECK(sum == doctest::Approx(split::enclosedVolume(source)).epsilon(1e-9));
    // Each plane fitted itself to the tube's cross-section.
    for (double radius : r.radii)
        CHECK(radius == doctest::Approx(8.0 * 1.05 + 1e-4).epsilon(1e-6));

    // A command run makes them as meshes beside the source.
    json made = run("splitMesh", {{"args", {"Tube"}}, {"planes", split::planesToJson(planes)}});
    CHECK(made.size() == 3);
    CHECK(graph().find("Tube"));
}

TEST_CASE("a plane cuts only within its disc")
{
    run("newScene");
    run("polyCylinder", {{"name", "A"}, {"radius", 5}, {"height", 40}});
    run("polyCylinder", {{"name", "B"}, {"radius", 5}, {"height", 40}});
    run("setAttr", {{"args", {"B.translate", v3(30, 0, 0)}}});
    std::string both = run("polyCombine", {{"args", {"A", "B"}}, {"name", "Both"}});
    const geo::Mesh m = *outMesh(both);
    const split::Plane plane[] = {{{0, 0, 0}, {0, 1, 0}, 0.0}};
    CHECK(split::splitByPlanes(m, plane, {.mergeLoose = false}).pieces.size() == 3);
    // The uncut tube joins the piece nearest to it.
    CHECK(split::splitByPlanes(m, plane).pieces.size() == 2);
    // A disc wide enough reaches both.
    const split::Plane wide[] = {{{15, 0, 0}, {0, 1, 0}, 30.0}};
    CHECK(split::splitByPlanes(m, wide).pieces.size() == 4);
}

TEST_CASE("anim proxy pieces follow their joints")
{
    const std::string mesh = proxyArm();
    const std::string set = run("animProxyFromMesh", {{"args", {mesh}}, {"planes", kArmPlanes}, {"remesh", "decimate"}, {"ratio", 0.5}});
    const json info = setInfo(set);
    REQUIRE(info["pieces"].size() == 3);
    CHECK(info["real"] == json::array({mesh}));
    const std::string fore = pieceOf(info, "lowerarm_l");
    pieceOf(info, "upperarm_l");
    pieceOf(info, "clavicle_l");
    CHECK(proxy::clickJoint(graph(), idOf(fore)) == idOf("lowerarm_l"));

    const glm::dmat4 jointBefore = world("lowerarm_l");
    const glm::dmat4 pieceBefore = world(fore);
    run("setAttr", {{"args", {"lowerarm_l.rotate", v3(0, 0, 90)}}});
    const glm::dmat4 expected = world("lowerarm_l") * glm::inverse(jointBefore) * pieceBefore;
    checkNear(glm::dvec3(world(fore)[3]), glm::dvec3(expected[3]), 1e-6);
    checkNear(glm::dvec3(world(fore)[0]), glm::dvec3(expected[0]), 1e-6);
}

TEST_CASE("proxy display hides the real mesh or the pieces")
{
    const std::string mesh = proxyArm();
    const std::string set = run("animProxyFromMesh", {{"args", {mesh}}, {"planes", kArmPlanes}, {"remesh", "decimate"}});
    const json info = setInfo(set);
    std::unordered_set<NodeId> h = hiddenAt(0);
    CHECK(h.contains(idOf(mesh)));
    for (const json& p : info["pieces"])
        CHECK_FALSE(h.contains(idOf(p["mesh"])));

    CHECK(run("setProxyDisplay", {{"args", {"upperarm_l"}}}) == "real");
    h = hiddenAt(0);
    CHECK_FALSE(h.contains(idOf(mesh)));
    for (const json& p : info["pieces"])
        CHECK(h.contains(idOf(p["mesh"])));
    run("undo");
    CHECK(hiddenAt(0).contains(idOf(mesh)));
}

TEST_CASE("a true piece shows alone for its joint")
{
    const std::string mesh = proxyArm();
    const std::string set = run("animProxyFromMesh",
                                {{"args", {mesh}}, {"planes", kArmPlanes}, {"remesh", "decimate"}, {"trueJoints", {"lowerarm_l"}}});
    const json info = setInfo(set);
    REQUIRE(info["pieces"].size() == 4);
    const std::string proxyFore = pieceOf(info, "lowerarm_l");
    const std::string trueFore = pieceOf(info, "lowerarm_l", "true");
    const std::string upper = pieceOf(info, "upperarm_l");
    CHECK(skin::find(graph(), idOf(trueFore)) != kNoNode);
    CHECK(hiddenAt(0).contains(idOf(trueFore)));

    run("setProxyDisplay", {{"piece", "lowerarm_l"}, {"pieceDisplay", "real"}});
    std::unordered_set<NodeId> h = hiddenAt(0);
    CHECK_FALSE(h.contains(idOf(trueFore)));
    CHECK(h.contains(idOf(proxyFore)));
    CHECK_FALSE(h.contains(idOf(upper)));
    CHECK(h.contains(idOf(mesh)));

    // The true piece deforms with the skin: its points sit where the real mesh's do.
    run("setAttr", {{"args", {"lowerarm_l.rotate", v3(0, 0, 45)}}});
    graph().evaluate();
    const geo::Bounds real = worldBounds(graph(), idOf(mesh));
    const geo::Bounds piece = worldBounds(graph(), idOf(trueFore));
    CHECK(piece.max.x <= real.max.x + 1e-3);
    CHECK(real.min.x < -20.0);
    CHECK(piece.min.x == doctest::Approx(real.min.x).epsilon(1e-3));

    run("setProxyDisplay", {{"piece", trueFore}, {"pieceDisplay", "follow"}});
    CHECK(hiddenAt(0).contains(idOf(trueFore)));
}

TEST_CASE("a mesh added by hand is a piece like any other")
{
    const std::string mesh = proxyArm();
    const std::string set = run("createProxySet", {{"args", {mesh}}});
    std::string cube = run("polyCube", {{"name", "HandProxy"}, {"width", 10}, {"height", 10}, {"depth", 10}})[0];
    run("setAttr", {{"args", {cube + ".translate", v3(0, 52, 0)}}});
    run("addToProxySet", {{"args", {set, cube}}});
    const json info = setInfo(set);
    REQUIRE(info["pieces"].size() == 1);
    CHECK(info["pieces"][0]["joint"] == "hand_l");
    CHECK(graph().parent(idOf(cube)) == idOf(set));
    CHECK(proxy::clickJoint(graph(), idOf(cube)) == idOf("hand_l"));
    CHECK(hiddenAt(0).contains(idOf(mesh)));

    const glm::dvec3 before = pos(cube);
    run("setAttr", {{"args", {"clavicle_l.translate", v3(0, -50, 0)}}});
    checkNear(pos(cube), before + glm::dvec3(0, 10, 0), 1e-6);

    run("removeFromProxySet", {{"args", {cube}}});
    CHECK(setInfo(set)["pieces"].empty());
}

TEST_CASE("proxies stay out of exports and survive a save")
{
    const std::string mesh = proxyArm();
    const std::string set = run("animProxyFromMesh", {{"args", {mesh}}, {"planes", kArmPlanes}, {"remesh", "decimate"}});
    const json info = setInfo(set);
    graph().evaluate();
    std::vector<NodeId> roots(graph().roots().begin(), graph().roots().end());
    const fbx::ExportScene scene = fbx::gatherExport(graph(), roots, {});
    bool sawArm = false;
    for (const fbx::ExportNode& n : scene.nodes)
    {
        sawArm |= n.source == idOf(mesh);
        CHECK_FALSE(n.source == idOf(set));
        for (const json& p : info["pieces"])
            CHECK_FALSE(n.source == idOf(p["mesh"]));
    }
    CHECK(sawArm);

    auto path = (std::filesystem::temp_directory_path() / "lila_rig_tests" / "animproxy.lilaa").string();
    run("saveScene", {{"args", {path}}});
    run("openScene", {{"args", {path}}});
    const json back = setInfo(set);
    CHECK(back["pieces"].size() == info["pieces"].size());
    CHECK(back["real"] == info["real"]);
    CHECK(hiddenAt(0).contains(idOf(mesh)));
}

TEST_CASE("the view's LOD picks the set it shows")
{
    const std::string mesh = proxyArm();
    const std::string near = run("createProxySet", {{"args", {mesh}}, {"name", "Near"}, {"lodLevel", 0}});
    const std::string far = run("createProxySet", {{"args", {mesh}}, {"name", "Far"}, {"lodLevel", 1}});
    std::string a = run("polyCube", {{"name", "NearPiece"}})[0];
    std::string b = run("polyCube", {{"name", "FarPiece"}})[0];
    run("addToProxySet", {{"args", {near, a}}, {"joint", "hand_l"}});
    run("addToProxySet", {{"args", {far, b}}, {"joint", "hand_l"}});
    std::unordered_set<NodeId> h0 = hiddenAt(0), h1 = hiddenAt(1), h3 = hiddenAt(3);
    CHECK_FALSE(h0.contains(idOf(a)));
    CHECK(h0.contains(idOf(b)));
    CHECK(h1.contains(idOf(a)));
    CHECK_FALSE(h1.contains(idOf(b)));
    CHECK_FALSE(h3.contains(idOf(b)));
    CHECK(h0.contains(idOf(mesh)));
    CHECK(h1.contains(idOf(mesh)));
}

TEST_CASE("an anim proxy regenerates with new counts and exact pieces from the adjust popup")
{
    const std::string mesh = proxyArm();
    lastOp::install();
    ops::execute("animProxyFromMesh", {{"args", {mesh}}, {"planes", kArmPlanes}, {"remesh", "decimate"}, {"ratio", 0.8}}, CallSource::UI);
    REQUIRE(lastOp::live());
    auto faces = [&](const std::string& joint, const std::string& kind = "proxy") {
        const json info = setInfo(mesh + "_animProxy");
        return outMesh(pieceOf(info, joint, kind))->numFaces();
    };
    const geo::Index before = faces("clavicle_l");
    lastOp::rerun({{"pieceFaces", "0:16"}});
    const geo::Index after = faces("clavicle_l");
    CHECK(after < before);
    CHECK(after <= 24);
    CHECK(faces("upperarm_l") > 24);
    CHECK(proxy::all(graph()).size() == 1);

    lastOp::rerun({{"exactPieces", "1"}});
    const json info = setInfo(mesh + "_animProxy");
    CHECK(skin::find(graph(), idOf(pieceOf(info, "upperarm_l", "true"))) != kNoNode);
    run("undo");
    run("undo");
    CHECK(faces("clavicle_l") == before);
}

TEST_CASE("regenerating an anim proxy leaves nothing behind")
{
    const std::string mesh = proxyArm();
    const json call = {{"args", {mesh}}, {"planes", kArmPlanes}, {"remesh", "decimate"}, {"trueJoints", {"lowerarm_l"}}};
    const size_t bare = graph().nodes().size();
    run("animProxyFromMesh", call);
    const size_t once = graph().nodes().size();
    CHECK(once > bare);
    for (int i = 0; i < 3; ++i)
    {
        run("animProxyFromMesh", call);
        CHECK(graph().nodes().size() == once);
    }
    CHECK(proxy::all(graph()).size() == 1);
    const json before = setInfo(mesh + "_animProxy");
    std::vector<std::string> names;
    for (NodeId id : graph().nodes())
        names.push_back(graph().path(id));
    std::ranges::sort(names);

    run("animProxyFromMesh", call);
    run("undo");
    CHECK(graph().nodes().size() == once);
    std::vector<std::string> back;
    for (NodeId id : graph().nodes())
        back.push_back(graph().path(id));
    std::ranges::sort(back);
    CHECK(back == names);
    CHECK(setInfo(mesh + "_animProxy") == before);
    run("undo");
    CHECK(graph().nodes().size() == once);
}

TEST_CASE("the adjust popup offers only the anim proxy's tuning flags")
{
    std::vector<std::string> flags;
    for (const FlagDef* f : lastOp::editableFlags("animProxyFromMesh"))
        flags.push_back(f->name);
    for (const char* want : {"remesh", "targetFaces", "resolution", "ratio", "pieceFaces", "exactPieces"})
        CHECK_MESSAGE(std::ranges::find(flags, want) != flags.end(), want);
    for (const char* hidden : {"replace", "suggest", "lodLevel", "planes", "name"})
        CHECK_MESSAGE(std::ranges::find(flags, hidden) == flags.end(), hidden);
}
