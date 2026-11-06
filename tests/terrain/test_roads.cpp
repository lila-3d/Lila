#include "TestUtil.h"
#include "Roads.h"
#include "Stamps.h"
#include "Terrain.h"

#include "geo/Heightfield.h"
#include "math/MathCore.h"

#include <doctest/doctest.h>
#include <tbb/task_arena.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>

using namespace lila;
using namespace terraintest;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
geo::Heightfield grid(double size, int resolution, const std::function<double(double, double)>& f)
{
    geo::Heightfield h = geo::makeHeightfieldCovering({0.0, 0.0}, {size, size}, resolution);
    for (int z = 0; z < h.nz; ++z)
        for (int x = 0; x < h.nx; ++x)
        {
            const glm::dvec2 p = h.samplePos(x, z);
            h.heights[h.index(x, z)] = float(f(p.x, p.y));
        }
    return h;
}

terrain::RoadInput roadAlong(const std::string& id, const std::string& kind, std::vector<glm::dvec2> path)
{
    terrain::RoadInput in;
    in.road = terrain::Road::fromJson({{"id", id}, {"kind", kind}});
    in.road.path = path;
    in.line = std::move(path);
    return in;
}

/** Every sample on the road's formation (not in a span or junction): its height and the design surface there. */
struct OnRoad
{
    double worstLevel = 0.0;
    double worstAbove = -1e30;
    size_t samples = 0;
};

OnRoad checkFormation(const geo::Heightfield& h, const terrain::RoadResult& r)
{
    OnRoad out;
    const std::vector<float>* surface = h.findMask(terrain::kRoadVergeMask);
    const std::vector<float>* paved = h.findMask(terrain::kRoadSurfaceMask);
    for (int z = 0; z < h.nz; ++z)
        for (int x = 0; x < h.nx; ++x)
        {
            const size_t i = h.index(x, z);
            if ((*surface)[i] <= 0.0f && (*paved)[i] <= 0.0f)
                continue;
            const terrain::RoadResult::Surface q = r.surfaceAt(h.samplePos(x, z));
            if (q.overshoot > 0.0 || std::abs(q.lateral) > r.formationHalf || r.span[q.station] != 0)
                continue;
            const double d = h.heights[i] - q.height;
            out.worstLevel = std::max(out.worstLevel, std::abs(d));
            out.worstAbove = std::max(out.worstAbove, d);
            ++out.samples;
        }
    return out;
}
} // namespace

TEST_CASE("roads: the longitudinal profile keeps max grade and min vertical radius over bumps, and passes through keys")
{
    std::vector<double> s, g;
    for (int i = 0; i <= 2000; ++i)
    {
        s.push_back(i * 100.0);
        g.push_back(4000.0 * std::sin(i * 100.0 / 30000.0) + 80.0 * std::sin(i * 100.0 / 900.0) + 1500.0 * std::cos(i * 100.0 / 11000.0));
    }
    const double G = 0.06, R = 80000.0;
    double achieved = 0.0;
    std::vector<std::string> warnings;
    const std::vector<double> y = terrain::solveRoadProfile(s, g, {{1000, 2500.0}}, G, R, 2000.0, 30.0, &achieved, &warnings);
    REQUIRE(y.size() == s.size());
    double worstGrade = 0.0, worstCurvature = 0.0, gap = 0.0;
    for (size_t i = 0; i + 1 < y.size(); ++i)
        worstGrade = std::max(worstGrade, std::abs(y[i + 1] - y[i]) / 100.0);
    for (size_t i = 1; i + 1 < y.size(); ++i)
        worstCurvature = std::max(worstCurvature, std::abs(y[i + 1] - 2.0 * y[i] + y[i - 1]) / (100.0 * 100.0));
    for (size_t i = 0; i < y.size(); ++i)
        gap = std::max(gap, std::abs(y[i] - g[i]));
    CHECK(worstGrade <= G + 1e-9);
    CHECK(worstCurvature <= 1.0 / R * 1.0001 + 1e-12);
    CHECK(achieved >= R * 0.999);
    CHECK(std::abs(y[1000] - 2500.0) < 1.0);
    // Smooth over the short bumps rather than following them.
    CHECK(gap > 60.0);
    CHECK(warnings.empty());
}

TEST_CASE("roads: over hills a road holds its grade and bank, lies level across its width with no ground above it, and masks itself")
{
    geo::Heightfield h = grid(200000.0, 513, [](double x, double z) {
        return 3000.0 * std::sin(x / 20000.0) + 2000.0 * std::cos(z / 15000.0) + 300.0 * std::sin(x / 1500.0) * std::sin(z / 1700.0);
    });
    terrain::RoadInput in = roadAlong("hillRoad", "road", {{-80000.0, -40000.0}, {-20000.0, 10000.0}, {30000.0, -20000.0}, {80000.0, 30000.0}});
    const auto results = terrain::applyRoads(h, {in});
    const terrain::RoadResult& r = results.at("hillRoad");
    REQUIRE_FALSE(r.empty());
    const terrain::RoadDesign& de = in.road.design;

    CHECK(r.maxGrade <= de.maxGrade + 1e-6);
    double worstBank = 0.0, bentBank = 0.0, worstRate = 0.0, worstCurvature = 0.0;
    const double rate = de.bankRunoff * 0.01 / in.road.profile.pavedHalf();
    for (size_t i = 0; i < r.centreline.size(); ++i)
    {
        worstBank = std::max(worstBank, std::abs(r.bank[i]));
        bentBank = std::max(bentBank, std::abs(r.curvature[i]) > 0.0 ? std::abs(r.bank[i]) : 0.0);
        if (i > 0)
            worstRate = std::max(worstRate, std::abs(r.bank[i] - r.bank[i - 1]) / (r.along[i] - r.along[i - 1]));
        if (i > 0 && i + 1 < r.centreline.size())
        {
            const double ds = r.along[i] - r.along[i - 1];
            worstCurvature = std::max(worstCurvature, std::abs(r.centreline[i + 1].y - 2.0 * r.centreline[i].y + r.centreline[i - 1].y) / (ds * ds));
        }
    }
    CHECK(worstBank <= de.maxBank * 0.01 + 1e-9);
    CHECK(bentBank > de.maxBank * 0.01 * 0.5);
    CHECK(worstRate <= rate * 1.0001);
    CHECK(worstCurvature <= 1.0 / de.minVerticalRadius * 1.001 + 1e-9);
    MESSAGE("length ", r.length, " maxGrade ", r.maxGrade, " maxBank ", r.maxBank, " vertical radius ", r.minVerticalRadius, " spans ", r.spans.size());

    const OnRoad on = checkFormation(h, r);
    CHECK(on.samples > 500);
    CHECK(on.worstLevel < 0.01);
    CHECK(on.worstAbove < 0.01);

    // Between samples too: the bilinear ground across the width stays on the banked plane.
    double worstBetween = 0.0;
    for (size_t i = 10; i + 10 < r.centreline.size(); i += 7)
    {
        if (r.span[i] != 0)
            continue;
        const glm::dvec2 c(r.centreline[i].x, r.centreline[i].z), left(-r.tangents[i].y, r.tangents[i].x);
        for (double d = -(r.formationHalf - h.cell * 1.5); d <= r.formationHalf - h.cell * 1.5; d += 37.0)
        {
            const glm::dvec2 p = c + left * d;
            const terrain::RoadResult::Surface q = r.surfaceAt(p);
            if (r.span[q.station] == 0)
                worstBetween = std::max(worstBetween, std::abs(h.heightAt(p.x, p.y) - q.height));
        }
    }
    CHECK(worstBetween < 3.0);

    // Masks: paved on the carriageway, verge at the edge, dirt beyond, wear in the wheel paths, erosion protected.
    const size_t mid = r.centreline.size() / 2;
    const glm::dvec2 c(r.centreline[mid].x, r.centreline[mid].z), left(-r.tangents[mid].y, r.tangents[mid].x);
    const terrain::RoadProfile& pr = in.road.profile;
    CHECK(h.maskAt(terrain::kRoadSurfaceMask, c.x, c.y) > 0.95);
    CHECK(h.maskAt("paved", c.x, c.y) > 0.95);
    const glm::dvec2 vergeAt = c + left * (0.5 * (pr.pavedHalf() + pr.formationHalf()));
    CHECK(h.maskAt(terrain::kRoadVergeMask, vergeAt.x, vergeAt.y) > 0.2);
    CHECK(h.maskAt(terrain::kRoadSurfaceMask, vergeAt.x, vergeAt.y) < 0.5);
    const glm::dvec2 dirtAt = c + left * (pr.formationHalf() + 30.0);
    CHECK(h.maskAt(terrain::kRoadDirtMask, dirtAt.x, dirtAt.y) > 0.0);
    CHECK(h.maskAt(terrain::kErosionProtectMask, c.x, c.y) == doctest::Approx(1.0));
    double wear = 0.0;
    for (double d = -pr.carriageway() * 0.5; d <= pr.carriageway() * 0.5; d += 10.0)
        wear = std::max(wear, h.maskAt(terrain::kRoadWearMask, (c + left * d).x, (c + left * d).y));
    CHECK(wear > 0.3);
    const glm::dvec2 far = c + left * 20000.0;
    CHECK(h.maskAt(terrain::kRoadSurfaceMask, far.x, far.y) == doctest::Approx(0.0));
}

TEST_CASE("roads: cuttings and embankments slope at their talus angles; taller ones become retaining walls")
{
    // A side slope rising 40% toward +Z, the road along X across it: cut uphill, fill downhill.
    auto side = [](double, double z) { return 0.4 * z; };
    for (const bool walls : {false, true})
    {
        CAPTURE(walls);
        geo::Heightfield h = grid(60000.0, 301, side);
        const geo::Heightfield before = h;
        terrain::RoadInput in = roadAlong("contour", "road", {{-25000.0, 0.0}, {25000.0, 0.0}});
        in.road.design.wallHeight = walls ? 200.0 : 5000.0;
        const terrain::RoadResult r = terrain::applyRoads(h, {in}).at("contour");
        REQUIRE_FALSE(r.empty());
        const double fh = r.formationHalf, ditch = in.road.profile.ditchWidth;
        const double tanCut = std::tan(in.road.design.cutAngle * 3.14159265358979 / 180.0);
        const double tanFill = std::tan(in.road.design.fillAngle * 3.14159265358979 / 180.0);
        int cutWalls = 0, fillWalls = 0;
        for (const terrain::RoadResult::Wall& w : r.walls)
            (w.cut ? cutWalls : fillWalls)++;
        double worstCut = 0.0, worstFill = 0.0, moved = 0.0;
        size_t slopeSamples = 0;
        for (int x = 100; x <= 200; x += 10)
            for (int z = 1; z < h.nz; ++z)
            {
                const glm::dvec2 p = h.samplePos(x, z), q = h.samplePos(x, z - 1);
                // Both samples past the formation edge and the ditch, on the same side.
                const bool cutSide = q.y > fh + ditch, fillSide = p.y < -fh;
                if (!cutSide && !fillSide)
                    continue;
                const size_t a = h.index(x, z), b = h.index(x, z - 1);
                const bool changed = h.heights[a] != before.heights[a] || h.heights[b] != before.heights[b];
                moved = std::max(moved, std::abs(double(h.heights[a]) - before.heights[a]));
                if (!changed)
                    continue;
                ++slopeSamples;
                const double grad = std::abs(double(h.heights[a]) - h.heights[b]) / h.cell;
                (cutSide ? worstCut : worstFill) = std::max(cutSide ? worstCut : worstFill, grad);
            }
        if (!walls)
        {
            CHECK(r.walls.empty());
            CHECK(slopeSamples > 20);
            CHECK(worstCut <= tanCut + 1e-3);
            CHECK(worstFill <= tanFill + 1e-3);
            CHECK(r.cut > 0.0);
            CHECK(r.fill > 0.0);
        }
        else
        {
            CHECK(cutWalls >= 1);
            CHECK(fillWalls >= 1);
            // Beyond a wall the ground is left as it was.
            CHECK(slopeSamples == 0);
            for (const terrain::RoadResult::Wall& w : r.walls)
                CHECK(w.points.front().top - w.points.front().bottom > 200.0);
        }
    }
}

TEST_CASE("roads: a deep valley becomes a bridge with pillars and a ridge a tunnel with portals, both left as ground")
{
    auto land = [](double x, double) {
        return 2000.0 - 4000.0 * std::exp(-std::pow((x - 30000.0) / 8000.0, 2.0)) + 5000.0 * std::exp(-std::pow((x + 40000.0) / 6000.0, 2.0));
    };
    geo::Heightfield h = grid(200000.0, 401, land);
    const geo::Heightfield before = h;
    terrain::RoadInput in = roadAlong("valley", "road", {{-90000.0, 0.0}, {90000.0, 0.0}});
    in.road.design.maxGrade = 2.0;
    const terrain::RoadResult r = terrain::applyRoads(h, {in}).at("valley");
    REQUIRE_FALSE(r.empty());
    REQUIRE(r.bridges.size() == 1);
    REQUIRE(r.tunnels.size() == 1);
    const terrain::RoadResult::Bridge& b = r.bridges[0];
    CHECK(b.maxHeight > in.road.design.bridgeHeight);
    REQUIRE(b.pillars.size() >= 2);
    for (size_t k = 1; k < b.pillars.size(); ++k)
        CHECK(glm::length(b.pillars[k].top - b.pillars[k - 1].top) <= in.road.design.pillarSpacing * 1.05);
    for (const terrain::RoadResult::Pillar& p : b.pillars)
        CHECK(p.top.y > p.ground);
    const terrain::RoadResult::Tunnel& t = r.tunnels[0];
    CHECK(t.radius > r.formationHalf);
    CHECK(t.length > 1000.0);
    CHECK(glm::dot(t.portals[0].direction, glm::dvec2(1.0, 0.0)) > 0.9);
    CHECK(glm::dot(t.portals[1].direction, glm::dvec2(-1.0, 0.0)) > 0.9);
    CHECK(t.axis.front().y > t.floor.front().y);
    // The ridge stays over the tunnel; the valley under the bridge is not filled.
    CHECK(h.heightAt(-40000.0, 0.0) == doctest::Approx(before.heightAt(-40000.0, 0.0)));
    CHECK(h.heightAt(30000.0, 0.0) <= before.heightAt(30000.0, 0.0) + 1e-3);
    CHECK(r.maxGrade <= 2.0 + 1e-6);
}

TEST_CASE("roads: a side road joins an earlier one at grade, with its cross fall and kerb returns, leaving the main road alone")
{
    geo::Heightfield h = grid(120000.0, 241, [](double x, double z) { return 0.03 * x + 0.01 * z; });
    terrain::RoadInput main = roadAlong("main", "road", {{-50000.0, 0.0}, {50000.0, 0.0}});
    terrain::RoadInput side = roadAlong("side", "road", {{0.0, -40000.0}, {0.0, -700.0}});
    const auto results = terrain::applyRoads(h, {main, side});
    const terrain::RoadResult& a = results.at("main");
    const terrain::RoadResult& b = results.at("side");
    for (const glm::dvec3& p : b.centreline)
        REQUIRE(std::isfinite(p.y));
    REQUIRE(b.junctions.size() == 1);
    CHECK(b.junctions[0].other == "main");
    CHECK(b.junctions[0].type == "T");
    CHECK(b.kerbReturns.size() == 2);
    for (const terrain::RoadResult::KerbReturn& k : b.kerbReturns)
        CHECK(k.arc.size() >= 2);
    // Where the side road leaves the main road's edge, both surfaces agree across the side road's width.
    size_t edge = 0;
    for (size_t i = 0; i < b.span.size(); ++i)
        if (b.span[i] == 3)
        {
            edge = i;
            break;
        }
    REQUIRE(edge > 0);
    const glm::dvec2 c(b.centreline[edge].x, b.centreline[edge].z), left(-b.tangents[edge].y, b.tangents[edge].x);
    for (double d : {-b.pavedHalf, 0.0, b.pavedHalf})
    {
        const glm::dvec2 p = c + left * d;
        CHECK(std::abs(b.surfaceAt(p).height - a.surfaceAt(p).height) < 1.0);
    }
    const OnRoad on = checkFormation(h, a);
    CHECK(on.worstLevel < 0.01);
}

TEST_CASE("roads: commands lay, list, route, build, export and protect roads on a terrain")
{
    loadTerrain();
    run("newScene");
    json t = run("terrainCreate", {{"size", {200000.0, 200000.0}}, {"resolution", 257}, {"amplitude", 3000.0}, {"seed", 5}, {"name", "Roads"}});
    const std::string mesh = t["mesh"];

    json r = run("terrainAddRoad", {{"args", {mesh}}, {"id", "pass"}, {"path", {{-80000.0, -30000.0}, {0.0, 20000.0}, {70000.0, -10000.0}}}, {"maxGrade", 6.0}});
    CHECK(r["applied"].get<bool>());
    CHECK(r["maxGrade"].get<double>() <= 6.0 + 1e-6);
    CHECK(r["maxBank"].get<double>() <= 6.0 + 1e-6);
    CHECK(r["length"].get<double>() > 100000.0);
    CHECK(r["centreline"].size() >= 2);
    CHECK(r.contains("revision"));

    json listed = run("terrainRoads", {{"args", {mesh}}});
    REQUIRE(listed["layers"]["Roads"].size() == 1);
    CHECK(listed["layers"]["Roads"][0]["id"] == "pass");

    json route = run("terrainRoute", {{"args", {mesh}}, {"start", {-60000.0, 60000.0}}, {"end", {60000.0, 70000.0}}, {"maxGrade", 10.0}, {"road", "track"}, {"roadId", "farm"}});
    REQUIRE(route.contains("road"));
    CHECK(route["road"]["applied"].get<bool>());
    CHECK(route["road"]["kind"] == "track");
    CHECK(route["road"]["maxGrade"].get<double>() <= 10.0 + 1e-6);

    // The terrain under the centreline is the road's surface.
    const json& line = r["centreline"];
    const json& mid = line[line.size() / 2];
    const double ground = heightAt(t, mid[0].get<double>(), mid[2].get<double>());
    CHECK(std::abs(ground - mid[1].get<double>()) < 1.0);

    const fs::path dir = fs::temp_directory_path() / "lila_roads_test";
    fs::remove_all(dir);
    json ex = run("terrainRoadExport", {{"args", {mesh}}, {"folder", dir.string()}, {"name", "Roads"}});
    CHECK(ex["roads"] == 2);
    REQUIRE(fs::exists(ex["file"].get<std::string>()));
    json doc = json::parse(std::ifstream(ex["file"].get<std::string>()));
    REQUIRE(doc["roads"].size() == 2);
    CHECK(doc["roads"][0]["points"].size() > 10);
    CHECK(doc["roads"][0]["points"][0].contains("unreal"));
    CHECK(doc["roads"][0]["points"][0].contains("bank"));
    json files = run("terrainExport", {{"args", {mesh}}, {"target", "files"}, {"folder", dir.string()}, {"name", "Roads"}});
    CHECK(files["files"].contains("roads"));

    // The built surface: its paved edges sit on the banked design surface (the sweep tilts the same way the road banks).
    json built = run("terrainRoadBuild", {{"args", {mesh}}, {"ids", {"pass"}}});
    REQUIRE(built.size() == 1);
    const std::string surface = built[0]["surface"];
    json data = run("meshData", {{"args", {surface}}, {"worldSpace", true}});
    REQUIRE(data["points"].size() > 100);
    json detail = run("terrainRoads", {{"args", {mesh}}, {"detail", true}});
    const json& pass = detail["layers"]["Roads"][0];
    size_t banked = 0;
    double worstEdge = 0.0;
    for (size_t i = 0; i < pass["centreline"].size(); i += 25)
    {
        const double bank = pass["bank"][i].get<double>() * 0.01;
        if (std::abs(bank) < 0.02)
            continue;
        const json& c = pass["centreline"][i];
        const json& n = pass["centreline"][std::min(i + 1, pass["centreline"].size() - 1)];
        const glm::dvec2 tan = glm::normalize(glm::dvec2(n[0].get<double>() - c[0].get<double>(), n[2].get<double>() - c[2].get<double>()));
        const glm::dvec2 left(-tan.y, tan.x);
        const double half = pass["pavedWidth"].get<double>() * 0.5;
        const glm::dvec2 at = glm::dvec2(c[0].get<double>(), c[2].get<double>()) + left * half;
        double best = 1e30, height = 0.0;
        for (const json& p : data["points"])
        {
            const double d = glm::length(glm::dvec2(p[0].get<double>(), p[2].get<double>()) - at);
            if (d < best)
                best = d, height = p[1].get<double>();
        }
        if (best > 300.0)
            continue;
        ++banked;
        worstEdge = std::max(worstEdge, std::abs(height - (c[1].get<double>() - bank * half)));
    }
    CHECK(banked > 0);
    CHECK(worstEdge < 15.0);

    // Laid before erosion, a road keeps its ground through it (spans excepted: they leave the ground alone).
    json t2 = run("terrainCreate", {{"size", {100000.0, 100000.0}}, {"resolution", 129}, {"amplitude", 5000.0}, {"seed", 9}, {"name", "Eroded"}});
    json early = run("terrainAddRoad", {{"args", {t2["mesh"]}}, {"id", "old"}, {"stage", "base"}, {"path", {{-40000.0, 0.0}, {40000.0, 5000.0}}}});
    const json& el = early["centreline"];
    json probe = json::array();
    for (size_t i = el.size() / 20; i + el.size() / 20 < el.size(); i += 5)
    {
        const double along = early["along"][i].get<double>();
        bool inSpan = false;
        for (const json& sp : early["spans"])
            inSpan |= along > sp["from"].get<double>() - 2000.0 && along < sp["to"].get<double>() + 2000.0;
        if (!inSpan)
            probe.push_back({el[i][0], el[i][2]});
    }
    REQUIRE(probe.size() > 10);
    const json before = run("terrainSample", {{"args", {t2["mesh"]}}, {"points", probe}});
    run("terrainErode", {{"args", {t2["mesh"]}}, {"iterations", 30}});
    const json after = run("terrainSample", {{"args", {t2["mesh"]}}, {"points", probe}});
    double worstKept = 0.0;
    for (size_t i = 0; i < probe.size(); ++i)
        worstKept = std::max(worstKept, std::abs(after[i]["height"].get<double>() - before[i]["height"].get<double>()));
    CHECK(worstKept < 0.01);
    CHECK(run("terrainInfo", {{"args", {t2["mesh"]}}})["masks"].contains("wear"));

    // A road following a curve.
    const std::string trail = run("curve", {{"points", {{-30000.0, 0.0, -60000.0}, {-10000.0, 0.0, -50000.0}, {20000.0, 0.0, -65000.0}}}, {"type", "poly"}, {"name", "Trail"}});
    json follows = run("terrainAddRoad", {{"args", {mesh, trail}}, {"kind", "path"}, {"id", "trail"}});
    CHECK(follows["applied"].get<bool>());
    CHECK(follows["maxGrade"].get<double>() <= 15.0 + 1e-6);
    CHECK(follows["length"].get<double>() > 40000.0);

    json removed = run("terrainRoadRemove", {{"args", {mesh}}, {"ids", {"farm", "trail"}}});
    CHECK(removed.size() == 2);
    CHECK(run("terrainRoads", {{"args", {mesh}}})["layers"]["Roads"].size() == 1);
    fs::remove_all(dir);
}

TEST_CASE("roads: on a Z-up scene a road lies on XY with its surface up Z, and its mesh banks the same way")
{
    loadTerrain();
    math::setUpAxis(math::UpAxis::Z);
    run("newScene");
    json t = run("terrainCreate", {{"size", {100000.0, 100000.0}}, {"resolution", 513}, {"amplitude", 2000.0}, {"seed", 3}, {"name", "ZRoads"}});
    json r = run("terrainAddRoad", {{"args", {t["mesh"]}}, {"id", "z"}, {"path", {{-40000.0, -30000.0}, {0.0, 25000.0}, {40000.0, -20000.0}}}, {"detail", true}});
    CHECK(r["applied"].get<bool>());
    CHECK(r["maxGrade"].get<double>() <= 8.0 + 1e-6);
    const json& line = r["centreline"];
    double worst = 0.0;
    for (size_t i = line.size() / 10; i < line.size() * 9 / 10; i += line.size() / 20)
    {
        const json s = run("terrainSample", {{"args", {t["mesh"]}}, {"points", {{line[i][0], line[i][1]}}}})[0];
        worst = std::max(worst, std::abs(s["height"].get<double>() - line[i][2].get<double>()));
    }
    CHECK(worst < 2.0);
    json built = run("terrainRoadBuild", {{"args", {t["mesh"]}}});
    REQUIRE(built.size() == 1);
    json data = run("meshData", {{"args", {built[0]["surface"]}}, {"worldSpace", true}});
    REQUIRE(data["points"].size() > 50);
    // The swept surface sits on the road: every vertex within the paved width is near the centreline's height there.
    const double half = r["pavedWidth"].get<double>() * 0.5;
    size_t near = 0;
    double off = 0.0;
    for (size_t i = 0; i < line.size(); i += 40)
    {
        const glm::dvec2 c(line[i][0].get<double>(), line[i][1].get<double>());
        for (const json& p : data["points"])
            if (glm::length(glm::dvec2(p[0].get<double>(), p[1].get<double>()) - c) < half * 1.05)
            {
                ++near;
                off = std::max(off, std::abs(p[2].get<double>() - line[i][2].get<double>()));
            }
    }
    CHECK(near > 0);
    CHECK(off < half * 0.1 + 30.0);
    math::setUpAxis(math::UpAxis::Y);
    run("newScene");
}

TEST_CASE("roads: the same roads give the same bytes at any thread count, and settings round trip")
{
    auto land = [](double x, double z) { return 2500.0 * std::sin(x / 17000.0) * std::cos(z / 23000.0) + 0.02 * x; };
    const terrain::RoadInput a = roadAlong("a", "road", {{-70000.0, -50000.0}, {0.0, 30000.0}, {60000.0, -20000.0}});
    const terrain::RoadInput b = roadAlong("b", "track", {{-60000.0, 50000.0}, {0.0, 0.0}});
    geo::Heightfield h1 = grid(160000.0, 401, land), h2 = h1;
    terrain::applyRoads(h1, {a, b});
    tbb::task_arena single(1);
    single.execute([&] { terrain::applyRoads(h2, {a, b}); });
    CHECK(terrain::revision(h1) == terrain::revision(h2));

    terrain::Road r = terrain::Road::fromJson({{"id", "x"}, {"kind", "rail"}, {"profileName", "rail"}, {"design", {{"maxGrade", 1.5}}}, {"keys", {{1.0, 2.0, 3.0}}}});
    CHECK(r.design.maxGrade == 1.5);
    CHECK(r.design.minVerticalRadius == terrain::roadDesignPreset("rail").minVerticalRadius);
    CHECK(r.profile.poleSpacing > 0.0);
    const terrain::Road back = terrain::Road::fromJson(r.toJson());
    CHECK(back.toJson() == r.toJson());
    CHECK_THROWS_AS(terrain::Road::fromJson({{"kind", "canal"}}), std::invalid_argument);
    CHECK_THROWS_AS(terrain::Road::fromJson({{"profileName", "boulevard"}}), std::invalid_argument);
}

TEST_CASE("roads: timing of a 10 km road on a 4097 terrain" * doctest::skip())
{
    geo::Heightfield h = grid(1000000.0, 4097, [](double x, double z) { return 6000.0 * std::sin(x / 60000.0) * std::cos(z / 45000.0) + 400.0 * std::sin(x / 3000.0); });
    const terrain::RoadInput a = roadAlong("long", "road", {{-450000.0, -300000.0}, {-100000.0, 200000.0}, {200000.0, -100000.0}, {450000.0, 300000.0}});
    const auto t0 = std::chrono::steady_clock::now();
    const terrain::RoadResult r = terrain::applyRoads(h, {a}).at("long");
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE("length ", r.length / 100000.0, " km in ", ms, " ms, cell ", h.cell, ", spans ", r.spans.size(), ", walls ", r.walls.size());
}
