#include <doctest/doctest.h>

#include "ops/Command.h"
#include "ops/Undo.h"
#include "geo/Mesh.h"
#include "geo/Ops.h"
#include "scene/LastOp.h"

using namespace lila;
using json = nlohmann::json;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }
json ui(const char* name, json call = json::object()) { return ops::execute(name, call, CallSource::UI); }
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
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
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

TEST_CASE("polyMirror adjusts through every direction and space, undoing and redoing on a transformed mesh")
{
    namespace lo = scene::lastOp;
    auto points = [](const std::string& m) { return run("meshData", {{"args", {m}}, {"base", true}}); };
    run("newScene");
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}, {"subdivisionsX", 3}})[0];
    run("setAttr", {{"args", {cube + ".translate", {30.0, 10.0, -5.0}}}});
    run("setAttr", {{"args", {cube + ".rotate", {0.0, 35.0, 20.0}}}});
    run("setAttr", {{"args", {cube + ".scale", {1.0, 2.0, 0.5}}}});
    run("select", {{"args", {cube}}});
    const json original = points(cube);
    ui("polyMirror", {{"axis", "x"}});
    REQUIRE(lo::live());
    std::vector<json> results = {points(cube)};
    for (const char* space : {"object", "world"})
        for (const char* direction : {"positive", "negative", "both"})
            for (bool weld : {true, false})
                for (const char* axis : {"x", "y", "z"})
                {
                    CAPTURE(space);
                    CAPTURE(direction);
                    CAPTURE(axis);
                    lo::rerun({{"axis", axis}, {"direction", direction}, {"space", space}, {"weld", weld}});
                    CHECK(faces(cube) > 0);
                    results.push_back(points(cube));
                }
    // Undo walks back through every setting to the original, redo forward again, twice over.
    for (int pass = 0; pass < 2; ++pass)
    {
        for (size_t k = results.size(); k-- > 0;)
        {
            CHECK(points(cube) == results[k]);
            run("undo");
        }
        CHECK(points(cube) == original);
        for (size_t k = 0; k < results.size(); ++k)
        {
            run("redo");
            CHECK(points(cube) == results[k]);
        }
    }
}

TEST_CASE("extract, connect and selection helpers")
{
    run("newScene");
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
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
    std::string plane = run("polyPlane", {{"width", 100.0}, {"depth", 100.0}, {"subdivisionsX", 4}, {"subdivisionsZ", 4}})[0];
    run("selectComponents", {{"args", {plane}}, {"faces", {json::array({0, 15})}}});
    run("shrinkSelection");
    CHECK(selectedCount(plane, "faces") == 4);
    std::string other = run("polyPlane", {{"width", 100.0}, {"depth", 100.0}})[0];
    run("selectComponents", {{"args", {plane}}, {"faces", {0}}});
    run("selectComponents", {{"args", {other}}, {"faces", {0}}, {"add", true}});
    run("shrinkSelection");
    CHECK(selectedCount(plane, "faces") == 0);
    CHECK(selectedCount(other, "faces") == 0);
    run("undo");
    CHECK(selectedCount(plane, "faces") == 1);
    CHECK(selectedCount(other, "faces") == 1);
    run("selectComponents", {{"args", {plane}}, {"faces", {json::array({0, 15})}}});
    run("selectBorder");
    CHECK(run("selectMode") == "edge");
    CHECK(selectedCount(plane, "edges") == 16);

    run("newScene");
    cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
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
    int id = ops::addExecuteListener([&](const ExecuteEvent& e) { (e.done ? after : before)++; });
    run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}});
    CHECK(before == 1);
    CHECK(after == 1);
    run("select", {{"clear", true}});
    CHECK_THROWS_AS(run("polyExtrude"), CommandError);
    CHECK(after == 3);
    ops::removeExecuteListener(id);
    uint64_t serial = UndoStack::get().serial();
    run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}});
    CHECK(UndoStack::get().serial() != serial);
}

TEST_CASE("adjusting the last operation re-runs it as one undo step")
{
    namespace lo = scene::lastOp;
    lo::install();
    run("newScene");
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
    run("selectComponents", {{"args", {cube}}, {"faces", {0}}});
    ui("selectByAngle", {{"angle", 100}});
    REQUIRE(lo::current());
    CHECK(lo::current()->command == "selectByAngle");
    CHECK(lo::live());
    CHECK(selectedCount(cube, "faces") == 6);
    lo::rerun({{"angle", 30}});
    CHECK(selectedCount(cube, "faces") == 1);
    CHECK(lo::live());
    lo::rerun({{"angle", 100}});
    CHECK(selectedCount(cube, "faces") == 6);
    // Each adjustment undoes on its own, then the op itself.
    run("undo");
    CHECK(selectedCount(cube, "faces") == 1);
    CHECK(lo::live());
    run("undo");
    CHECK(selectedCount(cube, "faces") == 6);
    CHECK(lo::live());
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
    CHECK(faces(cube) == three);
    run("undo");
    CHECK(faces(cube) == one);
    run("undo");
    CHECK(faces(cube) == 6);

    run("newScene");
    cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
    run("select", {{"args", {cube}}});
    ui("polyMirror", {{"axis", "x"}});
    int mirrored = faces(cube);
    CHECK_THROWS_AS(lo::rerun({{"axis", "q"}}), CommandError);
    CHECK(faces(cube) == mirrored);
    CHECK(lo::live());

    run("newScene");
    cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
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

TEST_CASE("adjust-last-op edits are undo steps of their own")
{
    namespace lo = scene::lastOp;
    lo::install();
    UndoStack& undo = UndoStack::get();
    run("newScene");
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
    run("selectComponents", {{"args", {cube}}, {"faces", {0}}});
    auto points = [&] { return run("meshData", {{"args", {cube}}})["points"]; };
    auto thickness = [] { return lo::current()->call["thickness"].get<double>(); };

    ui("polyInset", {{"thickness", 5}});
    json p5 = points();
    lo::rerun({{"thickness", 10}});
    CHECK(undo.undoLabel() == "Adjust Inset: thickness");
    json p10 = points();
    lo::rerun({{"thickness", 15}});
    json p15 = points();
    CHECK(p5 != p10);
    CHECK(p10 != p15);
    // The same value again is no step at all.
    size_t steps = undo.undoCount();
    CHECK(lo::rerun({{"thickness", 15}}).is_null());
    CHECK(undo.undoCount() == steps);

    run("undo");
    CHECK(points() == p10);
    CHECK(thickness() == 10);
    CHECK(lo::live());
    run("undo");
    CHECK(points() == p5);
    CHECK(thickness() == 5);
    CHECK(lo::live());
    run("redo");
    CHECK(points() == p10);
    CHECK(thickness() == 10);
    run("redo");
    CHECK(points() == p15);
    CHECK(thickness() == 15);

    run("undo");
    run("undo");
    run("undo");
    CHECK(faces(cube) == 6);
    CHECK_FALSE(lo::live());
    run("redo");
    CHECK(points() == p5);
    CHECK(lo::live());
    CHECK(thickness() == 5);

    // A new edit after undoing drops the redo steps.
    lo::rerun({{"thickness", 20}});
    json p20 = points();
    CHECK_FALSE(undo.canRedo());
    run("undo");
    CHECK(points() == p5);
    run("redo");
    CHECK(points() == p20);
    CHECK(thickness() == 20);

    // A drag is one step, and a cancelled drag leaves none.
    steps = undo.undoCount();
    for (double t : {21.0, 22.0, 23.0})
        lo::rerun({{"thickness", t}}, CallSource::Script, true);
    lo::endDrag();
    CHECK(undo.undoCount() == steps + 1);
    CHECK(thickness() == 23);
    run("undo");
    CHECK(points() == p20);
    for (double t : {30.0, 31.0})
        lo::rerun({{"thickness", t}}, CallSource::Script, true);
    lo::endDrag(false);
    CHECK(undo.undoCount() == steps);
    CHECK(points() == p20);
    CHECK(thickness() == 20);
    CHECK(lo::live());

    // Jumping through the history lands on the op's settings too.
    run("undoJump", {{"position", int(steps) - 1}});
    CHECK(points() == p5);
    CHECK(thickness() == 5);
    CHECK(lo::live());
    run("undoJump", {{"position", int(steps)}});
    CHECK(points() == p20);
    CHECK(thickness() == 20);

    // Back through every setting to before the op.
    lo::rerun({{"thickness", 8}});
    run("undo");
    CHECK(thickness() == 20);
    run("undo");
    run("undo");
    CHECK(faces(cube) == 6);
}
