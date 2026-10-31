#include "Generator.h"
#include "TrimBake.h"
#include "Strips.h"
#include "TrimSheet.h"

#include "extensions/ExtensionManager.h"
#include "geo/Mesh.h"
#include "geo/Normals.h"
#include "ops/Command.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "uv/Hotspot.h"

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <random>
#include <set>

using namespace lila;
using namespace trims;
using json = nlohmann::json;

namespace
{
std::filesystem::path scratch(const std::string& name)
{
    static const std::filesystem::path root = std::filesystem::temp_directory_path() / ("lila_trim_" + std::to_string(std::random_device{}()));
    const std::filesystem::path p = root / name;
    std::filesystem::create_directories(p);
    return p;
}

/** A vertical quad w wide (along X) and h tall (along Y), facing +Z. */
geo::Mesh wall(double w, double h)
{
    const glm::dvec3 pts[] = {{0, 0, 0}, {w, 0, 0}, {w, h, 0}, {0, h, 0}};
    const geo::Index sizes[] = {4};
    const geo::Index idx[] = {0, 1, 2, 3};
    return geo::Mesh::fromPolygons(pts, sizes, idx);
}

glm::dvec2 uvAt(const geo::Mesh& m, glm::dvec3 p)
{
    std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>("uv0");
    for (geo::Index c = 0; c < m.cornerCapacity(); ++c)
        if (m.cornerValid(c) && glm::length(m.position(m.cornerPoint(c)) - p) < 1e-6)
            return glm::dvec2(uv[c]);
    FAIL("no corner at the point");
    return {};
}

void loadTrims()
{
    if (!ops::find("trimSheetCreate"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_TRIM_PACKAGE) == "TrimSheets");
}
} // namespace

TEST_CASE("trim: every preset fills its sheet without overlaps")
{
    for (const PresetInfo& p : presets())
    {
        const Sheet s = preset(p.id);
        CAPTURE(p.id);
        REQUIRE(!s.trims.empty());
        for (const std::string& w : validate(s))
            CHECK_MESSAGE(w.find("overlaps") == std::string::npos, w);
        long long area = 0;
        for (const Trim& t : s.trims)
            area += (long long)(t.sizePx().x) * t.sizePx().y;
        CHECK(area == (long long)(s.width) * s.height);
    }
}

TEST_CASE("trim: a sheet survives JSON, .rect, SVG and ID map round trips")
{
    const Sheet s = preset("sciFiHull", 1024);
    const Sheet j = Sheet::fromJson(s.toJson());
    REQUIRE(j.trims.size() == s.trims.size());
    CHECK(j.trims[5].profile.style == s.trims[5].profile.style);
    CHECK(j.trims[5].kind == s.trims[5].kind);

    const Sheet r = fromRect(toRect(s), s.width, s.height);
    REQUIRE(r.trims.size() == s.trims.size());
    for (size_t i = 0; i < s.trims.size(); ++i)
    {
        CHECK(r.trims[i].min == s.trims[i].min);
        CHECK(r.trims[i].max == s.trims[i].max);
    }
    CHECK(r.trims[0].kind == uv::TrimKind::StripU);

    const Sheet v = fromSvg(toSvg(s));
    CHECK(v.width == s.width);
    REQUIRE(v.trims.size() == s.trims.size());
    CHECK(v.trims[3].name == s.trims[3].name);

    const Sheet id = fromIdMap(idMap(s));
    CHECK(id.trims.size() == s.trims.size());
}

TEST_CASE("trim: uv rectangles and world sizes follow the pixel layout")
{
    Sheet s = stripLadder("T_Test", 1024, 1024, std::vector<int>{512, 256, 256});
    s.pxPerM = 512.0;
    const Trim& t = s.trims[1];
    CHECK(s.uvMin(t).y == doctest::Approx(0.25));
    CHECK(s.uvMax(t).y == doctest::Approx(0.5));
    CHECK(s.worldSize(t).y == doctest::Approx(50.0));
    CHECK(s.trimAt({0.3, 0.3}) == 1);
    // Tiling strips repeat along U.
    CHECK(s.trimAt({3.3, 0.3}) == 1);
}

TEST_CASE("trim: the generator is deterministic, bevels at the profile width and tiles strips")
{
    Sheet s = stripLadder("T_Gen", 512, 512, std::vector<int>{256, 128, 128});
    s.pxPerM = 512.0;
    s.trims[0].profile.bevelPx = 16.0;
    const Generated a = generate(s), b = generate(s);
    REQUIRE(a.high.numPoints() == b.high.numPoints());
    for (geo::Index p = 0; p < a.high.pointCapacity(); ++p)
        CHECK(a.high.position(p) == b.high.position(p));
    CHECK(a.above == doctest::Approx(16.0 * 100.0 / 512.0));
    // Strips run a sheet width past each side.
    const geo::Bounds bounds = a.high.bounds();
    CHECK(bounds.min.x == doctest::Approx(-100.0));
    CHECK(bounds.max.x == doctest::Approx(200.0));
    CHECK(a.low.numFaces() == 1);
    CHECK(uvAt(a.low, {0, 0, 0}) == glm::dvec2(0, 1));
    CHECK(glm::length(a.low.faceNormal(0) - glm::dvec3(0, 1, 0)) < 1e-9);

    // Every style builds.
    for (const std::string& name : styleNames())
    {
        Sheet one = stripLadder("T_Style", 256, 256, std::vector<int>{128, 128});
        one.trims[0].profile.style = *styleFromName(name);
        one.trims[1] = Trim::fromJson({{"name", "R"}, {"min", {0, 128}}, {"max", {128, 256}}, {"kind", "rect"}, {"profile", {{"style", name}}}});
        CAPTURE(name);
        CHECK(generate(one).high.numFaces() > 0);
    }
}

TEST_CASE("trim: the sheet bakes on the cpu, tiles across U and rebakes one trim")
{
    Sheet s = stripLadder("T_Bake", 128, 128, std::vector<int>{64, 32, 32});
    s.pxPerM = 128.0;
    s.supersampling = 1;
    s.maps = {"normal", "ao"};
    const std::filesystem::path file = scratch("bake") / "T_Bake.lilatrim";
    save(s, file);
    SheetBakeOptions o;
    o.backend = "cpu";
    const json r = bakeSheet(s, file, o);
    REQUIRE(r["outputs"].size() == 4);
    CHECK(r["outputs"][3]["map"] == "trimRect");
    CHECK(std::filesystem::exists(r["idMap"].get<std::string>()));
    CHECK(r["seamError"].get<double>() < 0.02);

    o.only = {1};
    const json again = bakeSheet(s, file, o);
    CHECK(again["composited"].size() == 2);
    CHECK(!std::filesystem::exists(textureFolder(s, file) / ".rebake"));
}

TEST_CASE("hotspot: a wall goes to the strip whose world height matches, up the strip")
{
    const Sheet s = preset("industrialMetal");
    const std::vector<uv::HotspotTrim> trims = hotspotTrims(s);
    // 256 px strips are 50 cm tall at 512 px per metre.
    geo::Mesh m = wall(400.0, 50.0);
    const uv::HotspotResult r = uv::hotspot(m, {}, trims, "uv0");
    REQUIRE(r.islands.size() == 1);
    REQUIRE(r.islands[0].trim >= 0);
    const Trim& t = s.trims[size_t(r.islands[0].trim)];
    CHECK(t.sizePx().y == 256);
    CHECK(t.kind == uv::TrimKind::StripU);
    const glm::dvec2 bottom = uvAt(m, {0, 0, 0}), top = uvAt(m, {0, 50, 0});
    CHECK(top.y > bottom.y);
    CHECK(top.y - bottom.y == doctest::Approx(s.uvMax(t).y - s.uvMin(t).y - 2.0 * t.insetPx / s.width).epsilon(0.01));
    CHECK(r.islands[0].sizeError < 0.1);

    // Same seed, same uvs; the next cycle picks another trim.
    geo::Mesh again = wall(400.0, 50.0);
    uv::hotspot(again, {}, trims, "uv0");
    CHECK(uvAt(again, {400, 50, 0}) == uvAt(m, {400, 50, 0}));
    std::set<int> cycled;
    for (int c = 1; c <= 3; ++c)
    {
        uv::HotspotOptions o;
        o.cycle = c;
        geo::Mesh other = wall(400.0, 50.0);
        cycled.insert(uv::hotspot(other, {}, trims, "uv0", o).islands[0].trim);
    }
    CHECK(cycled.size() == 3);
}

TEST_CASE("hotspot: a square panel takes a square hotspot of its size")
{
    const Sheet s = preset("hotspotGrid");
    const std::vector<uv::HotspotTrim> trims = hotspotTrims(s);
    // 256 px at 256 px per metre is a metre.
    geo::Mesh m = wall(100.0, 100.0);
    const uv::HotspotResult r = uv::hotspot(m, {}, trims, "uv0");
    REQUIRE(r.islands[0].trim >= 0);
    const Trim& t = s.trims[size_t(r.islands[0].trim)];
    CHECK(t.sizePx() == glm::ivec2(256, 256));
    const glm::dvec2 lo = uvAt(m, {0, 0, 0}), hi = uvAt(m, {100, 100, 0});
    CHECK(lo.x >= s.uvMin(t).x - 1e-9);
    CHECK(hi.x <= s.uvMax(t).x + 1e-9);
    CHECK(lo.y >= s.uvMin(t).y - 1e-9);
    CHECK(hi.y <= s.uvMax(t).y + 1e-9);
}

TEST_CASE("weighted normals: big faces stay flat beside a chamfer")
{
    // An L of two big faces joined by a thin 45 degree chamfer, all edges soft.
    const glm::dvec3 pts[] = {{0, 0, 0}, {100, 0, 0}, {100, 0, 99}, {0, 0, 99}, {100, 1, 100}, {0, 1, 100}, {100, 100, 100}, {0, 100, 100}};
    const geo::Index sizes[] = {4, 4, 4};
    const geo::Index idx[] = {3, 2, 1, 0, 5, 4, 2, 3, 7, 6, 4, 5};
    geo::Mesh m = geo::Mesh::fromPolygons(pts, sizes, idx);
    REQUIRE(m.numFaces() == 3);
    geo::applyWeightedNormals(m);
    std::span<const glm::vec3> n = std::as_const(m).cornerAttrs().get<glm::vec3>(geo::attr::kNormal);
    REQUIRE(!n.empty());
    for (geo::Index f : {geo::Index(0), geo::Index(2)})
    {
        const glm::dvec3 fn = m.faceNormal(f);
        m.forEachFaceCorner(f, [&](geo::Index c) { CHECK(glm::dot(glm::dvec3(n[c]), fn) == doctest::Approx(1.0).epsilon(1e-5)); });
    }
}

TEST_CASE("trim commands: create, inspect, edit and export through the package")
{
    loadTrims();
    const std::filesystem::path file = scratch("commands") / "T_Ladder.lilatrim";
    const json made = ops::execute("trimSheetCreate", {{"preset", "ladder"}, {"width", 256}, {"file", file.generic_string()}, {"bake", false}});
    CHECK(made["created"].get<bool>());
    CHECK(made["trims"].size() == 7);
    CHECK(ops::execute("trimSheetCurrent", json::object()).get<std::string>() == file.generic_string());

    ops::execute("trimSheetEdit", {{"trim", 0}, {"values", {{"profile", {{"style", "rivets"}}}}}});
    const json info = ops::execute("trimSheetInfo", json::object());
    CHECK(info["trims"][0]["profile"]["style"] == "rivets");

    const std::string rect = ops::execute("trimSheetExport", {{"format", "rect"}}).get<std::string>();
    CHECK(std::filesystem::exists(rect));
    const json imported = ops::execute("trimSheetImport", {{"args", {rect}}, {"width", 256}, {"height", 256}, {"file", (scratch("imported") / "T_In.lilatrim").generic_string()}});
    CHECK(imported["trims"].size() == 7);
}

TEST_CASE("trim: index and rectangle maps carry each trim's number and uv rectangle")
{
    const Sheet s = stripLadder("T_Maps", 64, 64, std::vector<int>{32, 16, 16});
    const lila::image::Image index = trimIndexMap(s), rect = trimRectMap(s);
    const auto texel = [&](const lila::image::Image& img, int x, int y) { return &img.rgba[(size_t(y) * size_t(img.width) + size_t(x)) * 4]; };
    CHECK(texel(index, 10, 5)[0] == 1.0f);
    CHECK(texel(index, 10, 40)[0] == 2.0f);
    CHECK(texel(index, 63, 63)[0] == 3.0f);
    const float* r = texel(rect, 3, 40);
    CHECK(r[0] == doctest::Approx(s.uvMin(s.trims[1]).x));
    CHECK(r[1] == doctest::Approx(s.uvMin(s.trims[1]).y));
    CHECK(r[3] == doctest::Approx(s.uvMax(s.trims[1]).y));
    CHECK(trimsMatching(s, json::array({2, s.trims[0].name})) == std::vector<size_t>{0, 2});
    CHECK_THROWS(trimsMatching(s, "nothing"));
}

TEST_CASE("trim layout: borders move for the whole strip, clamp, and trims split")
{
    Sheet s = stripLadder("T_Edit", 256, 256, std::vector<int>{128, 64, 64});
    s.mipLevel = 3;
    const std::vector<size_t> moved = moveBorder(s, 1, 128, 40, 144);
    CHECK(moved == std::vector<size_t>{0, 1});
    CHECK(s.trims[0].max.y == 144);
    CHECK(s.trims[1].min.y == 144);
    // Never thinner than a pixel: trim 1 now spans 144 to 192.
    moveBorder(s, 1, 144, 0, 400);
    CHECK(s.trims[1].min.y == 191);
    CHECK(moveBorder(s, 1, 77, 0, 80).empty());
    CHECK(snapToGrid(s, 101.0) == 104);

    REQUIRE(splitTrim(s, 0, 0, 96));
    REQUIRE(s.trims.size() == 4);
    CHECK(s.trims[0].max.x == 96);
    CHECK(s.trims[1].min.x == 96);
    CHECK(s.trims[1].name == s.trims[0].name + "_b");
    CHECK_FALSE(splitTrim(s, 0, 0, 200));
    // The two halves now share a vertical border that moves on its own.
    CHECK(moveBorder(s, 0, 96, 10, 128) == std::vector<size_t>{0, 1});
}

TEST_CASE("trim strips: along a line, tiling in U, capped, and over a mesh edge")
{
    Sheet s = stripLadder("T_Strip", 512, 512, std::vector<int>{256, 128, 64, 64});
    s.pxPerM = 512.0;
    s.trims[3].kind = uv::TrimKind::Unique;
    s.trims[3].min = {0, 448};
    s.trims[3].max = {64, 512};
    s.trims[3].tags = {"cap"};
    const double sheetCm = s.worldExtent().x;
    const std::vector<glm::dvec3> line = {{0, 0, 0}, {sheetCm, 0, 0}, {2.0 * sheetCm, 0, 0}};
    const std::vector<Station> st = stationsAlong(line, false, 25.0, StripOrient::Flat, 0.0);
    REQUIRE(st.size() == 3);
    CHECK(glm::length(st[0].normal - glm::dvec3(0, 1, 0)) < 1e-9);

    StripOptions o;
    o.trim = 1;
    geo::Mesh m = stripMesh(s, st, o);
    CHECK(m.numFaces() == 2);
    // U runs a sheet width per sheet width of length; V stays inside the trim.
    const glm::dvec2 end = uvAt(m, st[2].across[1]), start = uvAt(m, st[0].across[0]);
    CHECK(end.x - start.x == doctest::Approx(2.0));
    CHECK(start.y >= s.uvMin(s.trims[1]).y);
    CHECK(end.y <= s.uvMax(s.trims[1]).y);
    CHECK(m.faceAttrs().get<int32_t>(kFaceTrim)[0] == 2);

    REQUIRE(bestStripTrim(s, 25.0, "") == 1);
    o.cap = capTrim(s);
    REQUIRE(o.cap == 3);
    geo::Mesh capped = stripMesh(s, st, o);
    CHECK(capped.numFaces() == 4);
    const glm::dvec2 head = uvAt(capped, st[0].across[0]);
    CHECK(head.x == doctest::Approx(s.uvMin(s.trims[3]).x));

    // A box's top front edge: the decal wraps over it, its middle on the edge.
    const glm::dvec3 pts[] = {{0, 0, 0}, {10, 0, 0}, {10, 10, 0}, {0, 10, 0}, {0, 10, -10}, {10, 10, -10}};
    const geo::Index sizes[] = {4, 4};
    const geo::Index idx[] = {0, 1, 2, 3, 3, 2, 5, 4};
    const geo::Mesh box = geo::Mesh::fromPolygons(pts, sizes, idx);
    const geo::Index edge = box.findEdge(2, 3);
    REQUIRE(edge != geo::kInvalid);
    const std::vector<EdgeChain> chains = edgeChains(box, std::vector<geo::Index>{edge});
    REQUIRE(chains.size() == 1);
    const std::vector<Station> over = stationsOverEdges(box, chains[0], 2.0, 0.0);
    REQUIRE(over.size() == 2);
    for (const Station& x : over)
    {
        CHECK(x.across[1].y == doctest::Approx(10.0));
        CHECK(x.across[1].z == doctest::Approx(0.0));
        // One side down the front, the other back along the top.
        const bool frontFirst = x.across[0].y < 10.0 - 1e-6;
        CHECK((frontFirst ? x.across[2].z : x.across[0].z) == doctest::Approx(-1.0));
        CHECK((frontFirst ? x.across[0].y : x.across[2].y) == doctest::Approx(9.0));
    }

    // Trim Cut's slab is closed.
    StripOptions cut;
    cut.trim = 2;
    cut.cutDepth = 0.5;
    const geo::Mesh slab = stripMesh(s, over, cut);
    for (geo::Index e = 0; e < slab.edgeCapacity(); ++e)
        if (slab.edgeValid(e))
            CHECK(slab.edgeFaceCount(e) == 2);
}

TEST_CASE("trim commands: a sheet textured with a smart material on some trims")
{
    loadTrims();
    const std::filesystem::path file = scratch("texture") / "T_Tex.lilatrim";
    ops::execute("trimSheetCreate", {{"preset", "ladder"}, {"width", 64}, {"file", file.generic_string()}, {"bake", false}});
    ops::execute("trimSheetEdit", {{"values", {{"supersampling", 1}}}});
    const json r = ops::execute("trimSheetTexture", {{"material", "Trim Hull Paint"}, {"trims", json::array({0})}});
    CHECK(r["set"] == "TS_TrimLadder");
    CHECK(r["project"] == "T_TrimLadder");
    const json stack = ops::execute("texLayerStack", {{"args", {"TS_TrimLadder"}}});
    REQUIRE(stack["layers"].size() == 2);
    CHECK(stack["layers"][0]["name"] == "Sheet Bake");
    REQUIRE(stack["layers"][1]["masks"].size() == 1);
    CHECK(stack["layers"][1]["masks"][0]["type"] == "trimMask");

    const json made = ops::execute("texEvaluate", {{"args", {"TS_TrimLadder"}},
                                                   {"channels", json::array({"baseColor"})},
                                                   {"folder", (scratch("texture") / "eval").generic_string()},
                                                   {"backend", "cpu"}});
    REQUIRE(made["files"].size() == 1);
    const lila::image::Image img = lila::image::read(made["files"][0].get<std::string>());
    REQUIRE(img.width == 64);
    // Strip 0 is the top half (32 px of 64): hull paint there, the set's plain grey below.
    const auto at = [&](int x, int y) { return glm::vec3(img.rgba[(size_t(y) * 64 + size_t(x)) * 4], img.rgba[(size_t(y) * 64 + size_t(x)) * 4 + 1], 0.0f); };
    CHECK(glm::length(at(32, 10) - at(32, 50)) > 0.02f);
}

TEST_CASE("trim commands: along a curve, a decal over edges and a trim cut")
{
    loadTrims();
    const std::filesystem::path file = scratch("strips") / "T_Strips.lilatrim";
    ops::execute("trimSheetCreate", {{"preset", "industrialMetal"}, {"width", 256}, {"file", file.generic_string()}, {"bake", false}});
    scene::Graph& g = scene::Graph::current();
    auto meshOf = [&](const std::string& name) {
        scene::Node* n = g.find(name);
        REQUIRE(n);
        return std::get<scene::MeshPtr>(g.value({n->id(), scene::MeshNode::kOutMesh, -1}));
    };

    const std::string c = ops::execute("curve", {{"points", {{0, 0, 0}, {300, 0, 0}, {300, 0, 300}}}, {"type", "poly"}}).get<std::string>();
    const json along = ops::execute("trimAlongCurve", {{"args", {c}}, {"width", 12.5}});
    REQUIRE(along.size() == 1);
    CHECK(along[0]["length"].get<double>() == doctest::Approx(600.0));
    CHECK(along[0]["cap"].get<int>() >= 0);
    CHECK(meshOf(along[0]["mesh"].get<std::string>())->numFaces() >= 4);

    const json box = ops::execute("createMesh", {{"points", {{0, 0, 0}, {100, 0, 0}, {100, 100, 0}, {0, 100, 0}, {0, 0, -100}, {100, 0, -100}, {100, 100, -100}, {0, 100, -100}}},
                                                 {"faces", {{0, 1, 2, 3}, {5, 4, 7, 6}, {3, 2, 6, 7}, {1, 0, 4, 5}, {1, 5, 6, 2}, {4, 0, 3, 7}}},
                                                 {"name", "Block"}});
    const std::string block = box.is_string() ? box.get<std::string>() : box.at(0).get<std::string>();
    const geo::Index edge = meshOf(block)->findEdge(2, 3);
    REQUIRE(edge != geo::kInvalid);
    scene::components::setMode(scene::ComponentMode::Edge);
    scene::components::select(g.find(block)->id(), scene::ComponentMode::Edge, {edge});
    const json decal = ops::execute("trimDecal", {{"width", 4.0}});
    REQUIRE(decal.size() == 1);
    CHECK(meshOf(decal[0]["decal"].get<std::string>())->numFaces() == 2);

    const size_t before = meshOf(block)->numFaces();
    const json cut = ops::execute("trimDecal", {{"width", 4.0}, {"cut", true}, {"depth", 1.0}});
    REQUIRE(cut.size() == 1);
    CHECK(cut[0]["cut"].get<bool>());
    CHECK(meshOf(block)->numFaces() > before);
}

TEST_CASE("trim commands: the test blockout is auto trimmed onto the sheet")
{
    loadTrims();
    const std::filesystem::path file = scratch("test") / "T_Try.lilatrim";
    ops::execute("trimSheetCreate", {{"preset", "woodTrims"}, {"width", 256}, {"file", file.generic_string()}, {"bake", false}});
    const json r = ops::execute("trimSheetTest", json::object());
    scene::Graph& g = scene::Graph::current();
    scene::Node* n = g.find(r["mesh"].get<std::string>());
    REQUIRE(n);
    const scene::MeshPtr m = std::get<scene::MeshPtr>(g.value({n->id(), scene::MeshNode::kOutMesh, -1}));
    CHECK(m->numFaces() > 48);
    std::span<const int32_t> trim = m->faceAttrs().get<int32_t>(kFaceTrim);
    REQUIRE(!trim.empty());
    for (geo::Index f = 0; f < m->faceCapacity(); ++f)
        if (m->faceValid(f))
            CHECK(trim[f] > 0);
}

TEST_CASE("hotspot: a wall taller than every strip is cut into courses at true scale")
{
    const Sheet s = preset("woodTrims");
    const std::vector<uv::HotspotTrim> trims = hotspotTrims(s);
    double tallest = 0.0;
    for (const uv::HotspotTrim& t : trims)
        tallest = std::max(tallest, t.worldSize.y);
    REQUIRE(tallest == doctest::Approx(50.0));

    geo::Mesh m = wall(400.0, 250.0);
    const uv::HotspotResult r = uv::hotspot(m, {}, trims, "uv0");
    CHECK(r.sliced == 1);
    CHECK(m.numFaces() == 5);
    REQUIRE(r.islands.size() == 5);
    for (const uv::HotspotIsland& is : r.islands)
    {
        CHECK_FALSE(is.poor);
        CHECK(is.measure.height == doctest::Approx(50.0).epsilon(0.01));
        CHECK(is.sizeError < 0.1);
    }

    uv::HotspotOptions keep;
    keep.sliceOversized = false;
    geo::Mesh whole = wall(400.0, 250.0);
    const uv::HotspotResult w = uv::hotspot(whole, {}, trims, "uv0", keep);
    CHECK(w.sliced == 0);
    CHECK(whole.numFaces() == 1);
    CHECK(w.islands[0].poor);
}

TEST_CASE("hotspot: alt groups only swap trims of the same size")
{
    const Sheet s = preset("hotspotGrid");
    const std::vector<uv::HotspotTrim> trims = hotspotTrims(s);
    // A 40 x 300 cm pillar face: an 8:1 panel fits; the 50 cm squares in its row's group must never be picked.
    for (uint32_t seed = 0; seed < 12; ++seed)
    {
        uv::HotspotOptions o;
        o.seed = seed;
        geo::Mesh m = wall(40.0, 300.0);
        const uv::HotspotResult r = uv::hotspot(m, {}, trims, "uv0", o);
        REQUIRE(r.islands.size() == 1);
        const Trim& t = s.trims[size_t(r.islands[0].trim)];
        CAPTURE(seed);
        CHECK(std::max(t.sizePx().x, t.sizePx().y) >= 4 * std::min(t.sizePx().x, t.sizePx().y));
        CHECK_FALSE(r.islands[0].poor);
    }
}

TEST_CASE("trim: the sheet's own high meshes bake on top of the generated relief")
{
    Sheet s = stripLadder("T_Motif", 128, 128, std::vector<int>{64, 64});
    s.pxPerM = 128.0;
    s.supersampling = 1;
    s.maps = {"normal"};
    const std::filesystem::path file = scratch("motif") / "T_Motif.lilatrim";
    save(s, file);
    SheetBakeOptions o;
    o.backend = "cpu";
    bakeSheet(s, file, o);
    const lila::image::Image plain = lila::image::read(mapFile(s, file, "normal"));

    // A 20 cm stud on the first strip, in sheet space: x right, z down the texture, y up off it.
    geo::Mesh stud;
    {
        const glm::dvec3 lo{30.0, 0.0, 10.0}, hi{50.0, 20.0, 30.0};
        const glm::dvec3 pts[] = {{lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z},
                                  {lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z}};
        const geo::Index sizes[] = {4, 4, 4, 4, 4, 4};
        const geo::Index idx[] = {0, 1, 2, 3, 5, 4, 7, 6, 3, 2, 6, 7, 1, 0, 4, 5, 1, 5, 6, 2, 4, 0, 3, 7};
        stud = geo::Mesh::fromPolygons(pts, sizes, idx);
    }
    o.high = {stud};
    bakeSheet(s, file, o);
    const lila::image::Image withStud = lila::image::read(mapFile(s, file, "normal"));
    REQUIRE(withStud.width == plain.width);
    // The stud's sides tilt the normals around its outline (texels 38 to 64 at 128 px per metre); the rest stays.
    double inside = 0.0, outside = 0.0;
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x)
        {
            const size_t i = (size_t(y) * 128 + size_t(x)) * 4;
            const double d = std::abs(withStud.rgba[i] - plain.rgba[i]) + std::abs(withStud.rgba[i + 1] - plain.rgba[i + 1]);
            ((x >= 36 && x <= 66 && y >= 10 && y <= 42) ? inside : outside) += d;
        }
    CHECK(inside > 5.0);
    CHECK(outside < inside * 0.2);
}

TEST_CASE("trim commands: a face as tall as the strip stays upright on it")
{
    loadTrims();
    const std::filesystem::path file = scratch("upright") / "T_Up.lilatrim";
    ops::execute("trimSheetCreate", {{"preset", "ladder"}, {"width", 1024}, {"file", file.generic_string()}, {"bake", false}});
    // Strip256 is 50 cm tall at 512 px per metre; the face is 42 cm wide and 50 cm tall, like a wall band's end.
    const json made = ops::execute("createMesh", {{"points", {{0, 0, 0}, {42, 0, 0}, {42, 50, 0}, {0, 50, 0}}}, {"faces", {{0, 1, 2, 3}}}, {"name", "End"}});
    const std::string end = made.is_string() ? made.get<std::string>() : made.at(0).get<std::string>();
    scene::components::setMode(scene::ComponentMode::Object);
    ops::execute("select", {{"args", {end}}});
    ops::execute("trimApply", {{"trim", "Strip256"}});
    scene::Graph& g = scene::Graph::current();
    const geo::Mesh m = *std::get<scene::MeshPtr>(g.value({g.find(end)->id(), scene::MeshNode::kOutMesh, -1}));
    const glm::dvec2 bottom = uvAt(m, {0, 0, 0}), top = uvAt(m, {0, 50, 0}), right = uvAt(m, {42, 0, 0});
    CHECK(std::abs(top.x - bottom.x) < 1e-6);
    CHECK(top.y > bottom.y);
    CHECK(std::abs(right.x - bottom.x) == doctest::Approx(42.0 / 200.0).epsilon(0.05));
    // Centred on the strip, so a motif in the middle of the sheet lands in the middle of the face.
    CHECK((right.x + bottom.x) * 0.5 == doctest::Approx(0.5).epsilon(1e-3));
}

TEST_CASE("trim commands: faces line up with the strip's motifs")
{
    loadTrims();
    // A strip with a stud every 25 cm (four across the 100 cm sheet), baked from detail meshes.
    Sheet s = stripLadder("T_Studs", 128, 128, std::vector<int>{64, 64});
    s.pxPerM = 128.0;
    s.supersampling = 1;
    s.maps = {"normal"};
    const std::filesystem::path file = scratch("studs") / "T_Studs.lilatrim";
    save(s, file);
    SheetBakeOptions o;
    o.backend = "cpu";
    for (int k = 0; k < 4; ++k)
    {
        const double x = 12.5 + 25.0 * k;
        const glm::dvec3 lo{x - 5.0, 0.0, 20.0}, hi{x + 5.0, 12.0, 30.0};
        const glm::dvec3 pts[] = {{lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z},
                                  {lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z}};
        const geo::Index sizes[] = {4, 4, 4, 4, 4, 4};
        const geo::Index idx[] = {0, 1, 2, 3, 5, 4, 7, 6, 3, 2, 6, 7, 1, 0, 4, 5, 1, 5, 6, 2, 4, 0, 3, 7};
        o.high.push_back(geo::Mesh::fromPolygons(pts, sizes, idx));
    }
    bakeSheet(s, file, o);
    ops::execute("trimSheetCurrent", {{"args", {file.generic_string()}}});

    auto place = [&](const char* name, double width) {
        const json made = ops::execute("createMesh", {{"points", {{0, 0, 0}, {width, 0, 0}, {width, 50, 0}, {0, 50, 0}}}, {"faces", {{0, 1, 2, 3}}}, {"name", name}});
        const std::string n = made.is_string() ? made.get<std::string>() : made.at(0).get<std::string>();
        scene::components::setMode(scene::ComponentMode::Object);
        ops::execute("select", {{"args", {n}}});
        ops::execute("trimApply", {{"trim", 0}});
        scene::Graph& g = scene::Graph::current();
        const geo::Mesh m = *std::get<scene::MeshPtr>(g.value({g.find(n)->id(), scene::MeshNode::kOutMesh, -1}));
        const double a = uvAt(m, {0, 0, 0}).x, b = uvAt(m, {width, 0, 0}).x;
        return std::pair{std::min(a, b), std::max(a, b)};
    };
    // Studs are centred on u = 0.125 + k / 4. A 100 cm face holds four repeats, so its middle sits on a gap, u = k / 4.
    const auto [lo, hi] = place("Long", 100.0);
    CHECK(std::abs((lo + hi) * 0.5 * 4.0 - std::round((lo + hi) * 0.5 * 4.0)) < 0.05);
    // A 25 cm face (one repeat) is centred on a stud.
    const auto [elo, ehi] = place("End", 25.0);
    const double mid = (elo + ehi) * 0.5;
    CHECK(std::abs((mid - 0.125) * 4.0 - std::round((mid - 0.125) * 4.0)) < 0.05);
}
