#include <doctest/doctest.h>

#include "TestUtil.h"

#include "ops/Command.h"
#include "geo/ArrayMesh.h"
#include "geo/Mesh.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <glm/gtc/constants.hpp>

#include <filesystem>
#include <map>

using namespace lila;
using json = nlohmann::json;
using namespace testutil;

namespace
{
json args(std::initializer_list<json> a) { return {{"args", json(a)}}; }
json info(const std::string& m) { return run("meshInfo", args({m})); }
int points(const std::string& m) { return info(m)["points"].get<int>(); }

/** Every edge has exactly two faces: a closed manifold surface. */
bool closedManifold(const geo::Mesh& m)
{
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.edgeFaceCount(e) != 2)
            return false;
    return true;
}

/** Every edge shared by two faces runs opposite ways in them: a consistently wound surface. */
bool consistentWinding(const geo::Mesh& m)
{
    std::map<std::pair<geo::Index, geo::Index>, int> directed;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        std::vector<geo::Index> p = m.facePoints(f);
        for (size_t i = 0; i < p.size(); ++i)
            if (++directed[{p[i], p[(i + 1) % p.size()]}] > 1)
                return false;
    }
    return true;
}

/**
 * A closed wedge of a cylinder, radius r, height h, spanning `degrees` about +y from the +x axis (turning toward -z, as a
 * positive rotation about y does). Its two cut walls lie in the seam planes.
 */
geo::Mesh wedge(double degrees, double r = 50.0, double h = 20.0)
{
    const double a = glm::radians(degrees);
    geo::Mesh m;
    const geo::Index b0 = m.addPoint({0, 0, 0}), t0 = m.addPoint({0, h, 0});
    const geo::Index b1 = m.addPoint({r, 0, 0}), t1 = m.addPoint({r, h, 0});
    const geo::Index b2 = m.addPoint({r * std::cos(a), 0, -r * std::sin(a)}), t2 = m.addPoint({r * std::cos(a), h, -r * std::sin(a)});
    m.addFace(std::vector<geo::Index>{b0, b2, b1});
    m.addFace(std::vector<geo::Index>{t0, t1, t2});
    m.addFace(std::vector<geo::Index>{b1, b2, t2, t1});
    m.addFace(std::vector<geo::Index>{b0, b1, t1, t0});
    m.addFace(std::vector<geo::Index>{b0, t0, t2, b2});
    return m;
}
} // namespace

TEST_CASE("array layout maths: linear, radial, helical, grid and centre frame")
{
    using namespace geo;
    ArrayLayout l;
    l.offset = {10, 0, 0};
    l.count = 4;
    CHECK(arrayCopyCount(l) == 4);
    CHECK(near(glm::dvec3(arrayCopyTransform(l, 0)[3]), {0, 0, 0}));
    CHECK(near(glm::dvec3(arrayCopyTransform(l, 3)[3]), {30, 0, 0}));

    // A rotation per copy curves the row: the second step heads off along the turned direction.
    l.rotate = {0, 90, 0};
    CHECK(near(glm::dvec3(arrayCopyTransform(l, 1)[3]), {10, 0, 0}));
    CHECK(near(glm::dvec3(arrayCopyTransform(l, 2)[3]), {10, 0, -10}));
    l.rotate = glm::dvec3(0.0);

    // Scale per copy compounds.
    l.scale = glm::dvec3(0.5);
    CHECK(glm::length(glm::dvec3(arrayCopyTransform(l, 2)[0])) == doctest::Approx(0.25));
    l.scale = glm::dvec3(1.0);

    ArrayLayout r;
    r.mode = ArrayMode::Radial;
    r.count = 6;
    r.offset = glm::dvec3(0.0);
    CHECK(arrayStepAngle(r) == doctest::Approx(60.0));
    CHECK(arrayClosesRing(r));
    const glm::dvec3 p(10, 0, 0);
    CHECK(near(glm::dvec3(arrayCopyTransform(r, 6) * glm::dvec4(p, 1.0)), p));
    // A partial arc puts copies at both ends.
    r.angle = 90.0;
    r.count = 4;
    CHECK(arrayStepAngle(r) == doctest::Approx(30.0));
    CHECK_FALSE(arrayClosesRing(r));
    CHECK(near(glm::dvec3(arrayCopyTransform(r, 3) * glm::dvec4(p, 1.0)), {0, 0, -10}));
    // Step mode: count copies the given angle apart; 8 x 45 closes the ring.
    r.stepAngle = true;
    r.angle = 45.0;
    r.count = 8;
    CHECK(arrayStepAngle(r) == doctest::Approx(45.0));
    CHECK(arrayClosesRing(r));
    // A rise climbs the axis each copy and keeps the ring open.
    r.rise = 5.0;
    CHECK_FALSE(arrayClosesRing(r));
    CHECK(near(glm::dvec3(arrayCopyTransform(r, 8) * glm::dvec4(p, 1.0)), {10, 40, 0}));
    r.rise = 0.0;

    // Around another axis, about a moved centre.
    r.axis = 2;
    r.stepAngle = false;
    r.angle = 360.0;
    r.count = 4;
    r.frame = glm::translate(glm::dmat4(1.0), glm::dvec3(5, 0, 0));
    CHECK(near(glm::dvec3(arrayCopyTransform(r, 2) * glm::dvec4(p, 1.0)), {0, 0, 0}));

    ArrayLayout gr;
    gr.mode = ArrayMode::Grid;
    gr.gridCount = {3, 2, 2};
    gr.offset = {10, 20, 30};
    CHECK(arrayCopyCount(gr) == 12);
    CHECK(arrayGridCell(gr, 7) == glm::ivec3(1, 0, 1));
    CHECK(near(glm::dvec3(arrayCopyTransform(gr, 7)[3]), {10, 0, 30}));
}

TEST_CASE("radial array welds a closed wedge into one closed manifold ring")
{
    for (int n : {3, 6, 8})
    {
        CAPTURE(n);
        geo::ArrayOptions o;
        o.layout.mode = geo::ArrayMode::Radial;
        o.layout.count = n;
        o.layout.angle = 360.0;
        const geo::Mesh piece = wedge(360.0 / n);

        geo::Mesh loose = geo::arrayMesh(piece, o);
        CHECK(loose.numFaces() == 5 * n);
        CHECK(loose.numPoints() == 6 * n);

        o.weld = true;
        geo::Mesh ring = geo::arrayMesh(piece, o);
        // The cut walls go; the centre line welds to one point top and bottom.
        CHECK(ring.numFaces() == 3 * n);
        CHECK(ring.numPoints() == 2 + 2 * n);
        CHECK(closedManifold(ring));
        CHECK(consistentWinding(ring));
        CHECK(int(ring.numPoints()) - int(ring.numEdges()) + int(ring.numFaces()) == 2);
    }

    // A partial arc leaves its ends open unless mergeEnds asks; an open arc of a closed wedge keeps its end walls.
    geo::ArrayOptions o;
    o.layout.mode = geo::ArrayMode::Radial;
    o.layout.count = 4;
    o.layout.stepAngle = true;
    o.layout.angle = 45.0;
    o.weld = true;
    geo::Mesh arc = geo::arrayMesh(wedge(45.0), o);
    CHECK(arc.numFaces() == 4 * 3 + 2);
    CHECK(closedManifold(arc));
}

TEST_CASE("linear array welds open strips at the seams, within tolerance only")
{
    geo::Mesh strip;
    const geo::Index a = strip.addPoint({0, 0, 0}), b = strip.addPoint({10, 0, 0}), c = strip.addPoint({10, 0, 10}), d = strip.addPoint({0, 0, 10});
    strip.addFace(std::vector<geo::Index>{a, d, c, b});
    geo::ArrayOptions o;
    o.layout.count = 5;
    o.layout.offset = {10.004, 0, 0};
    o.weld = true;
    o.tolerance = 0.01;
    geo::Mesh joined = geo::arrayMesh(strip, o);
    CHECK(joined.numFaces() == 5);
    CHECK(joined.numPoints() == 12);
    o.tolerance = 0.001;
    CHECK(geo::arrayMesh(strip, o).numPoints() == 20);

    // Ends meet only with mergeEnds: four strips turned about their shared edges make a square tube.
    geo::ArrayOptions ring;
    ring.layout.count = 4;
    ring.layout.offset = {10, 0, 0};
    ring.layout.rotate = {0, 0, 90};
    ring.weld = true;
    CHECK(geo::arrayMesh(strip, ring).numPoints() == 4 * 4 - 3 * 2);
    ring.mergeEnds = true;
    CHECK(geo::arrayMesh(strip, ring).numPoints() == 4 * 4 - 4 * 2);
}

TEST_CASE("arrayModifier keeps linear behaviour and adds radial, grid and weld")
{
    run("newScene");
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}, {"name", "Arrayed"}})[0];
    std::string mod = run("addModifier", {{"args", {cube}}, {"kind", "array"}, {"params", {{"count", 3}, {"offset", {200, 0, 0}}}}});
    CHECK(faces(cube) == 18);
    scene::MeshPtr m = outMesh(cube);
    glm::dvec3 hi(-1e9);
    for (const glm::dvec3& p : m->positions())
        hi = glm::max(hi, p);
    CHECK(hi.x == doctest::Approx(450.0));

    run("setAttr", args({mod + ".mode", 1}));
    run("setAttr", args({mod + ".count", 6}));
    run("setAttr", args({mod + ".centerPoint", {0, 0, -300}}));
    run("setAttr", args({mod + ".center", 1}));
    CHECK(faces(cube) == 36);
    m = outMesh(cube);
    glm::dvec3 lo(1e9);
    for (const glm::dvec3& p : m->positions())
        lo = glm::min(lo, p);
    CHECK(lo.z == doctest::Approx(-650.0).epsilon(1e-6));

    run("setAttr", args({mod + ".mode", 2}));
    run("setAttr", args({mod + ".count", 2}));
    run("setAttr", args({mod + ".countY", 3}));
    run("setAttr", args({mod + ".countZ", 2}));
    CHECK(faces(cube) == 6 * 12);

    // Cubes 100 apart touch face to face: weld merges the shared corners and drops the touching faces.
    run("setAttr", args({mod + ".spacing", {100, 100, 100}}));
    run("setAttr", args({mod + ".countY", 1}));
    run("setAttr", args({mod + ".countZ", 1}));
    run("setAttr", args({mod + ".weld", true}));
    CHECK(faces(cube) == 10);
    CHECK(points(cube) == 12);
    CHECK(closedManifold(*outMesh(cube)));

    run("undo");
    CHECK(faces(cube) == 12);
}

TEST_CASE("arrayCenter centres on another object or the mesh pivot")
{
    run("newScene");
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}, {"name", "Spoke"}})[0];
    std::string hub = run("spaceLocator", {{"name", "Hub"}}).get<std::string>();
    run("setAttr", args({hub + ".translate", {0, 0, -500}}));
    std::string mod = run("addModifier", {{"args", {cube}}, {"kind", "array"}, {"params", {{"mode", 1}, {"count", 4}}}});
    run("arrayCenter", args({mod, hub}));
    CHECK(run("getAttr", args({mod + ".center"})).get<int>() == 2);
    scene::MeshPtr m = outMesh(cube);
    glm::dvec3 lo(1e9);
    for (const glm::dvec3& p : m->positions())
        lo = glm::min(lo, p);
    CHECK(lo.z == doctest::Approx(-1050.0).epsilon(1e-6));

    // The centre follows the object live.
    run("setAttr", args({hub + ".translate", {0, 0, -100}}));
    m = outMesh(cube);
    lo = glm::dvec3(1e9);
    for (const glm::dvec3& p : m->positions())
        lo = glm::min(lo, p);
    CHECK(lo.z == doctest::Approx(-250.0).epsilon(1e-6));

    run("setAttr", args({cube + ".rotatePivot", {0, 0, 50}}));
    run("arrayCenter", {{"args", {mod}}, {"pivot", true}});
    CHECK(run("getAttr", args({mod + ".center"})).get<int>() == 1);
    CHECK(run("getAttr", args({mod + ".centerPoint"})) == json::array({0.0, 0.0, 50.0}));

    run("arrayCenter", args({mod}));
    CHECK(run("getAttr", args({mod + ".center"})).get<int>() == 0);
    CHECK_THROWS(run("arrayCenter", args({mod, cube})));
}

TEST_CASE("radial array settings save and load")
{
    run("newScene");
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}, {"name", "Saved"}})[0];
    run("addModifier", {{"args", {cube}}, {"kind", "array"}, {"params", {{"mode", 1}, {"count", 5}, {"rise", 12.5}, {"axis", 2}}}});
    const int before = faces(cube);
    auto dir = std::filesystem::temp_directory_path() / "lila_tests";
    for (const char* ext : {".lila", ".lilaa"})
    {
        auto path = (dir / (std::string("array") + ext)).string();
        run("saveScene", {{"args", {path}}});
        run("newScene");
        run("openScene", {{"args", {path}}});
        CHECK(faces(cube) == before);
        std::string mod = run("modifiers", args({cube}))[0];
        CHECK(run("getAttr", args({mod + ".rise"})).get<double>() == doctest::Approx(12.5));
        CHECK(run("getAttr", args({mod + ".axis"})).get<int>() == 2);
    }
}
