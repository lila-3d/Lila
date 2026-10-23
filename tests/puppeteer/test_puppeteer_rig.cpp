#include <doctest/doctest.h>

#include "rig/RigMath.h"

#include "ops/Command.h"
#include "extensions/ExtensionManager.h"
#include "scene/Graph.h"
#include "scene/Rig.h"
#include "scene/RigElements.h"
#include "scene/Transform.h"
#include "ui/Actions.h"
#include "ui/Gizmo.h"
#include "ui/Layouts.h"
#include "ui/Menus.h"
#include "ui/Modes.h"
#include "ui/SelectionHandlers.h"

using namespace lila;
using namespace lila::scene;
using json = nlohmann::json;
using Approx = doctest::Approx;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }
json v3(double x, double y, double z) { return json::array({x, y, z}); }

std::string package() { return LILA_TEST_PUPPETEER_PACKAGE; }

void loadPuppeteer()
{
    if (!ops::find("puppeteerIkChain"))
        REQUIRE(extensions::ExtensionManager::get().load(package()) == "Puppeteer");
}

Graph& graph() { return Graph::current(); }
NodeId id(const std::string& name)
{
    Node* n = graph().find(name);
    REQUIRE(n);
    return n->id();
}
glm::dmat4 world(const std::string& name)
{
    graph().evaluate();
    return std::get<glm::dmat4>(graph().value({id(name), Transform::kWorldMatrix, -1}));
}
glm::dvec3 pos(const std::string& name) { return glm::dvec3(world(name)[3]); }
double attr(const std::string& plug) { return std::get<double>(graph().value(graph().plug(plug))); }
void setTime(double t) { run("currentTime", {{"args", {t}}}); }

bool near(const glm::dvec3& a, const glm::dvec3& b, double eps) { return glm::length(a - b) <= eps; }
bool nearRotation(const glm::dmat4& a, const glm::dmat4& b, double eps)
{
    const glm::dmat3 ra = puppeteer::math::rotationOf(a), rb = puppeteer::math::rotationOf(b);
    for (int c = 0; c < 3; ++c)
        if (glm::length(ra[c] - rb[c]) > eps)
            return false;
    return true;
}

/** A left leg standing on the ground: hip, knee (bent slightly forward), ankle, ball, toe; plus a character. */
void legScene()
{
    run("newScene");
    run("autoKeyframe", {{"state", false}});
    run("joint", {{"position", v3(10, 100, 0)}, {"name", "hip_l"}});
    run("joint", {{"position", v3(10, 55, 6)}, {"name", "knee_l"}});
    run("joint", {{"position", v3(10, 10, 0)}, {"name", "ankle_l"}});
    run("joint", {{"position", v3(10, 3, 12)}, {"name", "ball_l"}});
    run("joint", {{"position", v3(10, 0, 20)}, {"name", "toe_l"}});
    run("puppeteerCharacter", {{"args", {"hip_l"}}, {"name", "hero"}});
}

json registries()
{
    return {{"modes", ui::modes::registry().ids()},
            {"gizmos", ui::gizmos::registry().ids()},
            {"selection", ui::selectionHandlers::registry().ids()},
            {"layouts", ui::layouts::contributed().ids()},
            {"actions", ui::actions::registry().ids()},
            {"items", ui::menus::itemRegistry().ids()},
            {"types", types::registry().ids()},
            {"commands", ops::registry().ids()}};
}
} // namespace

TEST_CASE("puppeteer: modes, gizmo and selection handlers come and go with the extension")
{
    extensions::ExtensionManager& em = extensions::ExtensionManager::get();
    run("newScene");
    if (ops::find("puppeteerIkChain"))
        em.unload("Puppeteer");
    const json before = registries();
    REQUIRE(em.load(package()) == "Puppeteer");

    const ui::modes::Mode* animate = ui::modes::registry().find("puppeteerAnimate");
    const ui::modes::Mode* rigMode = ui::modes::registry().find("puppeteerRig");
    REQUIRE(animate);
    REQUIRE(rigMode);
    CHECK(animate->gizmoProvider == "puppeteer.spinner");
    CHECK(animate->selectionHandler == "puppeteer.animate");
    CHECK(rigMode->selectionHandler == "puppeteer.rig");
    // Space's tap swaps in the graph editor; its hold (the hotbox) is left alone.
    CHECK(animate->hotkeys["viewport"]["Space"] == json{{"press", "puppeteer.graphFullScreen"}});
    CHECK_FALSE(animate->hotkeys["viewport"]["Space"].contains("hold"));
    CHECK(ui::gizmos::find("puppeteer.spinner"));
    CHECK(ui::selectionHandlers::find("puppeteer.animate"));
    CHECK(ui::layouts::contributed().find("Puppeteer Animate"));
    CHECK(ui::actions::find("mode.puppeteerAnimate"));
    CHECK(ui::actions::find("puppeteer.spinner.inheritRotate"));

    // Entering the mode switches the viewport's gizmo and selection handler; leaving puts them back.
    REQUIRE(ui::modes::activate("puppeteerAnimate"));
    CHECK(ui::gizmos::activeId() == "puppeteer.spinner");
    CHECK(ui::selectionHandlers::activeId() == "puppeteer.animate");
    REQUIRE(ui::modes::activate(ui::modes::kDefaultMode));
    CHECK(ui::gizmos::activeId() != "puppeteer.spinner");

    // Unloading while in the mode leaves it first.
    REQUIRE(ui::modes::activate("puppeteerRig"));
    em.unload("Puppeteer");
    CHECK(ui::modes::current() == ui::modes::kDefaultMode);
    CHECK(registries() == before);
    CHECK_FALSE(ops::find("puppeteerIkChain"));
}

TEST_CASE("puppeteer: the character finds its skeleton and groups its rig elements")
{
    loadPuppeteer();
    legScene();
    const json info = run("puppeteerCharacterInfo", {{"args", {"knee_l"}}});
    CHECK(info["name"] == "hero");
    CHECK(info["root"] == "hip_l");
    CHECK(info["poseLibrary"] == "hip_l");
    run("puppeteerIkChain", {{"args", {"hip_l", "ankle_l"}}});
    const json after = run("puppeteerCharacterInfo", {{"character", "hero"}});
    REQUIRE(after["elements"].size() == 1);
    CHECK(after["elements"][0]["type"] == "ikChain");
    CHECK_THROWS_AS(run("puppeteerCharacter", {{"args", {"toe_l"}}}), CommandError);
}

TEST_CASE("puppeteer: grabbing the controller poses the joints' rotations; there is no IK/FK switch")
{
    loadPuppeteer();
    legScene();
    const std::string chain = run("puppeteerIkChain", {{"args", {"hip_l", "ankle_l"}}}).get<std::string>();
    const std::string handle = run("getAttr", {{"args", {chain + ".handle"}}}).get<std::string>();
    CHECK_FALSE(graph().find(chain)->type().attrIndex("ik") >= 0);
    // Not held: the joints are FK, whatever the handle does on its own.
    CHECK(attr("knee_l.ikBlend") == Approx(0.0));
    run("setKeyframe", {{"args", {"hip_l.rotate", "knee_l.rotate", "ankle_l.rotate"}}, {"time", 0.0}});
    setTime(1.0);
    run("move", {{"args", {handle}}, {"by", v3(0, 25, 10)}});
    const glm::dvec3 knee = pos("knee_l"), ankle = pos("ankle_l");
    CHECK(glm::length(knee - glm::dvec3(10, 55, 6)) > 1.0);
    CHECK(near(ankle, pos(handle), 1e-6));
    // Solved into FK and keyed: the pose stays with the solver off, and at the key after a scrub.
    CHECK(attr("knee_l.ikBlend") == Approx(0.0));
    setTime(0.0);
    CHECK(near(pos("ankle_l"), glm::dvec3(10, 10, 0), 1e-6));
    setTime(1.0);
    CHECK(near(pos("knee_l"), knee, 1e-6));
    CHECK(near(pos("ankle_l"), ankle, 1e-6));
    // Turning the thigh FK moves the foot, and the drawn controller follows it until grabbed.
    run("puppeteerRotate", {{"args", {"hip_l"}}, {"axis", v3(1, 0, 0)}, {"angle", 10.0}});
    const glm::dvec3 moved = pos("ankle_l");
    CHECK(glm::length(moved - ankle) > 1.0);
    CHECK(near(glm::dvec3(lila::scene::rigel::controllerWorld(graph(), id(chain))[3]), moved, 1e-6));
    const glm::dvec3 kneeFk = pos("knee_l");
    run("ikMatch", {{"args", {chain}}});
    CHECK(near(pos(handle), moved, 1e-6));
    CHECK(near(pos("knee_l"), kneeFk, 1e-9));
    // Grabbing it from there solves from the FK pose: a zero move changes nothing.
    run("move", {{"args", {handle}}, {"by", v3(0, 0, 0)}});
    CHECK(near(pos("knee_l"), kneeFk, 1e-6));
    CHECK(near(pos("ankle_l"), moved, 1e-6));
}

TEST_CASE("puppeteer: a locked key holds the foot until the chain's next key")
{
    loadPuppeteer();
    legScene();
    run("joint", {{"position", v3(0, 100, 0)}, {"name", "pelvis"}, {"parent", ""}});
    run("parent", {{"args", {"hip_l", "pelvis"}}});
    const std::string chain = run("puppeteerIkChain", {{"args", {"hip_l", "ankle_l"}}}).get<std::string>();
    const std::string foot = run("puppeteerReverseFoot", {{"args", {"ankle_l"}}}).get<std::string>();
    // A key at 0 and 1, the pelvis lowered at 1 in FK.
    run("setKeyframe", {{"args", {"pelvis.translate", "hip_l.rotate", "knee_l.rotate", "ankle_l.rotate", "ball_l.rotate"}}, {"time", 0.0}});
    run("setKeyframe", {{"args", {"pelvis.ty"}}, {"time", 1.0}, {"value", 92.0}});
    run("setKeyframe", {{"args", {"hip_l.rotate", "knee_l.rotate", "ankle_l.rotate", "ball_l.rotate"}}, {"time", 1.0}});
    setTime(0.0);
    const glm::dvec3 planted = pos("ankle_l");
    setTime(0.5);
    CHECK(pos("ankle_l").y < planted.y - 1.0); // FK: the foot sinks with the hip
    run("puppeteerFootLock", {{"args", {foot}}, {"time", 0.0}, {"state", true}});
    CHECK(attr(chain + ".lock") == Approx(1.0));
    for (double t : {0.0, 0.25, 0.5, 0.9})
    {
        CAPTURE(t);
        setTime(t);
        CHECK(attr("knee_l.ikBlend") == Approx(1.0));
        CHECK(near(pos("ankle_l"), planted, 1e-4));
    }
    run("puppeteerFootLock", {{"args", {foot}}, {"time", 0.0}, {"state", false}});
    setTime(0.5);
    CHECK(attr("knee_l.ikBlend") == Approx(0.0));
    CHECK(pos("ankle_l").y < planted.y - 1.0);
}

TEST_CASE("puppeteer: older scenes' IK chains migrate, their IK pose keyed as rotations")
{
    loadPuppeteer();
    legScene();
    Graph& g = graph();
    // The old layout: an ikHandle whose ikBlend came from a puppeteerIkChain's keyed ik channel.
    const std::string handle = run("ikHandle", {{"startJoint", "hip_l"}, {"endEffector", "ankle_l"}, {"name", "leg_ikHandle"}}).get<std::string>();
    const std::string chain = run("createNode", {{"args", {"puppeteerIkChain"}}, {"name", "ankle_l_ik"}, {"skipSelect", true}}).get<std::string>();
    run("setAttr", {{"args", {chain + ".startJoint", "hip_l"}}});
    run("setAttr", {{"args", {chain + ".endJoint", "ankle_l"}}});
    run("setAttr", {{"args", {chain + ".handle", handle}}});
    run("connectAttr", {{"args", {chain + ".ikBlend", handle + ".ikBlend"}}});
    run("setKeyframe", {{"args", {chain + ".ik"}}, {"time", 0.0}, {"value", 1.0}});
    run("setKeyframe", {{"args", {handle + ".translate"}}, {"time", 0.0}});
    run("setKeyframe", {{"args", {handle + ".ty"}}, {"time", 1.0}, {"value", 30.0}});
    std::vector<std::pair<glm::dvec3, glm::dvec3>> before;
    for (double t : {0.0, 1.0})
    {
        setTime(t);
        before.push_back({pos("knee_l"), pos("ankle_l")});
    }
    CHECK(before[1].second.y == Approx(30.0));
    CHECK(run("puppeteerMigrateRig") == 1);
    REQUIRE(g.find(chain));
    CHECK(g.find(chain)->type().name == "ikChain");
    CHECK(g.find(chain)->type().attrIndex("ik") < 0);
    CHECK(run("keyframe", {{"args", {"knee_l.rotate"}}, {"query", true}, {"keyCount", true}}).get<int>() > 0);
    int i = 0;
    for (double t : {0.0, 1.0})
    {
        CAPTURE(t);
        setTime(t);
        CHECK(attr("knee_l.ikBlend") == Approx(0.0));
        CHECK(near(pos("knee_l"), before[size_t(i)].first, 1e-6));
        CHECK(near(pos("ankle_l"), before[size_t(i)].second, 1e-6));
        ++i;
    }
    CHECK(run("puppeteerMigrateRig") == 0);
}

TEST_CASE("puppeteer: reverse foot rolls on the ball, keeps the toes down, and snaps keeping the pose")
{
    loadPuppeteer();
    legScene();
    run("puppeteerIkChain", {{"args", {"hip_l", "ankle_l"}}});
    const std::string foot = run("puppeteerReverseFoot", {{"args", {"ankle_l"}}}).get<std::string>();
    const glm::dvec3 ankle0 = pos("ankle_l"), toe0 = pos("toe_l");
    const glm::dmat4 ball0 = world("ball_l");
    CHECK(near(ankle0, glm::dvec3(10, 10, 0), 1e-3));
    CHECK(near(toe0, glm::dvec3(10, 0, 20), 1e-3));

    run("puppeteerFootChannels", {{"args", {foot}}, {"roll", 20.0}});
    CHECK(attr(foot + ".roll") == Approx(20.0));
    CHECK(pos("ankle_l").y > ankle0.y + 1.0);
    // The foot rocks on the sole under the ball: the ball joint keeps its angle to the ground and the toes stay down.
    CHECK(nearRotation(world("ball_l"), ball0, 1e-3));
    CHECK(std::abs(pos("toe_l").y - toe0.y) < 0.5);
    CHECK(std::abs(pos("toe_l").z - toe0.z) < 1.5);

    run("puppeteerFootChannels", {{"args", {foot}}, {"bank", 10.0}, {"twist", -15.0}});
    const glm::dmat4 posed = world("ankle_l");
    run("puppeteerSnapReverseFoot", {{"args", {foot}}, {"key", false}});
    CHECK(attr(foot + ".roll") == Approx(0.0));
    CHECK(near(glm::dvec3(world("ankle_l")[3]), glm::dvec3(posed[3]), 1e-6));
    CHECK(nearRotation(world("ankle_l"), posed, 1e-6));
    run("puppeteerSnapReverseFoot", {{"args", {foot}}, {"align", true}, {"key", false}});
    CHECK(near(glm::dvec3(world(foot)[1]), glm::dvec3(0, 1, 0), 1e-6));
    CHECK(near(glm::dvec3(world("ankle_l")[3]), glm::dvec3(posed[3]), 1e-4));
    CHECK(nearRotation(world("ankle_l"), posed, 1e-4));
}

TEST_CASE("puppeteer: a looker turns the joint to face it")
{
    loadPuppeteer();
    run("newScene");
    run("joint", {{"position", v3(0, 150, 0)}, {"name", "neck"}});
    run("joint", {{"position", v3(0, 160, 0)}, {"name", "head"}});
    run("joint", {{"position", v3(0, 170, 0)}, {"name", "head_end"}});
    const std::string looker = run("puppeteerLooker", {{"args", {"head"}}, {"aimAxis", "x"}, {"upAxis", "y"}}).get<std::string>();
    run("move", {{"args", {looker}}, {"to", v3(0, 160, 100)}});
    const glm::dvec3 aim = puppeteer::math::rotationOf(world("head")) * glm::dvec3(1, 0, 0);
    CHECK(near(aim, glm::dvec3(0, 0, 1), 1e-6));
    run("setAttr", {{"args", {looker + ".weight", 0.0}}});
    CHECK(attr("head.ikBlend") == Approx(0.0));
}

TEST_CASE("puppeteer: inherit rotate keeps the children's world rotation")
{
    loadPuppeteer();
    run("newScene");
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "a"}});
    run("joint", {{"position", v3(0, 10, 0)}, {"name", "b"}});
    run("joint", {{"position", v3(0, 20, 0)}, {"name", "c"}});
    const glm::dmat4 c0 = world("c");
    run("puppeteerRotate", {{"args", {"b"}}, {"axis", v3(0, 0, 1)}, {"angle", 40.0}, {"inherit", true}});
    CHECK(nearRotation(world("c"), c0, 1e-9));
    CHECK_FALSE(nearRotation(world("b"), c0, 1e-3));
    const glm::dmat4 c1 = world("c");
    run("puppeteerRotate", {{"args", {"b"}}, {"axis", v3(0, 0, 1)}, {"angle", 20.0}});
    CHECK_FALSE(nearRotation(world("c"), c1, 1e-3));
    // Toon scale: the child keeps its size.
    run("puppeteerScale", {{"args", {"b"}}, {"by", v3(2, 1, 1)}, {"compensate", true}});
    CHECK(glm::length(glm::dvec3(world("c")[0])) == Approx(1.0).epsilon(1e-6));
    CHECK(glm::length(glm::dvec3(world("b")[0])) == Approx(2.0).epsilon(1e-6));
}

TEST_CASE("puppeteer: the breakdown keys the pose part way to the next or previous key")
{
    loadPuppeteer();
    run("newScene");
    run("autoKeyframe", {{"state", false}});
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "root"}});
    run("setKeyframe", {{"args", {"root.tx"}}, {"time", 0.0}, {"value", 0.0}});
    run("setKeyframe", {{"args", {"root.tx"}}, {"time", 1.0}, {"value", 4.0}});
    run("setKeyframe", {{"args", {"root.tx"}}, {"time", 2.0}, {"value", 10.0}});
    setTime(1.0);
    run("puppeteerBreakdown", {{"args", {"root"}}, {"weight", 0.5}});
    CHECK(attr("root.tx") == Approx(7.0));
    run("puppeteerBreakdown", {{"args", {"root"}}, {"weight", -1.0}});
    CHECK(attr("root.tx") == Approx(0.0));
    setTime(2.0);
    CHECK(attr("root.tx") == Approx(10.0));
}

TEST_CASE("puppeteer: a joint keyed in another space follows that space")
{
    loadPuppeteer();
    run("newScene");
    run("autoKeyframe", {{"state", false}});
    run("spaceLocator", {{"name", "prop"}});
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "body"}});
    run("joint", {{"position", v3(30, 0, 0)}, {"name", "hand"}});
    run("puppeteerSpace", {{"args", {"hand"}}, {"spaces", {"prop"}}});
    setTime(0.0);
    const glm::dvec3 hand0 = pos("hand");
    CHECK(near(hand0, glm::dvec3(30, 0, 0), 1e-6));
    run("puppeteerSwitchSpace", {{"args", {"hand"}}, {"space", "prop"}, {"time", 0.0}});
    CHECK(near(pos("hand"), hand0, 1e-6));
    run("move", {{"args", {"prop"}}, {"by", v3(0, 20, 0)}});
    CHECK(near(pos("hand"), hand0 + glm::dvec3(0, 20, 0), 1e-6));
    // Back in the parent space it ignores the prop again, where it was at the switch.
    run("puppeteerSwitchSpace", {{"args", {"hand"}}, {"space", "parent"}, {"time", 1.0}});
    setTime(1.0);
    const glm::dvec3 hand1 = pos("hand");
    run("move", {{"args", {"prop"}}, {"by", v3(0, 20, 0)}});
    CHECK(near(pos("hand"), hand1, 1e-6));
}

TEST_CASE("puppeteer: mirror joints by their side naming")
{
    loadPuppeteer();
    run("newScene");
    run("joint", {{"position", v3(10, 100, 0)}, {"name", "arm_l"}});
    run("joint", {{"position", v3(40, 100, 0)}, {"name", "hand_l"}});
    run("puppeteerMirrorJoints", {{"args", {"arm_l"}}});
    CHECK(near(pos("arm_r"), glm::dvec3(-10, 100, 0), 1e-6));
    CHECK(near(pos("hand_r"), glm::dvec3(-40, 100, 0), 1e-6));
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "spine"}, {"parent", ""}});
    CHECK_THROWS_AS(run("puppeteerMirrorJoints", {{"args", {"spine"}}}), CommandError);
}

TEST_CASE("puppeteer: older puppeteerSpace scenes migrate to base spaces with the same motion")
{
    loadPuppeteer();
    run("newScene");
    run("autoKeyframe", {{"state", false}});
    run("spaceLocator", {{"name", "prop"}});
    run("setAttr", {{"args", {"prop.translate", v3(10, 20, 0)}}});
    run("setKeyframe", {{"args", {"prop.rz"}}, {"time", 0.0}, {"value", 0.0}});
    run("setKeyframe", {{"args", {"prop.rz"}}, {"time", 1.0}, {"value", 60.0}});
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "body"}});
    run("joint", {{"position", v3(30, 0, 0)}, {"name", "hand"}});
    run("setKeyframe", {{"args", {"body.ty"}}, {"time", 0.0}, {"value", 0.0}});
    run("setKeyframe", {{"args", {"body.ty"}}, {"time", 1.0}, {"value", 25.0}});
    run("setKeyframe", {{"args", {"hand.translate", "hand.rotate"}}, {"time", 0.0}});
    run("setKeyframe", {{"args", {"hand.tx"}}, {"time", 1.0}, {"value", 5.0}});
    run("setKeyframe", {{"args", {"hand.rz"}}, {"time", 1.0}, {"value", 40.0}});

    // The old layout: a parent constraint over the keys, its weights and offset from a puppeteerSpace node.
    Graph& g = graph();
    const std::string con = run("parentConstraint", {{"args", {"prop", "hand"}}, {"maintainOffset", false}, {"name", "hand_spaceConstraint"}});
    const std::string sp = run("createNode", {{"args", {"puppeteerSpace"}}, {"name", "hand_space"}, {"skipSelect", true}});
    run("setAttr", {{"args", {sp + ".joint", "hand"}}});
    run("setAttr", {{"args", {sp + ".spaces", "prop"}}});
    const glm::dvec3 orient = std::get<glm::dvec3>(g.value(g.plug("hand.jointOrient")));
    run("setAttr", {{"args", {sp + ".jointOrient", v3(orient.x, orient.y, orient.z)}}});
    run("connectAttr", {{"args", {sp + ".weight0", con + ".target0Weight"}}});
    run("connectAttr", {{"args", {sp + ".offset", con + ".target0Offset"}}});
    const Node& c = *g.find(con);
    const Node& s = *g.find(sp);
    for (const char* out : {"constraintTranslate", "constraintRotate"})
        for (const PlugRef& d : g.destinations({c.id(), c.type().attrIndex(out), -1}))
        {
            const Node& blend = *g.node(d.node);
            g.connect({s.id(), s.type().attrIndex("blendWeight"), -1}, {blend.id(), blend.type().attrIndex("weight"), -1});
            const int keyed = s.type().attrIndex(out[10] == 'T' ? "keyedTranslate" : "keyedRotate");
            // Joints key rotate as one rotation curve; translate has a curve per component.
            if (auto src = g.source({blend.id(), blend.type().attrIndex("inputA"), -1}))
                g.connect(*src, {s.id(), keyed, -1});
            for (int k = 0; k < 3; ++k)
                if (auto src = g.source({blend.id(), blend.type().attrIndex("inputA"), k}))
                    g.connect(*src, {s.id(), keyed, k});
        }
    run("setKeyframe", {{"args", {sp + ".space"}}, {"time", 0.0}, {"value", 0.0}, {"outTangentType", "step"}});
    run("setKeyframe", {{"args", {sp + ".space"}}, {"time", 0.5}, {"value", 1.0}, {"outTangentType", "step"}});

    std::vector<glm::dmat4> before;
    for (int f = 0; f <= 30; ++f)
    {
        setTime(f / 30.0);
        before.push_back(world("hand"));
    }
    CHECK(run("puppeteerMigrateSpaces") == 1);
    CHECK_FALSE(g.find("hand_space"));
    CHECK_FALSE(g.find("hand_spaceConstraint"));
    const json spaces = run("listSpaces", {{"args", {"hand"}}});
    REQUIRE(spaces["spaces"].size() == 2);
    CHECK(spaces["spaces"][1]["name"] == "prop");
    for (int f = 0; f <= 30; ++f)
    {
        CAPTURE(f);
        setTime(f / 30.0);
        const glm::dmat4 w = world("hand");
        CHECK(near(glm::dvec3(w[3]), glm::dvec3(before[size_t(f)][3]), 1e-6));
        CHECK(nearRotation(w, before[size_t(f)], 1e-6));
    }
}
