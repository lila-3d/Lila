#include "TestUtil.h"
#include "prop/PropMath.h"

#include "scene/Anim.h"
#include "scene/Grapple.h"
#include "scene/Pin.h"

#include <glm/gtc/matrix_transform.hpp>

using namespace puppeteer::prop;
using namespace puptest;
using Approx = doctest::Approx;

namespace proptest
{
/** Points over a box's surface: corners, edge and face samples, as a mesh's points. */
void box(std::vector<glm::dvec3>& out, glm::dvec3 centre, glm::dvec3 size)
{
    for (int i = 0; i <= 4; ++i)
        for (int j = 0; j <= 4; ++j)
            for (int k = 0; k <= 4; ++k)
                if (i == 0 || i == 4 || j == 0 || j == 4 || k == 0 || k == 4)
                    out.push_back(centre + size * (glm::dvec3(i, j, k) / 4.0 - 0.5));
}

/** A sword along +Y: pommel at 0, handle to 20, guard at 21, blade to 100. */
std::vector<glm::dvec3> swordPoints(double handle = 20.0)
{
    std::vector<glm::dvec3> p;
    box(p, {0, handle / 2, 0}, {2.6, handle, 2.6});
    box(p, {0, handle + 1, 0}, {18, 2, 3});
    box(p, {0, handle + 2 + 40, 0}, {5, 80, 0.8});
    return p;
}

/** A pistol along +Z: slide at the top, grip below toward the back. */
std::vector<glm::dvec3> pistolPoints()
{
    std::vector<glm::dvec3> p;
    box(p, {0, 0, 0}, {3, 3.5, 19});
    box(p, {0, -7, -6}, {2.8, 11, 4.5});
    return p;
}

/** A rifle along +Z: stock at the back, pistol grip, magazine, barrel to the front. */
std::vector<glm::dvec3> riflePoints()
{
    std::vector<glm::dvec3> p;
    box(p, {0, 0, 0}, {4, 7, 60});
    box(p, {0, 1, 45}, {2, 2, 30});
    box(p, {0, -3, -40}, {4, 12, 22});
    box(p, {0, -8, -16}, {3, 10, 4});
    box(p, {0, -10, 2}, {3, 14, 5});
    return p;
}

const PointDef* find(const Fit& f, Role r)
{
    for (const PointDef& d : f.points)
        if (d.role == r)
            return &d;
    return nullptr;
}
glm::dvec3 origin(const PointDef& d) { return glm::dvec3(d.frame[3]); }
glm::dvec3 xAxis(const PointDef& d) { return glm::dvec3(d.frame[0]); }

// Scene helpers -------------------------------------------------------------------------------------------------------

void manny()
{
    auto& em = lila::extensions::ExtensionManager::get();
    if (!em.isLoaded("RigTemplates"))
        REQUIRE(em.load(LILA_TEST_RIG_TEMPLATES_PACKAGE) == "RigTemplates");
    loadPuppeteer();
    run("newScene");
    run("autoKeyframe", {{"state", false}});
    run("playbackOptions", {{"minTime", 0.0}, {"maxTime", 10.0 / 30.0}, {"fps", 30.0}});
    run("buildRigSkeleton", {{"args", {"UE5_Manny"}}, {"rig", false}});
    run("puppeteerCharacter", {{"args", {"root"}}, {"template", "UE5_Manny"}, {"name", "manny"}});
}

std::string cube(const std::string& name, glm::dvec3 at, glm::dvec3 size)
{
    run("polyCube", {{"name", name}, {"width", size.x}, {"height", size.y}, {"depth", size.z}, {"position", v3(at.x, at.y, at.z)}});
    return name;
}

/** A prop mesh group made of boxes, far from the character. */
std::string group(const std::string& name, const std::vector<std::string>& parts)
{
    json args = json::array();
    for (const std::string& p : parts)
        args.push_back(p);
    run("group", {{"args", args}, {"name", name}});
    return name;
}

std::string sword()
{
    const glm::dvec3 o(200, 0, 0);
    return group("sword", {cube("sword_handle", o + glm::dvec3(0, 12, 0), {2.6, 24, 2.6}), cube("sword_guard", o + glm::dvec3(0, 25, 0), {18, 2, 3}),
                           cube("sword_blade", o + glm::dvec3(0, 66, 0), {5, 80, 0.8})});
}
std::string pistol()
{
    const glm::dvec3 o(-200, 100, 0);
    return group("pistol", {cube("pistol_slide", o, {3, 3.5, 19}), cube("pistol_grip", o + glm::dvec3(0, -7, -6), {2.8, 11, 4.5})});
}
std::string rifle()
{
    const glm::dvec3 o(0, 100, 200);
    return group("rifle", {cube("rifle_body", o, {4, 7, 60}), cube("rifle_barrel", o + glm::dvec3(0, 1, 45), {2, 2, 30}),
                           cube("rifle_stock", o + glm::dvec3(0, -3, -40), {4, 12, 22}), cube("rifle_pgrip", o + glm::dvec3(0, -8, -16), {3, 10, 4}),
                           cube("rifle_mag", o + glm::dvec3(0, -10, 2), {3, 14, 5})});
}

std::string pointNamed(const json& prop, const std::string& role)
{
    for (const json& p : prop.at("points"))
        if (p.at("role") == role)
            return p.at("point");
    FAIL("no " << role << " point");
    return {};
}

std::string jointPath(const std::string& bare)
{
    for (scene::NodeId id : graph().nodes())
        if (graph().node(id)->name() == bare)
            return graph().path(id);
    FAIL("no joint " << bare);
    return {};
}

void at(double seconds)
{
    graph().setTime(seconds, false);
    graph().evaluate();
}
} // namespace proptest

using namespace proptest;

TEST_CASE("props: a sword's handle, guard and tip are found from its mesh")
{
    for (const bool flip : {false, true})
    {
        std::vector<glm::dvec3> pts = swordPoints();
        if (flip)
            for (glm::dvec3& p : pts)
                p = glm::dvec3(p.x, 100.0 - p.y, p.z);
        const Fit f = fitPoints(Class::Sword2h, pts);
        CHECK(f.from == "mesh");
        const PointDef *main = find(f, Role::Main), *off = find(f, Role::Off), *tip = find(f, Role::Tip);
        REQUIRE(main);
        REQUIRE(off);
        REQUIRE(tip);
        const double dir = flip ? -1.0 : 1.0;
        // Along the blade toward the tip, both grips on the handle, main by the guard.
        CHECK(glm::dot(xAxis(*main), glm::dvec3(0, dir, 0)) > 0.99);
        const double mainY = flip ? 100.0 - origin(*main).y : origin(*main).y, offY = flip ? 100.0 - origin(*off).y : origin(*off).y;
        CHECK(mainY > 10.0);
        CHECK(mainY < 20.0);
        CHECK(offY < mainY - 5.0);
        CHECK(offY > 0.0);
        CHECK((flip ? 100.0 - origin(*tip).y : origin(*tip).y) > 95.0);
        CHECK(main->radius == Approx(1.3 * std::sqrt(2.0)).epsilon(0.05));
    }
    // One-handed: the pommel is an alternate grip, not an off hand.
    const Fit one = fitPoints(Class::Sword1h, swordPoints());
    CHECK(find(one, Role::Off) == nullptr);
    CHECK(find(one, Role::Alt) != nullptr);
}

TEST_CASE("props: a pistol's grip hangs below the slide and its muzzle points forward")
{
    const Fit f = fitPoints(Class::Pistol, pistolPoints());
    const PointDef *main = find(f, Role::Main), *muzzle = find(f, Role::Muzzle);
    REQUIRE(main);
    REQUIRE(muzzle);
    CHECK(origin(*main).y < -4.0);
    CHECK(origin(*main).z < -3.0);
    // The handle runs up toward the slide; the palm faces forward.
    CHECK(xAxis(*main).y > 0.9);
    CHECK(glm::dvec3(main->frame[2]).z > 0.9);
    CHECK(origin(*muzzle).z == Approx(9.5).epsilon(0.02));
    CHECK(glm::dot(xAxis(*muzzle), glm::dvec3(0, 0, 1)) > 0.999);
}

TEST_CASE("props: a rifle has its stock behind, the off grip ahead of the main and the muzzle in front")
{
    const Fit f = fitPoints(Class::Rifle, riflePoints());
    const PointDef *main = find(f, Role::Main), *off = find(f, Role::Off), *stock = find(f, Role::Stock), *muzzle = find(f, Role::Muzzle);
    REQUIRE(main);
    REQUIRE(off);
    REQUIRE(stock);
    REQUIRE(muzzle);
    CHECK(origin(*stock).z < -45.0);
    CHECK(origin(*main).z < -5.0);
    CHECK(origin(*main).z > -30.0);
    CHECK(origin(*off).z > origin(*main).z + 15.0);
    CHECK(origin(*muzzle).z > 55.0);
    CHECK(glm::dot(xAxis(*muzzle), glm::dvec3(0, 0, 1)) > 0.999);
    // Palm up under the handguard.
    CHECK(glm::dvec3(off->frame[2]).y > 0.9);
}

TEST_CASE("props: no mesh still gives points, and says so")
{
    const Fit f = fitPoints(Class::Rifle, {});
    CHECK(f.from != "mesh");
    CHECK(find(f, Role::Main) != nullptr);
    CHECK(find(f, Role::Off) != nullptr);
}

TEST_CASE("props: a grip frame sits the same in mirrored hands")
{
    HandShape r;
    r.forward = {1, 0, 0};
    r.thumbSide = {0, 0, 1};
    r.palm = {0, -1, 0};
    r.knuckles = {9, 0, 0};
    HandShape l = r;
    // The left hand is the right mirrored across x = 0 (in its own joint's space the thumb flips side).
    l.thumbSide = {0, 0, -1};
    const glm::dmat4 gr = gripFrame(r, 1.5), gl = gripFrame(l, 1.5);
    CHECK(glm::length(glm::dvec3(gr[3]) - glm::dvec3(9 * 0.85, -(0.25 * 9 + 1.5), 0)) < 1e-9);
    CHECK(glm::length(glm::dvec3(gr[3]) - glm::dvec3(gl[3])) < 1e-9);
    // Thumb toward +X of the frame, palm toward +Z, for both.
    CHECK(glm::dvec3(gr[0]) == glm::dvec3(0, 0, 1));
    CHECK(glm::dvec3(gl[0]) == glm::dvec3(0, 0, -1));
    CHECK(glm::dvec3(gr[2]) == glm::dvec3(0, -1, 0));
    CHECK(glm::dvec3(gl[2]) == glm::dvec3(0, -1, 0));
    CHECK(glm::determinant(glm::dmat3(gr)) == Approx(1.0));
    CHECK(glm::determinant(glm::dmat3(gl)) == Approx(1.0));
}

TEST_CASE("props: classes resolve by id and by common names")
{
    CHECK(classFromId("rifle") == Class::Rifle);
    CHECK(classFromId("bazooka") == Class::Launcher);
    CHECK(classFromId("greatsword") == Class::Sword2h);
    CHECK(!classFromId("toaster"));
    for (const Preset& p : presets())
        CHECK(classFromId(p.id) == p.cls);
}

// On Manny ------------------------------------------------------------------------------------------------------------

TEST_CASE("props: a greatsword held in both hands, the arms sharing the reach")
{
    manny();
    sword();
    const json p = run("puppeteerPropSetup", {{"args", {"sword"}}, {"kind", "sword2h"}});
    CHECK(p.at("twoHanded") == true);
    const std::string mainPt = pointNamed(p, "main"), offPt = pointNamed(p, "off");
    const std::string handR = jointPath("hand_r"), handL = jointPath("hand_l");
    const glm::dvec3 rBefore = pos(handR), lBefore = pos(handL);
    const json h = run("puppeteerPropHold", {{"args", {"sword"}}, {"time", 0.0}, {"blend", 0.0}});
    CHECK(h.at("main") == handR);
    CHECK(h.at("off") == handL);
    REQUIRE(h.at("solve").is_object());
    CHECK(h.at("solve").at("contactError").get<double>() < 0.5);
    at(5.0 / 30.0);
    // The sword rides the right hand: its main grip is where the hand's grip frame is.
    const glm::dmat4 hr = world(handR);
    const glm::dvec3 mainInHand = glm::dvec3(glm::inverse(hr) * glm::dvec4(pos(mainPt), 1.0));
    CHECK(glm::length(mainInHand) > 3.0);
    CHECK(glm::length(mainInHand) < 15.0);
    // The left hand reached its grip, and both hands moved to meet (neither arm did it all).
    const json info = run("grappleInfo", {{"args", {"manny_props"}}});
    CHECK(info.at("contactError").get<double>() < 0.5);
    const double movedR = glm::length(pos(handR) - rBefore), movedL = glm::length(pos(handL) - lBefore);
    CHECK(movedR > 2.0);
    CHECK(std::abs(glm::length(pos(offPt) - pos(handL)) - glm::length(pos(mainPt) - pos(handR))) < 1.0);
    CHECK(movedL > 2.0);
    // Holding it with one hand lets the left go.
    run("puppeteerPropHold", {{"args", {"sword"}}, {"hands", 1}, {"time", 5.0 / 30.0}, {"blend", 3.0 / 30.0}});
    at(10.0 / 30.0);
    CHECK(run("puppeteerPropInfo", {{"args", {"sword"}}}).at("holder") == handR);
    for (const json& c : run("grappleInfo", {{"args", {"manny_props"}}}).at("contacts"))
        CHECK(c.at("weight").get<double>() == Approx(0.0));
}

TEST_CASE("props: handing a pistol from the right hand to the left never pops")
{
    manny();
    pistol();
    run("puppeteerPropSetup", {{"args", {"pistol"}}, {"kind", "pistol"}});
    run("puppeteerPropHold", {{"args", {"pistol"}}, {"time", 0.0}, {"blend", 0.0}, {"solve", false}});
    // The right hand lifts toward the left between frames 0 and 4, then the pistol changes hands at frame 4.
    const std::string handR = jointPath("hand_r"), handL = jointPath("hand_l");
    const json held = run("puppeteerPropHold", {{"args", {"pistol"}}, {"hand", "left"}, {"time", 4.0 / 30.0}, {"blend", 4.0 / 30.0}, {"solve", false}});
    CHECK(held.at("main") == handL);
    // The group's origin is far from its meshes, so its grip is what is followed.
    const std::string grip = pointNamed(held, "main");
    std::vector<glm::dvec3> path;
    for (int f = 0; f <= 10; ++f)
    {
        at(f / 30.0);
        path.push_back(pos(grip));
    }
    // At the switch the pistol is where it was; it then travels to the left hand over the blend.
    CHECK(glm::length(path[4] - path[3]) < 1e-6);
    const double travel = glm::length(path[8] - path[4]);
    CHECK(travel > 10.0);
    double worst = 0.0;
    for (size_t f = 4; f < 8; ++f)
        worst = std::max(worst, glm::length(path[f + 1] - path[f]));
    // An eased move: no step much over an even share of the way.
    CHECK(worst < travel * 0.6);
    at(8.0 / 30.0);
    CHECK(run("puppeteerPropInfo", {{"args", {"pistol"}}}).at("holder") == handL);
    const json holds = run("puppeteerPropInfo", {{"args", {"pistol"}}}).at("holds");
    REQUIRE(holds.size() >= 3);
    CHECK(holds.back().at("side") == "left");
    // The time slider and the Stacker see the handoff as keys on the pistol's space channel.
    const scene::NodeId curve = scene::anim::curveFor(graph(), {id("pistol"), scene::Transform::kSpace, -1});
    CHECK(curve != scene::kNoNode);
    // Holstered at frame 8, it settles on the thigh.
    const json hol = run("puppeteerPropHolster", {{"args", {"pistol"}}, {"time", 8.0 / 30.0}, {"blend", 2.0 / 30.0}, {"solve", false}});
    at(10.0 / 30.0);
    CHECK(glm::length(pos(pointNamed(hol, "main")) - pos(hol.at("holster"))) < 1e-3);
}

TEST_CASE("props: a rifle aimed at a target, shouldered, the body following")
{
    manny();
    rifle();
    run("puppeteerPropSetup", {{"args", {"rifle"}}, {"kind", "rifle"}});
    run("puppeteerPropHold", {{"args", {"rifle"}}, {"time", 0.0}, {"blend", 0.0}, {"solve", false}});
    run("spaceLocator", {{"name", "enemy"}});
    run("setAttr", {{"args", {"enemy.translate", 150.0, 140.0, 600.0}}});
    const json aimed = run("puppeteerPropAim", {{"args", {"rifle"}}, {"target", "enemy"}, {"time", 0.0}, {"blend", 0.0}});
    CHECK(aimed.at("style") == "shoulder");
    at(5.0 / 30.0);
    const json p = run("puppeteerPropInfo", {{"args", {"rifle"}}});
    const std::string muzzle = pointNamed(p, "muzzle"), stock = pointNamed(p, "stock");
    const glm::dmat4 m = world(muzzle);
    const glm::dvec3 toTarget = glm::normalize(pos("enemy") - glm::dvec3(m[3]));
    const double deg = glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(glm::dvec3(m[0])), toTarget), -1.0, 1.0)));
    CHECK(deg < 2.0);
    INFO("solve error " << run("grappleInfo", {{"args", {"manny_props"}}}).dump());
    // The off hand still holds its grip, as far from it as the right hand is from the main grip.
    const double offGap = glm::length(pos(pointNamed(p, "off")) - pos(jointPath("hand_l")));
    const double mainGap = glm::length(pos(pointNamed(p, "main")) - pos(jointPath("hand_r")));
    CHECK(std::abs(offGap - mainGap) < 1.0);
    // The stock sits at the shoulder pocket.
    CHECK(glm::length(pos(stock) - pos(jointPath("propShoulder_r"))) < 2.0);
    // Weight 0 later lets the aim go.
    run("puppeteerPropAim", {{"args", {"rifle"}}, {"off", true}, {"time", 5.0 / 30.0}, {"blend", 2.0 / 30.0}});
    at(10.0 / 30.0);
    const json info = run("grappleInfo", {{"args", {"manny_props"}}});
    REQUIRE(info.at("aims").size() == 1);
    CHECK(info.at("aims").at(0).at("weight").get<double>() == Approx(0.0));
}

TEST_CASE("props: Unreal export attaches to hand_r by default and keys ik bones only when asked")
{
    manny();
    rifle();
    run("puppeteerPropSetup", {{"args", {"rifle"}}, {"kind", "rifle"}});
    run("puppeteerPropHold", {{"args", {"rifle"}}, {"time", 0.0}, {"blend", 0.0}, {"solve", false}});
    const std::string handR = jointPath("hand_r"), handL = jointPath("hand_l"), gun = jointPath("ik_hand_gun");
    const json u = run("puppeteerPropUnreal", {{"args", {"rifle"}}});
    CHECK(u.at("bone") == "hand_r");
    CHECK(u.at("socket") == "weapon_r");
    CHECK(u.at("strayCm").get<double>() < 1e-6);
    const scene::Node& r = *graph().find("rifle");
    CHECK(scene::pins::socketOf(r) == "weapon_r");
    CHECK(scene::pins::socketBoneOf(r) == "hand_r");
    at(0.0);
    const glm::dmat4 rel = glm::inverse(world(handR)) * world("rifle");
    CHECK(near(scene::pins::socketOffsetOf(r), rel, 1e-6));
    // Nothing touches the ik bones unless asked.
    CHECK(scene::anim::animatedPlugs(graph(), id(gun)).empty());
    const json k = run("puppeteerPropUnreal", {{"args", {"rifle"}}, {"ikBones", true}});
    CHECK(k.at("ikFrames") == 11);
    for (int f : {0, 5, 10})
    {
        at(f / 30.0);
        CHECK(glm::length(pos(gun) - pos(handR)) < 1e-3);
        CHECK(glm::length(pos(jointPath("ik_hand_l")) - pos(handL)) < 1e-3);
        CHECK(glm::length(pos(jointPath("ik_hand_r")) - pos(handR)) < 1e-3);
        const glm::dmat3 a(world(gun)), b(world(handR));
        CHECK(glm::length(a[0] - b[0]) + glm::length(a[1] - b[1]) < 1e-4);
    }
    // Bone names that aren't there: refused, and the default export never needs them.
    CHECK_THROWS(run("puppeteerPropUnreal", {{"args", {"rifle"}}, {"ikBones", true}, {"gunBone", "weapon_ik"}}));
}
