#include "TestUtil.h"

#include "core/PrefStore.h"
#include "curve/CurveControls.h"
#include "curve/CurveNodes.h"
#include "scene/Rig.h"
#include "ui/Camera.h"
#include "ui/NodeHandles.h"
#include "ui/RigHandles.h"
#include "ui/ViewportHooks.h"

#include <glm/gtc/quaternion.hpp>

using namespace rigtest;
using namespace lila::scene;
using D3 = glm::dvec3;
namespace rh = lila::ui::rigHandles;

namespace
{
void handleCurves()
{
    curve::registerTypes();
    if (!ops::find("curve"))
        curve::registerCommands();
}

lila::ui::ViewInfo handlesView()
{
    lila::ui::Camera cam;
    cam.yaw = 0.0;
    cam.pitch = 0.0;
    cam.distance = 300.0;
    lila::ui::ViewInfo v;
    v.view = cam.view();
    v.proj = cam.proj(1.0);
    v.eye = cam.eye();
    v.width = 400;
    v.height = 400;
    return v;
}

/** a, b, c along x: b's bone points down x, so its hinge about z lies in the front view's plane. */
void limitArm()
{
    run("newScene");
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "la"}});
    run("joint", {{"position", v3(30, 0, 0)}, {"name", "lb"}});
    run("joint", {{"position", v3(60, 0, 0)}, {"name", "lc"}});
}

/** Presses on a handle and drags it to the screen point of world, as a viewport would. */
void dragHandle(lila::ui::nodeHandles::Driver& d, QPointF from, const D3& world, const lila::ui::ViewInfo& view)
{
    auto to = view.project(world);
    REQUIRE(to);
    REQUIRE(d.press(from, view));
    d.drag(from + (*to - from) * 0.5, view, Qt::NoModifier);
    d.drag(*to, view, Qt::NoModifier);
    d.release();
}

size_t overlayCount(const lila::ui::ViewInfo& view)
{
    lila::render::FrameState s;
    lila::ui::OverlayPainter painter(s.overlayTris, 1.0);
    for (const auto& dr : lila::ui::viewportHooks::drawers())
        dr(s, painter, view);
    return s.overlayTris.size();
}

std::string bestPick(QPointF at, const lila::ui::ViewInfo& view)
{
    std::string best;
    double px = 1e9;
    for (const auto& p : lila::ui::viewportHooks::pickers())
        for (const auto& h : p(at, at, true, view))
            if (h.pixels < px)
            {
                px = h.pixels;
                best = h.name;
            }
    return best;
}

std::optional<QPointF> glyphOf(const std::string& name, const lila::ui::ViewInfo& view)
{
    const NodeId id = node(name).id();
    for (const rh::Glyph& g : rh::glyphs(view))
        if (g.node == id)
            return g.at;
    return std::nullopt;
}
} // namespace

TEST_CASE("IK limit gizmo: dragging a hinge end sets the limit through ikLimit as one undo step and refreshes the IK handle")
{
    limitArm();
    run("ikLimit", {{"args", {"lb"}}, {"type", "hinge"}, {"axis", "z"}, {"min", -10.0}, {"max", 10.0}});
    const std::string handle = run("ikHandle", {{"startJoint", "la"}, {"endEffector", "lc"}}).get<std::string>();
    run("select", {{"args", {"lb"}}});
    lila::ui::ViewInfo view = handlesView();

    // The handles draw on the selected joint.
    {
        lila::render::FrameState s;
        lila::ui::OverlayPainter painter(s.overlayTris, 1.0);
        lila::ui::nodeHandles::Driver d;
        d.draw(painter, view);
        CHECK(s.overlayTris.size() > 30);
    }

    auto at = rh::limitHandleAt(node("lb").id(), rh::kLimitMax, view);
    REQUIRE(at);
    CHECK_FALSE(rh::limitHandleAt(node("lb").id(), rh::kLimitCone, view));
    const D3 b = pos("lb");
    // Dragged to 60 degrees about z from the bone.
    lila::ui::nodeHandles::Driver d;
    dragHandle(d, *at, b + D3(std::cos(glm::radians(60.0)), std::sin(glm::radians(60.0)), 0.0) * 15.0, view);
    CHECK(node("lb").as<double>(Joint::kLimitMax) == doctest::Approx(60.0).epsilon(0.01));
    CHECK(node("lb").as<double>(Joint::kLimitMin) == doctest::Approx(-10.0));
    CHECK(node(handle).as<double>(node(handle).type().attrIndex("limitMax1")) == doctest::Approx(60.0).epsilon(0.01));

    // The min end can't pass the max.
    at = rh::limitHandleAt(node("lb").id(), rh::kLimitMin, view);
    REQUIRE(at);
    dragHandle(d, *at, b + D3(std::cos(glm::radians(80.0)), std::sin(glm::radians(80.0)), 0.0) * 15.0, view);
    CHECK(node("lb").as<double>(Joint::kLimitMin) == doctest::Approx(60.0).epsilon(0.01));

    run("undo");
    CHECK(node("lb").as<double>(Joint::kLimitMin) == doctest::Approx(-10.0));
    CHECK(node("lb").as<double>(Joint::kLimitMax) == doctest::Approx(60.0).epsilon(0.01));
    run("undo");
    CHECK(node("lb").as<double>(Joint::kLimitMax) == doctest::Approx(10.0));
    CHECK(node(handle).as<double>(node(handle).type().attrIndex("limitMax1")) == doctest::Approx(10.0));
}

TEST_CASE("IK limit gizmo: the cone's rim handle sets the cone angle; show all draws unselected joints' limits")
{
    limitArm();
    run("ikLimit", {{"args", {"lb"}}, {"type", "cone"}, {"cone", 30.0}});
    run("select", {{"args", {"lb"}}});
    lila::ui::ViewInfo view = handlesView();
    auto at = rh::limitHandleAt(node("lb").id(), rh::kLimitCone, view);
    REQUIRE(at);
    lila::ui::nodeHandles::Driver d;
    dragHandle(d, *at, pos("lb") + D3(std::cos(glm::radians(45.0)), std::sin(glm::radians(45.0)), 0.0) * 15.0, view);
    CHECK(node("lb").as<double>(Joint::kConeAngle) == doctest::Approx(45.0).epsilon(0.01));
    run("undo");
    CHECK(node("lb").as<double>(Joint::kConeAngle) == doctest::Approx(30.0));

    run("select", {{"args", json::array()}});
    const size_t hidden = overlayCount(view);
    lila::PrefStore::get().set("general", rh::kShowAllLimits, true);
    const size_t all = overlayCount(view);
    lila::PrefStore::get().set("general", rh::kShowAllLimits, false);
    CHECK(all > hidden + 30);
}

TEST_CASE("spline chains and hoses draw their curves and relations, and their glyphs select them")
{
    handleCurves();
    run("newScene");
    run("select", {{"args", json::array()}});
    std::vector<std::string> js;
    for (int i = 0; i < 5; ++i)
        js.push_back(run("joint", {{"position", v3(-80, -40 + 20.0 * i, 0)}, {"name", "sc" + std::to_string(i)}}).get<std::string>());
    run("select", {{"args", {js.front(), js.back()}}});
    const std::string chain = run("splineChain").get<std::string>();
    lila::ui::ViewInfo view = handlesView();

    auto glyph = glyphOf(chain, view);
    REQUIRE(glyph);
    run("select", {{"args", json::array()}});
    const size_t idle = overlayCount(view);
    CHECK(idle > 0);
    CHECK(bestPick(*glyph, view) == node(chain).path());
    // Selected, the chain shows its curve and where each joint sits on it.
    run("select", {{"args", {chain}}});
    CHECK(overlayCount(view) > idle + 20);
    run("select", {{"args", {js[2]}}});
    CHECK(overlayCount(view) > idle + 20);

    // A hose limb bent in the view's plane: a glyph off the bow, and the bend and bow drawn while selected.
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(20, 0, 0)}, {"name", "hs"}});
    run("joint", {{"position", v3(50, 10, 0)}, {"name", "he"}});
    run("joint", {{"position", v3(80, 0, 0)}, {"name", "hw"}});
    const std::string hose = run("hoseLimb", {{"args", {"hs", "hw"}}, {"joints", 6}}).get<std::string>();
    run("select", {{"args", json::array()}});
    glyph = glyphOf(hose, view);
    REQUIRE(glyph);
    CHECK(bestPick(*glyph, view) == node(hose).path());
    const size_t before = overlayCount(view);
    run("select", {{"args", {hose}}});
    CHECK(overlayCount(view) > before + 6);

    // Hose fingers: a glyph per finger and one for the hand.
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(0, 60, 0)}, {"name", "palm"}});
    for (const char* f : {"fa", "fb"})
    {
        const double x = f[1] == 'a' ? -5.0 : 5.0;
        run("select", {{"args", {"palm"}}});
        run("joint", {{"position", v3(x, 70, 0)}, {"name", std::string(f) + "0"}});
        run("joint", {{"position", v3(x, 78, 0)}, {"name", std::string(f) + "1"}});
        run("joint", {{"position", v3(x, 84, 0)}, {"name", std::string(f) + "2"}});
    }
    const json made = run("hoseFingers", {{"args", {"fa0", "fb0"}}, {"curlAxis", "z"}});
    REQUIRE(made.size() == 3);
    run("select", {{"args", json::array()}});
    for (const json& n : made)
        CHECK(glyphOf(n.get<std::string>(), view));
    glyph = glyphOf(made[0].get<std::string>(), view);
    REQUIRE(glyph);
    CHECK(bestPick(*glyph, view) == node(made[0].get<std::string>()).path());
}

TEST_CASE("moving a curve point a control drives moves the control, so the point lands where dragged, as one undo step")
{
    handleCurves();
    run("newScene");
    const std::string c = run("curve", {{"type", "poly"}, {"points", {v3(0, 0, 0), v3(10, 0, 0), v3(20, 0, 0)}}, {"name", "Driven"}}).get<std::string>();
    const json controls = run("curveControls", {{"args", {c}}});
    REQUIRE(controls.size() == 3);
    const std::string ctl = controls[1].get<std::string>();
    CHECK(curve::curveControlNodes(graph(), node(c)).size() == 3);

    auto worldPoint = [&](size_t i) {
        graph().evaluate();
        return curve::splinesOf(node(c).value(curve::CurveShape::kWorldSpace)).front().points[i];
    };
    const D3 ctlBefore = pos(ctl);
    run("selectComponents", {{"args", {c}}, {"vertices", {int64_t(curve::componentId(0, 1, curve::PointRole::Point))}}});
    run("moveComponents", {{"by", v3(0, 5, 2)}});
    checkNear(worldPoint(1), D3(10, 5, 2));
    checkNear(pos(ctl), ctlBefore + D3(0, 5, 2));
    checkNear(worldPoint(0), D3(0, 0, 0));
    run("undo");
    checkNear(worldPoint(1), D3(10, 0, 0));
    checkNear(pos(ctl), ctlBefore);
    run("selectMode", {{"args", {"object"}}});
}
