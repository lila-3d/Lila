#include <doctest/doctest.h>

#include "core/MainQueue.h"
#include "geo/ColorSets.h"
#include "geo/Mesh.h"
#include "geo/ObjIO.h"
#include "geo/Primitives.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"
#include "seq/Alembic.h"
#include "seq/MeshSequence.h"

#include <filesystem>

using namespace lila;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
void setup()
{
    static bool once = [] {
        mainq::init();
        ops::registerBuiltins();
        scene::types::registerBuiltins();
        scene::registerCommands();
        seq::registerCommands();
        return true;
    }();
    (void)once;
}

fs::path tempDir(const char* name)
{
    fs::path d = fs::temp_directory_path() / ("lila_seq_" + std::string(name));
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

/** A 100 cm cube named Box whose translateY goes 0, 100, 200 over frames 1 to 3 (linear keys at 30 fps). */
std::string animatedBox(bool colors = false)
{
    ops::execute("newScene", json::object());
    ops::execute("playbackOptions", {{"fps", 30.0}, {"minTime", 1.0 / 30.0}, {"maxTime", 3.0 / 30.0}});
    scene::Graph& g = scene::Graph::current();
    geo::Mesh cube = geo::makeCube();
    if (colors)
        geo::colors::add(cube, "colorSet1", glm::vec4(1.0f, 0.5f, 0.25f, 1.0f));
    scene::Node* n = g.createNode(*scene::types::find("mesh"), "Box");
    g.setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(cube))));
    const std::string name = n->name();
    for (auto [frame, value] : {std::pair{1.0, 0.0}, {3.0, 200.0}})
        ops::execute("setKeyframe", {{"args", {name + ".ty"}}, {"time", frame / 30.0}, {"value", value}, {"inTangentType", "linear"},
                                     {"outTangentType", "linear"}});
    return name;
}
} // namespace

TEST_CASE("frame naming: index from 0 or frame number, zero padded")
{
    CHECK(seq::frameName("{name}_{index}", "Spike", 0, 32.0, 3) == "Spike_000");
    CHECK(seq::frameName("{name}_{index}", "Spike", 26, 58.0, 3) == "Spike_026");
    CHECK(seq::frameName("{name}_{frame}", "Spike", 0, 32.0, 3) == "Spike_032");
    CHECK(seq::frameName("{name}.{frame}", "Spike", 0, 7.0, 4) == "Spike.0007");
    CHECK(seq::frameName("f{frame}", "x", 0, -3.0, 2) == "f-03");
    CHECK(seq::frameName("f{frame}", "x", 0, 2.5, 3) == "f2_500");
    CHECK(seq::frameName("{index}", "x", 1234, 0.0, 2) == "1234");

    seq::SequenceOptions o;
    o.start = 32;
    o.end = 58;
    CHECK(seq::frames(o).size() == 27);
    o.step = 2;
    std::vector<double> f = seq::frames(o);
    REQUIRE(f.size() == 14);
    CHECK(f.back() == doctest::Approx(58.0));
    o.step = 0;
    CHECK_THROWS(seq::frames(o));
    o.step = 1;
    o.end = 10;
    CHECK_THROWS(seq::frames(o));
}

TEST_CASE("exportMeshSequence writes a file per frame with the scale and the animation baked")
{
    setup();
    const std::string box = animatedBox();
    fs::path dir = tempDir("obj");

    json files = ops::execute("exportMeshSequence", {{"args", {box}}, {"format", "obj"}, {"folder", dir.generic_string()}, {"scale", 0.1},
                                                     {"profile", "generic"}, {"pattern", "spike_{index}"}});
    REQUIRE(files.size() == 3);
    CHECK(fs::path(files[0].get<std::string>()).filename() == "spike_000.obj");
    CHECK(fs::path(files[2].get<std::string>()).filename() == "spike_002.obj");
    for (int i = 0; i < 3; ++i)
    {
        geo::ObjReadResult r = geo::readObjFile(files[size_t(i)].get<std::string>());
        REQUIRE(r.ok());
        REQUIRE(r.objects.size() == 1);
        geo::Bounds b = r.objects[0].mesh.bounds();
        CHECK(b.size().x == doctest::Approx(10.0));
        CHECK(b.center().y == doctest::Approx(10.0 * i));
        // Triangulated by default.
        CHECK(r.objects[0].mesh.numFaces() == 12);
    }
    CHECK(ops::execute("currentTime", json::object()).get<double>() == doctest::Approx(0.0));

    // Frame numbers in the name, an explicit range, recentring every frame, object space untouched by scale 1.
    json more = ops::execute("exportMeshSequence", {{"args", {box}}, {"format", "obj"}, {"folder", dir.generic_string()}, {"start", 2},
                                                    {"end", 3}, {"profile", "generic"}, {"pattern", "{name}_{frame}"}, {"padding", 4},
                                                    {"recenter", "each"}, {"triangulate", false}});
    REQUIRE(more.size() == 2);
    CHECK(fs::path(more[0].get<std::string>()).filename() == box + "_0002.obj");
    geo::ObjReadResult r = geo::readObjFile(more[1].get<std::string>());
    REQUIRE(r.ok());
    CHECK(glm::length(r.objects[0].mesh.bounds().center()) == doctest::Approx(0.0));
    CHECK(r.objects[0].mesh.numFaces() == 6);

    // The legacy single path form still works: {frame} padded to 4.
    json legacy = ops::execute("exportMeshSequence", {{"args", {box}}, {"start", 1}, {"end", 1}, {"path", (dir / "bob_{frame}.obj").generic_string()}});
    REQUIRE(legacy.size() == 1);
    CHECK(legacy[0].get<std::string>().ends_with("bob_0001.obj"));

    // A dry run names the files and writes nothing.
    json dry = ops::execute("exportMeshSequence", {{"args", {box}}, {"folder", (dir / "dry").generic_string()}, {"dryRun", true}});
    CHECK(dry.size() == 3);
    CHECK_FALSE(fs::exists(dir / "dry"));

    CHECK_THROWS_AS(ops::execute("exportMeshSequence", {{"args", {box}}, {"format", "usd"}}), CommandError);
    CHECK_THROWS_AS(ops::execute("exportMeshSequence", {{"args", {"nobody"}}}), CommandError);
    fs::remove_all(dir);
}

TEST_CASE("exportMeshSequence writes FBX frames")
{
    setup();
    const std::string box = animatedBox();
    fs::path dir = tempDir("fbx");
    json files = ops::execute("exportMeshSequence", {{"args", {box}}, {"folder", dir.generic_string()}, {"normals", false}});
    REQUIRE(files.size() == 3);
    for (const json& f : files)
    {
        CHECK(f.get<std::string>().ends_with(".fbx"));
        CHECK(fs::file_size(f.get<std::string>()) > 0);
    }
    fs::remove_all(dir);
}

TEST_CASE("Alembic round trip: a sample per frame with points, faces, normals, UVs and colors")
{
    setup();
    const std::string box = animatedBox(true);
    fs::path dir = tempDir("abc");
    fs::path file = dir / "box.abc";

    json written = ops::execute("exportAlembic", {{"args", {box}}, {"path", file.generic_string()}, {"scale", 0.1}, {"profile", "generic"}});
    CHECK(fs::path(written.get<std::string>()) == fs::path(file.generic_string()));
    REQUIRE(fs::exists(file));

    std::string err;
    std::vector<seq::abc::ObjectInfo> objects = seq::abc::read(file, &err);
    INFO(err);
    REQUIRE(objects.size() == 1);
    const seq::abc::ObjectInfo& o = objects[0];
    CHECK(o.path == "/" + box + "/" + box + "Shape");
    CHECK(o.samples == 3);
    CHECK(o.points == std::vector<size_t>{8, 8, 8});
    CHECK(o.faces == std::vector<size_t>{12, 12, 12});
    REQUIRE(o.times.size() == 3);
    CHECK(o.times[0] == doctest::Approx(1.0 / 30.0));
    CHECK(o.times[2] == doctest::Approx(3.0 / 30.0));
    CHECK(o.normals);
    CHECK(o.uvs);
    CHECK(o.colors);
    REQUIRE(o.firstPositions.size() == 8);
    for (const glm::vec3& p : o.firstPositions)
        CHECK(std::abs(p.x) == doctest::Approx(5.0));

    // exportMeshSequence format=abc writes the same single file.
    json viaSeq = ops::execute("exportMeshSequence", {{"args", {box}}, {"format", "abc"}, {"folder", dir.generic_string()}, {"colors", false}});
    REQUIRE(viaSeq.size() == 1);
    CHECK(fs::path(viaSeq[0].get<std::string>()).filename() == box + ".abc");
    std::vector<seq::abc::ObjectInfo> again = seq::abc::read(viaSeq[0].get<std::string>());
    REQUIRE(again.size() == 1);
    CHECK(again[0].samples == 3);
    CHECK_FALSE(again[0].colors);

    CHECK(seq::abc::read(dir / "missing.abc").empty());
    fs::remove_all(dir);
}

TEST_CASE("the Alembic writer allows topology to change between frames")
{
    fs::path dir = tempDir("topo");
    seq::abc::Writer w;
    seq::abc::WriteOptions wo;
    wo.fps = 24.0;
    w.open(dir / "grow.abc", wo);
    geo::Mesh small = geo::makeCube();
    geo::CubeOptions co;
    co.subdivX = 2;
    geo::Mesh big = geo::makeCube(co);
    w.addFrame({"Grow"}, {&small});
    w.addFrame({"Grow"}, {&big});
    // An object that turns up on the last frame starts there.
    w.addFrame({"Grow", "Late"}, {&small, &small});
    w.close();

    std::vector<seq::abc::ObjectInfo> objects = seq::abc::read(dir / "grow.abc");
    REQUIRE(objects.size() == 2);
    CHECK(objects[0].samples == 3);
    CHECK(objects[0].points[0] == 8);
    CHECK(objects[0].points[1] > 8);
    CHECK(objects[1].samples == 1);
    CHECK(objects[1].times[0] == doctest::Approx(2.0 / 24.0));
    fs::remove_all(dir);
}
