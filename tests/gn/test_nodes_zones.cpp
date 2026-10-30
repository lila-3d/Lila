#include <doctest/doctest.h>

#include "gn/Exec.h"
#include "gn/Gn.h"
#include "geo/Mesh.h"

using namespace lila;
using json = nlohmann::json;

namespace zones_test
{
using N = gn::GNode;
using L = gn::GLink;

/** Moves its geometry 1 along X and adds 1 to its count. */
gn::Group stepGroup()
{
    gn::Group g = gn::Group::makeDefault("Step");
    g.links.clear();
    g.inputs.push_back({.name = "count", .type = gn::SocketType::Float, .defaultValue = 0.0});
    g.outputs.push_back({.name = "count", .type = gn::SocketType::Float});
    g.nodes.push_back(N{"T", "TransformGeometry", {}, {{"translation", json::array({1, 0, 0})}}});
    g.nodes.push_back(N{"Add", "Math", {}, {{"b", 1.0}}});
    g.links = {L{"GroupInput", "geometry", "T", "geometry"}, L{"T", "geometry", "GroupOutput", "geometry"}, L{"GroupInput", "count", "Add", "a"},
               L{"Add", "value", "GroupOutput", "count"}};
    return g;
}

std::vector<gn::Value> evalOuter(const char* type, json values, const gn::Group& inner)
{
    gn::Group g = gn::Group::makeDefault("Outer");
    g.links.clear();
    g.subgroups[inner.name] = inner.toJson();
    g.outputs.push_back({.name = "count", .type = gn::SocketType::Float});
    values["group"] = inner.name;
    g.nodes.push_back(N{"C", "MeshCube", {}, {}});
    g.nodes.push_back(N{"Z", type, {}, values});
    g.links = {L{"C", "mesh", "Z", "geometry"}, L{"Z", "geometry", "GroupOutput", "geometry"}};
    if (inner.output("count"))
        g.links.push_back(L{"Z", "count", "GroupOutput", "count"});
    gn::PlanPtr plan = gn::compile(g);
    REQUIRE_MESSAGE(plan->error.empty(), plan->error);
    return gn::executeOutputs(*plan, {});
}
} // namespace zones_test

TEST_CASE("repeat zone feeds results back")
{
    using namespace zones_test;
    auto out = evalOuter("RepeatGroup", {{"iterations", 3}}, stepGroup());
    REQUIRE(out[0].geometry->mesh);
    CHECK(out[0].geometry->mesh->bounds().min.x == doctest::Approx(-50.0 + 3.0));
    CHECK(gn::evaluateSingle<double>(out[1].field) == doctest::Approx(3.0));

    auto none = evalOuter("RepeatGroup", {{"iterations", 0}, {"count", 7.0}}, stepGroup());
    CHECK(none[0].geometry->mesh->bounds().min.x == doctest::Approx(-50.0));
    CHECK(gn::evaluateSingle<double>(none[1].field) == doctest::Approx(7.0));
}

TEST_CASE("for each element runs per face and joins")
{
    using namespace zones_test;
    gn::Group inner = gn::Group::makeDefault("PerFace");
    auto out = evalOuter("ForEachElement", {{"element", "face"}}, inner);
    REQUIRE(out[0].geometry->mesh);
    CHECK(out[0].geometry->mesh->numFaces() == 6);
    CHECK(out[0].geometry->mesh->numPoints() == 24);

    auto parts = evalOuter("ForEachElement", {{"element", "part"}}, stepGroup());
    CHECK(parts[0].geometry->mesh->numFaces() == 6);
    CHECK(parts[0].geometry->mesh->bounds().min.x == doctest::Approx(-49.0));
}
