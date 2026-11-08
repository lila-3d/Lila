#include "FakeServer.h"

#include "link/PcgSend.h"
#include "link/RpcClient.h"
#include "math/Unreal.h"
#include "math/Vector.h"

#include <doctest/doctest.h>
#include <glm/gtc/matrix_transform.hpp>

using namespace lila::link;
using json = nlohmann::json;

namespace
{
json unreal(double x, double y, double z)
{
    const lila::math::Vector v = lila::math::toUnreal(lila::math::Vector(x, y, z));
    return json::array({v.x, v.y, v.z});
}

std::vector<PcgSet> sampleSets()
{
    PcgSet pts;
    pts.pin = "SeamPoints";
    pts.tag = "seam";
    PcgPoint p;
    p.world = glm::translate(glm::dmat4(1.0), glm::dvec3(10, 20, 30));
    p.density = 0.5;
    p.attributes = {{"Angle", 90.0}, {"Kind", "ground"}, {"NormalA", json::array({0, 1, 0})}};
    pts.points.push_back(p);
    pts.attributes = {{"Source", "seamFind"}};

    PcgSet line;
    line.kind = PcgSet::Kind::Spline;
    line.pin = "SeamCurves";
    line.path = {{0, 0, 0}, {100, 0, 0}, {100, 0, 100}};
    line.closed = true;
    return {pts, line};
}
} // namespace

TEST_CASE("pcgDataParams: points and splines in Unreal space, attributes kept")
{
    const json j = pcgDataParams("/Game/Seams/PCG_Seams", "Seams", sampleSets());
    CHECK(j["asset"] == "/Game/Seams/PCG_Seams");
    REQUIRE(j["sets"].size() == 2);
    const json& pts = j["sets"][0];
    CHECK(pts["kind"] == "points");
    CHECK(pts["pin"] == "SeamPoints");
    CHECK(pts["attributes"]["Source"] == "seamFind");
    REQUIRE(pts["points"].size() == 1);
    const json& p = pts["points"][0];
    CHECK(p["location"] == unreal(10, 20, 30));
    CHECK(p["density"] == 0.5);
    CHECK(p["attributes"]["Angle"] == 90.0);
    CHECK(p["attributes"]["Kind"] == "ground");
    CHECK(p["attributes"]["NormalA"] == unreal(0, 1, 0));
    const json& line = j["sets"][1];
    CHECK(line["kind"] == "spline");
    CHECK(line["closed"] == true);
    REQUIRE(line["path"].size() == 3);
    CHECK(line["path"][2] == unreal(100, 0, 100));
}

TEST_CASE("sendPcgData: needs the pcgData feature, then calls setPcgData")
{
    FakeServer server([](const json& req) -> std::optional<json> {
        if (req.value("method", "") == "setPcgData")
            return FakeServer::reply(req, {{"asset", req["params"]["asset"]}, {"sets", req["params"]["sets"].size()}});
        return FakeServer::reply(req, nullptr);
    });
    RpcClient client;
    REQUIRE(client.connect("127.0.0.1", server.port(), 1000));
    CHECK_THROWS(sendPcgData(client, json::array({"placements"}), "/Game/A", "A", sampleSets()));
    const json r = sendPcgData(client, json::array({"pcgData"}), "/Game/A", "A", sampleSets());
    CHECK(r["sets"] == 2);
}
