#include "TestUtil.h"

#include "curve/CurveNodes.h"
#include "curve/Tessellate.h"
#include "scene/Diagnostics.h"
#include "scene/Rig.h"

#include <glm/gtc/matrix_transform.hpp>

using namespace rigtest;
using namespace lila::scene;
using D3 = glm::dvec3;

namespace
{
void splineCurves()
{
    curve::registerTypes();
    if (!ops::find("curve"))
        curve::registerCommands();
}

/** Joints up the y axis at the given heights, each under the last. */
std::vector<std::string> spColumn(const std::vector<double>& ys, const char* prefix = "sp")
{
    run("select", {{"args", json::array()}});
    std::vector<std::string> out;
    for (size_t i = 0; i < ys.size(); ++i)
        out.push_back(run("joint", {{"position", v3(0, ys[i], 0)}, {"name", std::string(prefix) + std::to_string(i)}}).get<std::string>());
    return out;
}

/** A curve with one control per point. */
std::string spControlled(const char* name, const std::vector<D3>& pts)
{
    json p = json::array();
    for (const D3& v : pts)
        p.push_back(v3(v.x, v.y, v.z));
    const std::string c = run("curve", {{"type", "poly"}, {"points", p}, {"name", name}}).get<std::string>();
    run("curveControls", {{"args", {c}}});
    return c;
}

void spMoveBy(const std::string& n, const D3& by) { run("move", {{"args", {n}}, {"by", v3(by.x, by.y, by.z)}}); }

double spDistToCurve(const std::string& c, const D3& p)
{
    graph().evaluate();
    const curve::Splines& s = curve::splinesOf(node(c).value(curve::CurveShape::kWorldSpace));
    D3 at;
    curve::closestParam(s.front(), p, &at);
    return glm::length(at - p);
}

/** The rotation that takes a's world rotation to b's. */
glm::dmat3 spTurn(const glm::dmat4& a, const glm::dmat4& b) { return glm::dmat3(b) * glm::transpose(glm::dmat3(a)); }
} // namespace

TEST_CASE("spline chain: joints sit along the curve by arc length and spread with it as it stretches")
{
    splineCurves();
    run("newScene");
    const auto js = spColumn({0, 5, 20, 40});
    const std::string c = spControlled("Rope", {D3(0, 0, 0), D3(0, 40, 0)});
    const std::string chain = run("splineChain", {{"args", {js.front(), js.back(), c}}}).get<std::string>();
    CHECK(node(chain).type().name == "splineChain");
    for (size_t i = 0; i < js.size(); ++i)
        checkNear(pos(js[i]), D3(0, std::vector<double>{0, 5, 20, 40}[i], 0), 1e-6);

    spMoveBy("Rope_ctrl1", D3(0, 40, 0));
    checkNear(pos(js[1]), D3(0, 10, 0), 1e-6);
    checkNear(pos(js[2]), D3(0, 40, 0), 1e-6);
    checkNear(pos(js[3]), D3(0, 80, 0), 1e-6);
    // Stretch is translate-based: the joints keep unit scale outside squash.
    checkNear(node(js[2]).as<D3>(Transform::kScale), D3(1.0));

    run("editSplineChain", {{"args", {chain}}, {"maxStretch", 1.5}});
    checkNear(pos(js[3]), D3(0, 60, 0), 1e-6);
    run("editSplineChain", {{"args", {chain}}, {"maxStretch", 4.0}, {"minStretch", 0.75}});
    spMoveBy("Rope_ctrl1", D3(0, -60, 0));
    checkNear(pos(js[3]), D3(0, 30, 0), 1e-6);

    // Weight 0 hands the joints back to their own rotate and translate.
    run("editSplineChain", {{"args", {chain}}, {"weight", 0.0}});
    checkNear(pos(js[3]), D3(0, 40, 0), 1e-6);
}

TEST_CASE("spline chain: keep length slides the joints and keeps their spacing on a bent curve")
{
    splineCurves();
    run("newScene");
    const auto js = spColumn({0, 10, 20, 30});
    const std::string c = spControlled("Spine", {D3(0, 0, 0), D3(0, 20, 0), D3(0, 40, 0)});
    const std::string chain = run("splineChain", {{"args", {js.front(), js.back(), c}}, {"lengthMode", "keepLength"}}).get<std::string>();
    run("editSplineChain", {{"args", {chain}}, {"slide", 5.0}});
    for (size_t i = 0; i < js.size(); ++i)
        checkNear(pos(js[i]), D3(0, 5.0 + 10.0 * double(i), 0), 1e-6);

    spMoveBy("Spine_ctrl1", D3(12, 0, 0));
    spMoveBy("Spine_ctrl2", D3(0, 5, 0));
    for (size_t i = 0; i < js.size(); ++i)
    {
        CHECK(spDistToCurve(c, pos(js[i])) < 1e-4);
        if (i > 0)
            CHECK(glm::length(pos(js[i]) - pos(js[i - 1])) == doctest::Approx(10.0).epsilon(1e-6));
    }
    // The bones point along the chain: each joint's bone axis aims at the next joint.
    for (size_t i = 0; i + 1 < js.size(); ++i)
    {
        const D3 bone = glm::normalize(D3(world(js[i])[1]));
        CHECK(glm::dot(bone, glm::normalize(pos(js[i + 1]) - pos(js[i]))) == doctest::Approx(1.0).epsilon(1e-6));
    }
}

TEST_CASE("spline chain: twist ramps from start to end about the curve, roll turns all of it")
{
    splineCurves();
    run("newScene");
    const auto js = spColumn({0, 10, 20});
    const std::string c = spControlled("Neck", {D3(0, 0, 0), D3(0, 20, 0)});
    const std::string chain = run("splineChain", {{"args", {js.front(), js.back(), c}}}).get<std::string>();
    std::vector<glm::dmat4> rest;
    for (const std::string& j : js)
        rest.push_back(world(j));
    run("editSplineChain", {{"args", {chain}}, {"twistEnd", 90.0}});
    auto twistOf = [&](size_t i) {
        const glm::dmat3 r = spTurn(rest[i], world(js[i]));
        // Every turn is about the curve (the y axis here).
        checkNear(r * D3(0, 1, 0), D3(0, 1, 0), 1e-6);
        const D3 x = r * D3(1, 0, 0);
        return glm::degrees(std::atan2(-x.z, x.x));
    };
    CHECK(twistOf(0) == doctest::Approx(0.0).epsilon(1e-6).scale(1.0));
    CHECK(twistOf(1) == doctest::Approx(45.0).epsilon(1e-6));
    CHECK(twistOf(2) == doctest::Approx(90.0).epsilon(1e-6));
    run("editSplineChain", {{"args", {chain}}, {"twistEnd", 0.0}, {"roll", 30.0}});
    for (size_t i = 0; i < js.size(); ++i)
        CHECK(twistOf(i) == doctest::Approx(30.0).epsilon(1e-6));
    // An FK offset turns the joint, and what hangs below it, on top of the chain.
    run("editSplineChain", {{"args", {chain}}, {"roll", 0.0}});
    const std::string sj = graph().node(graph().source({node(js[1]).id(), Joint::kSolvedRotate, -1})->node)->name();
    run("setAttr", {{"args", {sj + ".offset", v3(0, 0, 30)}}});
    CHECK(glm::length(pos(js[2]) - D3(0, 20, 0)) > 1.0);
    CHECK(glm::length(pos(js[2]) - pos(js[1])) == doctest::Approx(10.0).epsilon(1e-6));
}

TEST_CASE("spline chain: squash thins stretched joints, undo removes the chain, and the bake passes the Unreal check")
{
    splineCurves();
    run("newScene");
    diagnostics::clear();
    const std::string mesh = run("polyCylinder", {{"name", "Tail"}, {"radius", 5}, {"height", 40}, {"subdivisionsHeight", 8}, {"caps", false}})[0];
    const auto js = spColumn({-20, -10, 0, 10, 20}, "tail");
    run("skinCluster", {{"args", {js.front(), mesh}}});
    std::vector<D3> before;
    for (const std::string& j : js)
        before.push_back(pos(j));

    const std::string chain = run("splineChain", {{"args", {js.front(), js.back()}}, {"lengthMode", "squash"}, {"controls", 3}}).get<std::string>();
    const std::string curveName = node(chain).as<std::string>(node(chain).type().attrIndex("curve"));
    for (size_t i = 0; i < js.size(); ++i)
        checkNear(pos(js[i]), before[i], 1e-6);

    spMoveBy(curveName + "_ctrl2", D3(10, 15, 0));
    spMoveBy(curveName + "_ctrl1", D3(4, 0, 0));
    const D3 s = node(js[2]).as<D3>(Transform::kScale);
    CHECK(s.y == doctest::Approx(1.0));
    CHECK(s.x < 0.95);
    CHECK(s.x == doctest::Approx(s.z));
    CHECK(spDistToCurve(curveName, pos(js.back())) < 1e-4);

    const json baked = run("bakeToDeformSkeleton", {{"args", {js.front()}}, {"start", 0.0}, {"end", 0.0}, {"fps", 30.0}});
    REQUIRE(baked.size() == 1);
    for (const std::string& j : js)
    {
        const glm::dmat4 a = world(j), b = world(j + "_bake");
        for (int k = 0; k < 4; ++k)
            checkNear(D3(a[k]), D3(b[k]), 1e-6);
    }
    const json check = run("unrealCheck", {{"args", {baked[0]}}, {"start", 0.0}, {"end", 0.0}});
    CHECK(check["skins"] == 1);
    CHECK(check["mismatches"].empty());
    CHECK(check["worst"].get<double>() < 1e-3);

    run("undo"); // the bake
    run("undo"); // the control moves
    run("undo");
    run("undo"); // the chain, its curve and controls
    CHECK_FALSE(graph().find(chain));
    CHECK_FALSE(graph().source({node(js[2]).id(), Joint::kSolvedRotate, -1}));
    for (size_t i = 0; i < js.size(); ++i)
        checkNear(pos(js[i]), before[i], 1e-6);
}
