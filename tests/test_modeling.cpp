#include <doctest/doctest.h>

#include "cmds/Command.h"

using namespace lila;
using json = nlohmann::json;

namespace
{
json run(const char* name, json call = json::object()) { return cmds::execute(name, call); }
int faces(const std::string& m) { return run("meshInfo", {{"args", {m}}})["faces"].get<int>(); }
} // namespace

TEST_CASE("component selection and modeling ops")
{
    run("newScene");
    std::string cube = run("polyCube")[0];
    run("selectComponents", {{"args", {cube}}, {"faces", {0}}});
    CHECK(run("selectMode") == "face");
    CHECK(run("componentSelection")[cube]["faces"] == json::array({0}));

    run("polyExtrude", {{"thickness", 20}});
    CHECK(faces(cube) == 10);
    // History was baked by the edit; the generator is gone.
    CHECK(run("ls", {{"type", "polyCube"}}).empty());
    run("undo");
    CHECK(faces(cube) == 6);
    run("redo");
    CHECK(faces(cube) == 10);

    run("polyInset", {{"thickness", 5}});
    CHECK(faces(cube) == 14);

    run("selectMode", {{"args", {"edge"}}});
    auto sel = run("componentSelection");
    CHECK(sel[cube]["edges"].size() == 4); // the inset face's border
    run("polyBevel", {{"offset", 1}});
    CHECK(faces(cube) > 14);

    run("selectComponents", {{"args", {cube}}, {"faces", {0, 1}}});
    run("deleteComponents");
    CHECK(run("meshInfo", {{"args", {cube}}})["faces"].get<int>() > 0);
    run("polyFillHole");

    run("newScene");
    std::string plane = run("polyPlane", {{"subdivisionsX", 4}, {"subdivisionsZ", 1}})[0];
    run("setSymmetry", {{"args", {plane}}, {"axis", "x"}});
    run("selectComponents", {{"args", {plane}}, {"vertices", {0}}});
    json before = run("meshInfo", {{"args", {plane}}});
    run("moveComponents", {{"by", {0, 10, 0}}});
    run("rotateComponents", {{"by", {0, 0, 0}}});
    run("scaleComponents", {{"by", {1, 1, 1}}});
    json after = run("meshInfo", {{"args", {plane}}});
    CHECK(after["boundsMax"][1].get<double>() == doctest::Approx(10));

    run("newScene");
    std::string a = run("polyCube", {{"position", {-100, 0, 0}}})[0];
    std::string b = run("polyCube", {{"position", {100, 0, 0}}})[0];
    std::string c = run("polyCombine", {{"args", {a, b}}, {"name", "Both"}});
    CHECK(faces(c) == 12);
    json parts = run("polySeparate", {{"args", {c}}});
    CHECK(parts.size() == 2);
}

TEST_CASE("duplicate, freeze, center pivot")
{
    run("newScene");
    std::string cube = run("polyCube", {{"name", "Box"}, {"position", {10, 20, 30}}})[0];
    run("addModifier", {{"args", {cube}}, {"kind", "subdivision"}});
    json dup = run("duplicate", {{"args", {cube}}});
    REQUIRE(dup.size() == 1);
    std::string copy = dup[0];
    CHECK(copy != cube);
    CHECK(faces(copy) == faces(cube));
    // The copy has its own live stack: changing the original's generator leaves the copy alone.
    CHECK(run("modifiers", {{"args", {copy}}}).size() == 1);
    run("setAttr", {{"args", {"PolyCube.subdivisionsX", 3}}});
    CHECK(faces(cube) != faces(copy));

    run("rotate", {{"args", {copy}}, {"by", {0, 45, 0}}});
    run("scale", {{"args", {copy}}, {"by", {2, 1, 1}}});
    json before = run("meshData", {{"args", {copy}}, {"worldSpace", true}});
    run("freezeTransforms", {{"args", {copy}}});
    CHECK(run("getAttr", {{"args", {copy + ".ry"}}}).get<double>() == doctest::Approx(0));
    CHECK(run("getAttr", {{"args", {copy + ".sx"}}}).get<double>() == doctest::Approx(1));
    json after = run("meshData", {{"args", {copy}}, {"worldSpace", true}});
    REQUIRE(before["points"].size() == after["points"].size());
    for (size_t i = 0; i < before["points"].size(); ++i)
        for (int k = 0; k < 3; ++k)
            CHECK(after["points"][i][k].get<double>() == doctest::Approx(before["points"][i][k].get<double>()).epsilon(1e-9));

    run("centerPivot", {{"args", {copy}}});
    run("undo");
}
