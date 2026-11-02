#include <doctest/doctest.h>

#include "ops/Command.h"
#include "geo/Mesh.h"
#include "scene/Graph.h"
#include "scene/NodeBounds.h"
#include "scene/Transform.h"
#include "scene/Turntable.h"

#include <string_view>

using namespace lila;
using namespace lila::scene;
namespace tt = lila::scene::turntable;

namespace
{
json ttbRun(const char* name, json call = json::object()) { return ops::execute(name, call); }
Node& ttbNode(const std::string& name) { return *Graph::current().find(name); }
} // namespace

TEST_CASE("turntable base: every style builds under the model, the foot of a lazy susan stays still")
{
    for (const char* style : {"plinth", "stepped", "record", "pedestal", "lazySusan"})
    {
        CAPTURE(style);
        ttbRun("newScene");
        const json made = ttbRun("polyCube");
        const std::string a = (made.is_array() ? made[0] : made).get<std::string>();
        ttbRun("move", {{"args", {a}}, {"to", {3.0, 5.0, 0.0}}});
        const json r = ttbRun("turntableBase", {{"args", {a}}, {"style", style}, {"material", "wood"}});
        REQUIRE(r.size() == 4);
        Graph& g = Graph::current();
        g.evaluate();
        const geo::Bounds cubeBox = worldBounds(g, ttbNode(a).id());
        const geo::Bounds baseBox = worldBounds(g, ttbNode(r[1].get<std::string>()).id());
        REQUIRE(baseBox.valid());
        CHECK(baseBox.size().x > cubeBox.size().x);
        const NodeId on = tt::turntableOf(g, ttbNode(a).id());
        REQUIRE(on != kNoNode);
        CHECK(tt::turntableOf(g, ttbNode(r[1].get<std::string>()).id()) == on);
        CHECK_FALSE(ttbNode(r[2].get<std::string>()).as<bool>(Transform::kVisibility));
        // The preset rides on the revolve, so the base keeps its live history.
        REQUIRE(g.find(r[3].get<std::string>()) != nullptr);
        CHECK(ttbRun("listMaterials").dump().find("M_TurntableWood") != std::string::npos);
        // The model stands on the top face (a record's lip rises past it at the rim).
        if (std::string_view(style) != "record")
            CHECK(baseBox.max.y == doctest::Approx(cubeBox.min.y).epsilon(1e-6));
        if (std::string_view(style) == "lazySusan")
        {
            const NodeId foot = Graph::current().find("turntableBaseFoot")->id();
            CHECK(tt::turntableOf(g, foot) == kNoNode);
        }
    }
}

TEST_CASE("turntable base: running it again on the base rebuilds that base, which turns with the model")
{
    ttbRun("newScene");
    const json made = ttbRun("polyCube");
    const std::string a = (made.is_array() ? made[0] : made).get<std::string>();
    const json first = ttbRun("turntableBase", {{"args", {a}}});
    const std::string group = first[0].get<std::string>();
    const json again = ttbRun("turntableBase", {{"args", {group}}, {"style", "plinth"}});
    CHECK(again[0].get<std::string>() == group);
    Graph& g = Graph::current();
    CHECK(tt::all(g).size() == 1);
    const NodeId table = tt::all(g).front()->id();
    CHECK(tt::members(g, table).size() == 2);
    // Selecting the model runs it on the same base too.
    ttbRun("turntableBase", {{"args", {a}}, {"style", "record"}});
    CHECK(tt::all(g).size() == 1);
    CHECK(g.children(ttbNode(group).id()).size() == 2);

    const NodeId mesh = ttbNode(ttbRun("turntableBase", {{"args", {a}}})[1].get<std::string>()).id();
    tt::setAngle(g, table, 90.0);
    g.evaluate();
    const glm::dmat4 w = g.node(mesh)->as<glm::dmat4>(Transform::kWorldMatrix);
    CHECK(std::abs(w[0][0]) < 1e-9);
}
