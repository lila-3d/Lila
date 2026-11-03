#include <doctest/doctest.h>

#include "TestUtil.h"
#include "ops/Command.h"
#include "geo/Mesh.h"
#include "gn/Gn.h"
#include "gn/Routing.h"
#include "scene/Graph.h"
#include "scene/Node.h"

#include <string>

using namespace lila;
using namespace gntest;
using json = nlohmann::json;

namespace
{
std::string addAt(const char* group, const char* type, double x = 0, double y = 0)
{
    return run("gnAddNode", {{"args", {group, type}}, {"position", {x, y}}}).get<std::string>();
}

/** A 4x4 grid offset upwards by Combine XYZ, applied to a plane; returns the plane. */
std::string buildOffsetPlane(const char* name)
{
    run("gnCreateGroup", args({name}));
    addAt(name, "MeshGrid");
    addAt(name, "SetPosition", 300);
    addAt(name, "CombineXYZ", 0, 200);
    run("gnSetValue", args({name, "MeshGrid.verticesX", 4}));
    run("gnSetValue", args({name, "MeshGrid.verticesY", 4}));
    run("gnSetValue", args({name, "CombineXYZ.z", 5.0}));
    run("gnConnect", args({name, "MeshGrid.mesh", "SetPosition.geometry"}));
    run("gnConnect", args({name, "CombineXYZ.vector", "SetPosition.offset"}));
    run("gnConnect", args({name, "SetPosition.geometry", "GroupOutput.geometry"}));
    std::string mesh = run("polyPlane", {{"name", "P"}})[0].get<std::string>();
    run("gnApply", args({mesh, name}));
    return mesh;
}

geo::Mesh result(const std::string& mesh)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::Node* n = g.find(mesh);
    return *n->as<scene::MeshPtr>(n->type().attrIndex("outMesh"));
}

double highestZ(const geo::Mesh& m)
{
    double z = -1e9;
    for (geo::Index i = 0; i < m.numPoints(); ++i)
        z = std::max(z, m.position(i).z);
    return z;
}
} // namespace

TEST_CASE("reroutes pass their wire through unchanged, chained and fanned out")
{
    run("newScene");
    std::string mesh = buildOffsetPlane("R");
    geo::Mesh before = result(mesh);
    REQUIRE(before.numPoints() == 16);

    // Two reroutes in a row on the geometry wire, one on the offset wire.
    std::string r1 = addAt("R", gn::kRerouteType, 150, 0), r2 = addAt("R", gn::kRerouteType, 220, 0), r3 = addAt("R", gn::kRerouteType, 150, 200);
    run("gnConnect", args({"R", "MeshGrid.mesh", r1 + ".input"}));
    run("gnConnect", args({"R", r1 + ".output", r2 + ".input"}));
    run("gnConnect", args({"R", r2 + ".output", "SetPosition.geometry"}));
    run("gnConnect", args({"R", "CombineXYZ.vector", r3 + ".input"}));
    run("gnConnect", args({"R", r3 + ".output", "SetPosition.offset"}));
    geo::Mesh routed = result(mesh);
    CHECK(routed.numPoints() == before.numPoints());
    CHECK(highestZ(routed) == doctest::Approx(highestZ(before)));

    // Each reroute shows the type of what feeds it.
    json info = run("gnGroupInfo", args({"R"}));
    CHECK_FALSE(info.contains("error"));
    for (const json& n : info["nodes"])
    {
        if (n["name"] == r2)
            CHECK(n["outputs"][0]["type"] == "geometry");
        if (n["name"] == r3)
            CHECK(n["outputs"][0]["type"] == "vector");
    }

    // A reroute feeds several inputs: the offset reroute also drives the grid's size.
    run("gnConnect", args({"R", r3 + ".output", "MeshGrid.sizeX"}));
    CHECK(group("R").links.size() == 7);
    CHECK(result(mesh).numPoints() == 16);

    // Unhooking a reroute's input leaves its readers on their own values, as an unlinked socket would.
    run("gnDisconnect", args({"R", r1 + ".input"}));
    CHECK(result(mesh).numPoints() == 0);
    run("undo");
    CHECK(result(mesh).numPoints() == 16);
}

TEST_CASE("a reroute can't close a loop")
{
    run("newScene");
    buildOffsetPlane("R");
    std::string r = addAt("R", gn::kRerouteType);
    run("gnConnect", args({"R", "SetPosition.geometry", r + ".input"}));
    CHECK_THROWS(run("gnConnect", args({"R", r + ".output", "SetPosition.geometry"})));
}

TEST_CASE("portals carry a value without a wire, and flag what can't work")
{
    run("newScene");
    std::string mesh = buildOffsetPlane("P");
    double lifted = highestZ(result(mesh));

    std::string send = addAt("P", gn::kSendType, 150, 200), receive = addAt("P", gn::kReceiveType, 150, 300);
    run("gnSetValue", args({"P", send + ".name", "lift"}));
    run("gnConnect", args({"P", "CombineXYZ.vector", send + ".value"}));
    run("gnSetValue", args({"P", receive + ".name", "lift"}));
    run("gnConnect", args({"P", receive + ".value", "SetPosition.offset"}));
    CHECK(group("P").linksInto("SetPosition", "offset").front().fromNode == receive);
    CHECK(highestZ(result(mesh)) == doctest::Approx(lifted));

    json info = run("gnGroupInfo", args({"P"}));
    CHECK_FALSE(info.contains("error"));
    for (const json& n : info["nodes"])
    {
        CHECK_FALSE(n.contains("problem"));
        if (n["name"] == receive)
            CHECK(n["outputs"][0]["type"] == "vector");
    }

    // Renaming the Send leaves the Receive dangling: it is flagged and its reader falls back to its own value.
    run("gnSetValue", args({"P", send + ".name", "raise"}));
    bool flagged = false;
    json after = run("gnGroupInfo", args({"P"}));
    for (const json& n : after["nodes"])
        if (n["name"] == receive)
            flagged = n.value("problem", "").find("No Send named lift") != std::string::npos;
    CHECK(flagged);
    CHECK(highestZ(result(mesh)) < lifted - 1.0);
    run("undo");
    CHECK(highestZ(result(mesh)) == doctest::Approx(lifted));

    // A Receive can't feed its own Send, by name or by link.
    std::string loopSend = addAt("P", gn::kSendType, 600, 0), loopReceive = addAt("P", gn::kReceiveType, 400, 0);
    std::string math = addAt("P", "VectorMath", 500, 0);
    run("gnConnect", args({"P", loopReceive + ".value", math + ".a"}));
    run("gnConnect", args({"P", math + ".vector", loopSend + ".value"}));
    run("gnSetValue", args({"P", loopSend + ".name", "round"}));
    CHECK_THROWS(run("gnSetValue", args({"P", loopReceive + ".name", "round"})));
    CHECK(gn::portalName(*group("P").node(loopReceive)).empty());
    run("gnSetValue", args({"P", loopReceive + ".name", "lift"}));
    CHECK_THROWS(run("gnConnect", args({"P", math + ".vector", send + ".value"})));
}

TEST_CASE("a muted node passes its matching input through")
{
    run("newScene");
    std::string mesh = buildOffsetPlane("M");
    double lifted = highestZ(result(mesh));
    double flat = lifted - 5.0;

    run("gnSetNodeFlags", {{"args", {"M", "SetPosition"}}, {"muted", true}});
    geo::Mesh m = result(mesh);
    CHECK(m.numPoints() == 16);
    CHECK(highestZ(m) == doctest::Approx(flat));

    // A muted node with no input of the output's type hands on nothing.
    run("gnSetNodeFlags", {{"args", {"M", "SetPosition"}}, {"muted", false}});
    run("gnSetNodeFlags", {{"args", {"M", "MeshGrid"}}, {"muted", true}});
    CHECK(result(mesh).numPoints() == 0);
    run("undo");
    CHECK(highestZ(result(mesh)) == doctest::Approx(lifted));

    // Muted nodes in a row pass along the chain.
    std::string second = addAt("M", "SetPosition", 500, 0);
    run("gnConnect", args({"M", "SetPosition.geometry", second + ".geometry"}));
    run("gnConnect", args({"M", second + ".geometry", "GroupOutput.geometry"}));
    run("gnSetNodeFlags", {{"args", {"M", "SetPosition", second}}, {"muted", true}});
    CHECK(highestZ(result(mesh)) == doctest::Approx(flat));
    CHECK(group("M").node(second)->muted);
    run("undo");
    CHECK(highestZ(result(mesh)) == doctest::Approx(lifted));
}

TEST_CASE("portal problems name each mistake")
{
    std::vector<gn::GNode> nodes = {{"S1", gn::kSendType, {}, {{"name", "a"}}}, {"S2", gn::kSendType, {}, {{"name", "a"}}},
                                    {"R1", gn::kReceiveType, {}, {{"name", "b"}}}, {"R2", gn::kReceiveType}, {"S3", gn::kSendType}};
    auto p = gn::portalProblems(nodes, {});
    CHECK(p.contains("S1"));
    CHECK(p.contains("S2"));
    CHECK(p["R1"].find("No Send named b") != std::string::npos);
    CHECK(p.contains("R2"));
    CHECK(p.contains("S3"));
}

TEST_CASE("resolveRouting splices reroutes out")
{
    std::vector<gn::GNode> nodes = {{"A", "Value"}, {"R1", gn::kRerouteType}, {"R2", gn::kRerouteType}, {"B", "Math"}, {"C", "Math"}};
    std::vector<gn::GLink> links = {{"A", "value", "R1", "input"}, {"R1", "output", "R2", "input"}, {"R2", "output", "B", "a"}, {"R1", "output", "C", "b"}};
    gn::resolveRouting(nodes, links, {});
    CHECK(nodes.size() == 3);
    REQUIRE(links.size() == 2);
    CHECK(links[0] == gn::GLink{"A", "value", "B", "a"});
    CHECK(links[1] == gn::GLink{"A", "value", "C", "b"});
}
