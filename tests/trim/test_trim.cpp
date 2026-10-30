#include "Generator.h"
#include "TrimBake.h"
#include "TrimSheet.h"

#include "extensions/ExtensionManager.h"
#include "geo/Mesh.h"
#include "geo/Normals.h"
#include "ops/Command.h"
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
    REQUIRE(r["outputs"].size() == 2);
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
