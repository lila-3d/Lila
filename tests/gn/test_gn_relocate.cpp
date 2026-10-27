#include <doctest/doctest.h>

#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

using namespace lila;
using json = nlohmann::json;

namespace
{
json rcall(const char* cmd, json call = json::object()) { return ops::execute(cmd, call); }
json rargs(std::initializer_list<json> a) { return {{"args", json(a)}}; }

/** A cube whose group joins in a sphere read through Object Info, both far from the origin. Returns the Object Info node. */
std::string relocObjectInfoScene(const std::string& relativeTo)
{
    rcall("newScene");
    rcall("polySphere", {{"radius", 20.0}, {"name", "Source"}});
    rcall("setAttr", rargs({"Source.translate", {3000, 0, 1000}}));
    rcall("polyCube", {{"width", 50.0}, {"height", 50.0}, {"depth", 50.0}, {"name", "User"}});
    rcall("setAttr", rargs({"User.translate", {2900, 0, 1000}}));
    rcall("gnCreateGroup", rargs({"Copy"}));
    std::string oi = rcall("gnAddNode", rargs({"Copy", "ObjectInfo"}));
    std::string join = rcall("gnAddNode", rargs({"Copy", "JoinGeometry"}));
    rcall("gnSetValue", rargs({"Copy", oi + ".object", "Source"}));
    rcall("gnSetValue", rargs({"Copy", oi + ".transformSpace", "world"}));
    rcall("gnSetValue", rargs({"Copy", oi + ".relativeTo", relativeTo}));
    rcall("gnDisconnect", rargs({"Copy", "GroupOutput.geometry"}));
    rcall("gnConnect", rargs({"Copy", "GroupInput.geometry", join + ".geometry"}));
    rcall("gnConnect", rargs({"Copy", oi + ".geometry", join + ".geometry"}));
    rcall("gnConnect", rargs({"Copy", join + ".geometry", "GroupOutput.geometry"}));
    rcall("gnApply", rargs({"User", "Copy"}));
    return oi;
}

/** The User mesh's points in world space. */
std::vector<glm::dvec3> relocUserPoints()
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    const scene::Node* n = g.find("User");
    const glm::dmat4 w = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
    std::vector<glm::dvec3> out;
    for (const glm::dvec3& p : n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh)->positions())
        out.push_back(glm::dvec3(w * glm::dvec4(p, 1.0)));
    return out;
}
} // namespace

TEST_CASE("Object Info relative to the owner reads the other object against the mesh the group runs on")
{
    relocObjectInfoScene("owner");
    const std::vector<glm::dvec3> before = relocUserPoints();
    // The sphere sits where it is in the world: relative to the owner it is placed back where the owner stood.
    double maxX = -1e300;
    for (const glm::dvec3& p : before)
        maxX = std::max(maxX, p.x);
    CHECK(maxX == doctest::Approx(3020.0));

    json r = rcall("relocateScene", {{"args", {"User"}}, {"from", "User"}, {"rotation", "none"}});
    INFO(r.dump());
    const std::vector<glm::dvec3> after = relocUserPoints();
    REQUIRE(after.size() == before.size());
    for (size_t i = 0; i < before.size(); ++i)
        CHECK(glm::length(after[i] - (before[i] + glm::dvec3(-2900, 0, -1000))) < 1e-6);
}

TEST_CASE("relocateScene refuses absolute Object Info and names the node")
{
    const std::string oi = relocObjectInfoScene("world");
    const std::vector<glm::dvec3> before = relocUserPoints();
    std::string message;
    try
    {
        rcall("relocateScene", {{"args", {"User"}}, {"from", "User"}});
    }
    catch (const CommandError& e)
    {
        message = e.what();
    }
    CHECK(message.find("Object Info node '" + oi + "'") != std::string::npos);
    CHECK(message.find("Relative To to owner") != std::string::npos);
    CHECK(relocUserPoints() == before);

    // Moving only the owner would pull it away from what it reads.
    rcall("gnSetValue", rargs({"Copy", oi + ".relativeTo", "owner"}));
    CHECK_THROWS_AS(rcall("relocateScene", {{"args", {"User"}}, {"from", "User"}, {"nodes", {"User"}}}), CommandError);
}
