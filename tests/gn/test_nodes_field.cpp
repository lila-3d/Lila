#include <doctest/doctest.h>

#include "gn/Exec.h"
#include "gn/NodesCommon.h"
#include "gn/Texture.h"
#include "geo/Mesh.h"

#include <cmath>

using namespace lila;
using json = nlohmann::json;

namespace fieldtests
{
using N = gn::GNode;
using L = gn::GLink;
constexpr double kPi = 3.14159265358979323846;

gn::Group makeGroup(std::vector<N> nodes, std::vector<L> links, std::vector<gn::GOutput> outputs = {})
{
    gn::Group g = gn::Group::makeDefault("T");
    g.links.clear();
    g.outputs = std::move(outputs);
    g.nodes.insert(g.nodes.end(), nodes.begin(), nodes.end());
    g.links = std::move(links);
    return g;
}

gn::GeometryPtr evalGeometry(std::vector<N> nodes, std::vector<L> links)
{
    gn::PlanPtr plan = gn::compile(makeGroup(std::move(nodes), std::move(links)));
    REQUIRE_MESSAGE(plan->error.empty(), plan->error);
    return gn::execute(*plan, {});
}

/** The group's outputs after the geometry, one per entry of outputs. */
std::vector<gn::Value> evalOutputs(std::vector<N> nodes, std::vector<L> links, std::vector<gn::GOutput> outputs)
{
    gn::PlanPtr plan = gn::compile(makeGroup(std::move(nodes), std::move(links), std::move(outputs)));
    REQUIRE_MESSAGE(plan->error.empty(), plan->error);
    std::vector<gn::Value> out = gn::executeOutputs(*plan, {});
    out.erase(out.begin());
    return out;
}

int64_t asInt(const gn::Value& v) { return gn::evaluateSingle<int64_t>(v.field); }
double asFloat(const gn::Value& v) { return gn::evaluateSingle<double>(v.field); }
glm::dvec3 asVec(const gn::Value& v) { return gn::evaluateSingle<glm::dvec3>(v.field); }

std::vector<float> pointFloats(const gn::GeometryPtr& g, const char* name)
{
    auto span = g->mesh->pointAttrs().get<float>(name);
    return {span.begin(), span.end()};
}

/** A Mesh Line of count points stepping by offset, then a float attribute stored from the source node's output. */
gn::GeometryPtr storedOnLine(int count, json offset, std::vector<N> extra, std::vector<L> links, const char* fromNode, const char* fromSocket)
{
    std::vector<N> nodes = {N{"Line", "MeshLine", {}, {{"count", count}, {"offset", offset}}}, N{"Store", "StoreNamedAttribute", {}, {{"name", "v"}}}};
    nodes.insert(nodes.end(), extra.begin(), extra.end());
    links.push_back(L{"Line", "mesh", "Store", "geometry"});
    links.push_back(L{fromNode, fromSocket, "Store", "value"});
    links.push_back(L{"Store", "geometry", "GroupOutput", "geometry"});
    return evalGeometry(std::move(nodes), std::move(links));
}

TEST_CASE("accumulate field")
{
    auto run = [](const char* socket) {
        return pointFloats(storedOnLine(4, json::array({0, 10, 0}), {N{"I", "Index", {}, {}}, N{"A", "AccumulateField", {}, {}}}, {L{"I", "value", "A", "value"}},
                                        "A", socket),
                           "v");
    };
    CHECK(run("leading") == std::vector<float>{0, 1, 3, 6});
    CHECK(run("trailing") == std::vector<float>{0, 0, 1, 3});
    CHECK(run("total") == std::vector<float>{6, 6, 6, 6});

    // Two groups, odd and even indices.
    auto grouped = pointFloats(storedOnLine(4, json::array({0, 10, 0}),
                                            {N{"I", "Index", {}, {}}, N{"M", "IntegerMath", {}, {{"operation", "modulo"}, {"b", 2}}}, N{"A", "AccumulateField", {}, {}}},
                                            {L{"I", "value", "A", "value"}, L{"I", "value", "M", "a"}, L{"M", "value", "A", "groupId"}}, "A", "leading"),
                               "v");
    CHECK(grouped == std::vector<float>{0, 1, 2, 4});
}

TEST_CASE("evaluate at index and on domain")
{
    // Each point reads the next point's index; the last is out of range and reads zero.
    auto next = pointFloats(storedOnLine(4, json::array({0, 10, 0}),
                                         {N{"I", "Index", {}, {}}, N{"Add", "IntegerMath", {}, {{"operation", "add"}, {"b", 1}}}, N{"E", "EvaluateAtIndex", {}, {}}},
                                         {L{"I", "value", "E", "value"}, L{"I", "value", "Add", "a"}, L{"Add", "value", "E", "index"}}, "E", "value"),
                            "v");
    CHECK(next == std::vector<float>{1, 2, 3, 0});

    // Face centres averaged back onto the points of a 2x2-face grid are half of each point's position.
    auto g = evalGeometry({N{"G", "MeshGrid", {}, {}}, N{"P", "Position", {}, {}}, N{"E", "EvaluateOnDomain", {}, {{"dataType", "vector"}, {"domain", "face"}}},
                           N{"S", "StoreNamedAttribute", {}, {{"name", "c"}, {"dataType", "vector"}}}},
                          {L{"G", "mesh", "S", "geometry"}, L{"P", "value", "E", "value"}, L{"E", "value", "S", "value"}, L{"S", "geometry", "GroupOutput", "geometry"}});
    auto centres = g->mesh->pointAttrs().get<glm::vec3>("c");
    REQUIRE(centres.size() == 9);
    for (geo::Index p = 0; p < 9; ++p)
        for (int k = 0; k < 3; ++k)
            CHECK(centres[p][k] == doctest::Approx(g->mesh->position(p)[k] * 0.5));
}

TEST_CASE("blur attribute")
{
    auto blurred = pointFloats(storedOnLine(3, json::array({0, 10, 0}), {N{"I", "Index", {}, {}}, N{"B", "BlurAttribute", {}, {}}}, {L{"I", "value", "B", "value"}}, "B", "value"), "v");
    CHECK(blurred[0] == doctest::Approx(0.5));
    CHECK(blurred[1] == doctest::Approx(1.0));
    CHECK(blurred[2] == doctest::Approx(1.5));

    auto none = pointFloats(storedOnLine(3, json::array({0, 10, 0}), {N{"I", "Index", {}, {}}, N{"B", "BlurAttribute", {}, {{"iterations", 0}}}},
                                         {L{"I", "value", "B", "value"}}, "B", "value"),
                            "v");
    CHECK(none == std::vector<float>{0, 1, 2});
}

TEST_CASE("remove named attribute")
{
    auto g = evalGeometry({N{"G", "MeshGrid", {}, {}}, N{"S1", "StoreNamedAttribute", {}, {{"name", "uvA"}, {"value", 1.0}}},
                           N{"S2", "StoreNamedAttribute", {}, {{"name", "keep"}, {"value", 2.0}}}, N{"R", "RemoveNamedAttribute", {}, {{"name", "uv*"}, {"pattern", "wildcard"}}}},
                          {L{"G", "mesh", "S1", "geometry"}, L{"S1", "geometry", "S2", "geometry"}, L{"S2", "geometry", "R", "geometry"}, L{"R", "geometry", "GroupOutput", "geometry"}});
    CHECK_FALSE(g->mesh->pointAttrs().has("uvA"));
    CHECK(g->mesh->pointAttrs().has("keep"));
}

TEST_CASE("integer, bit and hash nodes")
{
    using gn::SocketType;
    auto ints = evalOutputs({N{"A", "IntegerMath", {}, {{"operation", "divideFloor"}, {"a", -7}, {"b", 2}}},
                             N{"B", "IntegerMath", {}, {{"operation", "flooredModulo"}, {"a", -7}, {"b", 3}}},
                             N{"C", "IntegerMath", {}, {{"operation", "gcd"}, {"a", 12}, {"b", 18}}}, N{"D", "IntegerMath", {}, {{"operation", "power"}, {"a", 3}, {"b", 4}}},
                             N{"E", "IntegerMath", {}, {{"operation", "divide"}, {"a", 5}, {"b", 0}}}},
                            {L{"A", "value", "GroupOutput", "a"}, L{"B", "value", "GroupOutput", "b"}, L{"C", "value", "GroupOutput", "c"}, L{"D", "value", "GroupOutput", "d"},
                             L{"E", "value", "GroupOutput", "e"}},
                            {{"a", SocketType::Int, {}}, {"b", SocketType::Int, {}}, {"c", SocketType::Int, {}}, {"d", SocketType::Int, {}}, {"e", SocketType::Int, {}}});
    CHECK(asInt(ints[0]) == -4);
    CHECK(asInt(ints[1]) == 2);
    CHECK(asInt(ints[2]) == 6);
    CHECK(asInt(ints[3]) == 81);
    CHECK(asInt(ints[4]) == 0);

    auto bits = evalOutputs({N{"A", "BitMath", {}, {{"operation", "and"}, {"a", 12}, {"b", 10}}}, N{"B", "BitMath", {}, {{"operation", "shift"}, {"a", 1}, {"b", 4}}},
                             N{"C", "BitMath", {}, {{"operation", "not"}, {"a", 0}}}, N{"D", "BitMath", {}, {{"operation", "rotate"}, {"a", 1}, {"b", -1}}}},
                            {L{"A", "value", "GroupOutput", "a"}, L{"B", "value", "GroupOutput", "b"}, L{"C", "value", "GroupOutput", "c"}, L{"D", "value", "GroupOutput", "d"}},
                            {{"a", SocketType::Int, {}}, {"b", SocketType::Int, {}}, {"c", SocketType::Int, {}}, {"d", SocketType::Int, {}}});
    CHECK(asInt(bits[0]) == 8);
    CHECK(asInt(bits[1]) == 16);
    CHECK(asInt(bits[2]) == -1);
    CHECK(asInt(bits[3]) == int64_t(INT32_MIN));

    auto hashes = evalOutputs({N{"A", "HashValue", {}, {{"value", 5}, {"seed", 0}}}, N{"B", "HashValue", {}, {{"value", 5}, {"seed", 1}}}},
                              {L{"A", "hash", "GroupOutput", "a"}, L{"B", "hash", "GroupOutput", "b"}}, {{"a", SocketType::Int, {}}, {"b", SocketType::Int, {}}});
    CHECK(asInt(hashes[0]) == int64_t(gn::tex::hashU(5u, 0u)));
    CHECK(asInt(hashes[0]) != asInt(hashes[1]));
}

TEST_CASE("index and menu switch")
{
    using gn::SocketType;
    auto out = evalOutputs({N{"A", "IndexSwitch", {}, {{"dataType", "float"}, {"count", "3"}, {"index", 2}, {"item2", 7.5}}},
                            N{"B", "IndexSwitch", {}, {{"dataType", "float"}, {"count", "3"}, {"index", 5}, {"item2", 7.5}}},
                            N{"M", "MenuSwitch", {}, {{"dataType", "int"}, {"options", "Low, High"}, {"menu", "High"}, {"Low", 1}, {"High", 2}}},
                            N{"S", "IndexSwitch", {}, {{"dataType", "string"}, {"index", 1}, {"item0", "zero"}, {"item1", "one"}}}},
                           {L{"A", "output", "GroupOutput", "a"}, L{"B", "output", "GroupOutput", "b"}, L{"M", "output", "GroupOutput", "m"}, L{"S", "output", "GroupOutput", "s"}},
                           {{"a", SocketType::Float, {}}, {"b", SocketType::Float, {}}, {"m", SocketType::Int, {}}, {"s", SocketType::String, {}}});
    CHECK(asFloat(out[0]) == doctest::Approx(7.5));
    CHECK(asFloat(out[1]) == doctest::Approx(0.0));
    CHECK(asInt(out[2]) == 2);
    CHECK(out[3].string == "one");
}

TEST_CASE("rotation and transform nodes")
{
    using gn::SocketType;
    auto out = evalOutputs({N{"AA", "AxisAngleToRotation", {}, {{"axis", json::array({0, 0, 1})}, {"angle", kPi / 2}}}, N{"Back", "RotationToAxisAngle", {}, {}},
                            N{"Q", "EulerToQuaternion", {}, {{"rotation", json::array({0, 0, 90})}}},
                            N{"FromQ", "QuaternionToEuler", {}, {{"w", std::sqrt(0.5)}, {"z", std::sqrt(0.5)}}},
                            N{"Inv", "InvertRotation", {}, {{"rotation", json::array({0, 0, 90})}}},
                            N{"Rot", "RotateRotation", {}, {{"rotateBy", json::array({30, 0, 0})}}},
                            N{"TP", "TransformPoint", {}, {{"vector", json::array({1, 0, 0})}, {"translation", json::array({0, 0, 5})}, {"rotation", json::array({0, 0, 90})}}},
                            N{"TD", "TransformDirection", {}, {{"direction", json::array({1, 0, 0})}, {"scale", json::array({2, 2, 2})}, {"rotation", json::array({0, 0, 90})}}}},
                           {L{"AA", "rotation", "Back", "rotation"}, L{"Back", "angle", "GroupOutput", "angle"}, L{"Back", "axis", "GroupOutput", "axis"},
                            L{"Q", "w", "GroupOutput", "qw"}, L{"Q", "z", "GroupOutput", "qz"}, L{"FromQ", "rotation", "GroupOutput", "fromQ"},
                            L{"Inv", "rotation", "GroupOutput", "inv"}, L{"Rot", "rotation", "GroupOutput", "rot"}, L{"TP", "vector", "GroupOutput", "tp"},
                            L{"TD", "direction", "GroupOutput", "td"}},
                           {{"angle", SocketType::Float, {}}, {"axis", SocketType::Vector, {}}, {"qw", SocketType::Float, {}}, {"qz", SocketType::Float, {}},
                            {"fromQ", SocketType::Rotation, {}}, {"inv", SocketType::Rotation, {}}, {"rot", SocketType::Rotation, {}}, {"tp", SocketType::Vector, {}},
                            {"td", SocketType::Vector, {}}});
    CHECK(asFloat(out[0]) == doctest::Approx(kPi / 2));
    CHECK(asVec(out[1]).z == doctest::Approx(1.0));
    CHECK(asFloat(out[2]) == doctest::Approx(std::sqrt(0.5)));
    CHECK(asFloat(out[3]) == doctest::Approx(std::sqrt(0.5)));
    CHECK(asVec(out[4]).z == doctest::Approx(kPi / 2));
    CHECK(asVec(out[5]).z == doctest::Approx(-kPi / 2));
    CHECK(asVec(out[6]).x == doctest::Approx(kPi / 6));
    glm::dvec3 tp = asVec(out[7]);
    CHECK(tp.x == doctest::Approx(0.0).epsilon(1e-9));
    CHECK(tp.y == doctest::Approx(1.0));
    CHECK(tp.z == doctest::Approx(5.0));
    glm::dvec3 td = asVec(out[8]);
    CHECK(td.x == doctest::Approx(0.0).epsilon(1e-9));
    CHECK(td.y == doctest::Approx(2.0));
}

TEST_CASE("string nodes")
{
    using gn::SocketType;
    auto out = evalOutputs({N{"A", "String", {}, {{"value", "a"}}}, N{"B", "String", {}, {{"value", "b"}}}, N{"J", "JoinStrings", {}, {{"delimiter", ", "}}},
                            N{"R", "ReplaceString", {}, {{"string", "aXbX"}, {"find", "X"}, {"replace", "YY"}}},
                            N{"Sl", "SliceString", {}, {{"string", "h\xC3\xA9llo"}, {"position", 1}, {"length", 3}}},
                            N{"Len", "StringLength", {}, {{"string", "h\xC3\xA9llo"}}}, N{"V", "ValueToString", {}, {{"value", 3.14159}, {"decimals", 2}}}},
                           {L{"A", "value", "J", "strings"}, L{"B", "value", "J", "strings"}, L{"J", "string", "GroupOutput", "j"}, L{"R", "string", "GroupOutput", "r"},
                            L{"Sl", "string", "GroupOutput", "sl"}, L{"Len", "length", "GroupOutput", "len"}, L{"V", "string", "GroupOutput", "v"}},
                           {{"j", SocketType::String, {}}, {"r", SocketType::String, {}}, {"sl", SocketType::String, {}}, {"len", SocketType::Int, {}}, {"v", SocketType::String, {}}});
    CHECK(out[0].string == "a, b");
    CHECK(out[1].string == "aYYbYY");
    CHECK(out[2].string == "\xC3\xA9ll");
    CHECK(asInt(out[3]) == 5);
    CHECK(out[4].string == "3.14");
}

TEST_CASE("pattern textures")
{
    // Default scale 0.05: the origin is in an even cell, x = 25 cm in the next one.
    auto checker = pointFloats(storedOnLine(2, json::array({25, 0, 0}), {N{"C", "CheckerTexture", {}, {}}}, {}, "C", "fac"), "v");
    CHECK(checker == std::vector<float>{0, 1});

    auto ramp = pointFloats(storedOnLine(5, json::array({0.5, 0, 0}), {N{"G", "GradientTexture", {}, {}}}, {}, "G", "fac"), "v");
    CHECK(ramp[0] == doctest::Approx(0.0));
    CHECK(ramp[1] == doctest::Approx(0.5));
    CHECK(ramp[2] == doctest::Approx(1.0));
    CHECK(ramp[4] == doctest::Approx(1.0));

    auto white = storedOnLine(3, json::array({7, 3, 1}), {N{"W", "WhiteNoiseTexture", {}, {}}}, {}, "W", "value");
    auto values = pointFloats(white, "v");
    for (geo::Index p = 0; p < 3; ++p)
    {
        glm::dvec3 pos = white->mesh->position(p);
        CHECK(values[p] == doctest::Approx(gn::tex::hashToFloat(pos.x, pos.y, pos.z)));
    }

    // Sine bands start at their trough.
    auto wave = pointFloats(storedOnLine(1, json::array({0, 0, 0}), {N{"W", "WaveTexture", {}, {}}}, {}, "W", "fac"), "v");
    CHECK(wave[0] == doctest::Approx(0.0).epsilon(1e-6));

    // Mortar along the row line at y = 0, brick at y = 2.5 cm (0.125 scaled, half a row up).
    auto bricks = pointFloats(storedOnLine(2, json::array({0, 2.5, 0}), {N{"B", "BrickTexture", {}, {}}}, {}, "B", "fac"), "v");
    CHECK(bricks == std::vector<float>{1, 0});

    auto gabor = pointFloats(storedOnLine(20, json::array({3.7, 1.3, 0}), {N{"Gb", "GaborTexture", {}, {}}}, {}, "Gb", "value"), "v");
    for (float v : gabor)
        CHECK((v >= -0.5f && v <= 1.5f));
}
} // namespace fieldtests
