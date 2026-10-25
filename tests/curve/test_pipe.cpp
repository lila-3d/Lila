#include <doctest/doctest.h>

#include "curve/CurveNodes.h"
#include "curve/Pipe.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <glm/gtc/constants.hpp>

#include <filesystem>
#include <map>

using namespace lila;
using json = nlohmann::json;
using D3 = glm::dvec3;

namespace
{
json run(const char* cmd, json call = json::object()) { return ops::execute(cmd, call); }

scene::MeshPtr meshOf(const std::string& name)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::Node* n = g.find(name);
    REQUIRE(n);
    return n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
}

json pts(std::initializer_list<D3> list)
{
    json a = json::array();
    for (const D3& p : list)
        a.push_back({p.x, p.y, p.z});
    return a;
}

curve::Spline poly(std::initializer_list<D3> list)
{
    curve::Spline s;
    s.kind = curve::Kind::Poly;
    s.points = list;
    s.normalize();
    return s;
}

int facesOfSize(const geo::Mesh& m, size_t n)
{
    int count = 0;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f) && m.facePoints(f).size() == n)
            ++count;
    return count;
}

/** No open or non-manifold edges and a clean validate. */
void checkClosed(const geo::Mesh& m)
{
    std::vector<std::string> problems = m.validate();
    INFO((problems.empty() ? std::string() : problems.front()));
    CHECK(problems.empty());
    int open = 0, nonManifold = 0;
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e))
        {
            open += m.isBoundaryEdge(e) ? 1 : 0;
            nonManifold += m.edgeFaceCount(e) > 2 ? 1 : 0;
        }
    CHECK(open == 0);
    CHECK(nonManifold == 0);
}

double distToSegment(const D3& p, const D3& a, const D3& b)
{
    D3 d = b - a;
    double t = std::clamp(glm::dot(p - a, d) / glm::dot(d, d), 0.0, 1.0);
    return glm::distance(a + d * t, p);
}
} // namespace

TEST_CASE("pipe topology: quads, closed, ring counts")
{
    curve::PipeOptions o;
    o.radius = 5.0;
    o.sides = 8;
    o.startCap = o.endCap = curve::CapMode::Grid;
    curve::PipeResult r = curve::makePipe(poly({D3(0), D3(100, 0, 0)}), o);
    CHECK(r.rings == 2);
    CHECK(r.mesh.numFaces() == 8 + 2 * 4);
    CHECK(facesOfSize(r.mesh, 4) == int(r.mesh.numFaces()));
    checkClosed(r.mesh);
    // Cap grids have a centre point; every other point is on the radius.
    for (const D3& p : r.mesh.positions())
        if (std::hypot(p.y, p.z) > 1e-6)
            CHECK(std::hypot(p.y, p.z) == doctest::Approx(5.0));

    // A right-angle corner rounds into a bend at twice the radius, all quads, still closed.
    curve::PipeResult bent = curve::makePipe(poly({D3(0), D3(100, 0, 0), D3(100, 100, 0)}), o);
    CHECK(bent.rings > 4);
    CHECK(facesOfSize(bent.mesh, 4) == int(bent.mesh.numFaces()));
    checkClosed(bent.mesh);
    CHECK(bent.tightestBend == doctest::Approx(10.0).epsilon(0.05));
    CHECK(bent.warnings.empty());

    // A bend tighter than the radius is reported.
    o.bendRadius = 2.0;
    CHECK(!curve::makePipe(poly({D3(0), D3(100, 0, 0), D3(100, 100, 0)}), o).warnings.empty());

    // Corrugation adds rings at a quarter of the period and dips the radius between them.
    curve::PipeOptions h;
    h.radius = 2.0;
    h.sides = 6;
    h.corrugationDepth = 0.2;
    h.corrugationPeriod = 2.0;
    curve::PipeResult hose = curve::makePipe(poly({D3(0), D3(20, 0, 0)}), h);
    CHECK(hose.rings == 41);
    double lo = 1e9;
    for (const D3& p : hose.mesh.positions())
        lo = std::min(lo, std::hypot(p.y, p.z));
    CHECK(lo == doctest::Approx(1.6));
}

TEST_CASE("pipe wall thickness")
{
    curve::PipeOptions o;
    o.radius = 5.0;
    o.sides = 10;
    o.wall = 1.0;
    curve::PipeResult r = curve::makePipe(poly({D3(0), D3(0, 0, 60), D3(40, 0, 60)}), o);
    checkClosed(r.mesh);
    CHECK(facesOfSize(r.mesh, 4) == int(r.mesh.numFaces()));
    // Outer and inner tubes plus two rims of one ring each.
    CHECK(r.mesh.numFaces() == 2 * 10 * (r.rings - 1) + 2 * 10);
    // Every point sits on the outer or the inner radius around the straight first run.
    for (const D3& p : r.mesh.positions())
        if (p.z < 40.0)
        {
            double d = std::hypot(p.x, p.y);
            CHECK((std::abs(d - 5.0) < 1e-6 || std::abs(d - 4.0) < 1e-6));
        }
}

TEST_CASE("pipe parts follow the bent path")
{
    curve::PipeOptions o;
    o.radius = 4.0;
    o.sides = 8;
    o.splitEvery = 40.0;
    o.coupling = curve::CouplingKind::Collar;
    const curve::Spline path = poly({D3(0), D3(100, 0, 0), D3(100, 100, 0)});
    curve::PipeResult r = curve::makePipe(path, o);
    const size_t expected = r.segmentStarts.size() - 1;
    REQUIRE(expected >= 4);
    CHECK(r.placements.size() == expected);
    for (const curve::PipePlacement& p : r.placements)
    {
        // On the centreline, facing along it.
        double d = std::min(distToSegment(p.origin, D3(0), D3(100, 0, 0)), distToSegment(p.origin, D3(100, 0, 0), D3(100, 100, 0)));
        CHECK(d < 3.0);
        if (p.origin.x < 85.0)
            CHECK(glm::dot(p.frame.t, D3(1, 0, 0)) > 0.999);
        if (p.origin.y > 15.0)
            CHECK(glm::dot(p.frame.t, D3(0, 1, 0)) > 0.999);
        CHECK(std::abs(glm::dot(p.frame.n, p.frame.t)) < 1e-9);
    }
    // Segments are their own parts and UV islands.
    auto part = r.mesh.faceAttrs().getOrAdd<int32_t>("part");
    int maxPart = 0;
    for (geo::Index f = 0; f < r.mesh.faceCapacity(); ++f)
        if (r.mesh.faceValid(f))
            maxPart = std::max(maxPart, part[f]);
    CHECK(maxPart == int(expected));

    // Split at the bend with elbow collars: one sleeve over the bend, centred on it.
    o.splitEvery = 0.0;
    o.splitAtBends = true;
    o.coupling = curve::CouplingKind::ElbowCollar;
    curve::PipeResult e = curve::makePipe(path, o);
    REQUIRE(e.placements.size() == 1);
    CHECK(e.placements[0].bend);
    CHECK(glm::dot(e.placements[0].frame.t, glm::normalize(D3(1, 1, 0))) > 0.99);
}

TEST_CASE("pipe bolt rings")
{
    curve::PipeOptions o;
    o.radius = 5.0;
    o.sides = 12;
    o.splitAt = {0.5};
    o.coupling = curve::CouplingKind::Flange;
    o.couplingScale = 2.0;
    o.fasteners = curve::FastenerKind::BoltAndNut;
    o.boltCount = 6;
    o.boltPhase = glm::radians(15.0);
    o.startCap = o.endCap = curve::CapMode::Grid;
    curve::PipeResult r = curve::makePipe(poly({D3(0), D3(0, 100, 0)}), o);
    std::vector<curve::PipePlacement> bolts;
    const curve::PipePlacement* flange = nullptr;
    for (const curve::PipePlacement& p : r.placements)
        if (p.kind == curve::PipePart::Fastener)
            bolts.push_back(p);
        else
            flange = &p;
    REQUIRE(flange);
    REQUIRE(bolts.size() == 6);
    const double expected = (5.0 + 0.35 * 5.0 + 10.0) * 0.5;
    for (size_t k = 0; k < bolts.size(); ++k)
    {
        D3 off = bolts[k].origin - flange->origin;
        CHECK(glm::length(off) == doctest::Approx(expected));
        CHECK(std::abs(glm::dot(off, flange->frame.t)) < 1e-9);
        D3 next = bolts[(k + 1) % bolts.size()].origin - flange->origin;
        CHECK(std::acos(std::clamp(glm::dot(glm::normalize(off), glm::normalize(next)), -1.0, 1.0)) == doctest::Approx(glm::pi<double>() / 3.0));
    }
    D3 first = glm::normalize(bolts[0].origin - flange->origin);
    CHECK(glm::dot(first, flange->frame.n) == doctest::Approx(std::cos(glm::radians(15.0))));
    // Parts are tagged and closed shells of their own.
    auto kind = r.mesh.faceAttrs().getOrAdd<int32_t>("pipePart");
    int fastenerFaces = 0;
    for (geo::Index f = 0; f < r.mesh.faceCapacity(); ++f)
        if (r.mesh.faceValid(f) && kind[f] == int(curve::PipePart::Fastener))
            ++fastenerFaces;
    CHECK(fastenerFaces > 0);
    checkClosed(r.mesh);
    checkClosed(curve::collarPart(5.0, 7.0, 4.0, 12));
    checkClosed(curve::clampPart(5.0, 7.0, 4.0, 12, true));
}

TEST_CASE("pipe command: history, presets, save, load and undo")
{
    run("newScene");
    std::string path = run("curve", {{"type", "poly"}, {"points", pts({D3(0), D3(0, 0, 200), D3(150, 0, 200)})}, {"name", "Run"}});
    json r = run("pipe", {{"args", {path}}, {"preset", "pipe"}, {"sides", 10}});
    std::string mesh = r[0], node = r[1];
    scene::MeshPtr m = meshOf(mesh);
    REQUIRE(m);
    size_t faces = m->numFaces();
    CHECK(faces > 0);
    scene::Graph& g = scene::Graph::current();
    scene::Node* pn = g.find(node);
    REQUIRE(pn);
    CHECK(pn->as<int64_t>(pn->type().attrIndex("sides")) == 10);
    CHECK(pn->as<double>(pn->type().attrIndex("wallThickness")) == doctest::Approx(0.4));
    CHECK(pn->as<int64_t>(pn->type().attrIndex("partCount")) > 0);

    // Live: moving the path moves the pipe; a preset changes it in place.
    run("move", {{"args", {path}}, {"by", {0, 50, 0}}});
    CHECK(meshOf(mesh)->bounds().min.y > 30.0);
    run("pipePreset", {{"args", {mesh}}, {"preset", "cable"}});
    CHECK(meshOf(mesh)->numFaces() < faces);
    run("undo");
    CHECK(meshOf(mesh)->numFaces() == faces);

    auto dir = std::filesystem::temp_directory_path() / "lila_curve_tests";
    for (const char* ext : {".lila", ".lilaa"})
    {
        std::string file = (dir / (std::string("pipe") + ext)).string();
        run("saveScene", {{"args", {file}}});
        run("newScene");
        run("openScene", {{"args", {file}}});
        CHECK(meshOf(mesh)->numFaces() == faces);
        CHECK(run("objExists", {{"args", {node}}}).get<bool>());
    }

    // Points make the path curve in the same step, and undo takes both away.
    json q = run("pipe", {{"points", pts({D3(0, 0, -50), D3(0, 0, -150)})}, {"radius", 2.0}, {"name", "Quick"}});
    CHECK(run("objExists", {{"args", {q[0]}}}).get<bool>());
    run("undo");
    CHECK(!run("objExists", {{"args", {q[0]}}}).get<bool>());
    CHECK(!run("objExists", {{"args", {"PipePath"}}}).get<bool>());
}

TEST_CASE("pipe from a face loop")
{
    run("newScene");
    std::string cube = run("polyCube", {{"width", 20}, {"height", 20}, {"depth", 20}})[0];
    scene::MeshPtr before = meshOf(cube);
    geo::Index top = 0;
    for (geo::Index f = 0; f < before->faceCapacity(); ++f)
        if (before->faceValid(f) && before->faceCentroid(f).y > before->faceCentroid(top).y)
            top = f;
    const D3 c = before->faceCentroid(top);
    std::vector<geo::Index> loop = before->facePoints(top);
    const size_t faces0 = before->numFaces();

    // Live: the first ring lands exactly on the face's border.
    json live = run("pipe", {{"points", pts({c, c + D3(0, 40, 0)})}, {"loopMesh", cube}, {"loopFaces", {top}}});
    scene::MeshPtr pm = meshOf(live[0]);
    for (geo::Index p : loop)
    {
        double best = 1e9;
        for (const D3& q : pm->positions())
            best = std::min(best, glm::distance(q, before->position(p)));
        CHECK(best < 1e-9);
    }
    run("undo");

    // Attached: the face goes and the pipe welds onto its border, leaving one closed shell.
    run("selectComponents", {{"args", {cube}}, {"faces", {top}}});
    json att = run("pipe", {{"points", pts({c, c + D3(0, 40, 0)})}, {"fromLoop", true}, {"attach", true}, {"endCap", "grid"}});
    CHECK(att[0] == cube);
    scene::MeshPtr after = meshOf(cube);
    checkClosed(*after);
    CHECK(after->numFaces() == faces0 - 1 + 4 + 1);
    CHECK(after->numPoints() == before->numPoints() + 4);
    CHECK(after->bounds().max.y == doctest::Approx(c.y + 40.0));
    run("undo");
    CHECK(meshOf(cube)->numFaces() == faces0);
}

TEST_CASE("conform to curve")
{
    run("newScene");
    std::string plane = run("polyPlane", {{"width", 10}, {"depth", 10}})[0];
    std::string ring = run("circle", {{"radius", 30.0}, {"center", {0, 5, 0}}});
    scene::MeshPtr m = meshOf(plane);
    json all = json::array();
    for (geo::Index p = 0; p < m->pointCapacity(); ++p)
        if (m->pointValid(p))
            all.push_back(p);
    REQUIRE(all.size() == 4);
    run("selectMode", {{"args", {"vertex"}}});
    run("selectComponents", {{"args", {plane}}, {"vertices", all}});
    // Loop mode spreads the loop along the closed curve.
    run("polyConformToObject", {{"target", ring}});
    m = meshOf(plane);
    for (geo::Index p = 0; p < m->pointCapacity(); ++p)
        if (m->pointValid(p))
        {
            D3 q = m->position(p);
            CHECK(std::hypot(q.x, q.z) == doctest::Approx(30.0).epsilon(0.002));
            CHECK(q.y == doctest::Approx(5.0));
        }
    run("undo");

    // Surface mode: each point to the nearest point of an open curve.
    std::string line = run("curve", {{"type", "poly"}, {"points", pts({D3(-50, 0, 20), D3(50, 0, 20)})}});
    run("selectComponents", {{"args", {plane}}, {"vertices", all}});
    run("polyConformToObject", {{"target", line}, {"mode", "surface"}});
    m = meshOf(plane);
    for (geo::Index p = 0; p < m->pointCapacity(); ++p)
        if (m->pointValid(p))
            CHECK(m->position(p).z == doctest::Approx(20.0));
}

TEST_CASE("pipe per-point radius: smooth between points, the node's radius scales it")
{
    curve::Spline s = poly({D3(0), D3(100, 0, 0)});
    s.radius = {1.0, 2.0};
    curve::PipeOptions o;
    o.radius = 3.0;
    o.sides = 8;
    curve::PipeResult r = curve::makePipe(s, o);
    REQUIRE(r.rings > 5);
    auto radiusAt = [](const geo::Mesh& m, double x) {
        double best = 0.0;
        for (const D3& p : m.positions())
            if (std::abs(p.x - x) < 1e-6)
                best = std::max(best, std::hypot(p.y, p.z));
        return best;
    };
    CHECK(radiusAt(r.mesh, 0.0) == doctest::Approx(3.0));
    // Two points: a straight taper, every ring on the line between their radii.
    for (const D3& p : r.mesh.positions())
        if (std::hypot(p.y, p.z) > 1e-6)
            CHECK(std::hypot(p.y, p.z) == doctest::Approx(3.0 * (1.0 + p.x / 100.0)));
    CHECK(radiusAt(r.mesh, 100.0) == doctest::Approx(6.0));

    run("newScene");
    json j = curve::splineToJson(s);
    json made = run("pipe", {{"splines", json::array({j})}, {"radius", 3.0}, {"sides", 8}});
    std::string mesh = made[0], node = made[1];
    CHECK(radiusAt(*meshOf(mesh), 100.0) == doctest::Approx(6.0));
    run("setAttr", {{"args", {node + ".radius", 1.5}}});
    CHECK(radiusAt(*meshOf(mesh), 100.0) == doctest::Approx(3.0));
    CHECK(radiusAt(*meshOf(mesh), 0.0) == doctest::Approx(1.5));
}

TEST_CASE("pipe radius swells smoothly through its points")
{
    curve::Spline s = poly({D3(0), D3(100, 0, 0), D3(200, 0, 0)});
    s.radius = {1.0, 1.0, 3.0};
    curve::PipeOptions o;
    o.radius = 5.0;
    o.sides = 8;
    curve::PipeResult r = curve::makePipe(s, o);
    // Ring radius by x: rises monotonically from 5 to 15, with rings in between rather than one long cone.
    std::map<long, double> byX;
    for (const D3& p : r.mesh.positions())
        byX[std::lround(p.x * 1000.0)] = std::max(byX[std::lround(p.x * 1000.0)], std::hypot(p.y, p.z));
    CHECK(byX.size() > 10);
    double prev = 0.0;
    for (auto [x, rad] : byX)
    {
        CHECK(rad >= prev - 1e-6);
        prev = rad;
    }
    CHECK(byX.begin()->second == doctest::Approx(5.0));
    CHECK(prev == doctest::Approx(15.0));
}
