#include <doctest/doctest.h>

#include "cmds/Command.h"
#include "cmds/Undo.h"
#include "geo/Mesh.h"
#include "geo/Ops.h"
#include "scene/LastOp.h"

using namespace lila;
using json = nlohmann::json;

namespace
{
json run(const char* name, json call = json::object()) { return cmds::execute(name, call); }
json ui(const char* name, json call = json::object()) { return cmds::execute(name, call, CallSource::UI); }
int faces(const std::string& m) { return run("meshInfo", {{"args", {m}}})["faces"].get<int>(); }
size_t selectedCount(const std::string& m, const char* kind)
{
    json sel = run("componentSelection");
    return sel.contains(m) && sel[m].contains(kind) ? sel[m][kind].size() : 0;
}
} // namespace

TEST_CASE("connectPoints splits faces between non-neighbouring points")
{
    geo::Mesh m;
    for (int i = 0; i < 4; ++i)
        m.addPoint({double(i % 2 == 0 ? 0 : 1), 0.0, double(i < 2 ? 0 : 1)});
    m.addFace({0, 1, 3, 2});
    CHECK(geo::connectPoints(m, std::vector<geo::Index>{0, 1}).empty());
    CHECK(geo::connectPoints(m, std::vector<geo::Index>{0, 3}).size() == 1);
    CHECK(m.numFaces() == 2);
}

TEST_CASE("polyMirror directions and world space")
{
    namespace lo = scene::lastOp;
    auto xRange = [](const std::string& m) {
        json pts = run("meshData", {{"args", {m}}})["points"];
        double least = 1e9, most = -1e9;
        for (const json& p : pts)
        {
            least = std::min(least, p[0].get<double>());
            most = std::max(most, p[0].get<double>());
        }
        return std::pair{least, most};
    };
    run("newScene");
    std::string cube = run("polyCube")[0];
    run("polyMirror", {{"args", {cube}}});
    CHECK(faces(cube) == 10);
    run("undo");
    CHECK(faces(cube) == 6);
    run("polyMirror", {{"args", {cube}}, {"direction", "both"}});
    CHECK(faces(cube) == 12);
    run("undo");
    CHECK_THROWS_AS(run("polyMirror", {{"args", {cube}}, {"direction", "up"}}), CommandError);
    CHECK_THROWS_AS(run("polyMirror", {{"args", {cube}}, {"space", "parent"}}), CommandError);

    run("setAttr", {{"args", {cube + ".translate", {20.0, 0.0, 0.0}}}});
    run("polyMirror", {{"args", {cube}}, {"space", "world"}, {"direction", "negative"}});
    auto [xMin, xMax] = xRange(cube);
    CHECK(xMin == doctest::Approx(-50.0));
    CHECK(xMax == doctest::Approx(10.0));
    run("undo");

    run("select", {{"args", {cube}}});
    ui("polyMirror", {{"axis", "z"}});
    CHECK(faces(cube) == 10);
    lo::rerun({{"direction", "both"}});
    CHECK(faces(cube) == 12);
    lo::rerun({{"direction", "positive"}});
    lo::makeNonDestructive();
    json stack = run("modifiers", {{"args", {cube}}});
    REQUIRE(stack.size() == 1);
    CHECK(run("getAttr", {{"args", {stack[0].get<std::string>() + ".axis"}}}) == 2);
    CHECK(run("getAttr", {{"args", {stack[0].get<std::string>() + ".direction"}}}) == 0);
    CHECK(faces(cube) == 10);
    run("setAttr", {{"args", {stack[0].get<std::string>() + ".direction", 2}}});
    CHECK(faces(cube) == 12);
}

TEST_CASE("extract, connect and selection helpers")
{
    run("newScene");
    std::string cube = run("polyCube")[0];
    run("selectComponents", {{"args", {cube}}, {"faces", {0, 1}}});
    json made = run("polyExtract");
    REQUIRE(made.size() == 1);
    std::string piece = made[0];
    CHECK(faces(cube) == 4);
    CHECK(faces(piece) == 2);
    run("undo");
    CHECK(faces(cube) == 6);
    CHECK_FALSE(run("objExists", {{"args", {piece}}}).get<bool>());

    run("selectComponents", {{"args", {cube}}, {"edges", {json::array({0, 11})}}});
    run("polyConnect");
    CHECK(faces(cube) > 6);

    run("newScene");
    std::string plane = run("polyPlane", {{"subdivisionsX", 4}, {"subdivisionsZ", 4}})[0];
    run("selectComponents", {{"args", {plane}}, {"faces", {json::array({0, 15})}}});
    run("shrinkSelection");
    CHECK(selectedCount(plane, "faces") == 4);
    run("selectComponents", {{"args", {plane}}, {"faces", {json::array({0, 15})}}});
    run("selectBorder");
    CHECK(run("selectMode") == "edge");
    CHECK(selectedCount(plane, "edges") == 16);

    run("newScene");
    cube = run("polyCube")[0];
    run("selectComponents", {{"args", {cube}}, {"faces", {0}}});
    run("selectByAngle", {{"angle", 30}});
    CHECK(selectedCount(cube, "faces") == 1);
    run("selectByAngle", {{"angle", 100}});
    CHECK(selectedCount(cube, "faces") == 6);
}

TEST_CASE("execute listeners see top-level commands only")
{
    run("newScene");
    int before = 0, after = 0;
    int id = cmds::addExecuteListener([&](const ExecuteEvent& e) { (e.done ? after : before)++; });
    run("polyCube");
    CHECK(before == 1);
    CHECK(after == 1);
    CHECK_THROWS_AS(run("polyExtrude"), CommandError);
    CHECK(after == 2);
    cmds::removeExecuteListener(id);
    uint64_t serial = UndoStack::get().serial();
    run("polyCube");
    CHECK(UndoStack::get().serial() != serial);
}

TEST_CASE("adjusting the last operation re-runs it as one undo step")
{
    namespace lo = scene::lastOp;
    lo::install();
    run("newScene");
    std::string cube = run("polyCube")[0];
    run("selectComponents", {{"args", {cube}}, {"faces", {0}}});
    ui("selectByAngle", {{"angle", 100}});
    REQUIRE(lo::current());
    CHECK(lo::current()->command == "selectByAngle");
    CHECK(lo::live());
    CHECK(selectedCount(cube, "faces") == 6);
    // A re-run that changes nothing leaves no undo step but stays adjustable.
    lo::rerun({{"angle", 30}});
    CHECK(selectedCount(cube, "faces") == 1);
    CHECK(lo::live());
    lo::rerun({{"angle", 100}});
    CHECK(selectedCount(cube, "faces") == 6);
    run("undo");
    CHECK(selectedCount(cube, "faces") == 1);
    CHECK_FALSE(lo::live());

    run("selectComponents", {{"args", {cube}}, {"edges", {0}}});
    ui("polyBevel", {{"offset", 1}});
    int one = faces(cube);
    lo::rerun({{"segments", 3}});
    int three = faces(cube);
    CHECK(three > one);
    lo::rerun({{"segments", 1}});
    CHECK(faces(cube) == one);
    run("undo");
    CHECK(faces(cube) == 6);

    run("newScene");
    cube = run("polyCube")[0];
    run("select", {{"args", {cube}}});
    ui("polyMirror", {{"axis", "x"}});
    int mirrored = faces(cube);
    CHECK_THROWS_AS(lo::rerun({{"axis", "q"}}), CommandError);
    CHECK(faces(cube) == mirrored);
    CHECK(lo::live());

    run("newScene");
    cube = run("polyCube")[0];
    run("select", {{"args", {cube}}});
    ui("polySubdivide");
    CHECK(faces(cube) == 24);
    CHECK(lo::modifierEquivalent("polySubdivide") == "subdivision");
    lo::makeNonDestructive();
    CHECK(run("modifiers", {{"args", {cube}}}).size() == 1);
    CHECK(faces(cube) == 24);
    run("undo");
    CHECK(run("modifiers", {{"args", {cube}}}).empty());
    CHECK(faces(cube) == 6);
}
