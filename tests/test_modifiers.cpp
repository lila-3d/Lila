#include <doctest/doctest.h>

#include "TestUtil.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/LastOp.h"

#include <filesystem>

using namespace lila;
using json = nlohmann::json;
using namespace testutil;

namespace
{
/** The cage face whose normal points along +axis (or -axis). */
int faceOnAxis(const std::string& m, int axis, double sign)
{
    json d = run("meshData", {{"args", {m}}, {"base", true}});
    for (size_t f = 0; f < d["faces"].size(); ++f)
    {
        const json& face = d["faces"][f];
        double lo = 1e9, hi = -1e9, along = 0.0;
        for (const json& p : face)
        {
            const double v = d["points"][p.get<size_t>()][axis].get<double>();
            lo = std::min(lo, v);
            hi = std::max(hi, v);
            along += v;
        }
        if (hi - lo < 1e-6 && along * sign > 0.0)
            return int(f);
    }
    return -1;
}

/** The final mesh's extent along an axis. */
std::pair<double, double> extent(const std::string& m, int axis)
{
    double lo = 1e9, hi = -1e9;
    const json d = run("meshData", {{"args", {m}}});
    for (const json& p : d["points"])
    {
        lo = std::min(lo, p[axis].get<double>());
        hi = std::max(hi, p[axis].get<double>());
    }
    return {lo, hi};
}
} // namespace

TEST_CASE("modifier stack is live and edits go under it")
{
    run("newScene");
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
    std::string sub = run("addModifier", {{"args", {cube}}, {"kind", "subdivision"}});
    CHECK(faces(cube) == 24);
    std::string mirror = run("addModifier", {{"args", {cube}}, {"kind", "array"}, {"params", {{"count", 2}}}});
    CHECK(faces(cube) == 48);
    CHECK(run("modifiers", {{"args", {cube}}}) == json::array({sub, mirror}));

    // Turning a modifier off passes the mesh through.
    run("setAttr", {{"args", {sub + ".enabled", false}}});
    CHECK(faces(cube) == 12);
    run("setAttr", {{"args", {sub + ".enabled", true}}});

    // Component edits land on the cage (6 faces), and the stack re-runs on top.
    run("selectComponents", {{"args", {cube}}, {"faces", {0}}});
    run("polyExtrude", {{"thickness", 10}});
    CHECK(faces(cube) == 10 * 4 * 2);
    CHECK(run("modifiers", {{"args", {cube}}}).size() == 2);

    run("moveModifier", {{"args", {sub}}, {"by", 1}});
    CHECK(run("modifiers", {{"args", {cube}}}) == json::array({mirror, sub}));
    CHECK(faces(cube) == 80);

    run("applyModifier", {{"args", {mirror}}});
    CHECK(run("modifiers", {{"args", {cube}}}) == json::array({sub}));
    CHECK(faces(cube) == 80);
    run("removeModifier", {{"args", {sub}}});
    CHECK(faces(cube) == 20);
    CHECK(run("modifiers", {{"args", {cube}}}).empty());

    run("undo");
    CHECK(faces(cube) == 80);
}

TEST_CASE("every modifier kind evaluates")
{
    run("newScene");
    for (const char* kind : {"mirror", "subdivision", "bevel", "array", "triangulate", "weightedNormals", "solidify", "decimate"})
    {
        std::string m = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}, {"name", std::string("M") + kind}})[0];
        run("addModifier", {{"args", {m}}, {"kind", kind}});
        CHECK_MESSAGE(faces(m) > 0, kind);
    }
}

TEST_CASE("extrude modifier extrudes the faces selected when added, live and undoable")
{
    run("newScene");
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
    const int top = faceOnAxis(cube, 1, 1.0);
    REQUIRE(top >= 0);
    run("selectComponents", {{"args", {cube}}, {"faces", {top}}});
    std::string mod = run("addModifier", {{"args", {cube}}, {"kind", "extrude"}, {"params", {{"thickness", 20.0}}}});
    CHECK(faces(cube) == 10);
    CHECK(run("meshData", {{"args", {cube}}, {"base", true}})["faces"].size() == 6);
    CHECK(extent(cube, 1).second == doctest::Approx(70.0));

    run("setAttr", {{"args", {mod + ".thickness", 5.0}}});
    CHECK(extent(cube, 1).second == doctest::Approx(55.0));
    run("setAttr", {{"args", {mod + ".offset", 10.0}}});
    CHECK(faces(cube) == 10);
    run("undo");
    run("undo");
    CHECK(extent(cube, 1).second == doctest::Approx(70.0));
    run("undo");
    CHECK(faces(cube) == 6);
    CHECK(run("modifiers", {{"args", {cube}}}).empty());
    run("redo");
    CHECK(faces(cube) == 10);
    CHECK(run("getAttr", {{"args", {mod + ".thickness"}}}) == 20.0);

    // Two neighbouring faces extrude as one region unless individual.
    run("newScene");
    cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
    run("selectComponents", {{"args", {cube}}, {"faces", {faceOnAxis(cube, 1, 1.0), faceOnAxis(cube, 0, 1.0)}}});
    mod = run("addModifier", {{"args", {cube}}, {"kind", "extrude"}});
    CHECK(faces(cube) == 6 + 6);
    run("setAttr", {{"args", {mod + ".individual", true}}});
    CHECK(faces(cube) == 6 + 8);

    // No selection extrudes every face.
    run("newScene");
    cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
    run("addModifier", {{"args", {cube}}, {"kind", "extrude"}});
    CHECK(extent(cube, 0).second > 55.0);
}

TEST_CASE("extrude modifier keeps its faces through edits under it")
{
    run("newScene");
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
    // The highest id, so deleting face 0 shifts it when the cage compacts.
    int axis = -1;
    double sign = 0.0;
    for (int a = 0; a < 3; ++a)
        for (double s : {1.0, -1.0})
            if (faceOnAxis(cube, a, s) == 5)
                axis = a, sign = s;
    REQUIRE(axis >= 0);
    run("selectComponents", {{"args", {cube}}, {"faces", {5}}});
    std::string mod = run("addModifier", {{"args", {cube}}, {"kind", "extrude"}, {"params", {{"thickness", 30.0}}}});
    auto reach = [&] { auto [lo, hi] = extent(cube, axis); return sign > 0 ? hi : -lo; };
    CHECK(reach() == doctest::Approx(80.0));

    run("selectComponents", {{"args", {cube}}, {"faces", {0}}});
    run("deleteComponents");
    CHECK(faces(cube) == 5 + 4);
    CHECK(run("getAttr", {{"args", {mod + ".lostComponents"}}}) == 0);
    CHECK(reach() == doctest::Approx(80.0));
    run("undo");
    CHECK(faces(cube) == 10);
    CHECK(reach() == doctest::Approx(80.0));

    // A modifier below that changes the topology: the faces are found again where they were.
    run("newScene");
    cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
    run("addModifier", {{"args", {cube}}, {"kind", "array"}, {"params", {{"count", 2}}}});
    run("selectComponents", {{"args", {cube}}, {"faces", {faceOnAxis(cube, 1, 1.0)}}});
    mod = run("addModifier", {{"args", {cube}}, {"kind", "extrude"}});
    CHECK(faces(cube) == 12 + 4);
    CHECK(run("getAttr", {{"args", {mod + ".lostComponents"}}}) == 0);
    // A face that no longer exists is dropped rather than guessed.
    run("polySubdivide", {{"args", {cube}}});
    CHECK(run("getAttr", {{"args", {mod + ".lostComponents"}}}) == 1);
}

TEST_CASE("extrude modifier grows edges and follows symmetry")
{
    run("newScene");
    std::string plane = run("polyPlane", {{"width", 100.0}, {"depth", 100.0}})[0];
    run("selectComponents", {{"args", {plane}}, {"edges", {0}}});
    run("addModifier", {{"args", {plane}}, {"kind", "extrude"}, {"params", {{"thickness", 0.0}, {"offset", 25.0}}}});
    CHECK(faces(plane) == 2);
    auto [lo, hi] = extent(plane, 0);
    auto [zlo, zhi] = extent(plane, 2);
    CHECK((hi - lo) + (zhi - zlo) == doctest::Approx(225.0));

    run("newScene");
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
    run("toggleSymmetry", {{"args", {cube}}, {"axis", "x"}});
    run("selectComponents", {{"args", {cube}}, {"faces", {faceOnAxis(cube, 0, 1.0)}}});
    run("addModifier", {{"args", {cube}}, {"kind", "extrude"}});
    CHECK(faces(cube) == 6 + 8);
    CHECK(extent(cube, 0).first == doctest::Approx(-60.0));
}

TEST_CASE("polyExtrude made non-destructive becomes an extrude modifier")
{
    namespace lo = scene::lastOp;
    run("newScene");
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
    const int front = faceOnAxis(cube, 2, 1.0);
    run("selectComponents", {{"args", {cube}}, {"faces", {front}}});
    ops::execute("polyExtrude", {{"thickness", 15.0}, {"offset", 5.0}}, CallSource::UI);
    REQUIRE(lo::live());
    CHECK(faces(cube) == 10);
    lo::makeNonDestructive();
    json stack = run("modifiers", {{"args", {cube}}});
    REQUIRE(stack.size() == 1);
    const std::string mod = stack[0];
    CHECK(run("nodeType", {{"args", {mod}}}) == "extrudeModifier");
    CHECK(run("getAttr", {{"args", {mod + ".thickness"}}}) == 15.0);
    CHECK(run("getAttr", {{"args", {mod + ".offset"}}}) == 5.0);
    CHECK(faces(cube) == 10);
    CHECK(run("meshData", {{"args", {cube}}, {"base", true}})["faces"].size() == 6);
    CHECK(extent(cube, 2).second == doctest::Approx(65.0));
    run("undo");
    CHECK(faces(cube) == 6);
    CHECK(run("modifiers", {{"args", {cube}}}).empty());
    run("redo");
    CHECK(faces(cube) == 10);

    // In object mode the op extruded every face, and so does the modifier.
    run("newScene");
    cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
    run("select", {{"args", {cube}}});
    ops::execute("polyExtrude", {{"thickness", 10.0}}, CallSource::UI);
    const json destructive = run("meshData", {{"args", {cube}}});
    lo::makeNonDestructive();
    CHECK(run("modifiers", {{"args", {cube}}}).size() == 1);
    CHECK(run("meshData", {{"args", {cube}}}) == destructive);
}

TEST_CASE("extrude modifier saves and loads")
{
    run("newScene");
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}, {"name", "Extruded"}})[0];
    run("addModifier", {{"args", {cube}}, {"kind", "array"}, {"params", {{"count", 2}}}});
    run("selectComponents", {{"args", {cube}}, {"faces", {faceOnAxis(cube, 1, 1.0)}}});
    std::string faceMod = run("addModifier", {{"args", {cube}}, {"kind", "extrude"}, {"params", {{"thickness", 12.5}}}});
    std::string plane = run("polyPlane", {{"width", 100.0}, {"depth", 100.0}, {"name", "Strip"}})[0];
    run("selectComponents", {{"args", {plane}}, {"edges", {0}}});
    run("addModifier", {{"args", {plane}}, {"kind", "extrude"}, {"params", {{"thickness", 0.0}, {"offset", 10.0}}}});
    const json cubeData = run("meshData", {{"args", {cube}}});
    const json planeData = run("meshData", {{"args", {plane}}});
    auto dir = std::filesystem::temp_directory_path() / "lila_tests";
    std::filesystem::create_directories(dir);
    for (const char* ext : {".lila", ".lilaa"})
    {
        CAPTURE(ext);
        auto path = (dir / (std::string("extrude_modifier") + ext)).string();
        run("saveScene", {{"args", {path}}});
        run("newScene");
        run("openScene", {{"args", {path}}});
        CHECK(run("meshData", {{"args", {cube}}}) == cubeData);
        CHECK(run("meshData", {{"args", {plane}}}) == planeData);
        CHECK(run("getAttr", {{"args", {faceMod + ".thickness"}}}) == 12.5);
        CHECK(run("getAttr", {{"args", {faceMod + ".lostComponents"}}}) == 0);
        run("setAttr", {{"args", {faceMod + ".thickness", 2.5}}});
        CHECK(extent(cube, 1).second == doctest::Approx(52.5));
        run("undo");
        CHECK(run("meshData", {{"args", {cube}}}) == cubeData);
    }
}

TEST_CASE("modifier stacks save and load")
{
    run("newScene");
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}, {"name", "Stacked"}})[0];
    run("addModifier", {{"args", {cube}}, {"kind", "bevel"}, {"params", {{"segments", 3}}}});
    run("addModifier", {{"args", {cube}}, {"kind", "mirror"}});
    int before = faces(cube);
    auto dir = std::filesystem::temp_directory_path() / "lila_tests";
    for (const char* ext : {".lila", ".lilaa"})
    {
        auto path = (dir / (std::string("stack") + ext)).string();
        run("saveScene", {{"args", {path}}});
        run("newScene");
        run("openScene", {{"args", {path}}});
        CHECK(faces(cube) == before);
        CHECK(run("modifiers", {{"args", {cube}}}).size() == 2);
    }
}

TEST_CASE("triangulating a mesh with modifiers triangulates the result")
{
    run("newScene");
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
    run("addModifier", {{"args", {cube}}, {"kind", "bevel"}, {"params", {{"segments", 2}}}});
    const json before = run("meshInfo", {{"args", {cube}}});
    run("select", {{"args", {cube}}});
    run("polyTriangulate");
    const json after = run("meshInfo", {{"args", {cube}}});
    CHECK(after["points"] == before["points"]);
    CHECK(after["faces"] == before["triangles"]);
    CHECK(after["triangles"] == before["triangles"]);
    const json stack = run("modifiers", {{"args", {cube}}});
    REQUIRE(stack.size() == 2);
    // Again is a no-op rather than a second modifier.
    run("selectMode", {{"args", {"object"}}});
    run("select", {{"args", {cube}}});
    run("polyTriangulate");
    CHECK(run("modifiers", {{"args", {cube}}}).size() == 2);
    run("undo");
    CHECK(run("modifiers", {{"args", {cube}}}).size() == 1);
    CHECK(run("meshInfo", {{"args", {cube}}})["faces"] == before["faces"]);
}
