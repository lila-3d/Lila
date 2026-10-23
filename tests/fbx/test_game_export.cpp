#include <doctest/doctest.h>

#include "cmds/Command.h"
#include "fbx/GameExport.h"
#include "math/MathCore.h"
#include "scene/Graph.h"

#include <ufbx.h>

#include <filesystem>

using namespace lila;
namespace fs = std::filesystem;

namespace
{
json run(const std::string& name, const json& call = json::object()) { return cmds::execute(name, call); }
std::string made(const json& r) { return (r.is_array() ? r[0] : r).get<std::string>(); }

fs::path tempDir()
{
    fs::path dir = fs::temp_directory_path() / "lila_fbx_tests" / "game";
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

/** dryRun's [{file, nodes}] as file name -> node names. */
std::map<std::string, json> planned(const json& r)
{
    std::map<std::string, json> out;
    for (const json& j : r)
        out[fs::path(j["file"].get<std::string>()).filename().string()] = j["nodes"];
    return out;
}

struct Loaded
{
    ufbx_scene* scene = nullptr;
    explicit Loaded(const fs::path& p)
    {
        ufbx_load_opts opts{};
        ufbx_error err{};
        scene = ufbx_load_file(p.string().c_str(), &opts, &err);
    }
    ~Loaded() { ufbx_free_scene(scene); }
};
} // namespace

TEST_CASE("game export: what goes in which file")
{
    math::setUpAxis(math::UpAxis::Y);
    run("newScene");
    std::string a = made(run("polyCube", {{"name", "Rock"}})), b = made(run("polyCube", {{"name", "Tree"}}));
    std::string c = made(run("polyCube", {{"name", "Bush"}}));
    run("createSet", {{"args", {a, b + ".f[0]"}}, {"name", "Props"}});
    run("createSet", {{"args", {c}}, {"name", "Plants"}});
    const std::string dir = tempDir().string();

    auto files = planned(run("gameExport", {{"path", dir}, {"mode", "sets"}, {"sets", json::array({"Props", "Plants"})}, {"dryRun", true}}));
    REQUIRE(files.size() == 1);
    CHECK(files["export.fbx"] == json({"Rock", "Tree", "Bush"}));

    files = planned(run("gameExport", {{"path", dir}, {"mode", "sets"}, {"sets", json::array({"Props", "Plants"})}, {"separate", "perSet"}, {"dryRun", true}}));
    CHECK(files["Props.fbx"] == json({"Rock", "Tree"}));
    CHECK(files["Plants.fbx"] == json({"Bush"}));

    files = planned(run("gameExport", {{"path", dir}, {"mode", "sets"}, {"sets", json::array({"Props"})}, {"separate", "perMember"}, {"dryRun", true}}));
    CHECK(files.size() == 2);
    CHECK(files["Tree.fbx"] == json({"Tree"}));

    run("select", {{"args", {c, "Props"}}});
    files = planned(run("gameExport", {{"path", dir}, {"mode", "selection"}, {"filename", "Sel"}, {"fileType", "obj"}, {"dryRun", true}}));
    CHECK(files["Sel.obj"] == json({"Bush", "Rock", "Tree"}));

    files = planned(run("gameExport", {{"path", dir}, {"dryRun", true}}));
    CHECK(files["export.fbx"].size() == 3);

    CHECK_THROWS_AS(run("gameExport", {{"mode", "sets"}, {"sets", json::array({"Rock"})}, {"dryRun", true}}), CommandError);
    CHECK_THROWS_AS(run("gameExport", {{"mode", "sets"}, {"dryRun", true}}), CommandError);
    CHECK_THROWS_AS(run("gameExport", {{"separate", "perThing"}, {"dryRun", true}}), CommandError);
}

TEST_CASE("game export writes per-set files with units and smoothing options")
{
    math::setUpAxis(math::UpAxis::Y);
    run("newScene");
    std::string a = made(run("polyCube", {{"name", "Rock"}}));
    std::string b = made(run("polyCube", {{"name", "Tree"}}));
    run("setAttr", {{"args", {a + ".tx", 250.0}}});
    run("createSet", {{"args", {a}}, {"name", "Props"}});
    run("createSet", {{"args", {b}}, {"name", "Plants"}});
    const fs::path dir = tempDir();

    json written = run("gameExport", {{"path", dir.string()}, {"mode", "sets"}, {"sets", json::array({"Props", "Plants"})}, {"separate", "perSet"},
                                      {"units", "m"}, {"smoothingGroups", false}, {"profile", "generic"}});
    REQUIRE(written.size() == 2);
    CHECK(fs::exists(dir / "Props.fbx"));
    CHECK(fs::exists(dir / "Plants.fbx"));

    Loaded props(dir / "Props.fbx");
    REQUIRE(props.scene);
    CHECK(props.scene->settings.unit_meters == doctest::Approx(1.0));
    const ufbx_node* rock = ufbx_find_node(props.scene, "Rock");
    REQUIRE(rock);
    CHECK(rock->local_transform.translation.x == doctest::Approx(2.5));
    REQUIRE(rock->mesh);
    CHECK(rock->mesh->edge_smoothing.count == 0);
    CHECK_FALSE(ufbx_find_node(props.scene, "Tree"));

    run("gameExport", {{"path", dir.string()}, {"filename", "All"}});
    Loaded all(dir / "All.fbx");
    REQUIRE(all.scene);
    CHECK(all.scene->settings.unit_meters == doctest::Approx(0.01));
    REQUIRE(ufbx_find_node(all.scene, "Rock"));
    CHECK(ufbx_find_node(all.scene, "Rock")->mesh->edge_smoothing.count > 0);
    CHECK(ufbx_find_node(all.scene, "Tree"));
}

TEST_CASE("game export presets in the prefs and the scene")
{
    run("newScene");
    std::string a = made(run("polyCube", {{"name", "Rock"}}));
    run("createSet", {{"args", {a}}, {"name", "Props"}});
    run("gameExportSavePreset", {{"args", {"Props only"}}, {"mode", "sets"}, {"sets", json::array({"Props"})}, {"filename", "P"}, {"scene", true}});
    run("gameExportSavePreset", {{"args", {"Everything"}}, {"filename", "E"}});
    json presets = run("gameExportPresets");
    CHECK(presets["scene"]["Props only"]["mode"] == "sets");
    CHECK(presets["prefs"]["Everything"]["filename"] == "E");

    auto files = planned(run("gameExport", {{"preset", "Props only"}, {"path", tempDir().string()}, {"dryRun", true}}));
    CHECK(files["P.fbx"] == json({"Rock"}));
    // Flags override the preset.
    files = planned(run("gameExport", {{"preset", "Props only"}, {"filename", "Q"}, {"dryRun", true}}));
    CHECK(files.contains("Q.fbx"));

    run("gameExportDeletePreset", {{"args", {"Props only"}}, {"scene", true}});
    run("gameExportDeletePreset", {{"args", {"Everything"}}});
    presets = run("gameExportPresets");
    CHECK(presets["scene"].empty());
    CHECK_FALSE(presets["prefs"].contains("Everything"));
    CHECK_THROWS_AS(run("gameExport", {{"preset", "Everything"}, {"dryRun", true}}), CommandError);
}
