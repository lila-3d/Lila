#include "TestUtil.h"

#include "scene/Rig.h"
#include "scene/RigElements.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

using namespace rigtest;
using namespace lila::scene;

namespace
{
double attr(const std::string& plug) { return std::get<double>(graph().value(graph().plug(plug))); }
NodeId id(const std::string& name) { return node(name).id(); }

/** Degrees between two world orientations. */
double turned(const glm::dmat4& a, const glm::dmat4& b)
{
    const glm::dquat qa = glm::quat_cast(rigmath::rotationOf(a)), qb = glm::quat_cast(rigmath::rotationOf(b));
    return glm::degrees(2.0 * std::acos(std::min(1.0, std::abs(glm::dot(qa, qb)))));
}

void arm()
{
    run("newScene");
    run("autoKeyframe", {{"state", false}});
    run("joint", {{"position", v3(0, 140, 0)}, {"name", "chest"}});
    run("joint", {{"position", v3(20, 140, 0)}, {"name", "shoulder"}});
    run("joint", {{"position", v3(48, 140, -4)}, {"name", "elbow"}});
    run("joint", {{"position", v3(75, 140, 0)}, {"name", "wrist"}});
    run("joint", {{"position", v3(85, 140, 0)}, {"name", "finger"}});
}
} // namespace

TEST_CASE("rig elements: twist angle is the rotation's share about the axis")
{
    const glm::dvec3 x(1, 0, 0);
    const glm::dmat3 twist(glm::rotate(glm::dmat4(1.0), glm::radians(40.0), x));
    const glm::dmat3 swing(glm::rotate(glm::dmat4(1.0), glm::radians(30.0), glm::dvec3(0, 0, 1)));
    CHECK(rigmath::twistAngle(twist, x) == doctest::Approx(40.0));
    CHECK(rigmath::twistAngle(swing * twist, x) == doctest::Approx(40.0));
    CHECK(rigmath::twistAngle(swing, x) == doctest::Approx(0.0).scale(1.0));
    CHECK(rigmath::twistAngle(glm::transpose(twist), x) == doctest::Approx(-40.0));
}

TEST_CASE("rig elements: an IK controller poses FK rotations, undoably, and keys them when animated")
{
    arm();
    const std::string chain = run("ikChain", {{"args", {"shoulder", "wrist"}}}).get<std::string>();
    const std::string handle = run("getAttr", {{"args", {chain + ".handle"}}}).get<std::string>();
    CHECK(graph().selection() == std::vector<NodeId>{id(handle)});
    const glm::dvec3 elbow0 = pos("elbow"), wrist0 = pos("wrist");
    const glm::dmat4 hand0 = world("wrist");

    run("ikPose", {{"args", {handle}}, {"moveBy", v3(-10, 15, 5)}});
    checkNear(pos("wrist"), wrist0 + glm::dvec3(-10, 15, 5));
    CHECK(glm::length(pos("elbow") - elbow0) > 1.0);
    CHECK(attr("elbow.ikBlend") == doctest::Approx(0.0));
    // The hand keeps its world orientation as the controller moves without turning.
    CHECK(turned(world("wrist"), hand0) < 1e-6);
    // Unanimated: set, not keyed.
    CHECK(run("keyframe", {{"args", {"shoulder.rotate"}}, {"query", true}, {"keyCount", true}}).get<int>() == 0);
    run("undo");
    checkNear(pos("wrist"), wrist0);
    checkNear(pos("elbow"), elbow0);

    // Animated: posing keys the joints, the controller and the lock channel.
    run("setKeyframe", {{"args", {"shoulder.rotate", "elbow.rotate", "wrist.rotate"}}, {"time", 0.0}});
    run("currentTime", {{"args", {1.0}}});
    run("ikPose", {{"args", {"wrist"}}, {"rotateBy", v3(0, 0, 30)}});
    CHECK(turned(world("wrist"), hand0) == doctest::Approx(30.0).epsilon(1e-6));
    CHECK(run("keyframe", {{"args", {"wrist.rotate"}}, {"query", true}, {"keyCount", true}}).get<int>() >= 2);
    CHECK(run("keyframe", {{"args", {chain + ".lock"}}, {"query", true}, {"keyCount", true}}).get<int>() == 1);
    run("currentTime", {{"args", {0.0}}});
    CHECK(turned(world("wrist"), hand0) < 1e-6);
    run("currentTime", {{"args", {1.0}}});
    CHECK(turned(world("wrist"), hand0) == doctest::Approx(30.0).epsilon(1e-6));
}

TEST_CASE("rig elements: the controller sits on the FK pose and a lock holds it between keys")
{
    arm();
    const std::string chain = run("ikChain", {{"args", {"shoulder", "wrist"}}}).get<std::string>();
    const std::string handle = run("getAttr", {{"args", {chain + ".handle"}}}).get<std::string>();
    run("setKeyframe", {{"args", {"chest.translate", "shoulder.rotate", "elbow.rotate", "wrist.rotate"}}, {"time", 0.0}});
    run("setKeyframe", {{"args", {"chest.tx"}}, {"time", 1.0}, {"value", 12.0}});
    run("setKeyframe", {{"args", {"shoulder.rotate", "elbow.rotate", "wrist.rotate"}}, {"time", 1.0}});
    Graph& g = graph();
    run("currentTime", {{"args", {0.5}}});
    const glm::dvec3 drifted = pos("wrist");
    checkNear(glm::dvec3(rigel::controllerWorld(g, id(chain))[3]), drifted);

    // Planted at 0 (a hand on a table): held until the chain's next key, wherever the chest goes.
    const json locked = run("ikLock", {{"args", {handle}}, {"time", 0.0}});
    CHECK(locked[0]["locked"] == true);
    run("currentTime", {{"args", {0.0}}});
    const glm::dvec3 planted = pos("wrist");
    for (double t : {0.25, 0.5, 0.75})
    {
        CAPTURE(t);
        run("currentTime", {{"args", {t}}});
        CHECK(rigel::held(g, id(chain)));
        checkNear(pos("wrist"), planted, 1e-5);
    }
    CHECK(rigel::locked(g, id(chain), 0.5));
    run("ikLock", {{"args", {chain}}, {"time", 0.0}, {"state", false}});
    run("currentTime", {{"args", {0.5}}});
    CHECK_FALSE(rigel::held(g, id(chain)));
    checkNear(pos("wrist"), drifted);
}

TEST_CASE("rig elements: twist drivers counter the parent's twist or follow the hand's")
{
    arm();
    run("joint", {{"position", v3(29.33, 140, -1.33)}, {"name", "upper_twist"}, {"parent", "shoulder"}});
    run("joint", {{"position", v3(66, 140, -1.33)}, {"name", "fore_twist"}, {"parent", "elbow"}});
    const json counter = run("twistDriver", {{"args", {"upper_twist"}}});
    const json follow = run("twistDriver", {{"args", {"fore_twist"}}, {"source", "wrist"}});
    // Default shares from where they sit along the bone: a third of the way up, two thirds down the forearm.
    CHECK(attr(counter[0].get<std::string>() + ".weight") == doctest::Approx(-2.0 / 3.0).epsilon(1e-3));
    CHECK(attr(follow[0].get<std::string>() + ".weight") == doctest::Approx(2.0 / 3.0).epsilon(1e-2));
    const glm::dmat4 upper0 = world("upper_twist");
    // At rest nothing moves.
    CHECK(attr("upper_twist.ikBlend") == doctest::Approx(1.0));

    Graph& g = graph();
    auto spin = [&](const char* joint, const char* toward, double degrees) {
        const glm::dvec3 axis = glm::normalize(pos(toward) - pos(joint));
        const glm::dmat4 w = world(joint);
        glm::dmat4 want = glm::dmat4(glm::dmat3(glm::rotate(glm::dmat4(1.0), glm::radians(degrees), axis))) * w;
        want[3] = w[3];
        const NodeId n = id(joint);
        Transform::setLocalMatrix(g, n, glm::inverse(g.node(n)->as<glm::dmat4>(Transform::kParentMatrix)) * want);
    };
    spin("shoulder", "elbow", 60.0);
    CHECK(turned(world("upper_twist"), upper0) == doctest::Approx(20.0).epsilon(1e-2));
    const glm::dmat4 fore0 = world("fore_twist");
    spin("wrist", "finger", 45.0);
    // The wrist turns about the hand's axis, a little off the forearm's: two thirds of its twist along the forearm.
    CHECK(turned(world("fore_twist"), fore0) == doctest::Approx(30.0).epsilon(0.02));
    CHECK_THROWS_AS(run("twistDriver", {{"args", {"upper_twist"}}}), CommandError);
}

TEST_CASE("rig elements: deform flags default on")
{
    arm();
    CHECK(node("elbow").as<bool>(Joint::kDeform));
    run("setAttr", {{"args", {"elbow.deform", false}}});
    CHECK_FALSE(rigel::isDeform(node("elbow")));
    CHECK(rigel::isDeform(node("wrist")));
}

TEST_CASE("rig elements: a looker's axes are choices that read old string values")
{
    arm();
    const std::string looker = run("looker", {{"args", {"wrist"}}, {"aimAxis", "-z"}, {"upAxis", "+y"}}).get<std::string>();
    const AttrDef& def = node(looker).type().attrs[size_t(node(looker).type().attrIndex("aimAxis"))];
    CHECK(def.type == PlugType::Enum);
    CHECK(def.enumNames.size() == 6);
    CHECK(std::get<int64_t>(graph().value(graph().plug(looker + ".aimAxis"))) == 5);
    CHECK(std::get<int64_t>(graph().value(graph().plug(looker + ".upAxis"))) == 2);
    CHECK(std::get<int64_t>(fromJson(def, json("x"))) == 0);
    CHECK_THROWS(run("looker", {{"args", {"elbow"}}, {"aimAxis", "w"}}));
}
