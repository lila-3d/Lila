#include <doctest/doctest.h>

#include "ops/Command.h"
#include "scene/Graph.h"

using namespace lila;

namespace
{
json run(const std::string& name, const json& call = json::object()) { return ops::execute(name, call); }

std::string cube()
{
    run("newScene");
    return run("polyCube", {{"width", 20}, {"height", 10}, {"depth", 30}, {"subdivisionsX", 2}, {"position", {0, 5, 0}}})[0];
}
} // namespace

TEST_CASE("findComponents filters by normal, side, box, extreme and direction")
{
    std::string m = cube();
    json top = run("findComponents", {{"args", {m}}, {"kind", "face"}, {"normal", {0, 1, 0}}});
    CHECK(top.size() == 2);
    json topPlus = run("findComponents", {{"args", {m}}, {"kind", "face"}, {"normal", {0, 1, 0}}, {"side", "+x"}});
    REQUIRE(topPlus.size() == 1);
    json info = run("componentInfo", {{"args", {m}}, {"kind", "face"}, {"ids", topPlus}});
    REQUIRE(info.size() == 1);
    CHECK(info[0]["center"][0].get<double>() == doctest::Approx(5.0));
    CHECK(info[0]["center"][1].get<double>() == doctest::Approx(10.0));
    CHECK(info[0]["normal"][1].get<double>() == doctest::Approx(1.0));
    CHECK(info[0]["neighbors"].size() == 4);

    // Open box sides: everything above y=9 is the top two faces.
    json above = run("findComponents", {{"args", {m}}, {"kind", "face"}, {"box", {{nullptr, 9, nullptr}, {nullptr, nullptr, nullptr}}}});
    CHECK(above == top);

    json front = run("findComponents", {{"args", {m}}, {"kind", "face"}, {"extreme", "+z"}, {"count", 2}});
    for (const json& f : run("componentInfo", {{"args", {m}}, {"kind", "face"}, {"ids", front}}))
        CHECK(f["normal"][2].get<double>() == doctest::Approx(1.0));

    json centre = run("findComponents", {{"args", {m}}, {"kind", "vertex"}, {"side", "center"}});
    CHECK(centre.size() == 4);
    json vertical = run("findComponents", {{"args", {m}}, {"kind", "edge"}, {"direction", {0, 1, 0}}});
    CHECK(vertical.size() == 6);
    CHECK_THROWS_AS(run("findComponents", {{"args", {m}}, {"kind", "face"}, {"direction", {0, 1, 0}}}), CommandError);
}

TEST_CASE("selectComponentsWhere selects and feeds mesh edits; meshInfo reports health")
{
    std::string m = cube();
    run("setSymmetry", {{"args", {m}}, {"axis", "x"}});
    json sel = run("selectComponentsWhere", {{"args", {m}}, {"kind", "face"}, {"normal", {0, 0, 1}}, {"side", "+x"}});
    CHECK(sel.size() == 1);
    CHECK(run("selectMode") == "face");
    run("polyExtrude", {{"thickness", 5}});
    json info = run("meshInfo", {{"args", {m}}});
    // Symmetry mirrors the face; the pair touches the seam, so it extrudes as one region: 10 faces + 6 sides.
    CHECK(info["faces"] == 16);
    CHECK(info["closed"] == true);
    CHECK(info["shells"] == 1);
    CHECK(info["quads"] == 16);
    CHECK(info["symmetry"] == "x");
    CHECK(info["worldBoundsMax"][2].get<double>() == doctest::Approx(20.0));

    run("selectComponentsWhere", {{"args", {m}}, {"kind", "face"}, {"normal", {0, 0, 1}}, {"deselect", true}});
    CHECK(run("componentSelection").empty());
}

TEST_CASE("meshCheck finds floating parts and shell= selects one")
{
    std::string m = cube();
    json ok = run("meshCheck", {{"args", {m}}});
    CHECK(ok["ok"] == true);
    CHECK(ok["shells"] == 1);
    CHECK(ok["issues"].empty());

    std::string ball = run("polyCube", {{"width", 4}, {"height", 4}, {"depth", 4}, {"position", {0, 20, 0}}})[0];
    std::string both = run("polyCombine", {{"args", {m, ball}}, {"name", "Both"}});
    json bad = run("meshCheck", {{"args", {both}}});
    CHECK(bad["ok"] == false);
    CHECK(bad["shells"] == 2);
    REQUIRE(bad["shellList"].size() == 2);
    CHECK(bad["shellList"][0]["faces"] == 10);
    CHECK(bad["shellList"][1]["faces"] == 6);
    CHECK(bad["shellList"][1]["box"][0][1].get<double>() == doctest::Approx(18.0));
    CHECK(run("meshCheck", {{"args", {both}}, {"expectShells", 2}})["ok"] == true);

    json small = run("findComponents", {{"args", {both}}, {"kind", "face"}, {"shell", 1}});
    CHECK(small.size() == 6);
    json brief = run("componentInfo", {{"args", {both}}, {"kind", "face"}, {"ids", small}, {"fields", {"id", "center"}}});
    REQUIRE(brief.size() == 6);
    CHECK(brief[0].size() == 2);
    CHECK(brief[0]["center"][1].get<double>() > 17.0);
}

TEST_CASE("with symmetry, scaling a pair clear of the plane keeps each part in place")
{
    run("newScene");
    std::string m = run("polyCube", {{"width", 20}, {"height", 10}, {"depth", 30}, {"subdivisionsX", 4}})[0];
    run("setSymmetry", {{"args", {m}}, {"axis", "x"}});
    run("selectComponentsWhere", {{"args", {m}}, {"kind", "face"}, {"normal", {0, -1, 0}}, {"side", "+x"}, {"extreme", "+x"}});
    run("polyExtrude", {{"thickness", 5}});
    run("scaleComponents", {{"by", {0.5, 1, 0.5}}});
    json caps = run("componentInfo", {{"args", {m}}, {"kind", "face"}, {"fields", {"center", "area"}}});
    REQUIRE(caps.size() == 2);
    for (const json& c : caps)
    {
        CHECK(std::abs(c["center"][0].get<double>()) == doctest::Approx(7.5));
        CHECK(c["area"].get<double>() == doctest::Approx(5.0 * 30.0 * 0.25));
    }
}
