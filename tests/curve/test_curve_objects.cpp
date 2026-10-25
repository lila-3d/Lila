#include <doctest/doctest.h>

#include "curve/CurveNodes.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/NodeBounds.h"

using namespace lila;
using json = nlohmann::json;
using D3 = glm::dvec3;

namespace
{
json run(const char* cmd, json call = json::object()) { return ops::execute(cmd, call); }

json pts(std::initializer_list<D3> list)
{
    json a = json::array();
    for (const D3& p : list)
        a.push_back({p.x, p.y, p.z});
    return a;
}

D3 vec(const json& j) { return D3(j[0].get<double>(), j[1].get<double>(), j[2].get<double>()); }

scene::NodeId idOf(const std::string& name)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::Node* n = g.find(name);
    REQUIRE(n);
    return n->id();
}

void checkNear(const D3& a, const D3& b)
{
    for (int k = 0; k < 3; ++k)
        CHECK(a[k] == doctest::Approx(b[k]).epsilon(1e-9));
}
} // namespace

TEST_CASE("node bounds cover curves and meshes")
{
    run("newScene");
    std::string c = run("curve", {{"type", "poly"}, {"points", pts({D3(0), D3(10, 0, 0), D3(10, 20, -4)})}, {"name", "P"}});
    run("move", {{"args", {c}}, {"by", {100, 0, 0}}});
    scene::Graph& g = scene::Graph::current();
    geo::Bounds local = scene::objectBounds(g, idOf(c));
    REQUIRE(local.valid());
    checkNear(local.min, D3(0, 0, -4));
    checkNear(local.max, D3(10, 20, 0));
    geo::Bounds world = scene::worldBounds(g, idOf(c));
    checkNear(world.min, D3(100, 0, -4));
    checkNear(world.max, D3(110, 20, 0));

    std::string grp = run("group", {{"empty", true}});
    CHECK(!scene::objectBounds(g, idOf(grp)).valid());
}

TEST_CASE("center pivot and freeze transforms on curves")
{
    run("newScene");
    std::string c = run("curve", {{"type", "poly"}, {"points", pts({D3(0), D3(10, 0, 0), D3(10, 20, 0)})}, {"name", "P"}});
    run("move", {{"args", {c}}, {"by", {100, 0, 0}}});
    run("centerPivot", {{"args", {c}}});
    checkNear(vec(run("pivot", {{"args", {c}}})), D3(105, 10, 0));
    run("undo");
    checkNear(vec(run("pivot", {{"args", {c}}})), D3(100, 0, 0));

    run("rotate", {{"args", {c}}, {"by", {0, 0, 90}}});
    run("freezeTransforms", {{"args", {c}}});
    CHECK(run("getAttr", {{"args", {c + ".tx"}}}).get<double>() == doctest::Approx(0));
    CHECK(run("getAttr", {{"args", {c + ".rz"}}}).get<double>() == doctest::Approx(0));
    json frozen = run("curveData", {{"args", {c}}})[0]["points"];
    REQUIRE(frozen.size() == 3);
    // A 90 degree turn about Z at the pivot (100, 0, 0): (10, 0, 0) lands on (100, 10, 0).
    checkNear(vec(frozen[0]), D3(100, 0, 0));
    checkNear(vec(frozen[1]), D3(100, 10, 0));
    checkNear(vec(frozen[2]), D3(80, 10, 0));
    checkNear(vec(run("pivot", {{"args", {c}}})), D3(100, 0, 0));
    run("undo");
    CHECK(run("curveData", {{"args", {c}}})[0]["points"][1] == json::array({10.0, 0.0, 0.0}));
}
