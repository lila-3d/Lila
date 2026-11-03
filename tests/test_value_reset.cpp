#include <doctest/doctest.h>

#include "TestUtil.h"

#include "ops/Command.h"
#include "scene/Graph.h"
#include "ui/ValueReset.h"

using namespace lila;
using namespace testutil;
namespace vr = lila::ui::valueReset;

namespace
{
/** What the editors do on a middle click: setAttr the plug's AttrDef default. */
void resetPlug(const std::string& plug)
{
    scene::Graph& g = scene::Graph::current();
    scene::PlugRef ref = g.plug(plug);
    std::optional<json> def = vr::plugDefault(g.attr(ref), ref.component);
    REQUIRE(def);
    ops::execute("setAttr", {{"args", {plug, *def}}}, CallSource::UI);
}

double number(const std::string& plug) { return run("getAttr", {{"args", {plug}}}).get<double>(); }
} // namespace

TEST_CASE("a middle press is a click only when short and still")
{
    vr::ClickTracker t;
    t.press(100, 100, 0);
    CHECK_FALSE(t.dragging());
    CHECK(t.release(102, 101, 150));
    CHECK_FALSE(t.down());

    t.press(100, 100, 0);
    t.move(104, 100);
    CHECK(t.dragging());
    CHECK_FALSE(t.release(104, 100, 100));

    // Coming back to the start after a drag is still a drag.
    t.press(100, 100, 0);
    t.move(110, 100);
    t.move(100, 100);
    CHECK_FALSE(t.release(100, 100, 100));

    t.press(100, 100, 0);
    CHECK_FALSE(t.release(100, 100, vr::kMaxMillis));

    t.press(0, 0, 0);
    CHECK(t.release(0, vr::kMaxTravel - 1, vr::kMaxMillis - 1));
    CHECK_FALSE(t.release(0, 0, 1)); // no press

    t.press(0, 0, 0);
    t.cancel();
    CHECK_FALSE(t.release(0, 0, 1));
}

TEST_CASE("plug defaults come from the AttrDef, per component for Vec3")
{
    scene::AttrDef f{.name = "f", .type = scene::PlugType::Float, .defaultValue = 2.5};
    CHECK(vr::plugDefault(f) == json(2.5));

    scene::AttrDef i{.name = "i", .type = scene::PlugType::Int, .defaultValue = 3.0};
    CHECK(vr::plugDefault(i) == json(int64_t(3)));
    CHECK(vr::plugDefault(i)->is_number_integer());

    scene::AttrDef b{.name = "b", .type = scene::PlugType::Bool, .defaultValue = true};
    CHECK(vr::plugDefault(b) == json(true));

    scene::AttrDef e{.name = "e", .type = scene::PlugType::Enum, .defaultValue = int64_t(1), .enumNames = {"off", "angle"}};
    CHECK(vr::plugDefault(e) == json(int64_t(1)));
    CHECK(vr::describe(*vr::plugDefault(e), e.enumNames) == "angle");

    scene::AttrDef v{.name = "v", .type = scene::PlugType::Vec3, .defaultValue = glm::dvec3(1, 2, 3)};
    CHECK(vr::plugDefault(v) == json::array({1.0, 2.0, 3.0}));
    CHECK(vr::plugDefault(v, 1) == json(2.0));

    scene::AttrDef s{.name = "s", .type = scene::PlugType::String, .defaultValue = std::string("abc")};
    CHECK(vr::plugDefault(s) == json("abc"));

    scene::AttrDef m{.name = "m", .type = scene::PlugType::Matrix, .defaultValue = glm::dmat4(1.0)};
    CHECK_FALSE(vr::plugDefault(m));
}

TEST_CASE("flag defaults and how defaults read")
{
    CHECK(vr::flagDefault(FlagDef{.name = "a", .defaultValue = 0.5}) == json(0.5));
    CHECK_FALSE(vr::flagDefault(FlagDef{.name = "b"}));

    CHECK(vr::describe(json(30.0)) == "30");
    CHECK(vr::describe(json(0.125)) == "0.125");
    CHECK(vr::describe(json(-1e-9)) == "0");
    CHECK(vr::describe(json(false)) == "off");
    CHECK(vr::describe(json(int64_t(4))) == "4");
    CHECK(vr::describe(json::array({0.0, 1.0, 0.0})) == "0, 1, 0");
    CHECK(vr::describe(json("")) == "empty");
}

TEST_CASE("resetting a modifier setting is one undoable setAttr")
{
    run("newScene");
    std::string cube = run("polyCube")[0];
    std::string mod = run("autoBevel", {{"args", {cube}}})[0];
    scene::Graph& g = scene::Graph::current();
    const scene::AttrDef& def = g.attr(g.plug(mod + ".minAngle"));
    double original = std::get<double>(def.defaultValue);

    run("setAttr", {{"args", {mod + ".minAngle", original + 12.0}}});
    resetPlug(mod + ".minAngle");
    CHECK(number(mod + ".minAngle") == doctest::Approx(original));
    run("undo");
    CHECK(number(mod + ".minAngle") == doctest::Approx(original + 12.0));
}

TEST_CASE("resetting one channel of a vector leaves the others")
{
    run("newScene");
    std::string t = run("createNode", {{"args", {"transform"}}}).get<std::string>();
    run("setAttr", {{"args", {t + ".translate", {1.0, 2.0, 3.0}}}});
    run("setAttr", {{"args", {t + ".scale", {4.0, 5.0, 6.0}}}});
    resetPlug(t + ".ty");
    resetPlug(t + ".sx");
    CHECK(number(t + ".tx") == doctest::Approx(1.0));
    CHECK(number(t + ".ty") == doctest::Approx(0.0));
    CHECK(number(t + ".sx") == doctest::Approx(1.0));
    CHECK(number(t + ".sy") == doctest::Approx(5.0));
}
