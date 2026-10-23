#include <doctest/doctest.h>

#include "geo/Primitives.h"
#include "geo/SmartJoin.h"
#include "ops/Command.h"

#include <glm/gtc/constants.hpp>

#include <cmath>

using namespace lila;
using json = nlohmann::json;

namespace
{
/** Lays a Y-up cylinder along +X starting at x0 (its bottom cap ends up at x0). */
geo::Mesh arm(int segments, double radius, double length, double x0, bool caps = true)
{
    geo::Mesh m = geo::makeCylinder({.radius = radius, .height = length, .subdivAxis = segments, .caps = caps, .baseAtOrigin = true});
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
        {
            const glm::dvec3 q = m.position(p);
            m.setPosition(p, {q.y + x0, -q.x, q.z});
        }
    return m;
}

int boundaryEdges(const geo::Mesh& m)
{
    int n = 0;
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.isBoundaryEdge(e))
            ++n;
    return n;
}

int facesOfSize(const geo::Mesh& m, bool (*pred)(geo::Index))
{
    int n = 0;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f) && pred(m.faceSize(f)))
            ++n;
    return n;
}

json run(const char* name, const json& call = nlohmann::json::object()) { return ops::execute(name, call); }

geo::Mesh body8() { return geo::makeCylinder({.radius = 50.0, .height = 200.0, .subdivAxis = 8}); }
} // namespace

TEST_CASE("smartJoin: perpendicular 8-segment cylinders")
{
    const geo::Mesh body = body8();
    const geo::Mesh limb = arm(8, 20.0, 100.0, 30.0); // pokes about 16 cm into the body
    geo::SmartJoinResult r = geo::smartJoin(limb, body);
    INFO(r.summary);
    REQUIRE(r.ok);
    CHECK(r.confidence >= 90);
    CHECK(r.limbLoopSize == 8);
    CHECK(r.holeSize == 8);
    CHECK(r.trimmed);
    CHECK(r.triangles == 0);
    CHECK(r.ngons == 0);
    CHECK(r.mesh.validate().empty());
    CHECK(boundaryEdges(r.mesh) == 0);
    // Only the three caps are not quads.
    CHECK(facesOfSize(r.mesh, [](geo::Index s) { return s != 4; }) == 3);
    CHECK(double(r.preservedPoints) / r.originalPoints >= 0.7);
    // Nothing of the limb is left inside the body.
    for (geo::Index p = 0; p < r.mesh.pointCapacity(); ++p)
        if (r.mesh.pointValid(p))
        {
            const glm::dvec3 q = r.mesh.position(p);
            CHECK(std::hypot(q.x, q.z) > 50.0 * std::cos(glm::pi<double>() / 8.0) - 1e-3);
        }
}

TEST_CASE("smartJoin: open-ended limb near the body uses its border")
{
    const geo::Mesh body = body8();
    const geo::Mesh limb = arm(8, 20.0, 100.0, 60.0, false);
    geo::SmartJoinResult r = geo::smartJoin(limb, body, {.segments = 3});
    INFO(r.summary);
    REQUIRE(r.ok);
    CHECK_FALSE(r.trimmed);
    CHECK(r.segments == 3);
    CHECK(r.confidence >= 85);
    CHECK(r.triangles == 0);
    CHECK(r.mesh.validate().empty());
    // The far end stays open; everything else is closed.
    CHECK(boundaryEdges(r.mesh) == 8);
}

TEST_CASE("smartJoin: mismatched and grazing joins rate lower")
{
    const geo::Mesh body = body8();
    geo::SmartJoinResult good = geo::smartJoin(arm(8, 20.0, 100.0, 30.0), body);
    geo::SmartJoinResult odd = geo::smartJoin(arm(7, 20.0, 100.0, 30.0), body);
    INFO(odd.summary);
    REQUIRE(odd.ok);
    CHECK(odd.confidence < good.confidence);
    CHECK(odd.triangles > 0);
    CHECK(odd.mesh.validate().empty());

    // A limb tilted 60 degrees off the body normal.
    geo::Mesh tilted = arm(8, 20.0, 100.0, 0.0, false);
    const double c = std::cos(glm::radians(60.0)), s = std::sin(glm::radians(60.0));
    for (geo::Index p = 0; p < tilted.pointCapacity(); ++p)
        if (tilted.pointValid(p))
        {
            const glm::dvec3 q = tilted.position(p);
            tilted.setPosition(p, glm::dvec3(q.x * c - q.y * s, q.x * s + q.y * c, q.z) + glm::dvec3(70.0, 0.0, 0.0));
        }
    geo::SmartJoinResult graze = geo::smartJoin(tilted, body);
    MESSAGE(graze.summary);
    REQUIRE(graze.ok);
    CHECK(graze.confidence < good.confidence);
    CHECK(graze.mesh.validate().empty());
}

TEST_CASE("smartJoin: parameters")
{
    const geo::Mesh body = body8();
    const geo::Mesh limb = arm(8, 20.0, 100.0, 30.0);
    for (int twist : {0, 1, 3})
        for (double scale : {0.8, 1.0, 1.2})
        {
            geo::SmartJoinResult r = geo::smartJoin(limb, body, {.segments = 4, .tension = 1.5, .twist = twist, .holeScale = scale, .relax = 2});
            INFO(r.summary);
            REQUIRE(r.ok);
            CHECK(r.mesh.validate().empty());
            CHECK(r.triangles == 0);
        }
    geo::SmartJoinResult kept = geo::smartJoin(limb, body, {.deleteBuried = false});
    REQUIRE(kept.ok);
    for (const std::string& issue : kept.mesh.validate())
        MESSAGE(issue);
    CHECK(kept.mesh.validate().empty());
    // The detached buried part keeps its own open border.
    CHECK(boundaryEdges(kept.mesh) == 8);
    geo::SmartJoinResult moved = geo::smartJoin(limb, body, {.offsetV = 10.0});
    REQUIRE(moved.ok);
    CHECK(moved.mesh.validate().empty());
}

TEST_CASE("smartJoin command: undoable, scriptable")
{
    run("newScene");
    std::string body = run("polyCylinder", {{"radius", 50}, {"height", 200}, {"subdivisionsAxis", 8}})[0];
    std::string limb = run("polyCylinder", {{"radius", 20}, {"height", 100}, {"subdivisionsAxis", 8}, {"position", {80, 0, 0}}})[0];
    run("rotate", {{"args", {limb}}, {"to", {0, 0, 90}}});
    // Selection order does not matter: the smaller mesh is the limb.
    json r = run("smartJoin", {{"args", {body, limb}}, {"name", "Joined"}});
    INFO(r.dump());
    CHECK(r["limb"] == limb);
    CHECK(r["confidence"].get<int>() >= 90);
    CHECK(r["triangles"] == 0);
    CHECK(run("objExists", {{"args", {"Joined"}}}) == true);
    CHECK(run("objExists", {{"args", {limb}}}) == false);
    json info = run("meshInfo", {{"args", {"Joined"}}});
    CHECK(info["faces"].get<int>() > 24);

    run("undo");
    CHECK(run("objExists", {{"args", {"Joined"}}}) == false);
    CHECK(run("objExists", {{"args", {limb}}}) == true);
    CHECK(run("objExists", {{"args", {body}}}) == true);
    CHECK(run("meshInfo", {{"args", {body}}})["faces"].get<int>() == 10);
    run("redo");
    CHECK(run("objExists", {{"args", {"Joined"}}}) == true);
    run("undo");

    json kept = run("smartJoin", {{"args", {limb, body}}, {"keepOriginals", true}, {"segments", 2}});
    CHECK(kept["segments"] == 2);
    CHECK(run("objExists", {{"args", {limb}}}) == true);
    CHECK_THROWS(run("smartJoin", {{"args", {limb}}}));
}
