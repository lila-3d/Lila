#include <doctest/doctest.h>

#include "TestUtil.h"

#include "curve/Along.h"
#include "curve/CurveControls.h"
#include "curve/CurveNodes.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

using namespace lila;
using json = nlohmann::json;
using D3 = glm::dvec3;
using namespace curvetest;

namespace
{
json ccall(const char* cmd, json call = json::object()) { return ops::execute(cmd, call); }

json cpts(std::initializer_list<D3> list)
{
    json a = json::array();
    for (const D3& p : list)
        a.push_back({p.x, p.y, p.z});
    return a;
}

D3 cvec(const json& j) { return D3(j[0].get<double>(), j[1].get<double>(), j[2].get<double>()); }

json cdata(const std::string& name) { return ccall("curveData", {{"args", {name}}}); }

D3 cpoint(const std::string& curve, size_t i, const char* key = "points") { return cvec(cdata(curve)[0][key][i]); }

bool cexists(const std::string& name) { return ccall("objExists", {{"args", {name}}}).get<bool>(); }

void cmove(const std::string& node, const D3& by) { ccall("move", {{"args", {node}}, {"by", {by.x, by.y, by.z}}}); }
} // namespace

TEST_CASE("curveControls: driven points follow, undo")
{
    ccall("newScene");
    std::string c = ccall("curve", {{"type", "poly"}, {"points", cpts({D3(0), D3(10, 0, 0), D3(20, 0, 0)})}, {"name", "Spine"}});
    json before = cdata(c);
    json made = ccall("curveControls", {{"args", {c}}});
    REQUIRE(made.size() == 3);
    CHECK(cexists("Spine_controls"));
    CHECK(cexists("Spine_ctrl1_grp"));
    CHECK(cdata(c) == before);

    cmove("Spine_ctrl1", D3(0, 5, 0));
    checkNear(cpoint(c, 1), D3(10, 5, 0));
    checkNear(cpoint(c, 0), D3(0));
    ccall("undo");
    checkNear(cpoint(c, 1), D3(10, 0, 0));

    // Undoing the build takes the controls and the attrs' elements away.
    ccall("undo");
    CHECK(!cexists("Spine_controls"));
    scene::Graph& g = scene::Graph::current();
    CHECK(!curve::hasControls(*g.find(c)));
    CHECK(cdata(c) == before);
}

TEST_CASE("curveControls: a curve under a moved parent")
{
    ccall("newScene");
    std::string c = ccall("curve", {{"type", "poly"}, {"points", cpts({D3(0), D3(10, 0, 0)})}, {"name", "Arm"}});
    std::string grp = ccall("group", {{"args", {c}}, {"name", "Body"}});
    ccall("setAttr", {{"args", {grp + ".translate", json::array({100, 0, 0})}}});
    ccall("setAttr", {{"args", {grp + ".rotate", json::array({0, 0, 90})}}});
    ccall("curveControls", {{"args", {c}}});
    checkNear(cpoint(c, 1), D3(10, 0, 0));
    // World +Y is the curve's +X under a parent turned 90 degrees about Z.
    cmove("Arm_ctrl1", D3(0, 5, 0));
    checkNear(cpoint(c, 1), D3(15, 0, 0));
    // Moving the parent moves the controls with the curve: the object-space shape holds.
    ccall("setAttr", {{"args", {grp + ".translate", json::array({0, 50, 0})}}});
    checkNear(cpoint(c, 1), D3(15, 0, 0));
}

TEST_CASE("curveControls: count, Bezier handles and mirrored names")
{
    ccall("newScene");
    std::string b = ccall("bezierCurve", {{"points", cpts({D3(-10, 0, 0), D3(0, 5, 0), D3(10, 0, 0)})}, {"name", "Lip"}});
    json made = ccall("curveControls", {{"args", {b}}, {"handles", true}, {"mirror", true}});
    CHECK(made.size() == 9);
    for (const char* n : {"Lip_ctrl0_r", "Lip_ctrl0_l", "Lip_ctrl1", "Lip_ctrl1_in", "Lip_ctrl1_out", "Lip_ctrl0_l_in", "Lip_ctrl0_r_out"})
        CHECK_MESSAGE(cexists(n), n);
    json before = cdata(b);
    CHECK(before[0]["points"] == cdata(b)[0]["points"]);

    // Turning the middle control turns both its tangents.
    const D3 p = cpoint(b, 1), out = cpoint(b, 1, "handleR") - p;
    ccall("setAttr", {{"args", {"Lip_ctrl1.rotate", json::array({0, 0, 90})}}});
    checkNear(cpoint(b, 1, "handleR") - p, D3(-out.y, out.x, out.z));

    // A tangent control moves its handle alone.
    ccall("setAttr", {{"args", {"Lip_ctrl1.rotate", json::array({0, 0, 0})}}});
    const D3 inBefore = cpoint(b, 1, "handleL");
    cmove("Lip_ctrl1_out", D3(0, 0, 3));
    checkNear(cpoint(b, 1, "handleR") - p, out + D3(0, 0, 3));
    checkNear(cpoint(b, 1, "handleL"), inBefore);

    ccall("newScene");
    std::string c = ccall("curve", {{"type", "poly"}, {"points", cpts({D3(0), D3(1, 0, 0), D3(2, 0, 0), D3(3, 0, 0), D3(30, 0, 0)})}, {"name", "Tail"}});
    CHECK(ccall("curveControls", {{"args", {c}}, {"count", 3}}).size() == 3);
    CHECK(cdata(c)[0]["points"].size() == 3);
    CHECK_THROWS(ccall("curveControls", {{"args", {c}}}));
}

TEST_CASE("removeCurveControls keeps the shape")
{
    ccall("newScene");
    std::string c = ccall("bezierCurve", {{"points", cpts({D3(0), D3(10, 5, 0), D3(20, 0, 0)})}, {"name", "Hose"}});
    ccall("curveControls", {{"args", {c}}, {"handles", true}});
    cmove("Hose_ctrl2", D3(0, 0, 8));
    cmove("Hose_ctrl1_in", D3(0, -2, 0));
    json shaped = cdata(c);
    ccall("removeCurveControls", {{"args", {c}}});
    CHECK(!cexists("Hose_controls"));
    CHECK(!cexists("Hose_ctrl1"));
    CHECK(!curve::hasControls(*scene::Graph::current().find(c)));
    json after = cdata(c);
    for (const char* key : {"points", "handleL", "handleR"})
        for (size_t i = 0; i < 3; ++i)
            checkNear(cvec(after[0][key][i]), cvec(shaped[0][key][i]));
    ccall("undo");
    CHECK(cexists("Hose_ctrl1_in"));
    CHECK(cdata(c) == shaped);
    CHECK_THROWS(ccall("removeCurveControls", {{"args", {ccall("curve", {{"type", "poly"}, {"points", cpts({D3(0), D3(1, 0, 0)})}})}}}));
}

TEST_CASE("curveControls: Rail and history see the driven curve")
{
    ccall("newScene");
    std::string c = ccall("curve", {{"type", "poly"}, {"points", cpts({D3(0), D3(10, 0, 0)})}, {"name", "Rope"}});
    ccall("curveControls", {{"args", {c}}});
    cmove("Rope_ctrl1", D3(0, 10, 0));
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    const curve::Splines& ws = curve::splinesOf(g.find(c)->value(curve::CurveShape::kWorldSpace));
    REQUIRE(ws.size() == 1);
    curve::Rail rail(ws[0]);
    CHECK(rail.length() == doctest::Approx(std::sqrt(200.0)));
    checkNear(rail.at(rail.length()).p, D3(10, 10, 0));

    // A live history node built on the curve rebuilds when a control moves.
    std::string beam = ccall("polyCube", {{"width", 10}, {"height", 1}, {"depth", 1}, {"subdivisionsX", 4}})[0];
    ccall("deformAlongCurve", {{"args", {beam}}, {"curve", c}});
    auto highest = [&] {
        g.evaluate();
        const scene::MeshPtr m = g.find(beam)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
        REQUIRE(m);
        double y = -1e300;
        for (const D3& p : m->positions())
            y = std::max(y, p.y);
        return y;
    };
    const double before = highest();
    cmove("Rope_ctrl1", D3(0, 30, 0));
    CHECK(std::abs(highest() - before) > 0.5);
}

TEST_CASE("curveControls: save and load")
{
    ccall("newScene");
    std::string c = ccall("bezierCurve", {{"points", cpts({D3(0), D3(10, 5, 0), D3(20, 0, 0)})}, {"name", "Whip"}});
    ccall("curveControls", {{"args", {c}}, {"handles", true}});
    cmove("Whip_ctrl1", D3(0, 4, 2));
    json before = cdata(c);
    auto dir = std::filesystem::temp_directory_path() / "lila_curve_tests";
    std::filesystem::create_directories(dir);
    for (const char* ext : {".lila", ".lilaa"})
    {
        std::string path = (dir / (std::string("controls") + ext)).string();
        ccall("saveScene", {{"args", {path}}});
        ccall("newScene");
        ccall("openScene", {{"args", {path}}});
        CHECK(cdata(c) == before);
        cmove("Whip_ctrl1", D3(0, 1, 0));
        checkNear(cpoint(c, 1), cvec(before[0]["points"][1]) + D3(0, 1, 0));
        cmove("Whip_ctrl1", D3(0, -1, 0));
    }
}
