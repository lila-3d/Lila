#include "TestUtil.h"

#include "Brushes.h"
#include "Landform.h"

#include "gn/Gn.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "sculpt/SculptSession.h"

#include <cmath>

using namespace terraintest;
using lila::geo::Heightfield;

namespace
{
Heightfield flatField(double h = 0.0)
{
    return lila::geo::makeHeightfieldCovering({0.0, 0.0}, {20000.0, 20000.0}, 101, float(h));
}

terrain::SculptStroke dab(terrain::SculptOp op, glm::dvec2 at, double radius, double strength)
{
    terrain::SculptStroke s;
    s.op = op;
    s.points = {at};
    s.radius = radius;
    s.strength = strength;
    return s;
}

double heightAtPoint(const Heightfield& h, double x, double z) { return h.heightAt(x, z); }
} // namespace

TEST_CASE("brushes: pressure scales a dab, mirror copies it, and a falloff shape is used")
{
    Heightfield h = flatField();
    terrain::SculptStroke s = dab(terrain::SculptOp::Raise, {4000.0, 2000.0}, 2000.0, 100.0);
    s.pressure = {0.5f};
    s.mirror = "x";
    terrain::applyStroke(h, s);
    CHECK(heightAtPoint(h, 4000.0, 2000.0) == doctest::Approx(50.0).epsilon(0.02));
    CHECK(heightAtPoint(h, -4000.0, 2000.0) == doctest::Approx(50.0).epsilon(0.02));
    CHECK(heightAtPoint(h, 4000.0, -2000.0) == doctest::Approx(0.0));

    Heightfield c = flatField();
    terrain::SculptStroke k = dab(terrain::SculptOp::Raise, {0.0, 0.0}, 2000.0, 100.0);
    k.falloff = "constant";
    terrain::applyStroke(c, k);
    CHECK(heightAtPoint(c, 1800.0, 0.0) == doctest::Approx(100.0));
    CHECK(heightAtPoint(c, 2400.0, 0.0) == doctest::Approx(0.0));
}

TEST_CASE("brushes: flatten to a plane, ramp between two heights, terrace into steps")
{
    Heightfield h = flatField();
    for (int z = 0; z < h.nz; ++z)
        for (int x = 0; x < h.nx; ++x)
        {
            const glm::dvec2 q = h.samplePos(x, z);
            h.heights[h.index(x, z)] = float(q.x * 0.3 + 400.0 * std::sin(q.y / 700.0) * std::cos(q.x / 900.0));
        }
    terrain::SculptStroke f = dab(terrain::SculptOp::Flatten, {0.0, 0.0}, 4000.0, 1.0);
    f.settings = {{"plane", true}};
    terrain::applyStroke(h, f);
    // The plane follows the tilt, so the middle sits on it.
    CHECK(heightAtPoint(h, 1000.0, 0.0) == doctest::Approx(300.0).epsilon(0.25));
    CHECK(heightAtPoint(h, -1000.0, 0.0) == doctest::Approx(-300.0).epsilon(0.25));

    Heightfield r = flatField();
    terrain::SculptStroke ramp;
    ramp.op = terrain::SculptOp::Ramp;
    ramp.points = {{-6000.0, 0.0}, {6000.0, 0.0}};
    ramp.radius = 1500.0;
    ramp.strength = 1.0;
    ramp.height = 0.0;
    ramp.settings = {{"endHeight", 1200.0}};
    terrain::applyStroke(r, ramp);
    CHECK(heightAtPoint(r, 0.0, 0.0) == doctest::Approx(600.0).epsilon(0.02));
    CHECK(heightAtPoint(r, 6000.0, 0.0) == doctest::Approx(1200.0).epsilon(0.02));
    CHECK(heightAtPoint(r, 0.0, 4000.0) == doctest::Approx(0.0));

    Heightfield t = flatField();
    for (int z = 0; z < t.nz; ++z)
        for (int x = 0; x < t.nx; ++x)
            t.heights[t.index(x, z)] = float(t.samplePos(x, z).x * 0.5);
    terrain::SculptStroke tr = dab(terrain::SculptOp::Terrace, {0.0, 0.0}, 5000.0, 1.0);
    tr.settings = {{"step", 500.0}, {"sharpness", 1.0}};
    tr.falloff = "constant";
    terrain::applyStroke(t, tr);
    size_t on = 0, n = 0;
    for (int x = 40; x <= 60; ++x)
    {
        const double v = t.at(x, 50);
        on += std::abs(v / 500.0 - std::round(v / 500.0)) < 0.05;
        ++n;
    }
    CHECK(double(on) / double(n) > 0.8);
}

TEST_CASE("brushes: library stamps lay their shape, rotated, and an unknown stamp is an error")
{
    for (const std::string& name : terrain::stampNames())
    {
        CHECK(std::abs(terrain::stampShape(name, 0.99, 0.0)) < 1e-3);
        CHECK(terrain::stampShape(name, 1.2, 0.0) == 0.0);
    }
    Heightfield h = flatField();
    terrain::SculptStroke s = dab(terrain::SculptOp::Stamp, {0.0, 0.0}, 6000.0, 1000.0);
    s.settings = {{"stamp", "crater"}};
    terrain::applyStroke(h, s);
    CHECK(heightAtPoint(h, 0.0, 0.0) < -500.0);
    CHECK(heightAtPoint(h, 6000.0 * 0.62, 0.0) > 300.0);
    s.settings = {{"stamp", "nothing_like_this"}};
    CHECK_THROWS(terrain::applyStroke(h, s));
    const std::vector<float> img = terrain::stampImage("peak", 32);
    CHECK(img.size() == 32 * 32);
}

TEST_CASE("brushes: erode works under the brush only, relax evens bumps, masks paint toward a value")
{
    Heightfield h = flatField();
    for (int z = 0; z < h.nz; ++z)
        for (int x = 0; x < h.nx; ++x)
        {
            const glm::dvec2 q = h.samplePos(x, z);
            h.heights[h.index(x, z)] = float(3000.0 - glm::length(q) * 0.2 + 150.0 * std::sin(q.x / 300.0) * std::sin(q.y / 400.0));
        }
    const Heightfield h0 = h;
    terrain::SculptStroke e = dab(terrain::SculptOp::Erode, {0.0, 0.0}, 4000.0, 1.0);
    e.settings = {{"iterations", 20}};
    terrain::applyStroke(h, e);
    double inside = 0.0, outside = 0.0;
    for (int z = 0; z < h.nz; ++z)
        for (int x = 0; x < h.nx; ++x)
        {
            const double d = std::abs(double(h.at(x, z)) - double(h0.at(x, z)));
            const double r = glm::length(h.samplePos(x, z));
            if (r < 3000.0)
                inside = std::max(inside, d);
            if (r > 4100.0)
                outside = std::max(outside, d);
        }
    CHECK(inside > 1.0);
    CHECK(outside == doctest::Approx(0.0));

    Heightfield r = h0;
    terrain::SculptStroke relax = dab(terrain::SculptOp::Relax, {0.0, 0.0}, 4000.0, 1.0);
    relax.settings = {{"iterations", 8}};
    terrain::applyStroke(r, relax);
    auto roughness = [](const Heightfield& f) {
        double s = 0.0;
        for (int z = 45; z < 55; ++z)
            for (int x = 45; x < 55; ++x)
                s += std::abs(4.0 * f.at(x, z) - f.at(x + 1, z) - f.at(x - 1, z) - f.at(x, z + 1) - f.at(x, z - 1));
        return s;
    };
    CHECK(roughness(r) < roughness(h0) * 0.7);

    Heightfield m = flatField();
    terrain::SculptStroke paint = dab(terrain::SculptOp::Mask, {0.0, 0.0}, 3000.0, 1.0);
    paint.settings = {{"mask", "rock"}, {"value", 0.8}};
    terrain::applyStroke(m, paint);
    CHECK(m.maskAt("rock", 0.0, 0.0) == doctest::Approx(0.8));
    CHECK(m.maskAt("rock", 5000.0, 0.0) == doctest::Approx(0.0));
    terrain::SculptStroke up = dab(terrain::SculptOp::Uplift, {0.0, 0.0}, 3000.0, 1.0);
    terrain::applyStroke(m, up);
    CHECK(m.maskAt("uplift", 0.0, 0.0) == doctest::Approx(1.0));
}

TEST_CASE("brushes: patches round trip and fade at their rim")
{
    const std::vector<float> v = {1.5f, -2.25f, 1e6f, 0.0f, 3.14159f};
    CHECK(terrain::unpackFloats(terrain::packFloats(v)) == v);
    CHECK(terrain::unpackFloats("not base64!").empty());
    Heightfield h = flatField();
    std::vector<float> delta(21 * 21, 500.0f);
    terrain::SculptStroke p = dab(terrain::SculptOp::Patch, {0.0, 0.0}, 1.0, 1.0);
    p.settings = {{"patch", {{"x0", 40}, {"z0", 40}, {"step", 1}, {"nx", 21}, {"nz", 21}, {"delta", terrain::packFloats(delta)}}}};
    terrain::applyStroke(h, p);
    CHECK(h.at(50, 50) == doctest::Approx(500.0));
    CHECK(h.at(40, 50) == doctest::Approx(0.0));
    CHECK(h.at(39, 50) == doctest::Approx(0.0));
}

TEST_CASE("brushes: terrainStroke records each stroke so replaying its calls gives the same terrain")
{
    loadTerrain();
    run("newScene");
    const json calls = json::array({
        {{"brush", "raise"}, {"path", {{-20000.0, 0.0}, {20000.0, 5000.0}}}, {"radius", 6000.0}, {"strength", 900.0}, {"pressure", {0.3, 1.0}}, {"mirror", "z"}},
        {{"brush", "stamp"}, {"stamp", "volcano"}, {"points", {{10000.0, -10000.0}}}, {"radius", 9000.0}, {"strength", 4000.0}, {"angle", 30.0}},
        {{"brush", "terrace"}, {"points", {{0.0, 0.0}}}, {"radius", 15000.0}, {"strength", 1.0}, {"step", 700.0}},
        {{"brush", "ramp"}, {"points", {{-30000.0, -30000.0}, {-5000.0, -30000.0}}}, {"radius", 2500.0}, {"strength", 1.0}, {"height", 200.0}, {"endHeight", 2500.0},
         {"stage", "final"}},
        {{"brush", "mask"}, {"mask", "rock"}, {"points", {{0.0, 20000.0}}}, {"radius", 8000.0}, {"strength", 1.0}, {"value", 1.0}, {"stage", "final"}},
        {{"brush", "relax"}, {"points", {{0.0, 0.0}}}, {"radius", 8000.0}, {"strength", 1.0}, {"stage", "final"}},
    });
    auto build = [&](const std::string& name) {
        run("terrainCreate", {{"size", {100000.0, 100000.0}}, {"resolution", 129}, {"amplitude", 3000.0}, {"name", name}});
        json last;
        for (json c : calls)
        {
            c["args"] = {name};
            last = run("terrainStroke", c);
        }
        return last;
    };
    const json a = build("StrokeA");
    CHECK(a["masks"].contains("rock"));
    const json b = build("StrokeB");
    CHECK(a["revision"] == b["revision"]);

    // The ramp lands on its heights along the up axis.
    CHECK(heightAt(a, -5000.0, -30000.0) == doctest::Approx(2500.0).epsilon(0.05));

    json u = run("terrainStroke", {{"args", {"StrokeA"}}, {"brush", "uplift"}, {"points", {{0.0, 0.0}}}, {"radius", 10000.0}, {"strength", 1.0}});
    CHECK(u["stage"] == "base");
    CHECK(u["erosion"]["upliftMask"] == "uplift");
    CHECK(u["erosion"]["erosionSet"] == 60);
    run("newScene");
}

TEST_CASE("brushes: Sculpt Region bakes a sculpted patch back and lists the overhang it cannot hold")
{
    loadTerrain();
    if (!ops::find("sculptEnter"))
        sculpt::registerCommands();
    run("newScene");
    json t = run("terrainCreate", {{"size", {40000.0, 40000.0}}, {"resolution", 129}, {"flat", true}, {"name", "RegionPatch"}});
    const double before = heightAt(t, 0.0, 0.0);
    json r = run("terrainSculptRegion", {{"args", {"RegionPatch"}}, {"center", {0.0, 0.0}}, {"size", 10000.0}});
    CHECK(r["sculpting"] == true);
    CHECK(run("getAttr", {{"args", {"RegionPatch.visibility"}}}) == false);
    run("sculptCancel");
    // Sculpt the proxy by hand: a mound in the middle, and a shelf floating over one corner.
    scene::Graph& g = scene::Graph::current();
    scene::Node* proxy = g.find(r["mesh"].get<std::string>());
    REQUIRE(proxy);
    g.evaluate();
    geo::Mesh m = *proxy->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
        {
            glm::dvec3 q = m.position(p);
            q.y += 800.0 * std::max(0.0, 1.0 - std::hypot(q.x, q.z) / 3000.0);
            m.setPosition(p, q);
        }
    const double y0 = 300.0, y1 = 900.0, a0 = 3000.0, a1 = 4500.0;
    const geo::Index base = m.addPoint({a0, y0, a0});
    for (const glm::dvec3& q : {glm::dvec3(a1, y0, a0), glm::dvec3(a1, y0, a1), glm::dvec3(a0, y0, a1), glm::dvec3(a0, y1, a0), glm::dvec3(a1, y1, a0),
                                glm::dvec3(a1, y1, a1), glm::dvec3(a0, y1, a1)})
        m.addPoint(q);
    m.addFace({base + 0, base + 1, base + 2, base + 3}); // underside, facing down
    m.addFace({base + 4, base + 7, base + 6, base + 5}); // top, facing up
    g.setValue({proxy->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));

    json f = run("terrainSculptRegion", {{"args", {"RegionPatch"}}, {"finish", true}});
    CHECK(f["changedSamples"].get<int64_t>() > 0);
    CHECK(heightAt(f, 0.0, 0.0) == doctest::Approx(before + 800.0).epsilon(0.05));
    REQUIRE(f["overhangs"].size() == 1);
    CHECK(f["overhangs"][0]["bottom"].get<double>() == doctest::Approx(y0).epsilon(0.01));
    CHECK(f["overhangs"][0]["top"].get<double>() == doctest::Approx(y1).epsilon(0.01));
    CHECK(g.find(r["mesh"].get<std::string>()) == nullptr);
    CHECK(run("getAttr", {{"args", {"RegionPatch.visibility"}}}) == true);
    run("newScene");
}
