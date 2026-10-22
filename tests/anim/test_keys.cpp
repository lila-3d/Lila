#include <doctest/doctest.h>

#include "cmds/Command.h"
#include "scene/Anim.h"
#include "scene/Graph.h"

#include <filesystem>

using namespace lila;
using json = nlohmann::json;
using scene::Graph;

namespace
{
json run(const char* name, json call = json::object()) { return cmds::execute(name, call); }
double get(const std::string& plug) { return run("getAttr", {{"args", {plug}}}).get<double>(); }
void at(double seconds) { run("currentTime", {{"args", {seconds}}}); }
bool exists(const std::string& name) { return run("objExists", {{"args", {name}}}).get<bool>(); }

std::string tempPath(const char* name)
{
    auto dir = std::filesystem::temp_directory_path() / "lila_anim_tests" / "scenes";
    std::filesystem::create_directories(dir);
    return (dir / name).generic_string();
}

/** loc.ty keyed 0 at 0s and 10 at 1s. */
void keyedLocator()
{
    run("newScene");
    run("autoKeyframe", {{"state", false}});
    run("spaceLocator", {{"name", "loc"}});
    run("setKeyframe", {{"args", {"loc.ty"}}, {"time", 0.0}, {"value", 0.0}});
    run("setKeyframe", {{"args", {"loc.ty"}}, {"time", 1.0}, {"value", 10.0}});
}
} // namespace

TEST_CASE("setKeyframe creates a curve and evaluates through currentTime")
{
    keyedLocator();
    CHECK(exists("loc_translateY"));
    CHECK(run("nodeType", {{"args", {"loc_translateY"}}}) == "animCurveTL");
    at(0.0);
    CHECK(get("loc.ty") == doctest::Approx(0));
    at(0.5);
    CHECK(get("loc.ty") == doctest::Approx(5));
    at(1.0);
    CHECK(get("loc.ty") == doctest::Approx(10));
    at(3.0);
    CHECK(get("loc.ty") == doctest::Approx(10));
    CHECK(run("keyframe", {{"args", {"loc"}}, {"query", true}}) == json({0.0, 1.0}));
    CHECK(run("keyframe", {{"args", {"loc"}}, {"query", true}, {"valueChange", true}}) == json({0.0, 10.0}));
    CHECK(run("keyframe", {{"args", {"loc"}}, {"query", true}, {"keyCount", true}}) == 2);
}

TEST_CASE("setKeyframe with no arguments keys every keyable channel of the selection")
{
    run("newScene");
    run("spaceLocator", {{"name", "a"}});
    run("setAttr", {{"args", {"a.translate", 1, 2, 3}}});
    at(0.0);
    int keys = run("setKeyframe").get<int>();
    CHECK(keys >= 10);
    CHECK(exists("a_translateX"));
    CHECK(exists("a_rotateZ"));
    CHECK(exists("a_scaleY"));
    CHECK(exists("a_visibility"));
    CHECK(run("nodeType", {{"args", {"a_rotateZ"}}}) == "animCurveTA");
    CHECK(run("nodeType", {{"args", {"a_visibility"}}}) == "animCurveTU");
    CHECK(get("a.tz") == doctest::Approx(3));

    run("newScene");
    run("spaceLocator", {{"name", "b"}});
    CHECK(run("setKeyframe", {{"attribute", "rotate"}}) == 3);
    CHECK(exists("b_rotateY"));
    CHECK(!exists("b_translateX"));
}

TEST_CASE("setKeyframe and cutKey undo")
{
    keyedLocator();
    run("undo");
    CHECK(run("keyframe", {{"args", {"loc.ty"}}, {"query", true}, {"keyCount", true}}) == 1);
    run("undo");
    CHECK(!exists("loc_translateY"));
    CHECK(!Graph::current().source(Graph::current().plug("loc.ty")));
    run("redo");
    run("redo");
    CHECK(run("keyframe", {{"args", {"loc.ty"}}, {"query", true}, {"keyCount", true}}) == 2);

    CHECK(run("cutKey", {{"args", {"loc"}}, {"time", 1.0}}) == 1);
    CHECK(run("keyframe", {{"args", {"loc.ty"}}, {"query", true}}) == json({0.0}));
    CHECK(run("cutKey", {{"args", {"loc"}}}) == 1);
    CHECK(!exists("loc_translateY"));
    run("undo");
    CHECK(exists("loc_translateY"));
    at(0.5);
    CHECK(get("loc.ty") == doctest::Approx(0)); // only the key at 0 is back
    run("undo");
    at(0.5);
    CHECK(get("loc.ty") == doctest::Approx(5));
}

TEST_CASE("keyframe edits, keyTangent and setInfinity")
{
    keyedLocator();
    run("keyframe", {{"args", {"loc.ty"}}, {"time", 1.0}, {"valueChange", 20.0}});
    at(1.0);
    CHECK(get("loc.ty") == doctest::Approx(20));
    run("keyframe", {{"args", {"loc.ty"}}, {"time", 1.0}, {"timeChange", 1.0}, {"relative", true}});
    CHECK(run("keyframe", {{"args", {"loc.ty"}}, {"query", true}}) == json({0.0, 2.0}));

    run("keyTangent", {{"args", {"loc.ty"}}, {"inTangentType", "linear"}, {"outTangentType", "linear"}});
    at(1.0);
    CHECK(get("loc.ty") == doctest::Approx(10));
    run("keyTangent", {{"args", {"loc.ty"}}, {"time", 0.0}, {"outTangentType", "step"}});
    CHECK(get("loc.ty") == doctest::Approx(0));
    json q = run("keyTangent", {{"args", {"loc.ty"}}, {"query", true}});
    CHECK(q[0]["out"] == "step");
    CHECK(q[1]["in"] == "linear");
    CHECK_THROWS_AS(run("keyTangent", {{"args", {"loc.ty"}}, {"inTangentType", "bogus"}}), CommandError);

    run("keyTangent", {{"args", {"loc.ty"}}, {"outTangentType", "linear"}});
    run("setInfinity", {{"args", {"loc.ty"}}, {"postInfinite", "cycleRelative"}});
    at(3.0);
    CHECK(get("loc.ty") == doctest::Approx(30));
    CHECK(run("setInfinity", {{"args", {"loc"}}, {"query", true}})[0]["post"] == "cycleRelative");
    run("undo");
    CHECK(get("loc.ty") == doctest::Approx(20));
}

TEST_CASE("copyKey and pasteKey")
{
    keyedLocator();
    CHECK(run("copyKey", {{"args", {"loc.ty"}}}) == 2);
    run("spaceLocator", {{"name", "other"}});
    CHECK(run("pasteKey", {{"args", {"other.tx"}}, {"time", 2.0}}) == 2);
    CHECK(run("keyframe", {{"args", {"other.tx"}}, {"query", true}}) == json({2.0, 3.0}));
    at(2.5);
    CHECK(get("other.tx") == doctest::Approx(5));
}

TEST_CASE("component connections: a keyed translateY leaves X and Z editable")
{
    keyedLocator();
    at(0.5);
    run("setAttr", {{"args", {"loc.tx", 3}}});
    run("setAttr", {{"args", {"loc.tz", -2}}});
    CHECK(get("loc.tx") == doctest::Approx(3));
    CHECK(get("loc.ty") == doctest::Approx(5));
    CHECK(get("loc.tz") == doctest::Approx(-2));

    // A whole-vector set keeps the driven component on its curve once time moves.
    run("setAttr", {{"args", {"loc.translate", 1, 99, 4}}});
    CHECK(get("loc.tx") == doctest::Approx(1));
    CHECK(get("loc.ty") == doctest::Approx(99)); // held until the time changes
    at(1.0);
    CHECK(get("loc.ty") == doctest::Approx(10));
    CHECK(get("loc.tz") == doctest::Approx(4));

    // The world matrix follows the driven component.
    json m = run("xform", {{"args", {"loc"}}, {"query", true}, {"worldSpace", true}, {"translation", true}});
    CHECK(m[1].get<double>() == doctest::Approx(10));

    // The whole vector and one of its components can't both be driven.
    run("spaceLocator", {{"name", "src"}});
    CHECK_THROWS_AS(run("connectAttr", {{"args", {"src.translate", "loc.translate"}}}), CommandError);
    run("connectAttr", {{"args", {"src.translate", "loc.translate"}}, {"force", true}});
    CHECK_FALSE(Graph::current().source(Graph::current().plug("loc.ty")).has_value());
    run("undo");
    CHECK(Graph::current().source(Graph::current().plug("loc.ty")).has_value());

    // Component to component, and component to scalar.
    run("connectAttr", {{"args", {"src.tx", "loc.rz"}}});
    run("setAttr", {{"args", {"src.tx", 45}}});
    CHECK(get("loc.rz") == doctest::Approx(45));
    CHECK(get("loc.rx") == doctest::Approx(0));
    CHECK_THROWS_AS(run("connectAttr", {{"args", {"src.translate", "loc.tz"}}}), CommandError);
}

TEST_CASE("setting an animated channel holds it, or keys it with auto key")
{
    keyedLocator();
    at(0.5);
    run("setAttr", {{"args", {"loc.ty", 7}}});
    CHECK(get("loc.ty") == doctest::Approx(7));
    CHECK(Graph::current().isHeld(Graph::current().plug("loc.ty")));
    // S keys the held value.
    run("setKeyframe", {{"args", {"loc.ty"}}});
    at(0.0);
    at(0.5);
    CHECK(get("loc.ty") == doctest::Approx(7));
    CHECK(run("keyframe", {{"args", {"loc.ty"}}, {"query", true}, {"keyCount", true}}) == 3);

    run("autoKeyframe", {{"state", true}});
    at(1.0);
    run("setAttr", {{"args", {"loc.ty", 12}}});
    at(0.0);
    at(1.0);
    CHECK(get("loc.ty") == doctest::Approx(12));
    // Auto key only touches channels that are already animated.
    run("setAttr", {{"args", {"loc.tx", 4}}});
    CHECK(!exists("loc_translateX"));
    run("undo");
    run("undo");
    at(1.0);
    CHECK(get("loc.ty") == doctest::Approx(10));
    run("autoKeyframe", {{"state", false}});
}

TEST_CASE("animation round trips through .lila and .lilaa")
{
    for (const char* file : {"anim.lila", "anim.lilaa"})
    {
        CAPTURE(file);
        keyedLocator();
        run("keyTangent", {{"args", {"loc.ty"}}, {"time", 0.0}, {"outTangentType", "linear"}});
        run("keyTangent", {{"args", {"loc.ty"}}, {"time", 1.0}, {"inAngle", 30.0}});
        run("setInfinity", {{"args", {"loc.ty"}}, {"postInfinite", "oscillate"}});
        run("setKeyframe", {{"args", {"loc.rx"}}, {"time", 0.5}, {"value", 90.0}});
        std::vector<double> before;
        for (double t : {0.0, 0.25, 0.5, 0.75, 1.0, 1.5, 2.5})
        {
            at(t);
            before.push_back(get("loc.ty"));
        }
        at(0.25);
        std::string path = tempPath(file);
        run("saveScene", {{"args", {path}}});
        run("newScene");
        run("openScene", {{"args", {path}}});
        CHECK(Graph::current().time() == doctest::Approx(0.25));
        CHECK(run("nodeType", {{"args", {"loc_translateY"}}}) == "animCurveTL");
        CHECK(run("setInfinity", {{"args", {"loc.ty"}}, {"query", true}})[0]["post"] == "oscillate");
        CHECK(get("loc.rx") == doctest::Approx(90));
        size_t i = 0;
        for (double t : {0.0, 0.25, 0.5, 0.75, 1.0, 1.5, 2.5})
        {
            at(t);
            CHECK(get("loc.ty") == doctest::Approx(before[i++]));
        }
        // Undo is clear after a load; the loaded curve still edits.
        run("setKeyframe", {{"args", {"loc.ty"}}, {"time", 2.0}, {"value", -5.0}});
        at(2.0);
        CHECK(get("loc.ty") == doctest::Approx(-5));
    }
}

TEST_CASE("findKeyframe and playbackOptions")
{
    keyedLocator();
    run("select", {{"args", {"loc"}}});
    at(0.5);
    CHECK(run("findKeyframe", {{"which", "next"}}) == 1.0);
    CHECK(run("findKeyframe", {{"which", "previous"}}) == 0.0);
    at(1.0);
    CHECK(run("findKeyframe", {{"which", "next"}}) == 0.0); // wraps

    json p = run("playbackOptions", {{"minTime", 1.0}, {"maxTime", 2.0}, {"fps", 60.0}, {"loop", "once"}});
    CHECK(p["minTime"] == 1.0);
    CHECK(p["fps"] == 60.0);
    CHECK(p["loop"] == false);
    CHECK(scene::anim::frameOf(0.5) == doctest::Approx(30));
    CHECK_THROWS_AS(run("playbackOptions", {{"fps", 0.0}}), CommandError);
    run("playbackOptions", {{"minTime", 0.0}, {"maxTime", 4.0}, {"fps", 30.0}, {"loop", true}});

    CHECK(run("play") == true);
    CHECK(run("play", {{"query", true}}) == true);
    run("stop");
    CHECK(run("play", {{"query", true}}) == false);
}
