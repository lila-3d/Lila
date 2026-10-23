#include "Isolate.h"

#include "extensions/ExtensionManager.h"
#include "gn/NodeLib.h"
#include "io/FileTranslators.h"
#include "io/SceneFile.h"
#include "link/Exporters.h"
#include "mcp/McpServer.h"
#include "python/Python.h"
#include "scene/AnimCurve.h"
#include "scene/AnimInterpolator.h"
#include "scene/Deformers.h"
#include "scene/Graph.h"
#include "scene/IkSolver.h"
#include "scene/SceneCommands.h"
#include "scene/SceneScripts.h"

#include <doctest/doctest.h>

#include <cstdlib>
#include <fstream>

using namespace lila;

namespace
{
json run(const char* cmd, json call = json::object())
{
    return ops::execute(cmd, call);
}

/** The installed-SDK build of the sample when LILA_TEST_SCENE_POINTS_PACKAGE names one, else the in-tree build. */
std::string scenePointsPackage()
{
    const char* env = std::getenv("LILA_TEST_SCENE_POINTS_PACKAGE");
    return env && *env ? env : LILA_TEST_SCENE_POINTS_PACKAGE;
}

/** Every registry an extension can add to on the scene side, as ids. */
json registries()
{
    json gnTypes = json::array();
    for (const gn::NodeDecl* d : gn::allDecls())
        gnTypes.push_back(d->type);
    return {{"translators", io::translators::registry().ids()},
            {"interpolators", scene::anim::interpolators().ids()},
            {"ikSolvers", scene::ik::solvers().ids()},
            {"gn", gnTypes},
            {"dataKinds", scene::data::kinds().ids()},
            {"linkExporters", link::exporterFormats()},
            {"types", scene::types::registry().ids()},
            {"commands", ops::registry().ids()},
            {"mcpTool", mcp::Server::get().hasTool("scene_points_count")}};
}

std::string tempFile(const char* name)
{
    return (std::filesystem::temp_directory_path() / name).generic_string();
}
} // namespace

TEST_CASE("scene extension points: an extension registers one of each, and unloading restores every registry")
{
    isolate();
    extensions::registerCommands();
    scene::registerCommands();
    io::registerCommands();
    gn::registerNodeLibrary();
    io::translators::registerBuiltins();
    io::translators::registerCommands();
    run("newScene");

    const json before = registries();
    extensions::ExtensionManager& em = extensions::ExtensionManager::get();
    REQUIRE(em.load(scenePointsPackage()) == "scenePoints");
    CHECK(registries() != before);

    // File translator: found by extension, run through its command, listed with its owner.
    const io::translators::FileTranslator* pts = io::translators::forPath(io::translators::Op::Import, "C:/x/points.PTS");
    REQUIRE(pts);
    CHECK(pts->id == "scenePoints");
    CHECK(io::translators::describe(*pts)["owner"] == "scenePoints");
    CHECK(io::translators::dialogFilter(io::translators::Op::Import, "All").find("Point lists (*.pts)") != std::string::npos);
    const std::string in = tempFile("lila_scene_points_in.pts");
    std::ofstream(in) << "PtA 1 2 3\nPtB 4 5 6\n";
    CHECK(run("importFile", {{"args", {in}}}) == json::array({"PtA", "PtB"}));
    scene::Graph& g = scene::Graph::current();
    CHECK(std::get<glm::dvec3>(g.value(g.plug("PtB.translate"))) == glm::dvec3(4.0, 5.0, 6.0));
    run("select", {{"args", {"PtA"}}});
    const std::string out = tempFile("lila_scene_points_out.pts");
    CHECK(run("exportFile", {{"args", {out}}}) == 1);
    std::string line;
    std::getline(std::ifstream(out), line);
    CHECK(line == "PtA 1 2 3");

    // Interpolator: a key naming it hands its segment over, and the name round-trips through the key json.
    scene::anim::Curve curve;
    curve.keys = {{.time = 0.0, .value = 0.0, .interpolator = "smoothStep"}, {.time = 1.0, .value = 10.0}};
    CHECK(curve.evaluate(0.25) == doctest::Approx(10.0 * 0.25 * 0.25 * 2.5));
    CHECK(scene::anim::Curve::keysFromJson(curve.keysToJson())[0].interpolator == "smoothStep");

    // IK solver: aims a two-joint chain along +x at a target on +y.
    const scene::ik::Solver* aim = scene::ik::findSolver("scenePointsAim");
    REQUIRE(aim);
    scene::ik::Problem p;
    p.world = {glm::dmat4(1.0), glm::translate(glm::dmat4(1.0), glm::dvec3(10.0, 0.0, 0.0))};
    p.target = {0.0, 5.0, 0.0};
    scene::ik::Solution s = aim->solve(p);
    REQUIRE(s.size() == 1);
    glm::dvec3 bone = s[0] * glm::dvec3(10.0, 0.0, 0.0);
    CHECK(bone.y == doctest::Approx(10.0));
    CHECK(bone.x == doctest::Approx(0.0).epsilon(1e-9));

    // Geometry node, data kind, MCP tool, link exporter.
    REQUIRE(gn::findDecl("ScenePointsGolden"));
    CHECK(*gn::declRegistry().ownerOf("ScenePointsGolden") == "scenePoints");
    scene::AttrDef dataAttr{.name = "note", .type = scene::PlugType::Data, .defaultValue = scene::DataPtr()};
    scene::DataPtr note = std::get<scene::DataPtr>(scene::fromJson(dataAttr, {{"kind", "scenePointsNote"}, {"text", "hi"}}));
    REQUIRE(note);
    CHECK(note->toJson()["text"] == "hi");
    note.reset();
    CHECK(mcp::Server::get().hasTool("scene_points_count"));
    CHECK(link::exporterOwner("scenePoints:pts") == "scenePoints");
    std::string err;
    CHECK(link::findExporter("scenePoints:pts")({}, tempFile("lila_scene_points_link.txt"), &err));

    // Scene ui section: loaded from the file's "ui", written back on save.
    json doc = io::serialize();
    doc["ui"]["scenePoints"] = {{"zoom", 2}};
    io::deserialize(doc);
    CHECK(io::serialize()["ui"]["scenePoints"]["zoom"] == 2);

    // Deformer subclassed in the extension, added through the generic deformer command.
    std::string cube = run("polyCube")[0];
    const double y0 = run("meshData", {{"args", {cube}}})["points"][0][1].get<double>();
    json made = run("deformer", {{"args", {cube}}, {"type", "liftDeformer"}, {"params", {{"amount", 2.0}}}});
    REQUIRE(made.size() == 1);
    CHECK(run("deformers", {{"args", {cube}}}) == made);
    CHECK(run("meshData", {{"args", {cube}}})["points"][0][1].get<double>() == doctest::Approx(y0 + 2.0));
    CHECK_THROWS_AS(run("deformer", {{"args", {cube}}, {"type", "polyCube"}}), CommandError);

    json described = em.describe("scenePoints");
    CHECK(described["registrations"].size() == 12);

    // Unloading needs the extension's node types gone from the scene.
    run("newScene");
    em.unload("scenePoints");
    CHECK_FALSE(em.isLoaded("scenePoints"));
    CHECK(registries() == before);
    CHECK_FALSE(io::translators::forPath(io::translators::Op::Import, "points.pts"));
    CHECK_FALSE(scene::anim::findInterpolator("smoothStep"));
    // A key naming an interpolator that is gone falls back to its tangents (auto: a smooth ease here too).
    CHECK(curve.evaluate(0.5) == doctest::Approx(5.0));
}

// Python can't start again after the python extension test shuts it down, so this runs as its own ctest.
TEST_CASE("scene extension points from Python: owned by the loading extension, gone when it unloads" * doctest::skip())
{
    isolate();
    REQUIRE(python::init({"lila_extensions_tests"}));
    io::translators::registerBuiltins();
    const json before = registries();

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "lila_py_scene_points";
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "pyscenepoints.py") << "from lila import extension\n"
                                               "state = {}\n"
                                               "def initialize_extension():\n"
                                               "    extension.register_file_translator('pyPoints', ['pyp'], label='Py points', import_='pyPointsImport')\n"
                                               "    extension.register_mcp_tool('py_points', lambda who='x': 'hi ' + who)\n"
                                               "    extension.register_link_exporter('pyPoints:txt', lambda ids, path: True)\n"
                                               "    extension.register_scene_ui_section('pyPoints', lambda: state.get('v'), lambda v: state.update(v=v))\n";
    extensions::ExtensionManager& em = extensions::ExtensionManager::get();
    em.addSearchPath(dir);
    REQUIRE(em.load("pyscenepoints") == "pyscenepoints");

    const io::translators::FileTranslator* t = io::translators::forPath(io::translators::Op::Import, "a.pyp");
    REQUIRE(t);
    CHECK(t->import.command == "pyPointsImport");
    CHECK(*io::translators::registry().ownerOf("pyPoints") == "pyscenepoints");
    CHECK(mcp::Server::get().hasTool("py_points"));
    CHECK(link::exporterOwner("pyPoints:txt") == "pyscenepoints");
    std::string err;
    CHECK(link::findExporter("pyPoints:txt")({1, 2}, "unused.txt", &err));
    CHECK(python::run("from lila import extension\nassert any(t['id'] == 'pyPoints' for t in extension.file_translators())").ok);

    json doc = io::serialize();
    doc["ui"]["pyPoints"] = {1, 2};
    io::deserialize(doc);
    CHECK(io::serialize()["ui"]["pyPoints"] == json::array({1, 2}));

    em.unload("pyscenepoints");
    CHECK_FALSE(mcp::Server::get().hasTool("py_points"));
    CHECK(registries() == before);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
