#include <doctest/doctest.h>

#include "io/SceneFile.h"
#include "ops/Command.h"
#include "rigmath/Expressions.h"
#include "scene/Graph.h"

#include <cmath>

using namespace lila;
using namespace lila::scene;
namespace ex = lila::scene::expressions;

namespace expressionTests
{
json eCmd(const char* name, json call = json::object()) { return ops::execute(name, call); }
double value(const std::string& plug) { return eCmd("getAttr", {{"args", {plug}}}).get<double>(); }
} // namespace expressionTests
using namespace expressionTests;

TEST_CASE("Expressions parse Python syntax; the subset compiles, the rest is flagged for Python")
{
    eCmd("newScene");
    eCmd("spaceLocator", {{"name", "ctrl"}});
    std::string why;
    CHECK(ex::compiles("ctrl.ry * 0.5 + clamp(ctrl.tx, 0, 90)", &why));
    CHECK(ex::compiles("-ctrl.tx ** 2 if ctrl.ty > 1 and not ctrl.tz == 0 else 3 % 2", &why));
    CHECK_FALSE(ex::compiles("noise(ctrl.tx)", &why));
    CHECK(why.find("noise") != std::string::npos);
    CHECK_FALSE(ex::compiles("[x for x in range(3)][0]", &why));
    CHECK_FALSE(ex::compiles("1 < ctrl.tx < 2", &why));
    CHECK_FALSE(ex::compiles("nobody.tx + 1", &why));
    CHECK_THROWS(ex::parse("ctrl.tx +"));
    CHECK_THROWS(ex::parse("(1"));
    CHECK(ex::parse("pi")->number == doctest::Approx(3.14159265));
}

TEST_CASE("expression compiles to rig math nodes that evaluate, query and delete")
{
    eCmd("newScene");
    eCmd("spaceLocator", {{"name", "ctrl"}});
    eCmd("spaceLocator", {{"name", "foot"}});
    eCmd("spaceLocator", {{"name", "box"}});
    eCmd("addAttr", {{"args", {"foot"}}, {"longName", "roll"}, {"keyable", true}});
    json r = eCmd("expression", {{"args", {"box.ty"}}, {"string", "= ctrl.ry * 0.5 + clamp(foot.roll, 0, 90)"}});
    CHECK(r["native"] == true);
    CHECK(r["nodes"].size() == 3);
    eCmd("setAttr", {{"args", {"ctrl.ry", 20.0}}});
    eCmd("setAttr", {{"args", {"foot.roll", 120.0}}});
    CHECK(value("box.ty") == doctest::Approx(10.0 + 90.0));
    eCmd("setAttr", {{"args", {"foot.roll", -5.0}}});
    CHECK(value("box.ty") == doctest::Approx(10.0));

    json q = eCmd("expression", {{"args", {"box.ty"}}, {"query", true}});
    CHECK(q["text"] == "ctrl.ry * 0.5 + clamp(foot.roll, 0, 90)");
    CHECK(q["native"] == true);

    // Conditionals, comparisons and logic.
    eCmd("expression", {{"args", {"box.tz"}}, {"string", "ctrl.tx * 2 if ctrl.tx > 1 and not ctrl.tz > 0 else -1"}});
    eCmd("setAttr", {{"args", {"ctrl.tx", 3.0}}});
    CHECK(value("box.tz") == doctest::Approx(6.0));
    eCmd("setAttr", {{"args", {"ctrl.tz", 1.0}}});
    CHECK(value("box.tz") == doctest::Approx(-1.0));
    eCmd("expression", {{"args", {"box.sx"}}, {"string", "2 ** 3 + 7 % 4"}});
    CHECK(value("box.sx") == doctest::Approx(11.0));

    // Setting again replaces the old nodes; save and load keep it; undo and delete clean up.
    const size_t before = Graph::current().nodes().size();
    eCmd("expression", {{"args", {"box.ty"}}, {"string", "ctrl.ry"}});
    CHECK(Graph::current().nodes().size() == before - 2); // three nodes out, one in
    CHECK(value("box.ty") == doctest::Approx(20.0));
    eCmd("expression", {{"args", {"box.ty"}}, {"delete", true}});
    CHECK(eCmd("expression", {{"args", {"box.ty"}}, {"query", true}}).is_null());
    CHECK_FALSE(Graph::current().source(Graph::current().plug("box.ty")));
    eCmd("undo");
    CHECK(eCmd("expression", {{"args", {"box.ty"}}, {"query", true}})["text"] == "ctrl.ry");
    json doc = io::serialize();
    eCmd("newScene");
    io::deserialize(doc);
    CHECK(value("box.ty") == doctest::Approx(20.0));
    CHECK(eCmd("expression", {{"args", {"box.ty"}}, {"query", true}})["text"] == "ctrl.ry");
    CHECK_THROWS(eCmd("expression", {{"args", {"box.translate"}}, {"string", "1"}}));
}

TEST_CASE("Expressions beyond the subset run through the Python evaluator with plugs as v0, v1")
{
    eCmd("newScene");
    eCmd("spaceLocator", {{"name", "ctrl"}});
    eCmd("spaceLocator", {{"name", "box"}});
    std::string seen;
    ex::setPythonEvaluator([&](const std::string& code, const std::vector<double>& v, std::string*) -> std::optional<double> {
        seen = code;
        return v.empty() ? 0.0 : v[0] * 10.0;
    });
    eCmd("setAttr", {{"args", {"ctrl.tx", 2.0}}});
    json r = eCmd("expression", {{"args", {"box.ty"}}, {"string", "noise(ctrl.tx)"}});
    CHECK(r["native"] == false);
    CHECK(r["reason"].get<std::string>().find("noise") != std::string::npos);
    CHECK(value("box.ty") == doctest::Approx(20.0));
    CHECK(seen == "noise(v0)");
    json forced = eCmd("expression", {{"args", {"box.tz"}}, {"string", "ctrl.tx + 1"}, {"python", true}});
    CHECK(forced["native"] == false);
    ex::setPythonEvaluator({});
}
