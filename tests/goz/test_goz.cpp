#include <doctest/doctest.h>

#include "GoZFile.h"

#include "ops/Command.h"
#include "extensions/ExtensionManager.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>

using namespace lila;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }

void loadExtension()
{
    if (!ops::find("gozSend"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_GOZ_PACKAGE) == "GoZ");
}

std::string readBytes(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

void writeBytes(const fs::path& p, const std::string& s)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
}

/** A fake Public/Pixologic folder for LILA_GOZ_ROOT. */
fs::path fakeRoot()
{
    std::random_device rd;
    fs::path root = fs::temp_directory_path() / ("lila_goz_root_" + std::to_string(rd()));
    fs::create_directories(root / "GoZBrush");
#ifdef _WIN32
    _putenv_s("LILA_GOZ_ROOT", root.string().c_str());
#else
    setenv("LILA_GOZ_ROOT", root.string().c_str(), 1);
#endif
    return root;
}
} // namespace

TEST_CASE("a mesh goes out to GoZ and back with its UVs, polypaint, mask and polygroups")
{
    const std::vector<glm::dvec3> pts = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {2, 0, 0}, {3, 1, 0}, {2.5, 2, 0}, {1.5, 2, 0}, {2, 1.5, 0}};
    const std::vector<geo::Index> sizes = {4, 3, 5};
    const std::vector<geo::Index> idx = {0, 1, 2, 3, 1, 4, 2, 4, 5, 6, 7, 8};
    geo::Mesh m = geo::Mesh::fromPolygons(pts, sizes, idx);
    REQUIRE(m.numFaces() == 3);
    std::span<glm::vec2> uv = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
    for (geo::Index c = 0; c < m.cornerCapacity(); ++c)
        uv[c] = glm::vec2(m.position(m.cornerPoint(c)).x * 0.25, m.position(m.cornerPoint(c)).y * 0.5);
    std::span<float> mask = m.pointAttrs().getOrAdd<float>(goz::kMaskLayer);
    mask[2] = 1.0f;
    mask[5] = 0.5f;
    std::span<int32_t> groups = m.faceAttrs().getOrAdd<int32_t>(goz::kPolygroupLayer);
    groups[0] = 7;
    groups[1] = 8;
    groups[2] = 9;
    geo::colors::add(m, "colorSet", glm::vec4(0.2f, 0.5f, 1.0f, 1.0f));

    goz::File f = goz::read(goz::write(m, "Plate"));
    CHECK(f.name == "Plate");
    CHECK(f.mesh.numPoints() == 9);
    // The pentagon fans into three triangles.
    REQUIRE(f.mesh.numFaces() == 5);
    CHECK(f.mesh.facePoints(0) == std::vector<geo::Index>{0, 1, 2, 3});
    CHECK(f.mesh.facePoints(1) == std::vector<geo::Index>{1, 4, 2});
    CHECK(f.mesh.position(6).x == doctest::Approx(2.5));
    CHECK(f.mesh.position(6).y == doctest::Approx(2.0));

    std::span<const glm::vec2> uv2 = std::as_const(f.mesh).cornerAttrs().get<glm::vec2>(geo::attr::uvName(0));
    REQUIRE(!uv2.empty());
    for (geo::Index c = 0; c < f.mesh.cornerCapacity(); ++c)
    {
        CHECK(uv2[c].x == doctest::Approx(f.mesh.position(f.mesh.cornerPoint(c)).x * 0.25));
        CHECK(uv2[c].y == doctest::Approx(f.mesh.position(f.mesh.cornerPoint(c)).y * 0.5));
    }
    std::span<const float> mask2 = std::as_const(f.mesh).pointAttrs().get<float>(goz::kMaskLayer);
    REQUIRE(mask2.size() >= 9);
    CHECK(mask2[0] == doctest::Approx(0.0));
    CHECK(mask2[2] == doctest::Approx(1.0));
    CHECK(mask2[5] == doctest::Approx(0.5).epsilon(0.001));
    std::span<const int32_t> groups2 = std::as_const(f.mesh).faceAttrs().get<int32_t>(goz::kPolygroupLayer);
    REQUIRE(groups2.size() >= 5);
    CHECK(groups2[0] == 7);
    CHECK(groups2[1] == 8);
    CHECK(groups2[2] == 9);
    CHECK(groups2[4] == 9);
    REQUIRE(geo::colors::has(f.mesh, goz::kPolypaintSet));
    const std::vector<glm::vec4> paint = geo::colors::pointColors(f.mesh, goz::kPolypaintSet);
    CHECK(paint[3].r == doctest::Approx(0.2).epsilon(0.02));
    CHECK(paint[3].g == doctest::Approx(0.5).epsilon(0.02));
    CHECK(paint[3].b == doctest::Approx(1.0).epsilon(0.02));
}

TEST_CASE("a GoZ file ZBrush wrote reads")
{
    const fs::path star = "C:/Program Files/Maxon ZBrush 2026/ZResources/DXFStar.GoZ";
    if (!fs::exists(star))
        return;
    const std::string bytes = readBytes(star);
    goz::File f = goz::read(bytes);
    CHECK(f.name == "DXFStar");
    CHECK(f.mesh.numPoints() == 14);
    CHECK(f.mesh.numFaces() == 24);
}

TEST_CASE("broken GoZ files throw")
{
    CHECK_THROWS(goz::read(std::string("OBJ file")));
    std::string cut = goz::write(geo::Mesh::fromPolygons(std::vector<glm::dvec3>{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}},
                                                         std::vector<geo::Index>{3}, std::vector<geo::Index>{0, 1, 2}),
                                 "Tri");
    cut.resize(60);
    CHECK_THROWS(goz::read(cut));
}

TEST_CASE("meshes go to ZBrush and come back through the GoZ folders")
{
    loadExtension();
    const fs::path root = fakeRoot();
    run("newScene");
    const std::string box = run("polyCube", {{"width", 10.0}, {"height", 10.0}, {"depth", 10.0}, {"name", "Box"}})[0];
    run("move", {{"args", {box}}, {"by", {100, 0, 0}}});

    CHECK_THROWS(run("gozSend", {{"args", {box}}, {"launch", false}}));
    run("gozInstall");
    json files = run("gozSend", {{"args", {box}}, {"launch", false}});
    REQUIRE(files.size() == 1);
    const fs::path file = files[0].get<std::string>();
    REQUIRE(fs::exists(file));
    CHECK(fs::exists(root / "GoZApps" / "Lila" / "GoZ_Info.txt"));
    CHECK(fs::exists(root / "GoZApps" / "Lila" / "GoZBrushToLila.bat"));
    CHECK(readBytes(root / "GoZBrush" / "GoZ_Application.txt") == "Lila");
    CHECK(readBytes(root / "GoZBrush" / "GoZ_ObjectList.txt").find("Box-\n") != std::string::npos);

    // Sent in world space, in ZBrush's axes (Y down) and units (cm / 100).
    goz::File sent = goz::read(readBytes(file));
    double minX = 1e9, minY = 1e9;
    for (geo::Index p = 0; p < sent.mesh.pointCapacity(); ++p)
    {
        minX = std::min(minX, sent.mesh.position(p).x);
        minY = std::min(minY, sent.mesh.position(p).y);
    }
    CHECK(minX == doctest::Approx(0.95));
    CHECK(minY == doctest::Approx(-0.05));

    // Nothing from ZBrush yet: the list is still the one Lila wrote.
    CHECK_THROWS(run("gozReceive"));
    const auto touchList = [&] { writeBytes(root / "GoZBrush" / "GoZ_ObjectList.txt", readBytes(root / "GoZBrush" / "GoZ_ObjectList.txt") + "\n"); };

    SUBCASE("the same topology moves the points of the mesh it came from")
    {
        for (geo::Index p = 0; p < sent.mesh.pointCapacity(); ++p)
            sent.mesh.setPosition(p, sent.mesh.position(p) + glm::dvec3(0, -0.2, 0));
        writeBytes(file, goz::write(sent.mesh, "Box"));
        touchList();
        json got = run("gozReceive");
        CHECK(got["updated"].size() == 1);
        CHECK(got["created"].empty());
        json info = run("meshInfo", {{"args", {box}}});
        CHECK(info["boundsMin"][1].get<double>() == doctest::Approx(15.0));
        CHECK(info["boundsMin"][0].get<double>() == doctest::Approx(-5.0));
        run("undo");
        CHECK(run("meshInfo", {{"args", {box}}})["boundsMin"][1].get<double>() == doctest::Approx(-5.0));
    }

    SUBCASE("new topology replaces the mesh, and new subtools come in as new meshes")
    {
        const geo::Mesh tri = geo::Mesh::fromPolygons(std::vector<glm::dvec3>{{1.0, 0, 0}, {1.1, 0, 0}, {1.0, -0.1, 0}}, std::vector<geo::Index>{3},
                                                      std::vector<geo::Index>{0, 1, 2});
        writeBytes(file, goz::write(tri, "Box"));
        const fs::path extra = file.parent_path() / "Extra.GoZ";
        writeBytes(extra, goz::write(tri, "Extra"));
        std::string list = readBytes(root / "GoZBrush" / "GoZ_ObjectList.txt");
        list += (file.parent_path() / "Extra").generic_string() + "\n";
        writeBytes(root / "GoZBrush" / "GoZ_ObjectList.txt", list);
        json got = run("gozReceive");
        CHECK(got["updated"].size() == 1);
        REQUIRE(got["created"].size() == 1);
        json data = run("meshData", {{"args", {box}}, {"base", true}});
        CHECK(data["points"].size() == 3);
        CHECK(data["points"][0][0].get<double>() == doctest::Approx(0.0));
        CHECK(run("meshInfo", {{"args", {got["created"][0]}}})["boundsMin"][0].get<double>() == doctest::Approx(100.0));
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("GoZ files import and export through File > Import and Export")
{
    loadExtension();
    run("newScene");
    const std::string box = run("polyCube", {{"name", "Crate"}})[0];
    const fs::path path = fs::temp_directory_path() / "lila_goz_test.GoZ";
    run("exportFile", {{"args", {path.string()}}});
    REQUIRE(fs::exists(path));
    json got = run("importFile", {{"args", {path.string()}}});
    json a = run("meshInfo", {{"args", {box}}});
    json b = run("meshInfo", {{"args", {got[0]}}});
    CHECK(a["points"] == b["points"]);
    CHECK(a["faces"] == b["faces"]);
    fs::remove(path);
}

TEST_CASE("names that start each other go to ZBrush under names that don't")
{
    loadExtension();
    const fs::path root = fakeRoot();
    run("newScene");
    const std::string dress = run("polyCube", {{"name", "Dress"}})[0];
    const std::string pocket = run("polyCube", {{"name", "DressPocket"}})[0];
    run("move", {{"args", {pocket}}, {"by", {0, 50, 0}}});
    run("gozInstall");
    json files = run("gozSend", {{"args", {dress, pocket}}, {"launch", false}});
    REQUIRE(files.size() == 2);
    CHECK(fs::path(files[0].get<std::string>()).stem() == "Dress-");
    CHECK(fs::path(files[1].get<std::string>()).stem() == "DressPocket-");

    // ZBrush sends both back: each goes into its own mesh.
    writeBytes(root / "GoZBrush" / "GoZ_ObjectList.txt", readBytes(root / "GoZBrush" / "GoZ_ObjectList.txt") + "\n");
    json got = run("gozReceive");
    CHECK(got["updated"].size() == 2);
    CHECK(got["created"].empty());
    CHECK(run("meshInfo", {{"args", {pocket}}})["worldBoundsMin"][1].get<double>() == doctest::Approx(45.0));
    CHECK(run("meshInfo", {{"args", {dress}}})["worldBoundsMin"][1].get<double>() == doctest::Approx(-5.0));
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("a mesh keeps its ZBrush name when renamed, and new subtools get new names")
{
    loadExtension();
    const fs::path root = fakeRoot();
    run("newScene");
    std::string head = run("polyCube", {{"name", "Head"}})[0];
    run("gozInstall");
    CHECK(fs::path(run("gozSend", {{"args", {head}}, {"launch", false}})[0].get<std::string>()).stem() == "Head-");
    head = run("rename", {{"args", {head, "Face"}}}).get<std::string>();
    CHECK(fs::path(run("gozSend", {{"args", {head}}, {"launch", false}})[0].get<std::string>()).stem() == "Head-");

    // A new subtool: a name no subtool has, which the mesh then keeps.
    CHECK(fs::path(run("gozSend", {{"args", {head}}, {"launch", false}, {"newSubtools", true}})[0].get<std::string>()).stem() == "Face-");
    CHECK(fs::path(run("gozSend", {{"args", {head}}, {"launch", false}, {"newSubtools", true}})[0].get<std::string>()).stem() == "Face_2-");
    CHECK(fs::path(run("gozSend", {{"args", {head}}, {"launch", false}})[0].get<std::string>()).stem() == "Face_2-");

    // ZBrush sends it back under that name: it goes into the renamed mesh.
    writeBytes(root / "GoZBrush" / "GoZ_ObjectList.txt", readBytes(root / "GoZBrush" / "GoZ_ObjectList.txt") + "\n");
    json got = run("gozReceive");
    CHECK(got["updated"].size() == 1);
    CHECK(got["created"].empty());
    std::error_code ec;
    fs::remove_all(root, ec);
}
