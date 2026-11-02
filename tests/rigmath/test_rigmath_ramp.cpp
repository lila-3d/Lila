#include <doctest/doctest.h>

#include "rigmath/MathNodes.h"
#include "scene/Graph.h"

#include <nlohmann/json.hpp>

using namespace lila;
using scene::PlugValue;
using D3 = glm::dvec3;
using json = nlohmann::json;

namespace
{
scene::Graph& graph() { return scene::Graph::current(); }

std::string ramp(const char* type)
{
    const scene::NodeType* t = scene::types::find(type);
    REQUIRE_MESSAGE(t, type);
    return graph().createNode(*t)->name();
}

void put(const std::string& node, const char* attr, PlugValue v) { graph().setValue(graph().plug(node + "." + attr), std::move(v)); }
PlugValue read(const std::string& node, const char* attr) { return graph().value(graph().plug(node + "." + attr)); }

double at(const std::string& n, double value)
{
    put(n, "value", value);
    return std::get<double>(read(n, "result"));
}

D3 at3(const std::string& n, double value)
{
    put(n, "value", value);
    return std::get<D3>(read(n, "result"));
}
} // namespace

TEST_CASE("evaluateCurve: a float ramp maps value through its keys and ranges")
{
    graph().clear();
    const std::string n = ramp("evaluateCurveFloat");
    const rigmath::Overload* o = rigmath::overloadFor("evaluateCurveFloat");
    REQUIRE(o);
    CHECK(o->inputs[1].kind == rigmath::Kind::Curve);
    const scene::NodeType* type = scene::types::find("evaluateCurveFloat");
    CHECK(type->attrs[size_t(type->attrIndex("curve"))].type == scene::PlugType::String);

    // The default ramp is linear 0..1, so it passes value through.
    CHECK(json::parse(std::get<std::string>(read(n, "curve"))).size() == 2);
    CHECK(at(n, 0.25) == doctest::Approx(0.25));

    // Source and target ranges remap around the curve.
    put(n, "sourceMinimum", 10.0);
    put(n, "sourceMaximum", 20.0);
    put(n, "targetMinimum", -1.0);
    put(n, "targetMaximum", 3.0);
    CHECK(at(n, 15.0) == doctest::Approx(1.0));
    CHECK(at(n, 30.0) == doctest::Approx(3.0)); // constant past the last key

    // Compact keys: a linear tent.
    put(n, "sourceMinimum", 0.0);
    put(n, "sourceMaximum", 1.0);
    put(n, "targetMinimum", 0.0);
    put(n, "targetMaximum", 1.0);
    put(n, "curve", std::string(R"([[0, 0, "linear"], [0.5, 1, "linear"], [1, 0, "linear"]])"));
    CHECK(at(n, 0.25) == doctest::Approx(0.5));
    CHECK(at(n, 0.5) == doctest::Approx(1.0));
    CHECK(at(n, 0.75) == doctest::Approx(0.5));
    CHECK(at(n, -2.0) == doctest::Approx(0.0));

    // keysToJson's own form, with a stepped segment and an auto (eased) one.
    put(n, "curve", std::string(R"([{"t": 0, "v": 0, "o": "step"}, {"t": 0.5, "v": 1}, {"t": 1, "v": 0}])"));
    CHECK(at(n, 0.4) == doctest::Approx(0.0));
    CHECK(at(n, 0.5) == doctest::Approx(1.0));
    put(n, "curve", std::string(R"([[0, 0], [1, 1]])"));
    CHECK(at(n, 0.5) == doctest::Approx(0.5));
    CHECK(at(n, 0.2) < 0.2); // auto tangents are flat at the ends

    // Malformed keys leave the output at its default.
    put(n, "curve", std::string("not json"));
    CHECK(at(n, 0.5) == doctest::Approx(0.0));
}

TEST_CASE("evaluateCurve: vector and colour ramps take a curve per channel")
{
    graph().clear();
    const std::string v = ramp("evaluateCurveVector");
    const D3 lin = at3(v, 0.5);
    CHECK(lin.x == doctest::Approx(0.5));
    CHECK(lin.z == doctest::Approx(0.5));

    put(v, "curve", std::string(R"([[[0, 0, "linear"], [1, 2, "linear"]], [[0, 1], [1, 1]], [[0, 0, "linear"], [1, -4, "linear"]]])"));
    const D3 p = at3(v, 0.25);
    CHECK(p.x == doctest::Approx(0.5));
    CHECK(p.y == doctest::Approx(1.0));
    CHECK(p.z == doctest::Approx(-1.0));

    // Colour: one list shared by every channel, over a source range, driven through a connection.
    const std::string c = ramp("evaluateCurveColor");
    const scene::NodeType* type = scene::types::find("evaluateCurveColor");
    CHECK((type->attrs[size_t(type->attrIndex("result"))].flags & scene::AttrColor) != 0);
    put(c, "curve", std::string(R"([[0, 0.2, "linear"], [1, 0.8, "linear"]])"));
    put(c, "sourceMaximum", 10.0);
    const std::string src = graph().createNode(*scene::types::find("transform"), "rampSrc")->name();
    graph().connect(graph().plug(src + ".translateX"), graph().plug(c + ".value"));
    put(src, "translate", D3(5.0, 0.0, 0.0));
    graph().evaluate();
    const D3 col = std::get<D3>(read(c, "result"));
    CHECK(col.x == doctest::Approx(0.5));
    CHECK(col.y == doctest::Approx(0.5));
    CHECK(col.z == doctest::Approx(0.5));

    // Two channels is neither one list nor three.
    put(v, "curve", std::string(R"([[[0, 0], [1, 1]], [[0, 0], [1, 1]]])"));
    const D3 bad = at3(v, 0.5);
    CHECK(bad.x == doctest::Approx(0.0));
}
