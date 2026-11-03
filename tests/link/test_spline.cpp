#include <doctest/doctest.h>

#include "TestUtil.h"
#include "FakeServer.h"

#include "core/MainQueue.h"
#include "core/PrefStore.h"
#include "curve/CurveNodes.h"
#include "link/SplineSend.h"
#include "link/UnrealLink.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"

#include <filesystem>
#include <mutex>

using namespace lila;
using namespace lila::link;
using namespace linktest;
using json = nlohmann::json;
using D3 = glm::dvec3;
namespace fs = std::filesystem;

namespace
{
void setupSplineTest()
{
    static bool once = [] {
        mainq::init();
        ops::registerBuiltins();
        scene::types::registerBuiltins();
        scene::registerCommands();
        curve::registerTypes();
        curve::registerCommands();
        link::registerCommands();
        return true;
    }();
    (void)once;
}

D3 at(const json& v) { return D3(v[0].get<double>(), v[1].get<double>(), v[2].get<double>()); }

/** A LilaLink editor that answers hello (with or without spline support) and records setSpline and removeSpline. */
FakeServer::Handler splineEditor(bool splines)
{
    return [splines](const json& req) -> std::optional<json> {
        const std::string m = req["method"];
        if (m == "hello")
        {
            json features = {"progress", "materials"};
            if (splines)
                features.push_back("splines");
            return FakeServer::reply(req, {{"project", "Roads"}, {"projectDir", "C:/Projects/Roads/"}, {"pluginVersion", "0.6.0"}, {"features", features}});
        }
        if (m == "setSpline")
            return FakeServer::reply(req, {{"actor", "/Game/Maps/Main.Main:PersistentLevel.Spline_1"}, {"components", 1}, {"warnings", json::array()}});
        if (m == "removeSpline")
            return FakeServer::reply(req, {{"removed", true}});
        return json{{"jsonrpc", "2.0"}, {"id", req["id"]}, {"error", {{"code", -32601}, {"message", "Unknown method"}}}};
    };
}

std::vector<json> requestsOf(FakeServer& s, const char* method)
{
    std::vector<json> out;
    for (const json& r : s.requests())
        if (r["method"] == method)
            out.push_back(r);
    return out;
}
} // namespace

TEST_CASE("splinePoints: Unreal axes, Hermite tangents from Bezier handles, up from tilt, scale from radius")
{
    setupSplineTest();
    // Y-up Lila (forward +Z, right -X, up +Y) maps onto Unreal X, Y, Z: (x, y, z) -> (z, -x, y).
    curve::Spline s;
    s.kind = curve::Kind::Bezier;
    s.points = {D3(0, 0, 0), D3(0, 0, 100)};
    s.handleL = {D3(0, 0, -30), D3(0, 0, 70)};
    s.handleR = {D3(0, 0, 30), D3(10, 0, 130)};
    s.typeL = s.typeR = {curve::HandleType::Free, curve::HandleType::Free};
    s.tilt = {0.0, 1.5707963267948966};
    s.radius = {1.0, 2.5};
    json out = splinePoints({s}, D3(0, 0, 50));
    REQUIRE(out.size() == 1);
    const json& pts = out[0]["points"];
    REQUIRE(pts.size() == 2);
    CHECK(out[0]["closed"] == false);
    CHECK(glm::distance(at(pts[0]["position"]), D3(-50, 0, 0)) < 1e-9);
    CHECK(glm::distance(at(pts[1]["position"]), D3(50, 0, 0)) < 1e-9);
    // Leave tangent of the first point: 3 * (0, 0, 30) in Lila is (90, 0, 0) in Unreal; its open start arrives the same way.
    CHECK(glm::distance(at(pts[0]["leaveTangent"]), D3(90, 0, 0)) < 1e-9);
    CHECK(glm::distance(at(pts[0]["arriveTangent"]), D3(90, 0, 0)) < 1e-9);
    CHECK(glm::distance(at(pts[1]["arriveTangent"]), D3(90, 0, 0)) < 1e-9);
    // Untilted, up is world up; a quarter turn about +Z (Unreal +X) points it along Lila's -X... Unreal +Y.
    CHECK(glm::distance(at(pts[0]["up"]), D3(0, 0, 1)) < 1e-9);
    D3 up1 = at(pts[1]["up"]);
    CHECK(std::abs(glm::dot(up1, glm::normalize(at(pts[1]["leaveTangent"])))) < 1e-9);
    CHECK(glm::length(up1) == doctest::Approx(1.0));
    CHECK(pts[1]["roll"].get<double>() == doctest::Approx(-90.0));
    CHECK(glm::distance(at(pts[1]["scale"]), D3(1.0, 2.5, 2.5)) < 1e-12);
    CHECK(glm::distance(at(pts[0]["scale"]), D3(1.0)) < 1e-12);

    // A poly square: straight spans have the chord as both tangents, so Unreal draws them straight.
    curve::Spline sq;
    sq.kind = curve::Kind::Poly;
    sq.closed = true;
    sq.points = {D3(0), D3(100, 0, 0), D3(100, 0, 100), D3(0, 0, 100)};
    sq.normalize();
    json square = splinePoints({sq}, D3(0));
    REQUIRE(square.size() == 1);
    CHECK(square[0]["closed"] == true);
    REQUIRE(square[0]["points"].size() == 4);
    const D3 chord = at(square[0]["points"][1]["position"]) - at(square[0]["points"][0]["position"]);
    CHECK(glm::distance(at(square[0]["points"][0]["leaveTangent"]), chord) < 1e-9);
    CHECK(glm::distance(at(square[0]["points"][1]["arriveTangent"]), chord) < 1e-9);

    // A cubic NURBS goes as the Bezier through the same shape.
    curve::Spline nurbs;
    nurbs.kind = curve::Kind::Nurbs;
    nurbs.degree = 3;
    nurbs.points = {D3(0), D3(30, 20, 0), D3(60, -20, 0), D3(90, 10, 0), D3(120, 0, 0)};
    json n = splinePoints({nurbs}, D3(0));
    REQUIRE(n.size() == 1);
    CHECK(n[0]["points"].size() >= 2);
    CHECK(glm::distance(at(n[0]["points"].back()["position"]), D3(0, -120, 0)) < 1e-6);

    // Nothing to send for an empty list or a degenerate spline.
    CHECK(splinePoints({}, D3(0)).empty());
}

TEST_CASE("a curve mapped as a spline is sent with setSpline on save, under a stable id")
{
    setupSplineTest();
    fs::path ws = tempDir("splines");
    PrefStore::get().setWorkspaceDir(ws / ".lila");
    scene::Graph& g = scene::Graph::current();
    std::string road = ops::execute("curve", {{"type", "bezier"}, {"points", {{0, 0, 0}, {500, 0, 0}, {1000, 0, 300}}}, {"name", "Road"}});
    scene::Node* curve = g.find(road);
    REQUIRE(curve);

    json m = ops::execute("unrealMap", {{"args", {road}}, {"kind", "spline"}, {"splineType", "landscape"}, {"width", 350.0}});
    CHECK(m["kind"] == "spline");
    REQUIRE(mappings().size() == 1);
    CHECK(mappings()[0]["splineId"] == curve->fullPath());
    // A mesh cannot be mapped as a spline; a spline needs no contentPath.
    std::string cube = ops::execute("polyCube", json::object())[0];
    CHECK_THROWS_AS(ops::execute("unrealMap", {{"args", {cube}}, {"kind", "spline"}}), CommandError);

    FakeServer server(splineEditor(true));
    Link::ConnectOptions o;
    o.port = server.port();
    Link::get().connect(o);

    onSceneSaved();
    Link::get().waitIdle();
    mainq::drain();
    std::vector<json> sent = requestsOf(server, "setSpline");
    REQUIRE(sent.size() == 1);
    const json& p = sent[0]["params"];
    CHECK(p["id"] == curve->fullPath());
    CHECK(p["label"] == curve->name());
    CHECK(p["mode"] == "landscape");
    CHECK(p["width"] == 350.0);
    REQUIRE(p["splines"].size() == 1);
    CHECK(p["splines"][0]["points"].size() == 3);

    // A re-send keeps the id the mapping was made with; the spline moves with the curve.
    ops::execute("move", {{"args", {road}}, {"by", {0, 100, 0}}});
    ops::execute("unrealSync", json::object());
    Link::get().waitIdle();
    mainq::drain();
    sent = requestsOf(server, "setSpline");
    REQUIRE(sent.size() == 2);
    CHECK(sent[1]["params"]["id"] == sent[0]["params"]["id"]);
    CHECK(at(sent[1]["params"]["location"]).z == doctest::Approx(at(sent[0]["params"]["location"]).z + 100.0));

    CHECK(ops::execute("unrealUnmap", {{"args", {road}}}) == 1);
    Link::get().disconnect();
    PrefStore::get().setWorkspaceDir({});
    fs::remove_all(ws);
}

TEST_CASE("unrealSendCurve sends now, maps for later saves, resolves spline meshes and removes")
{
    setupSplineTest();
    fs::path ws = tempDir("sendcurve");
    PrefStore::get().setWorkspaceDir(ws / ".lila");
    scene::Graph& g = scene::Graph::current();
    std::string fence = ops::execute("curve", {{"type", "poly"}, {"points", {{0, 0, 0}, {200, 0, 0}, {200, 0, 200}}}, {"name", "Fence"}});
    std::string post = ops::execute("polyCube", {{"name", "FencePost"}})[0];
    ops::execute("unrealMap", {{"args", {post}}, {"contentPath", "/Game/Props/SM_FencePost"}});

    {
        // An editor whose plugin predates splines is refused with a clear error.
        FakeServer old(splineEditor(false));
        Link::ConnectOptions o;
        o.port = old.port();
        Link::get().connect(o);
        CHECK_THROWS_AS(ops::execute("unrealSendCurve", {{"args", {fence}}, {"wait", true}}), CommandError);
        Link::get().disconnect();
    }

    FakeServer server(splineEditor(true));
    Link::ConnectOptions o;
    o.port = server.port();
    Link::get().connect(o);
    json replies = ops::execute("unrealSendCurve", {{"args", {fence}}, {"mesh", post}, {"meshAxis", "y"}, {"wait", true}});
    REQUIRE(replies.size() == 1);
    CHECK(replies[0]["components"] == 1);
    std::vector<json> sent = requestsOf(server, "setSpline");
    REQUIRE(sent.size() == 1);
    CHECK(sent[0]["params"]["mode"] == "actor");
    CHECK(sent[0]["params"]["mesh"]["asset"] == "/Game/Props/SM_FencePost");
    CHECK(sent[0]["params"]["mesh"]["forwardAxis"] == "y");
    bool mapped = false;
    for (const json& m : mappings())
        mapped = mapped || (m["kind"] == "spline" && m["splineMesh"] == post);
    CHECK(mapped);

    // A mesh that was never sent cannot be the spline mesh.
    std::string loose = ops::execute("polyCube", json::object())[0];
    CHECK_THROWS_AS(ops::execute("unrealSendCurve", {{"args", {fence}}, {"mesh", loose}, {"wait", true}}), CommandError);
    CHECK_THROWS_AS(ops::execute("unrealSendCurve", {{"args", {post}}, {"wait", true}}), CommandError);

    json removed = ops::execute("unrealSendCurve", {{"args", {fence}}, {"remove", true}, {"wait", true}});
    REQUIRE(removed.size() == 1);
    std::vector<json> gone = requestsOf(server, "removeSpline");
    REQUIRE(gone.size() == 1);
    CHECK(gone[0]["params"]["id"] == g.find(fence)->fullPath());
    for (const json& m : mappings())
        CHECK(m["kind"] != "spline");

    Link::get().disconnect();
    PrefStore::get().setWorkspaceDir({});
    fs::remove_all(ws);
}
