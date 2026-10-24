#include <doctest/doctest.h>

#include "core/MainQueue.h"
#include "fbx/Fbx.h"
#include "geo/ColorSets.h"
#include "geo/Mesh.h"
#include "geo/ObjIO.h"
#include "geo/Primitives.h"
#include "math/Unreal.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"
#include "seq/Alembic.h"
#include "seq/MeshSequence.h"
#include "seq/Vat.h"

#include <filesystem>
#include <fstream>
#include <set>

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

namespace
{
glm::dvec3 ue(const glm::dvec3& v)
{
    math::Vector u = math::toUnreal(math::Vector(v.x, v.y, v.z));
    return {u.x, u.y, u.z};
}

glm::dvec3 texel(const seq::VatImage& img, int col, int row)
{
    const float* p = &img.rgba[(size_t(row) * size_t(img.width) + size_t(col)) * 4];
    return {p[0], p[1], p[2]};
}

/** Vertex i of a frame through the layout the metadata describes. */
glm::dvec3 vertexTexel(const seq::VatImage& img, const json& meta, int frame, int i)
{
    const int w = meta["width"], rpf = meta["rowsPerFrame"];
    return texel(img, i % w, frame * rpf + i / w);
}

/** The rest mesh as Unreal-facing data: each point's Lila position and the texture column its VAT UV addresses. */
std::vector<std::pair<glm::dvec3, int>> restColumns(const fs::path& fbxFile, const json& meta)
{
    ops::execute("newScene", json::object());
    scene::Graph& g = scene::Graph::current();
    fbx::ImportResult r = fbx::importFbx(g, fbxFile);
    REQUIRE(r.ok);
    std::vector<std::pair<glm::dvec3, int>> out;
    const int w = meta["width"], h = meta["height"];
    for (scene::NodeId id : r.nodes)
    {
        scene::Node* n = g.node(id);
        if (!n->type().isA("mesh"))
            continue;
        g.evaluate();
        const geo::Mesh& m = *n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
        std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>(geo::attr::uvName(meta["uvChannel"].get<int>()));
        REQUIRE(uv.size() == m.cornerCapacity());
        for (geo::Index c = 0; c < m.cornerCapacity(); ++c)
        {
            if (!m.cornerValid(c))
                continue;
            // Unreal flips V on import, so its V is 1 - the file's.
            const int col = int(std::floor(uv[c].x * w));
            const int row = int(std::floor((1.0f - uv[c].y) * h));
            out.push_back({m.position(m.cornerPoint(c)), row * w + col});
        }
    }
    return out;
}
} // namespace

TEST_CASE("exportVertexAnimation bakes offsets and normals that decode back to the animation")
{
    setup();
    const std::string box = animatedBox();
    fs::path dir = tempDir("vat");

    json r = ops::execute("exportVertexAnimation", {{"args", {box}}, {"folder", dir.generic_string()}});
    const json& meta = r["meta"];
    CHECK(meta["mode"] == "constant");
    CHECK(meta["frameCount"] == 3);
    CHECK(meta["vertexCount"] == 8);
    CHECK(meta["width"] == 8);
    CHECK(meta["rowsPerFrame"] == 1);
    CHECK(meta["height"] == 3);
    CHECK(meta["uvChannel"] == 1);
    CHECK(meta["fps"].get<double>() == doctest::Approx(30.0));
    for (const char* k : {"mesh", "positions", "normals", "meta"})
        CHECK(fs::exists(r["files"][k].get<std::string>()));
    CHECK(json::parse(std::ifstream(r["files"]["meta"].get<std::string>())) == meta);
    // The material inputs, by the names a VAT material uses.
    const json& mp = r["materialParams"];
    CHECK(mp == meta["materialParams"]);
    CHECK(mp["HDR"] == 1);
    CHECK(mp["FrameCount"] == 3);
    CHECK(mp["RowsPerFrame"] == 1);
    CHECK(mp["TextureHeight"] == 3);
    CHECK(mp["PositionTexture"] == fs::path(r["files"]["positions"].get<std::string>()).filename().string());
    CHECK(mp["NormalTexture"] == fs::path(r["files"]["normals"].get<std::string>()).filename().string());
    CHECK(mp["BoundsMax"][2].get<double>() == doctest::Approx(200.0));
    CHECK(seq::vatMaterialSummary(mp).find("HDR=1 BoundsMin=(0, 0, 0) BoundsMax=(0, 0, 200) FrameCount=3") == 0);

    std::string err;
    seq::VatImage pos = seq::readVatImage(r["files"]["positions"].get<std::string>(), &err);
    INFO(err);
    REQUIRE(pos.width == 8);
    REQUIRE(pos.height == 3);
    for (int f = 0; f < 3; ++f)
    {
        const glm::dvec3 expect = ue(glm::dvec3(0.0, 100.0 * f, 0.0));
        for (int i = 0; i < 8; ++i)
        {
            const glm::dvec3 got = vertexTexel(pos, meta, f, i);
            CHECK(glm::length(got - expect) < 0.1);
            CHECK(pos.rgba[(size_t(f) * 8 + size_t(i)) * 4 + 3] == 1.0f);
        }
    }
    // Box corner normals average to the diagonals.
    seq::VatImage nrm = seq::readVatImage(r["files"]["normals"].get<std::string>());
    REQUIRE(nrm.width == 8);
    for (int i = 0; i < 8; ++i)
    {
        const glm::dvec3 n = vertexTexel(nrm, meta, 1, i) * 2.0 - 1.0;
        CHECK(glm::length(n) == doctest::Approx(1.0).epsilon(0.02));
        CHECK(std::abs(n.x) == doctest::Approx(1.0 / std::sqrt(3.0)).epsilon(0.03));
    }

    // Absolute positions read through the rest mesh's UV land on its own vertices.
    json abs = ops::execute("exportVertexAnimation", {{"args", {box}}, {"folder", dir.generic_string()}, {"name", "abs"}, {"positions", "absolute"}});
    seq::VatImage absPos = seq::readVatImage(abs["files"]["positions"].get<std::string>());
    std::vector<std::pair<glm::dvec3, int>> cols = restColumns(abs["files"]["mesh"].get<std::string>(), abs["meta"]);
    // Triangulated on import: 12 triangles.
    REQUIRE(cols.size() == 36);
    for (const auto& [p, col] : cols)
    {
        REQUIRE(col >= 0);
        REQUIRE(col < 8);
        CHECK(glm::length(texel(absPos, col, 0) - ue(p)) < 0.05);
        CHECK(glm::length(texel(absPos, col, 2) - ue(p + glm::dvec3(0, 200, 0))) < 0.15);
    }

    // PNG positions are normalised by the bounds in the metadata.
    const std::string box2 = animatedBox();
    json png = ops::execute("exportVertexAnimation", {{"args", {box2}}, {"folder", dir.generic_string()}, {"name", "png"}, {"format", "png16"},
                                                      {"normalBits", 16}, {"positions", "absolute"}});
    CHECK(fs::path(png["files"]["positions"].get<std::string>()).extension() == ".png");
    seq::VatImage p16 = seq::readVatImage(png["files"]["positions"].get<std::string>());
    REQUIRE(p16.width == 8);
    const json& pm = png["meta"];
    const json& bp = pm["materialParams"];
    CHECK(bp["HDR"] == 0);
    glm::dvec3 lo(bp["BoundsMin"][0], bp["BoundsMin"][1], bp["BoundsMin"][2]), hi(bp["BoundsMax"][0], bp["BoundsMax"][1], bp["BoundsMax"][2]);
    CHECK(glm::length(lo - glm::dvec3(pm["bounds"]["min"][0], pm["bounds"]["min"][1], pm["bounds"]["min"][2])) < 1e-9);
    // Frame 3 is the top of the animation: every corner's up coordinate decodes to the box top plus 200.
    double topUe = -1e9;
    for (int i = 0; i < 8; ++i)
        topUe = std::max(topUe, (lo + vertexTexel(p16, pm, 2, i) * (hi - lo)).z);
    CHECK(topUe == doctest::Approx(250.0).epsilon(1e-3));

    // 8-bit positions decode within one step (1/255 of the range) of the 16-bit ones.
    json png8 = ops::execute("exportVertexAnimation", {{"args", {box2}}, {"folder", dir.generic_string()}, {"name", "png8"}, {"format", "png8"},
                                                       {"positions", "absolute"}});
    CHECK(png8["meta"]["positionBits"] == 8);
    CHECK(png8["materialParams"]["HDR"] == 0);
    CHECK(png8["materialParams"]["BoundsMin"] == bp["BoundsMin"]);
    seq::VatImage p8 = seq::readVatImage(png8["files"]["positions"].get<std::string>());
    REQUIRE(p8.width == 8);
    for (int f = 0; f < 3; ++f)
        for (int i = 0; i < 8; ++i)
        {
            const glm::dvec3 a = lo + vertexTexel(p8, pm, f, i) * (hi - lo), b = lo + vertexTexel(p16, pm, f, i) * (hi - lo);
            for (int c = 0; c < 3; ++c)
                CHECK(std::abs(a[c] - b[c]) <= (hi[c] - lo[c]) / 255.0 + 1e-6);
        }
    // "png" still means png16.
    json legacyPng = ops::execute("exportVertexAnimation", {{"args", {box2}}, {"folder", dir.generic_string()}, {"name", "legacy"}, {"format", "png"}});
    CHECK(legacyPng["meta"]["format"] == "png16");

    // Dry run plans the files only.
    json dry = ops::execute("exportVertexAnimation", {{"args", {box2}}, {"folder", (dir / "dry").generic_string()}, {"dryRun", true}});
    CHECK(dry["files"]["mesh"].get<std::string>().ends_with(box2 + ".fbx"));
    CHECK_FALSE(fs::exists(dir / "dry"));

    CHECK_THROWS_AS(ops::execute("exportVertexAnimation", {{"args", {box2}}, {"format", "tga"}}), CommandError);
    fs::remove_all(dir);
}

TEST_CASE("exportVertexAnimation wraps wide frames onto several rows")
{
    setup();
    const std::string box = animatedBox();
    fs::path dir = tempDir("vatwrap");
    json r = ops::execute("exportVertexAnimation", {{"args", {box}}, {"folder", dir.generic_string()}, {"maxWidth", 3}, {"positions", "absolute"}});
    const json& meta = r["meta"];
    CHECK(meta["width"] == 3);
    CHECK(meta["rowsPerFrame"] == 3);
    CHECK(meta["height"] == 9);
    seq::VatImage pos = seq::readVatImage(r["files"]["positions"].get<std::string>());
    REQUIRE(pos.height == 9);
    // The last row of each frame has one texel unused.
    CHECK(pos.rgba[(size_t(2) * 3 + 2) * 4 + 3] == 0.0f);
    std::vector<std::pair<glm::dvec3, int>> cols = restColumns(r["files"]["mesh"].get<std::string>(), meta);
    REQUIRE_FALSE(cols.empty());
    std::set<int> seen;
    for (const auto& [p, index] : cols)
    {
        // index is row * width + column within frame 0's rows.
        const int col = index % 3, row = index / 3;
        REQUIRE(row < 3);
        seen.insert(index);
        CHECK(glm::length(texel(pos, col, row) - ue(p)) < 0.05);
        CHECK(glm::length(texel(pos, col, 2 * 3 + row) - ue(p + glm::dvec3(0, 200, 0))) < 0.15);
    }
    CHECK(seen.size() == 8);
    fs::remove_all(dir);
}

TEST_CASE("exportVertexAnimation bakes changing topology as a padded triangle soup")
{
    setup();
    ops::execute("newScene", json::object());
    ops::execute("playbackOptions", {{"fps", 30.0}, {"minTime", 1.0 / 30.0}, {"maxTime", 3.0 / 30.0}});
    json made = ops::execute("polyPlane", {{"name", "Grow"}});
    const std::string plane = made.is_array() ? made[0].get<std::string>() : made.get<std::string>();
    scene::Graph& g = scene::Graph::current();
    std::string gen = plane;
    if (g.find(plane)->type().attrIndex("subdivisionsX") < 0)
        for (const json& m : made)
            if (g.find(m.get<std::string>())->type().attrIndex("subdivisionsX") >= 0)
                gen = m.get<std::string>();
    for (auto [frame, value] : {std::pair{1.0, 1.0}, {2.0, 2.0}, {3.0, 3.0}})
        ops::execute("setKeyframe", {{"args", {gen + ".subdivisionsX"}}, {"time", frame / 30.0}, {"value", value}});
    fs::path dir = tempDir("vatsoup");

    CHECK_THROWS_WITH_AS(ops::execute("exportVertexAnimation", {{"args", {plane}}, {"folder", dir.generic_string()}, {"mode", "constant"}}),
                         doctest::Contains("frame 2"), CommandError);

    json r = ops::execute("exportVertexAnimation", {{"args", {plane}}, {"folder", dir.generic_string()}, {"positions", "absolute"}});
    const json& meta = r["meta"];
    CHECK(meta["mode"] == "soup");
    CHECK(meta["triangles"] == 6);
    CHECK(meta["vertexCount"] == 18);
    seq::VatImage pos = seq::readVatImage(r["files"]["positions"].get<std::string>());
    REQUIRE(pos.width == 18);
    auto area = [&](int frame, int tri) {
        const glm::dvec3 a = vertexTexel(pos, meta, frame, tri * 3), b = vertexTexel(pos, meta, frame, tri * 3 + 1),
                         c = vertexTexel(pos, meta, frame, tri * 3 + 2);
        return glm::length(glm::cross(b - a, c - a)) * 0.5;
    };
    // Frame 1 has 2 triangles, so 4 slots collapse; frame 3 fills all 6. The real ones cover the 100 x 100 plane.
    double total1 = 0.0, total3 = 0.0;
    for (int t = 0; t < 6; ++t)
    {
        CHECK((t < 2) == (area(0, t) > 1.0));
        CHECK(area(2, t) > 1.0);
        total1 += area(0, t);
        total3 += area(2, t);
    }
    CHECK(total1 == doctest::Approx(10000.0).epsilon(1e-3));
    CHECK(total3 == doctest::Approx(10000.0).epsilon(1e-3));
    // The rest mesh is the full soup: 6 triangles, 18 unwelded points, each addressing its own column.
    std::vector<std::pair<glm::dvec3, int>> cols = restColumns(r["files"]["mesh"].get<std::string>(), meta);
    CHECK(cols.size() == 18);
    std::set<int> seen;
    for (const auto& c : cols)
        seen.insert(c.second);
    CHECK(seen.size() == 18);
    fs::remove_all(dir);
}
