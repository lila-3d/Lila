#include <doctest/doctest.h>

#include "TestUtil.h"

#include "ops/Command.h"
#include "geo/ArrayTransform.h"
#include "scene/ArrayGroup.h"
#include "scene/Graph.h"
#include "scene/HudStats.h"
#include "scene/Instancing.h"
#include "scene/LastOp.h"
#include "scene/MeshNodes.h"
#include "scene/Repeat.h"
#include "scene/Transform.h"
#include "ui/ArrayTool.h"
#include "ui/ViewInfo.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <filesystem>

using namespace lila;
using json = nlohmann::json;
using namespace testutil;

namespace
{
bool nearMat(const glm::dmat4& a, const glm::dmat4& b)
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (std::abs(a[c][r] - b[c][r]) > 1e-9)
                return false;
    return true;
}

/** Where every generated copy of a node puts its origin, world space. */
std::vector<glm::dvec3> copyOrigins(const std::string& n)
{
    g().evaluate();
    const glm::dmat4 w = g().find(n)->as<glm::dmat4>(scene::Transform::kWorldMatrix);
    std::vector<glm::dvec3> out;
    for (const scene::arrays::Copy& c : scene::arrays::copiesOf(g(), id(n)))
        out.push_back(glm::dvec3((c.transform * w)[3]));
    return out;
}

/** A unit-ish cube moved out to x = 100. */
std::string spoke()
{
    run("newScene");
    std::string c = run("polyCube", {{"name", "Spoke"}})[0];
    run("move", {{"args", {c}}, {"by", {100, 0, 0}}});
    return c;
}

/** Looking down -Z, 1 px per cm, the world origin at pixel (100, 100). */
ui::ViewInfo topView()
{
    ui::ViewInfo view;
    view.proj = glm::ortho(-100.0, 100.0, -100.0, 100.0, -500.0, 500.0);
    view.width = view.height = 200.0;
    view.ortho = true;
    view.eye = {0.0, 0.0, 200.0};
    view.camera = "front";
    return view;
}
} // namespace

TEST_CASE("array maths: one implementation behind duplicate, the modifier and the array group")
{
    // trsMatrix is the scene's Euler XYZ convention.
    const glm::dvec3 t(1, 2, 3), r(10, 20, 30), s(1, 2, 0.5);
    const glm::dmat4 expect = glm::translate(glm::dmat4(1.0), t) * scene::eulerToMatrix(r, scene::RotateOrder::XYZ) * glm::scale(glm::dmat4(1.0), s);
    CHECK(nearMat(geo::trsMatrix(t, r, s), expect));

    // duplicate's step about a pivot in a frame is the array's step about its centre frame.
    scene::repeat::Step step{t, r, s};
    const glm::dmat3 frame(glm::rotate(glm::dmat4(1.0), glm::radians(30.0), glm::dvec3(0, 1, 0)));
    const glm::dvec3 pivot(5, 0, -5);
    CHECK(nearMat(scene::repeat::stepMatrix(step, pivot, frame), geo::aboutFrame(geo::pivotFrame(pivot, frame), geo::trsMatrix(t, r, s))));
    const glm::dmat4 base = glm::translate(glm::dmat4(1.0), glm::dvec3(10, 0, 0));
    const std::vector<glm::dmat4> a = scene::repeat::copyMatrices(base, expect, 4), b = geo::repeatMatrices(base, expect, 4);
    REQUIRE(a.size() == 4);
    for (size_t i = 0; i < a.size(); ++i)
        CHECK(nearMat(a[i], b[i]));

    // Every copy of every layout agrees with arrayCopyTransform, the modifier's per-copy maths.
    for (geo::ArrayMode mode : {geo::ArrayMode::Linear, geo::ArrayMode::Radial, geo::ArrayMode::Grid})
    {
        geo::ArrayLayout l;
        l.mode = mode;
        l.count = 5;
        l.gridCount = {3, 2, 2};
        l.offset = {20, 0, 5};
        l.rotate = {0, 15, 0};
        l.scale = glm::dvec3(0.9);
        l.angle = 270.0;
        l.rise = 4.0;
        l.frame = geo::pivotFrame({3, 0, 1});
        const std::vector<glm::dmat4> all = geo::arrayCopyTransforms(l);
        REQUIRE(int(all.size()) == geo::arrayCopyCount(l));
        for (int k = 0; k < int(all.size()); ++k)
            CHECK(nearMat(all[size_t(k)], geo::arrayCopyTransform(l, k)));
    }
}

TEST_CASE("array group: radial copies are generated, follow the settings and the source, and pick as the array")
{
    std::string src = spoke();
    std::string arr = run("array", {{"args", {src}}, {"mode", "radial"}, {"count", 6}, {"center", {0, 0, 0}}, {"axis", "y"}}).get<std::string>();
    CHECK(scene::arrays::isArray(*g().find(arr)));
    CHECK(g().parent(id(src)) == id(arr));
    // The source stays put, and no nodes are made for the copies.
    checkNear(worldPos(src), {100, 0, 0});
    CHECK(count("mesh") == 1);
    std::vector<glm::dvec3> at = copyOrigins(src);
    REQUIRE(at.size() == 5);
    for (size_t k = 0; k < at.size(); ++k)
    {
        const double a = glm::radians(60.0 * double(k + 1));
        checkNear(at[k], {100.0 * std::cos(a), 0.0, -100.0 * std::sin(a)});
    }
    for (const scene::arrays::Copy& c : scene::arrays::copiesOf(g(), id(src)))
        CHECK(c.array == id(arr));
    json info = run("arrayInfo", {{"args", {arr}}});
    CHECK(info["copies"] == 6);
    CHECK(info["mode"] == "radial");
    CHECK(info["placements"][0]["worlds"].size() == 5);

    // Settings and the source drive the copies live.
    run("setAttr", {{"args", {arr + ".count", 4}}});
    CHECK(copyOrigins(src).size() == 3);
    run("move", {{"args", {src}}, {"by", {0, 50, 0}}});
    checkNear(copyOrigins(src)[1], {-100, 50, 0});
    run("setAttr", {{"args", {arr + ".rise", 10.0}}});
    checkNear(copyOrigins(src)[2], {0, 80, 100});

    // The HUD counts the copies as drawn but the shared mesh once.
    run("select", {{"args", json::array()}});
    g().evaluate();
    scene::hud::PolyStats s = scene::hud::polyStats(g());
    CHECK(s.scene.tris == 48);
    CHECK(s.uniqueTris == 12);

    // Editing settings by command keeps unspecified ones.
    run("array", {{"args", {arr}}, {"angle", 180.0}});
    CHECK(run("getAttr", {{"args", {arr + ".count"}}}).get<int>() == 4);
    CHECK(run("getAttr", {{"args", {arr + ".angle"}}}).get<double>() == doctest::Approx(180.0));
}

TEST_CASE("array group: linear and grid layouts, nesting multiplies, centre is the pivot")
{
    std::string src = spoke();
    std::string row = run("array", {{"args", {src}}, {"count", 3}, {"offset", {0, 0, 40}}}).get<std::string>();
    std::vector<glm::dvec3> at = copyOrigins(src);
    REQUIRE(at.size() == 2);
    checkNear(at[1], {100, 0, 80});

    // An array of the row: 3 x 4 copies in all.
    std::string outer = run("array", {{"args", {row}}, {"edit", false}, {"mode", "grid"}, {"count", 2}, {"countY", 2}, {"countZ", 1},
                                      {"spacing", {0, 0, 0}}})
                            .get<std::string>();
    run("setAttr", {{"args", {outer + ".spacing", 300, 200, 0}}});
    at = copyOrigins(src);
    CHECK(at.size() == 11);
    checkNear(at.back(), {400, 200, 80});
    // A copy of the outer array picks as the outer array; the inner row's own copies as the row.
    const std::vector<scene::arrays::Copy> copies = scene::arrays::copiesOf(g(), id(src));
    CHECK(copies[0].array == id(row));
    CHECK(copies.back().array == id(outer));

    // centre=: a radial array turns about the given world point, which becomes its pivot.
    std::string ring = spoke();
    std::string r = run("array", {{"args", {ring}}, {"mode", "radial"}, {"count", 2}, {"center", {50, 0, 0}}, {"axis", "y"}}).get<std::string>();
    checkNear(copyOrigins(ring)[0], {0, 0, 0});
    run("array", {{"args", {r}}, {"center", {100, 0, 0}}});
    checkNear(copyOrigins(ring)[0], {100, 0, 0});
    checkNear(worldPos(ring), {100, 0, 0});
}

TEST_CASE("array group: undo, adjust last op, save and open regenerate the copies")
{
    scene::lastOp::install();
    std::string src = spoke();
    run("select", {{"args", {src}}});
    std::string arr = ops::execute("array", {{"args", {src}}, {"mode", "radial"}, {"count", 5}, {"center", {0, 0, 0}}}, CallSource::UI).get<std::string>();
    REQUIRE(scene::lastOp::live());
    CHECK(scene::lastOp::current()->command == "array");
    scene::lastOp::rerun({{"count", 9}});
    CHECK(copyOrigins(src).size() == 8);
    CHECK(count("array") == 1);
    run("undo");
    CHECK(copyOrigins(src).size() == 4);
    run("undo");
    CHECK(count("array") == 0);
    CHECK(g().parent(id(src)) == scene::kNoNode);
    checkNear(worldPos(src), {100, 0, 0});
    run("redo");
    REQUIRE(count("array") == 1);
    arr = run("ls", {{"type", "array"}})[0].get<std::string>();

    for (const char* ext : {".lila", ".lilaa"})
    {
        std::string path = tempPath("array", (std::string("array") + ext).c_str());
        run("saveScene", {{"args", {path}}});
        run("newScene");
        run("openScene", {{"args", {path}}});
        CHECK(count("array") == 1);
        CHECK(count("mesh") == 1);
        CHECK(copyOrigins("Spoke").size() == 4);
        CHECK(run("getAttr", {{"args", {arr + ".mode"}}}).get<int>() == 1);
    }
}

TEST_CASE("bakeArray: real objects where the copies were, instances or copies, undoable")
{
    std::string src = spoke();
    std::string arr = run("array", {{"args", {src}}, {"mode", "radial"}, {"count", 4}, {"center", {0, 0, 0}}, {"axis", "y"}, {"name", "Wheel"}})
                          .get<std::string>();
    const std::vector<glm::dvec3> expect = copyOrigins(src);
    json groups = run("bakeArray", {{"args", {arr}}});
    REQUIRE(groups.size() == 1);
    CHECK(count("array") == 0);
    CHECK(count("mesh") == 4);
    const std::string grp = groups[0].get<std::string>();
    CHECK(grp == "Wheel");
    const std::vector<scene::NodeId>& kids = g().children(id(grp));
    REQUIRE(kids.size() == 4);
    CHECK(kids[0] == id(src));
    g().evaluate();
    for (size_t k = 1; k < kids.size(); ++k)
    {
        CHECK(scene::instancing::kind(g(), kids[k]) == scene::instancing::Kind::Instance);
        checkNear(glm::dvec3(g().node(kids[k])->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]), expect[k - 1]);
    }
    checkNear(worldPos(src), {100, 0, 0});
    run("undo");
    CHECK(count("array") == 1);
    CHECK(count("mesh") == 1);
    CHECK(copyOrigins(src).size() == 3);

    run("bakeArray", {{"args", {arr}}, {"mode", "copy"}});
    CHECK(count("mesh") == 4);
    for (scene::NodeId k : g().children(id("Wheel")))
        CHECK(scene::instancing::kind(g(), k) == scene::instancing::Kind::None);
}

TEST_CASE("array tool: drag a row, click a ring, drag the handles, Enter makes one array")
{
    scene::lastOp::install();
    run("newScene");
    std::string c = run("polyCube", {{"name", "Crate"}})[0];
    run("select", {{"args", {c}}});
    const ui::ViewInfo view = topView();
    auto px = [&](const glm::dvec3& w) { return *view.project(w); };

    {
        ui::ArrayTool tool;
        REQUIRE(tool.begin());
        CHECK(tool.sources().size() == 1);
        REQUIRE(tool.press(px({0, 0, 0}), view));
        tool.drag(px({20, 0, 0}), view);
        tool.release(px({30, 0, 0}), view);
        REQUIRE(tool.placed());
        CHECK(tool.settings().mode == geo::ArrayMode::Linear);
        checkNear(tool.settings().offset, {30, 0, 0}, 1e-4);
        // The count handle sits past the last copy; dragging it to x = 150 makes six.
        std::optional<QPointF> h = tool.handlePos(ui::ArrayTool::Handle::Count, view);
        REQUIRE(h);
        REQUIRE(tool.press(*h, view));
        tool.release(px({150, 0, 0}), view);
        CHECK(tool.settings().count == 6);
        // Spacing handle: copy 1 dragged to x = 40.
        h = tool.handlePos(ui::ArrayTool::Handle::Spacing, view);
        REQUIRE(h);
        REQUIRE(tool.press(*h, view));
        tool.release(px({40, 0, 0}), view);
        checkNear(tool.settings().offset, {40, 0, 0}, 1e-4);
        // Nothing in the scene changes until Enter.
        CHECK(count("array") == 0);
        const std::string arr = tool.finish();
        REQUIRE(!arr.empty());
        CHECK(run("getAttr", {{"args", {arr + ".count"}}}).get<int>() == 6);
        const std::vector<glm::dvec3> at = copyOrigins(c);
        REQUIRE(at.size() == 5);
        checkNear(at.back(), {200, 0, 0}, 1e-4);
        // It is the adjustable last op.
        REQUIRE(scene::lastOp::live());
        scene::lastOp::rerun({{"count", 3}});
        CHECK(copyOrigins(c).size() == 2);
        run("undo");
        run("undo");
        CHECK(count("array") == 0);
    }

    {
        run("select", {{"args", {c}}});
        ui::ArrayTool tool;
        REQUIRE(tool.begin());
        // A click places a ring's centre; the plane faces the view, so it turns about z.
        REQUIRE(tool.press(px({0, -50, 0}), view));
        tool.release(px({0, -50, 0}), view);
        CHECK(tool.settings().mode == geo::ArrayMode::Radial);
        CHECK(tool.settings().axis == 2);
        CHECK(tool.settings().count == 8);
        checkNear(tool.settings().center, {0, -50, 0}, 1e-4);
        // Dragging the first copy round to a quarter turn (right handed about +z) makes four.
        std::optional<QPointF> h = tool.handlePos(ui::ArrayTool::Handle::Step, view);
        REQUIRE(h);
        REQUIRE(tool.press(*h, view));
        tool.release(px({-50, -50, 0}), view);
        CHECK(tool.settings().count == 4);
        // The arc's end starts at a full turn; sweeping it back through 270 to 180 opens the ring to a half.
        h = tool.handlePos(ui::ArrayTool::Handle::Angle, view);
        REQUIRE(h);
        REQUIRE(tool.press(*h, view));
        tool.drag(px({50, -50, 0}), view);
        tool.release(px({0, -100, 0}), view);
        CHECK(tool.settings().angle == doctest::Approx(180.0));
        const std::string arr = tool.finish();
        REQUIRE(!arr.empty());
        const std::vector<glm::dvec3> at = copyOrigins(c);
        REQUIRE(at.size() == 3);
        checkNear(at.back(), {0, -100, 0}, 1e-4);
        CHECK(run("getAttr", {{"args", {arr + ".axis"}}}).get<int>() == 2);
    }

    {
        // Esc leaves everything as it was.
        run("newScene");
        std::string d = run("polyCube")[0];
        run("select", {{"args", {d}}});
        ui::ArrayTool tool;
        REQUIRE(tool.begin());
        tool.press(px({0, 0, 0}), view);
        tool.release(px({50, 0, 0}), view);
        CHECK(tool.key(Qt::Key_3));
        CHECK(tool.settings().mode == geo::ArrayMode::Grid);
        CHECK(tool.key(Qt::Key_Plus));
        CHECK(tool.settings().gridCount.x == 4);
        CHECK(tool.call()["countY"] == 3);
        CHECK(tool.key(Qt::Key_Escape));
        CHECK(count("array") == 0);
        CHECK(g().parent(id(d)) == scene::kNoNode);
    }

    ui::ArrayTool none;
    run("select", {{"args", json::array()}});
    CHECK_FALSE(none.begin());
}

TEST_CASE("duplicate count in copy mode gives every copy a history of its own")
{
    run("newScene");
    std::string c = run("polyCube", {{"name", "Brick"}})[0];
    json made = run("duplicate", {{"args", {c}}, {"count", 3}, {"translate", {30, 0, 0}}});
    REQUIRE(made.size() == 3);
    for (const json& m : made)
        CHECK(scene::instancing::kind(g(), id(m.get<std::string>())) == scene::instancing::Kind::None);
    CHECK(count("polyCube") == 4);
}
