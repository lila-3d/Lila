#include "TexTest.h"

#include "ops/Command.h"
#include "scene/Graph.h"
#include "tex/TextureSet.h"

#include <filesystem>

using namespace textest;

namespace
{
namespace fs = std::filesystem;

/** A small set: bricks feed base colour (through colorize) and height, height feeds the normal; roughness is a constant. */
void buildRock()
{
    run("newScene");
    run("texCreateSet", {{"args", {"Rock"}}, {"resolution", 64}});
    run("texAddNode", {{"args", {"Rock", "bricks"}}, {"name", "Bricks"}});
    run("texAddNode", {{"args", {"Rock", "colorize"}}, {"name", "Tint"}});
    run("texAddNode", {{"args", {"Rock", "heightToNormal"}}, {"name", "Nrm"}});
    run("texConnect", {{"args", {"Rock", "Bricks.mask", "Tint.input"}}});
    run("texConnect", {{"args", {"Rock", "Tint.out", "Output.baseColor"}}});
    run("texConnect", {{"args", {"Rock", "Bricks.mask", "Output.height"}}});
    run("texConnect", {{"args", {"Rock", "Bricks.mask", "Nrm.height"}}});
    run("texConnect", {{"args", {"Rock", "Nrm.normal", "Output.normal"}}});
    run("texSetValue", {{"args", {"Rock", "Output.roughness", 0.7}}});
    run("texSetValue", {{"args", {"Rock", "Output.metallic", 0.2}}});
    run("texSetValue", {{"args", {"Rock", "Output.ao", 0.9}}});
}
} // namespace

TEST_CASE("tex: commands build, edit and undo a texture set")
{
    buildRock();
    CHECK(run("texSets") == json::array({"Rock"}));
    json info = run("texGraphInfo", {{"args", {"Rock"}}});
    CHECK_FALSE(info.contains("error"));
    CHECK(info["nodes"].size() == 4);
    CHECK(info["plan"]["passes"].size() >= 1);
    const std::string hash = info["plan"]["structureHash"];

    run("texSetValue", {{"args", {"Rock", "Bricks.rows", 12}}});
    CHECK(run("texGraphInfo", {{"args", {"Rock"}}})["plan"]["structureHash"] == hash);
    run("undo");
    json node = run("texGraphInfo", {{"args", {"Rock"}}})["nodes"];
    for (const json& n : node)
        if (n["name"] == "Bricks")
            for (const json& p : n["params"])
                if (p["id"] == "rows")
                    CHECK(p["value"] == 8);

    CHECK_THROWS_AS(run("texConnect", {{"args", {"Rock", "Tint.out", "Bricks.nothing"}}}), CommandError);
    CHECK_THROWS_AS(run("texConnect", {{"args", {"Rock", "Nrm.normal", "Nrm.height"}}}), CommandError);
    CHECK_THROWS_AS(run("texSetValue", {{"args", {"Rock", "Bricks.rows", "many"}}}), CommandError);
    CHECK_THROWS_AS(run("texSetValue", {{"args", {"Rock", "Tint.gradient", 3}}}), CommandError);
    CHECK_THROWS_AS(run("texAddNode", {{"args", {"Rock", "nope"}}}), CommandError);

    CHECK(run("texDisconnect", {{"args", {"Rock", "Output.height"}}}) == 1);
    CHECK(run("texGraphInfo", {{"args", {"Rock"}}})["plan"]["structureHash"] != hash);
    run("undo");
    CHECK(run("texGraphInfo", {{"args", {"Rock"}}})["plan"]["structureHash"] == hash);

    json dup = run("texDuplicateNodes", {{"args", {"Rock", "Bricks", "Tint"}}});
    CHECK(dup.size() == 2);
    std::string text = run("texCopyNodes", {{"args", {"Rock", "Bricks", "Tint"}}});
    json pasted = run("texPasteNodes", {{"args", {"Rock", text}}});
    CHECK(pasted.size() == 2);
    run("texDeleteNode", {{"args", {"Rock", dup[0], dup[1], pasted[0], pasted[1]}}});
    CHECK(run("texGraphInfo", {{"args", {"Rock"}}})["nodes"].size() == 4);

    json types = run("texNodeTypes", {{"linkType", "material"}});
    CHECK(std::any_of(types.begin(), types.end(), [](const json& t) { return t["type"] == "materialOutput"; }));
    CHECK(std::none_of(types.begin(), types.end(), [](const json& t) { return t["type"] == "levels"; }));
}

TEST_CASE("tex: texture sets save and load in .lila and .lilaa")
{
    buildRock();
    run("texSetSettings", {{"args", {"Rock", {{"tiles", json::array({1001, json{{"udim", 1002}, {"width", 32}}})}, {"bakeProject", "RockBake"}}}}});
    json before = run("texGraphInfo", {{"args", {"Rock"}}});
    for (const char* ext : {".lila", ".lilaa"})
    {
        CAPTURE(ext);
        fs::path path = tempDir("files") / (std::string("rock") + ext);
        run("saveScene", {{"args", {path.string()}}});
        run("newScene");
        CHECK(run("texSets").empty());
        run("openScene", {{"args", {path.string()}}});
        json after = run("texGraphInfo", {{"args", {"Rock"}}});
        CHECK(after["definition"] == before["definition"]);
        CHECK(after["settings"] == before["settings"]);
        CHECK(after["plan"]["structureHash"] == before["plan"]["structureHash"]);
    }
}

TEST_CASE("tex: texEvaluate writes every channel of every tile")
{
    buildRock();
    run("texSetSettings", {{"args", {"Rock", {{"tiles", json::array({1001, 1002})}}}}});
    fs::path dir = tempDir("evaluate");
    json r = run("texEvaluate", {{"args", {"Rock"}}, {"folder", dir.string()}, {"channels", {"baseColor", "roughness"}}});
    CHECK(r["files"].size() == 4);
    CHECK(fs::exists(dir / "Rock_baseColor.1001.png"));
    CHECK(fs::exists(dir / "Rock_roughness.1002.png"));
    image::Image rough = image::read(dir / "Rock_roughness.1001.png");
    REQUIRE(rough.width == 64);
    CHECK(rough.rgba[0] == doctest::Approx(0.7).epsilon(0.01));
}

TEST_CASE("tex: 8-bit colour channels are written sRGB-encoded, float files and data channels linear")
{
    run("newScene");
    run("texCreateSet", {{"args", {"Srgb"}}, {"resolution", 16}});
    run("texSetValue", {{"args", {"Srgb", "Output.baseColor", json::array({0.214, 0.214, 0.214, 1.0})}}});
    run("texSetValue", {{"args", {"Srgb", "Output.roughness", 0.214}}});
    fs::path dir = tempDir("srgb");
    run("texEvaluate", {{"args", {"Srgb"}}, {"folder", dir.string()}, {"channels", {"baseColor", "roughness"}}});
    CHECK(image::read(dir / "Srgb_baseColor.png").rgba[0] == doctest::Approx(0.5).epsilon(0.01));
    CHECK(image::read(dir / "Srgb_roughness.png").rgba[0] == doctest::Approx(0.214).epsilon(0.01));
    run("texEvaluate", {{"args", {"Srgb"}}, {"folder", dir.string()}, {"channels", {"baseColor"}}, {"format", "exr"}});
    CHECK(image::read(dir / "Srgb_baseColor.exr").rgba[0] == doctest::Approx(0.214).epsilon(0.01));

    json r = run("texExport", {{"args", {"Srgb"}}, {"preset", "unreal"}, {"folder", dir.string()}});
    CAPTURE(r.dump());
    CHECK(image::read(dir / "T_Srgb_BC.png").rgba[0] == doctest::Approx(0.5).epsilon(0.01));
}

TEST_CASE("tex: texExport packs channels through the Unreal preset")
{
    buildRock();
    fs::path dir = tempDir("export");
    json r = run("texExport", {{"args", {"Rock"}}, {"preset", "unreal"}, {"folder", dir.string()}});
    CAPTURE(r.dump());
    CHECK(fs::exists(dir / "T_Rock_ORM.png"));
    CHECK(fs::exists(dir / "T_Rock_N.png"));
    CHECK(fs::exists(dir / "T_Rock_BC.png"));
    CHECK(fs::exists(dir / "T_Rock_H.png"));
    // Only the normal map carries a green convention: a reader flips the green of any map marked opengl.
    for (const json& o : r["outputs"])
        CHECK(o["normalFormat"] == (o["map"] == "normal" ? "directx" : ""));
    image::Image orm = image::read(dir / "T_Rock_ORM.png");
    REQUIRE(orm.width == 64);
    CHECK(orm.rgba[0] == doctest::Approx(0.9).epsilon(0.01));
    CHECK(orm.rgba[1] == doctest::Approx(0.7).epsilon(0.01));
    CHECK(orm.rgba[2] == doctest::Approx(0.2).epsilon(0.01));

    // Unreal wants DirectX normals: the graph's OpenGL green is flipped on the way out.
    fs::path raw = tempDir("rawNormal");
    run("texEvaluate", {{"args", {"Rock"}}, {"folder", raw.string()}, {"channels", {"normal"}}, {"bits", 16}});
    image::Image gl = image::read(raw / "Rock_normal.png");
    image::Image dx = image::read(dir / "T_Rock_N.png");
    REQUIRE(gl.width == dx.width);
    double worst = 0.0;
    for (size_t i = 0; i < gl.rgba.size(); i += 4)
        worst = std::max(worst, std::abs(double(gl.rgba[i + 1]) - (1.0 - double(dx.rgba[i + 1]))));
    CHECK(worst < 0.01);
}
