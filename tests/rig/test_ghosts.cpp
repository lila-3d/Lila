#include "TestUtil.h"

#include "scene/Anim.h"
#include "scene/Ghosts.h"

using namespace rigtest;
using namespace lila::scene;

namespace
{
/** root -> tip with tip.tx keyed 10, 20, 30 at 0, 1 and 2 seconds; root selected. */
void keyedChain()
{
    run("newScene");
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "root"}});
    run("joint", {{"position", v3(10, 0, 0)}, {"name", "tip"}});
    for (int i = 0; i < 3; ++i)
        run("setKeyframe", {{"args", {"tip.tx"}}, {"time", double(i)}, {"value", 10.0 * (i + 1)}});
    run("select", {{"args", {"root"}}});
}

ghosts::Settings keySettings()
{
    ghosts::Settings s;
    s.enabled = true;
    s.keys = true;
    s.before = 1;
    s.after = 1;
    s.mesh = false;
    s.skeleton = true;
    return s;
}

double tipX(const ghosts::Ghost& g)
{
    REQUIRE(g.frame->bones.size() == 1);
    return g.frame->bones[0].second.x;
}
} // namespace

TEST_CASE("ghost times sit at the neighbouring keys or every step frames")
{
    ghosts::Settings s = keySettings();
    s.before = 2;
    s.after = 2;
    std::vector<double> keys = {0.0, 1.0, 2.0, 3.0, 4.0};
    auto t = ghosts::ghostTimes(s, 2.0, keys, 30.0);
    REQUIRE(t.size() == 4);
    CHECK(t[0] == ghosts::GhostTime{1.0, -1});
    CHECK(t[1] == ghosts::GhostTime{0.0, -2});
    CHECK(t[2] == ghosts::GhostTime{3.0, 1});
    CHECK(t[3] == ghosts::GhostTime{4.0, 2});

    s.step = 2;
    t = ghosts::ghostTimes(s, 2.5, keys, 30.0);
    REQUIRE(t.size() == 3);
    CHECK(t[0].first == 2.0);
    CHECK(t[1].first == 0.0);
    CHECK(t[2].first == 3.0);

    s.keys = false;
    s.step = 3;
    t = ghosts::ghostTimes(s, 1.0, {}, 30.0);
    REQUIRE(t.size() == 4);
    CHECK(t[0].first == doctest::Approx(0.9));
    CHECK(t[3].first == doctest::Approx(1.2));
}

TEST_CASE("ghost frames are cached and recaptured only when keys change")
{
    keyedChain();
    ghosts::Cache cache(graph());
    const ghosts::Settings s = keySettings();
    run("currentTime", {{"args", {1.5}}});
    const double live = pos("tip").x;

    std::vector<ghosts::Ghost> g = cache.ghosts(s);
    REQUIRE(g.size() == 2);
    CHECK(g[0].offset == -1);
    CHECK(g[0].frame->time == doctest::Approx(1.0));
    CHECK(tipX(g[0]) == doctest::Approx(20));
    CHECK(tipX(g[1]) == doctest::Approx(30));
    CHECK(cache.captures() == 2);
    // Capturing stepped the time and put it back.
    CHECK(graph().time() == doctest::Approx(1.5));
    CHECK(pos("tip").x == doctest::Approx(live));

    cache.ghosts(s);
    run("currentTime", {{"args", {1.2}}});
    cache.ghosts(s);
    CHECK(cache.captures() == 2);

    // Posing a DAG node without keying it leaves the ghosts alone.
    run("setAttr", {{"args", {"root.translate", v3(0, 5, 0)}}});
    cache.ghosts(s);
    CHECK(cache.captures() == 2);

    // A key edit recaptures.
    run("setKeyframe", {{"args", {"tip.tx"}}, {"time", 1.0}, {"value", 50.0}});
    g = cache.ghosts(s);
    CHECK(cache.captures() == 4);
    CHECK(tipX(g[0]) == doctest::Approx(50));

    // So does undoing it.
    run("undo");
    g = cache.ghosts(s);
    CHECK(cache.captures() == 6);
    CHECK(tipX(g[0]) == doctest::Approx(20));

    // Moving to a new pair of keys captures only the frame not seen yet.
    run("currentTime", {{"args", {0.5}}});
    g = cache.ghosts(s);
    REQUIRE(g.size() == 2);
    CHECK(g[0].frame->time == doctest::Approx(0.0));
    CHECK(cache.captures() == 7);

    // Deselecting drops the targets.
    run("select", {{"args", json::array()}});
    CHECK(cache.ghosts(s).empty());
}

TEST_CASE("ghosts never capture over an unkeyed edit on an animated channel")
{
    keyedChain();
    ghosts::Cache cache(graph());
    run("currentTime", {{"args", {1.5}}});
    run("setAttr", {{"args", {"tip.tx", 99.0}}});
    REQUIRE(graph().isHeld(graph().plug("tip.tx")));
    CHECK(cache.ghosts(keySettings()).empty());
    CHECK(cache.captures() == 0);
    CHECK(graph().isHeld(graph().plug("tip.tx")));
    CHECK(pos("tip").x == doctest::Approx(99));
}
