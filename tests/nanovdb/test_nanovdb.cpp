#include <doctest/doctest.h>

#include "ops/Command.h"
#include "extensions/ExtensionManager.h"
#include "gn/NodeLib.h"

#include <filesystem>

using namespace lila;
using json = nlohmann::json;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }
json args(std::initializer_list<json> a) { return {{"args", json(a)}}; }
} // namespace

TEST_CASE("a geometry-nodes volume goes out to NanoVDB and back")
{
    if (!gn::findDecl("ReadNanoVDB"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_NANOVDB_PACKAGE) == "NanoVDB");
    run("newScene");
    const std::string box = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}, {"name", "Box"}})[0];
    run("gnCreateGroup", args({"Sdf"}));
    std::string sdf = run("gnAddNode", args({"Sdf", "MeshToSDF"})).get<std::string>();
    run("gnDisconnect", args({"Sdf", "GroupOutput.geometry"}));
    run("gnConnect", args({"Sdf", "GroupInput.geometry", sdf + ".mesh"}));
    run("gnConnect", args({"Sdf", sdf + ".volume", "GroupOutput.geometry"}));
    run("gnApply", args({box, "Sdf"}));
    json sheet = run("gnSpreadsheet", {{"args", {box}}, {"component", "volume"}, {"domain", "grid"}});
    REQUIRE(sheet["rows"] == 1);
    double lo = 0.0;
    for (const json& c : sheet["columns"])
        if (c["name"] == "min")
            lo = c["values"][0].get<double>();
    CHECK(lo < 0.0);

    auto file = std::filesystem::temp_directory_path() / "lila_nanovdb_test.nvdb";
    std::string written = run("exportNanoVDB", args({box, file.string()})).get<std::string>();
    REQUIRE(std::filesystem::exists(written));

    const std::string other = run("polyCube", {{"name", "Reader"}})[0];
    run("gnCreateGroup", args({"Read"}));
    std::string read = run("gnAddNode", args({"Read", "ReadNanoVDB"})).get<std::string>();
    run("gnSetValue", args({"Read", read + ".path", written}));
    std::string mesh = run("gnAddNode", args({"Read", "VolumeToMesh"})).get<std::string>();
    run("gnDisconnect", args({"Read", "GroupOutput.geometry"}));
    run("gnConnect", args({"Read", read + ".volume", mesh + ".volume"}));
    run("gnConnect", args({"Read", mesh + ".mesh", "GroupOutput.geometry"}));
    run("gnApply", args({other, "Read"}));
    json info = run("meshInfo", args({other}));
    CHECK(info["boundsMax"][0].get<double>() == doctest::Approx(50.0).epsilon(0.05));
    CHECK(info["boundsMin"][1].get<double>() == doctest::Approx(-50.0).epsilon(0.05));
    std::filesystem::remove(written);
}

TEST_CASE("a geometry-nodes volume goes out to OpenVDB and back")
{
    if (!gn::findDecl("ReadVDB"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_NANOVDB_PACKAGE) == "NanoVDB");
    run("newScene");
    const std::string box = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}, {"name", "Box"}})[0];
    run("gnCreateGroup", args({"Sdf"}));
    std::string sdf = run("gnAddNode", args({"Sdf", "MeshToSDF"})).get<std::string>();
    run("gnDisconnect", args({"Sdf", "GroupOutput.geometry"}));
    run("gnConnect", args({"Sdf", "GroupInput.geometry", sdf + ".mesh"}));
    run("gnConnect", args({"Sdf", sdf + ".volume", "GroupOutput.geometry"}));
    run("gnApply", args({box, "Sdf"}));
    auto file = std::filesystem::temp_directory_path() / "lila_vdb_test.vdb";
    std::string written = run("exportVDB", args({box, file.string()})).get<std::string>();
    REQUIRE(std::filesystem::exists(written));

    const std::string other = run("polyCube", {{"name", "Reader"}})[0];
    run("gnCreateGroup", args({"Read"}));
    std::string read = run("gnAddNode", args({"Read", "ReadVDB"})).get<std::string>();
    run("gnSetValue", args({"Read", read + ".path", written}));
    std::string mesh = run("gnAddNode", args({"Read", "VolumeToMesh"})).get<std::string>();
    run("gnDisconnect", args({"Read", "GroupOutput.geometry"}));
    run("gnConnect", args({"Read", read + ".volume", mesh + ".volume"}));
    run("gnConnect", args({"Read", mesh + ".mesh", "GroupOutput.geometry"}));
    run("gnApply", args({other, "Read"}));
    json info = run("meshInfo", args({other}));
    CHECK(info["boundsMax"][0].get<double>() == doctest::Approx(50.0).epsilon(0.05));
    CHECK(info["boundsMin"][2].get<double>() == doctest::Approx(-50.0).epsilon(0.05));
    std::filesystem::remove(written);
}
