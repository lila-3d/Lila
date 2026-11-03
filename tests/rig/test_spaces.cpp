#include "TestUtil.h"

#include "scene/Anim.h"
#include "scene/AnimLayer.h"
#include "scene/RotationCurve.h"
#include "scene/Spaces.h"
#include "scene/TransformOps.h"

#include <glm/gtc/quaternion.hpp>

#include <filesystem>

using namespace rigtest;
using namespace lila::scene;

namespace
{
void evalAt(double t)
{
    graph().setTime(t);
    graph().evaluate();
}

double maxDiff(const glm::dmat4& a, const glm::dmat4& b)
{
    double worst = 0.0;
    for (int c = 0; c < 4; ++c)
        worst = std::max(worst, glm::length(a[c] - b[c]));
    return worst;
}

std::vector<glm::dmat4> worldsAt(const std::string& name, const std::vector<double>& times)
{
    std::vector<glm::dmat4> out;
    for (double t : times)
    {
        evalAt(t);
        out.push_back(world(name));
    }
    return out;
}

std::vector<double> frames(double t0, double t1)
{
    std::vector<double> out;
    for (long f = std::lround(t0 * kFps); f <= std::lround(t1 * kFps); ++f)
        out.push_back(double(f) / kFps);
    return out;
}

int spaceAt(const std::string& node, double t)
{
    evalAt(t);
    return int(std::get<int64_t>(graph().value({rigtest::node(node).id(), Transform::kSpace, -1})));
}

/** root > chest > hand, with a prop to the side and every joint turned a little. */
void propCharacter()
{
    run("newScene");
    run("autoKeyframe", {{"state", false}});
    run("playbackOptions", {{"fps", kFps}, {"minTime", 0.0}, {"maxTime", 1.0}});
    run("joint", {{"name", "root"}, {"position", v3(0, 0, 0)}});
    run("joint", {{"name", "chest"}, {"position", v3(0, 100, 0)}, {"orientation", v3(0, 0, 20)}});
    run("joint", {{"name", "hand"}, {"position", v3(30, 110, 5)}, {"orientation", v3(10, 30, 0)}});
    run("spaceLocator", {{"name", "prop"}});
    run("setAttr", {{"args", {"prop.translate", v3(50, 80, 20)}}});
    run("setAttr", {{"args", {"prop.rotate", v3(0, 90, 0)}}});
    run("select", {{"args", json::array()}});
}

void keyAnimation()
{
    run("setKeyframe", {{"args", {"root.translate", "root.rotate"}}, {"time", 0.0}});
    run("setKeyframe", {{"args", {"hand.translate", "hand.rotate"}}, {"time", 0.0}});
    evalAt(1.0);
    run("setAttr", {{"args", {"root.translate", v3(40, 0, -10)}}});
    run("setAttr", {{"args", {"root.rotate", v3(0, 60, 0)}}});
    run("setAttr", {{"args", {"hand.rotate", v3(20, -40, 70)}}});
    run("setKeyframe", {{"args", {"root.translate", "root.rotate", "hand.rotate"}}, {"time", 1.0}});
    evalAt(0.5);
    run("setAttr", {{"args", {"hand.rotate", v3(-30, 15, 100)}}});
    run("setAttr", {{"args", {"hand.translate", v3(10, 20, 30)}}});
    run("setKeyframe", {{"args", {"hand.translate", "hand.rotate"}}, {"time", 0.5}});
    evalAt(0.0);
}
} // namespace

TEST_CASE("spaces: adding and switching keep the pose, and each space follows what it names")
{
    propCharacter();
    json spaces = run("addSpace", {{"args", {"hand"}}, {"space", {"world", "prop"}}});
    REQUIRE(spaces["spaces"].size() == 3);
    CHECK(spaces["spaces"][0]["name"] == "parent");
    CHECK(spaces["spaces"][1]["name"] == "world");
    CHECK(spaces["spaces"][2]["name"] == "prop");
    // The parent is always there; adding it or a space again changes nothing.
    run("addSpace", {{"args", {"hand"}}, {"space", {"world", "chest", "parent"}}});
    CHECK(run("listSpaces", {{"args", {"hand"}}})["spaces"].size() == 3);

    const glm::dmat4 rest = world("hand");
    CHECK(run("switchSpace", {{"args", {"hand"}}, {"space", "world"}}) == 1);
    CHECK(maxDiff(world("hand"), rest) < 1e-9);
    run("setAttr", {{"args", {"root.translate", v3(0, 0, 50)}}});
    CHECK(maxDiff(world("hand"), rest) < 1e-9);

    run("switchSpace", {{"args", {"hand"}}, {"space", "prop"}});
    CHECK(maxDiff(world("hand"), rest) < 1e-9);
    const glm::dmat4 prop = world("prop");
    run("setAttr", {{"args", {"prop.translate", v3(0, 0, 0)}}});
    run("setAttr", {{"args", {"prop.rotate", v3(45, 0, 0)}}});
    CHECK(maxDiff(world("hand"), world("prop") * glm::inverse(prop) * rest) < 1e-9);

    run("undo");
    run("undo");
    CHECK(maxDiff(world("hand"), rest) < 1e-9);

    // A space the node itself drives would be a cycle.
    CHECK_THROWS_AS(run("addSpace", {{"args", {"chest"}}, {"space", "hand"}}), CommandError);
    CHECK_THROWS_AS(run("switchSpace", {{"args", {"hand"}}, {"space", "nowhere"}}), CommandError);
}

TEST_CASE("spaces: the transform tools work in the active space's frame")
{
    propCharacter();
    run("addSpace", {{"args", {"hand"}}, {"space", "prop"}, {"maintainOffset", false}});
    run("switchSpace", {{"args", {"hand"}}, {"space", "prop"}});
    Graph& g = graph();
    const NodeId hand = node("hand").id();
    const glm::dmat4 before = world("hand");
    // Without an offset the frame is the prop's own.
    CHECK(maxDiff(node("hand").as<glm::dmat4>(Transform::kParentMatrix), world("prop")) < 1e-9);

    xformops::moveBy(g, {hand}, {10, -5, 2}, Space::World);
    checkNear(pos("hand"), glm::dvec3(before[3]) + glm::dvec3(10, -5, 2), 1e-9);

    const glm::dmat4 moved = world("hand");
    const glm::dquat turn = glm::angleAxis(glm::radians(40.0), glm::normalize(glm::dvec3(1, 2, 0)));
    xformops::rotateBy(g, {hand}, turn, Space::World);
    CHECK(maxDiff(glm::dmat4(glm::dmat3(world("hand"))), glm::dmat4(glm::mat3_cast(turn) * glm::dmat3(moved))) < 1e-9);
    checkNear(pos("hand"), glm::dvec3(moved[3]), 1e-9);

    // Parent space moves along the prop's axes: its +X is world -Z after the 90 degree turn.
    const glm::dvec3 p = pos("hand");
    run("move", {{"args", {"hand"}}, {"by", v3(5, 0, 0)}, {"space", "parent"}});
    checkNear(pos("hand"), p + glm::dvec3(0, 0, -5), 1e-9);
    checkNear(xformops::worldPivot(g, hand), pos("hand"), 1e-9);
}

TEST_CASE("spaces: an animated switch keys the space stepped and keeps the pose")
{
    propCharacter();
    keyAnimation();
    run("addSpace", {{"args", {"hand"}}, {"space", "world"}});
    const std::vector<double> all = frames(0.0, 1.0);
    const std::vector<glm::dmat4> before = worldsAt("hand", all);

    run("switchSpace", {{"args", {"hand"}}, {"space", "world"}, {"time", 0.5}});
    CHECK(spaceAt("hand", 0.5) == 1);
    CHECK(spaceAt("hand", 1.0) == 1);
    CHECK(spaceAt("hand", 0.5 - 1.0 / kFps) == 0);
    const std::vector<glm::dmat4> after = worldsAt("hand", all);
    // The switch frame, the held frame before it and the first key keep their pose.
    CHECK(maxDiff(after[15], before[15]) < 1e-6);
    CHECK(maxDiff(after[14], before[14]) < 1e-6);
    CHECK(maxDiff(after[0], before[0]) < 1e-6);
    // After the switch the hand ignores the root's motion: at the last key it still sits where it was keyed in world.
    CHECK(maxDiff(after[30], before[30]) > 1e-3);

    run("undo");
    const std::vector<glm::dmat4> undone = worldsAt("hand", all);
    for (size_t i = 0; i < all.size(); ++i)
        CHECK(maxDiff(undone[i], before[i]) < 1e-9);
    CHECK_FALSE(anim::curveFor(graph(), {node("hand").id(), Transform::kSpace, -1}));
}

TEST_CASE("spaces: a range switch keeps every key's pose, and bake every frame, with Euler and rotation curves")
{
    for (std::string interp : {"euler", "smooth"})
        for (bool bake : {false, true})
        {
            CAPTURE(interp);
            CAPTURE(bake);
            propCharacter();
            run("setAttr", {{"args", {"hand.rotationInterpolation", interp}}});
            keyAnimation();
            if (interp == "smooth")
                REQUIRE(anim::isRotationCurve(*graph().node(anim::curveFor(graph(), {node("hand").id(), Transform::kRotate, 0}))));
            run("addSpace", {{"args", {"hand"}}, {"space", "prop"}});
            run("setKeyframe", {{"args", {"prop.ty"}}, {"time", 0.0}, {"value", 80.0}});
            run("setKeyframe", {{"args", {"prop.ty"}}, {"time", 1.0}, {"value", 140.0}});
            const std::vector<double> all = frames(0.0, 1.0);
            const std::vector<glm::dmat4> before = worldsAt("hand", all);

            run("switchSpace", {{"args", {"hand"}}, {"space", "prop"}, {"range", {0.2, 0.8}}, {"bake", bake}});
            CHECK(spaceAt("hand", 0.2) == 1);
            CHECK(spaceAt("hand", 0.8) == 1);
            CHECK(spaceAt("hand", 0.8 + 1.0 / kFps) == 0);
            const std::vector<glm::dmat4> after = worldsAt("hand", all);
            // Keys in the range, its ends and the held frames either side.
            for (int f : {5, 6, 15, 24, 25})
            {
                CAPTURE(f);
                CHECK(maxDiff(after[size_t(f)], before[size_t(f)]) < 1e-6);
            }
            for (int f : {0, 30})
                CHECK(maxDiff(after[size_t(f)], before[size_t(f)]) < 1e-6);
            if (bake)
                for (int f = 6; f <= 24; ++f)
                {
                    CAPTURE(f);
                    CHECK(maxDiff(after[size_t(f)], before[size_t(f)]) < 1e-6);
                }
        }
}

TEST_CASE("spaces: switching on an animation layer keys the layer and keeps the pose")
{
    propCharacter();
    keyAnimation();
    run("addSpace", {{"args", {"hand"}}, {"space", "world"}});
    run("createAnimLayer", {{"name", "Over"}, {"empty", true}});
    const std::vector<double> all = frames(0.0, 1.0);
    const std::vector<glm::dmat4> before = worldsAt("hand", all);
    evalAt(0.5);
    run("switchSpace", {{"args", {"hand"}}, {"space", "world"}});
    CHECK(graph().find("hand_space_Over"));
    const std::vector<glm::dmat4> after = worldsAt("hand", all);
    CHECK(maxDiff(after[15], before[15]) < 1e-6);
    CHECK(maxDiff(after[14], before[14]) < 1e-6);
    CHECK(spaceAt("hand", 0.6) == 1);
    // Muting the layer goes back to the base animation in the parent space.
    run("editAnimLayer", {{"args", {"Over"}}, {"mute", true}});
    CHECK(spaceAt("hand", 0.6) == 0);
    CHECK(maxDiff(worldsAt("hand", {0.7})[0], before[21]) < 1e-6);
}

TEST_CASE("spaces: each take keeps its own space keys")
{
    propCharacter();
    keyAnimation();
    run("addSpace", {{"args", {"hand"}}, {"space", "world"}});
    run("createTake", {{"name", "A"}, {"start", 0.0}, {"end", 1.0}});
    run("switchSpace", {{"args", {"hand"}}, {"space", "world"}, {"time", 0.5}});
    const std::vector<glm::dmat4> a = worldsAt("hand", frames(0.0, 1.0));
    run("createTake", {{"name", "B"}, {"start", 0.0}, {"end", 1.0}});
    run("setActiveTake", {{"args", {"B"}}});
    CHECK(spaceAt("hand", 0.8) == 0);
    run("setActiveTake", {{"args", {"A"}}});
    CHECK(spaceAt("hand", 0.8) == 1);
    const std::vector<glm::dmat4> back = worldsAt("hand", frames(0.0, 1.0));
    for (size_t i = 0; i < a.size(); ++i)
        CHECK(maxDiff(back[i], a[i]) < 1e-9);
}

TEST_CASE("spaces: save and load keep spaces, offsets and keys")
{
    propCharacter();
    keyAnimation();
    run("addSpace", {{"args", {"hand"}}, {"space", {"world", "prop"}}});
    run("switchSpace", {{"args", {"hand"}}, {"space", "prop"}, {"time", 0.5}});
    run("addSpace", {{"args", {"chest"}}, {"space", "world"}});
    run("switchSpace", {{"args", {"chest"}}, {"space", "world"}});
    const std::vector<double> all = frames(0.0, 1.0);
    const std::vector<glm::dmat4> hand = worldsAt("hand", all);
    const std::vector<glm::dmat4> chest = worldsAt("chest", all);
    const auto dir = std::filesystem::temp_directory_path() / "lila_rig_tests";
    std::filesystem::create_directories(dir);
    for (const char* file : {"spaces.lila", "spaces.lilaa"})
    {
        CAPTURE(file);
        const std::string path = (dir / file).string();
        run("saveScene", {{"args", {path}}});
        run("newScene");
        run("openScene", {{"args", {path}}});
        json list = run("listSpaces", {{"args", {"hand"}}});
        REQUIRE(list["spaces"].size() == 3);
        CHECK(list["spaces"][2]["name"] == "prop");
        const std::vector<glm::dmat4> h = worldsAt("hand", all), c = worldsAt("chest", all);
        for (size_t i = 0; i < all.size(); ++i)
        {
            CHECK(maxDiff(h[i], hand[i]) < 1e-9);
            CHECK(maxDiff(c[i], chest[i]) < 1e-9);
        }
    }
}

TEST_CASE("spaces: removing a space hands its frames back to the parent, keeping the pose")
{
    propCharacter();
    keyAnimation();
    run("addSpace", {{"args", {"hand"}}, {"space", {"world", "prop"}}});
    run("switchSpace", {{"args", {"hand"}}, {"space", "world"}, {"time", 0.3}});
    run("switchSpace", {{"args", {"hand"}}, {"space", "prop"}, {"time", 0.7}});
    const std::vector<double> all = frames(0.0, 1.0);
    const std::vector<glm::dmat4> before = worldsAt("hand", all);

    json list = run("removeSpace", {{"args", {"hand"}}, {"space", "world"}});
    REQUIRE(list["spaces"].size() == 2);
    CHECK(spaceAt("hand", 0.4) == 0);
    CHECK(spaceAt("hand", 0.8) == 1);
    const std::vector<glm::dmat4> after = worldsAt("hand", all);
    for (int f : {0, 9, 15, 20, 21, 30})
    {
        CAPTURE(f);
        CHECK(maxDiff(after[size_t(f)], before[size_t(f)]) < 1e-6);
    }

    // The last space takes the switch node with it.
    run("removeSpace", {{"args", {"hand"}}, {"space", "prop"}});
    CHECK_FALSE(spaces::hasSpaces(graph(), node("hand").id()));
    CHECK_FALSE(graph().find("hand_spaceSwitch"));
    CHECK(maxDiff(worldsAt("hand", {1.0})[0], before[30]) < 1e-6);
    run("undo");
    CHECK(spaces::hasSpaces(graph(), node("hand").id()));
}

TEST_CASE("spaces: the space channel keys with the node and stays out of poses")
{
    propCharacter();
    Graph& g = graph();
    const std::vector<PlugRef> none = anim::keyablePlugs(g, node("hand").id());
    CHECK(std::none_of(none.begin(), none.end(), [](const PlugRef& p) { return p.attr == Transform::kSpace; }));
    run("addSpace", {{"args", {"hand"}}, {"space", "world"}});
    const std::vector<PlugRef> plugs = anim::keyablePlugs(g, node("hand").id());
    CHECK(std::any_of(plugs.begin(), plugs.end(), [](const PlugRef& p) { return p.attr == Transform::kSpace; }));
    run("select", {{"args", {"hand"}}});
    run("setKeyframe");
    const NodeId curve = anim::curveFor(g, {node("hand").id(), Transform::kSpace, -1});
    REQUIRE(curve);
    CHECK(anim::curveOf(*g.node(curve)).keys.front().out == anim::Tangent::Step);
}
