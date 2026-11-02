#include <doctest/doctest.h>

#include "ops/Command.h"
#include "ops/Undo.h"
#include "geo/Mesh.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/NodeBounds.h"
#include "scene/Turntable.h"
#include "scene/ViewState.h"

#include <filesystem>

using namespace lila;
using namespace lila::scene;
namespace tt = lila::scene::turntable;

namespace
{
json ttRun(const char* name, json call = json::object()) { return ops::execute(name, call); }
std::string ttMade(const json& r) { return (r.is_array() ? r[0] : r).get<std::string>(); }
Node& ttNode(const std::string& name) { return *Graph::current().find(name); }

glm::dvec3 ttWorldPos(const std::string& name)
{
    Graph::current().evaluate();
    return glm::dvec3(ttNode(name).as<glm::dmat4>(Transform::kWorldMatrix)[3]);
}

bool ttNear(const glm::dvec3& a, const glm::dvec3& b, double eps = 1e-6) { return glm::length(a - b) < eps; }

/** Two cubes either side of the origin on a turntable pivoting at the origin. */
std::tuple<std::string, std::string, std::string> ttTwoCubes()
{
    ttRun("newScene");
    const std::string a = ttMade(ttRun("polyCube")), b = ttMade(ttRun("polyCube"));
    ttRun("move", {{"args", {a}}, {"to", {10.0, 0.0, 0.0}}});
    ttRun("move", {{"args", {b}}, {"to", {-10.0, 0.0, 0.0}}});
    const std::string t = ttRun("turntable", {{"args", {a, b}}, {"pivot", {0.0, 0.0, 0.0}}}).get<std::string>();
    return {a, b, t};
}

std::string ttTempPath(const std::string& name)
{
    auto dir = std::filesystem::temp_directory_path() / "lila_tests" / "turntable";
    std::filesystem::create_directories(dir);
    return (dir / name).generic_string();
}
} // namespace

TEST_CASE("turntable: members turn about the pivot without their own values changing")
{
    auto [a, b, t] = ttTwoCubes();
    CHECK(ttRun("turntable", {{"query", t}})["members"] == json({a, b}));
    CHECK(ttNear(ttWorldPos(a), {10, 0, 0}));

    ttRun("turntableTurn", {{"args", {t}}, {"to", 90.0}, {"ease", false}});
    // A quarter turn counter-clockwise from above takes +X to -Z.
    CHECK(ttNear(ttWorldPos(a), {0, 0, -10}));
    CHECK(ttNear(ttWorldPos(b), {0, 0, 10}));
    CHECK(ttNear(ttNode(a).as<glm::dvec3>(Transform::kTranslate), {10, 0, 0}));

    // Moving a turned member in world space writes the values that land it there through the turn.
    ttRun("move", {{"args", {a}}, {"to", {0.0, 0.0, -20.0}}});
    CHECK(ttNear(ttNode(a).as<glm::dvec3>(Transform::kTranslate), {20, 0, 0}, 1e-5));
    CHECK(ttNear(ttWorldPos(a), {0, 0, -20}, 1e-5));
}

TEST_CASE("turntable: a member under another member turns once")
{
    auto [a, b, t] = ttTwoCubes();
    const std::string grp = ttRun("group", {{"args", {a}}, {"name", "holder"}}).get<std::string>();
    ttRun("turntable", {{"edit", t}, {"add", {grp}}});
    ttRun("turntableTurn", {{"args", {t}}, {"to", 180.0}, {"ease", false}});
    CHECK(ttNear(ttWorldPos(ttNode(a).path()), {-10, 0, 0}));
}

TEST_CASE("turntable: exports see no turn, renders do")
{
    auto [a, b, t] = ttTwoCubes();
    ttRun("turntableTurn", {{"args", {t}}, {"to", 90.0}, {"ease", false}});
    {
        viewState::FinalScope exporting(true);
        CHECK(ttNear(ttWorldPos(a), {10, 0, 0}));
    }
    {
        viewState::FinalScope rendering(false);
        CHECK(ttNear(ttWorldPos(a), {0, 0, -10}));
        ttRun("turntable", {{"edit", t}, {"includeInRenders", false}});
        CHECK(ttNear(ttWorldPos(a), {10, 0, 0}));
    }
    CHECK(ttNear(ttWorldPos(a), {0, 0, -10}));
}

TEST_CASE("turntable: turning records no undo and the scene opens at rest")
{
    auto [a, b, t] = ttTwoCubes();
    const uint64_t newest = UndoStack::get().newestId();
    ttRun("turntableTurn", {{"args", {t}}, {"to", 45.0}, {"ease", false}});
    ttRun("turntablePlay", {{"args", {t}}});
    tt::advance(Graph::current(), 0.5, tt::now());
    CHECK(UndoStack::get().newestId() == newest);

    const std::string path = ttTempPath("spin.lila");
    ttRun("saveScene", {{"args", {path}}});
    ttRun("openScene", {{"args", {path}}});
    const json q = ttRun("turntable", {{"query", t}});
    CHECK(q["members"] == json({a, b}));
    CHECK(q["angle"].get<double>() == 0.0);
    CHECK_FALSE(q["playing"].get<bool>());
    CHECK(ttNear(ttWorldPos(a), {10, 0, 0}));
}

TEST_CASE("turntable: spin eases in and out without jumps in speed")
{
    auto [a, b, t] = ttTwoCubes();
    Graph& g = Graph::current();
    tt::Turntable* table = tt::get(g, ttNode(t).id());
    ttRun("turntable", {{"edit", t}, {"secondsPerTurn", 20.0}, {"ease", 0.8}});
    double clock = 100.0, lastV = 0.0, maxJump = 0.0;
    tt::setPlaying(g, table->id(), true);
    for (int i = 0; i < 180; ++i)
    {
        clock += 1.0 / 60.0;
        tt::advance(g, 1.0 / 60.0, clock);
        maxJump = std::max(maxJump, std::abs(table->velocity - lastV));
        lastV = table->velocity;
    }
    CHECK(table->velocity == doctest::Approx(18.0).epsilon(0.01));
    CHECK(maxJump < 2.0);
    tt::setPlaying(g, table->id(), false);
    for (int i = 0; i < 180; ++i)
    {
        clock += 1.0 / 60.0;
        tt::advance(g, 1.0 / 60.0, clock);
        maxJump = std::max(maxJump, std::abs(table->velocity - lastV));
        lastV = table->velocity;
    }
    CHECK(table->velocity == 0.0);
    CHECK_FALSE(tt::moving(*table));
    CHECK(maxJump < 2.0);

    // An edit to a member holds it, and it spins on after resumeAfter seconds of quiet.
    tt::setPlaying(g, table->id(), true);
    for (int i = 0; i < 120; ++i)
        tt::advance(g, 1.0 / 60.0, clock += 1.0 / 60.0);
    tt::memberEdited(g, table->id(), clock);
    for (int i = 0; i < 30; ++i)
        tt::advance(g, 1.0 / 60.0, clock += 1.0 / 60.0);
    CHECK(std::abs(table->velocity) < 0.5);
    for (int i = 0; i < 300; ++i)
        tt::advance(g, 1.0 / 60.0, clock += 1.0 / 60.0);
    CHECK_FALSE(table->held);
    CHECK(table->velocity == doctest::Approx(18.0).epsilon(0.01));
}

TEST_CASE("turntable: steps land on stops and Front returns to rest")
{
    auto [a, b, t] = ttTwoCubes();
    Graph& g = Graph::current();
    tt::Turntable* table = tt::get(g, ttNode(t).id());
    double clock = 10.0;
    tt::step(g, table->id(), 1, clock);
    tt::step(g, table->id(), 1, clock);
    for (int i = 0; i < 240; ++i)
        tt::advance(g, 1.0 / 60.0, clock += 1.0 / 60.0);
    CHECK(table->angle == doctest::Approx(180.0));
    CHECK(ttNear(ttWorldPos(a), {-10, 0, 0}, 1e-5));
    tt::step(g, table->id(), -1, clock);
    for (int i = 0; i < 240; ++i)
        tt::advance(g, 1.0 / 60.0, clock += 1.0 / 60.0);
    CHECK(table->angle == doctest::Approx(90.0));
    tt::setPlaying(g, table->id(), true);
    tt::front(g, table->id(), clock);
    for (int i = 0; i < 240; ++i)
        tt::advance(g, 1.0 / 60.0, clock += 1.0 / 60.0);
    CHECK(table->angle == doctest::Approx(0.0).epsilon(1e-9));
    CHECK_FALSE(table->playing);
}

TEST_CASE("turntable: put on, take off, undo")
{
    ttRun("newScene");
    const std::string a = ttMade(ttRun("polyCube")), b = ttMade(ttRun("polyCube"));
    ttRun("select", {{"args", {a}}});
    const std::string t = ttRun("putOnTurntable").get<std::string>();
    ttRun("select", {{"args", {t, b}}});
    CHECK(ttRun("putOnTurntable").get<std::string>() == t);
    CHECK(ttRun("turntable", {{"query", t}})["members"] == json({a, b}));
    ttRun("takeOffTurntable", {{"args", {a, b}}});
    CHECK(Graph::current().find(t) == nullptr);
    ttRun("undo");
    CHECK(ttRun("turntable", {{"query", t}})["members"] == json({a, b}));
    ttRun("turntableTurn", {{"args", {t}}, {"to", 90.0}, {"ease", false}});
    ttRun("takeOffTurntable", {{"args", {a}}});
    CHECK(ttNear(ttWorldPos(a), glm::dvec3(0.0)));
}

TEST_CASE("turntable: framing sweeps the whole turn")
{
    auto [a, b, t] = ttTwoCubes();
    Graph& g = Graph::current();
    ttRun("turntable", {{"edit", t}, {"remove", {b}}});
    g.evaluate();
    const geo::Bounds box = worldBounds(g, ttNode(a).id());
    glm::dvec3 lo = box.min, hi = box.max;
    REQUIRE(tt::sweepBounds(g, {ttNode(a).id()}, lo, hi));
    CHECK(lo.z < -10.0);
    CHECK(hi.z > 10.0);
    CHECK(lo.x < -10.0);
    CHECK(hi.y == doctest::Approx(box.max.y));
}

TEST_CASE("turntable: pitch tilts the members about the centre, Front levels it, exports never see it")
{
    auto [a, b, t] = ttTwoCubes();
    Graph& g = Graph::current();
    tt::Turntable* table = tt::get(g, ttNode(t).id());
    tt::dragPitch(g, table->id(), 90.0, {0, 0, 1}, {0, 0, 0});
    CHECK(table->pitch == doctest::Approx(89.0));
    tt::dragPitch(g, table->id(), -59.0, {0, 0, 1}, {0, 0, 0});
    CHECK(table->pitch == doctest::Approx(30.0));
    const glm::dvec3 p = ttWorldPos(a);
    CHECK(p.y == doctest::Approx(10.0 * std::sin(glm::radians(30.0))));
    {
        viewState::FinalScope exporting(true);
        CHECK(ttNear(ttWorldPos(a), {10, 0, 0}));
    }
    double clock = 50.0;
    tt::front(g, table->id(), clock);
    for (int i = 0; i < 240; ++i)
        tt::advance(g, 1.0 / 60.0, clock += 1.0 / 60.0);
    CHECK(table->pitch == doctest::Approx(0.0));
    CHECK(ttNear(ttWorldPos(a), {10, 0, 0}, 1e-6));
}
