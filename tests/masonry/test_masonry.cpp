#include "MasonryCore.h"

#include "extensions/ExtensionManager.h"
#include "geo/Mesh.h"
#include "gn/NodeLib.h"
#include "ops/Command.h"
#include "scene/Graph.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <set>

using namespace lila;
using namespace masonry;
using json = nlohmann::json;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }

Footprint line(std::initializer_list<glm::dvec3> pts, bool closed = false) { return {std::vector<glm::dvec3>(pts), closed}; }

Footprint circle(double radius, int segments)
{
    Footprint f;
    for (int k = 0; k < segments; ++k)
    {
        const double a = 2.0 * 3.14159265358979323846 * k / segments;
        f.points.push_back({radius * std::cos(a), 0.0, radius * std::sin(a)});
    }
    f.closed = true;
    return f;
}

double signedVolume(const geo::Mesh& m)
{
    double v = 0.0;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        const std::vector<geo::Index> p = m.facePoints(f);
        for (size_t k = 1; k + 1 < p.size(); ++k)
            v += glm::dot(m.position(p[0]), glm::cross(m.position(p[k]), m.position(p[k + 1]))) / 6.0;
    }
    return v;
}

bool closedManifold(const geo::Mesh& m)
{
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.edgeFaceCount(e) != 2)
            return false;
    return m.validate().empty();
}

void loadMasonry()
{
    if (!gn::findDecl("MasonryWall"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_MASONRY_PACKAGE) == "Masonry");
}
} // namespace

TEST_CASE("masonry: a straight running bond wall fits its height and never overlaps")
{
    WallParams p;
    const Layout l = layoutWall(line({{0, 0, 0}, {500, 0, 0}}), p);
    CHECK(l.courses == int(std::lround(p.height / (p.stone.height + p.stone.joint))));
    CHECK(l.pitch * l.courses == doctest::Approx(p.height));
    CHECK(countOverlaps(l.stones) == 0);
    // Two faces, roughly a stone and a joint each along 5 m.
    const double perCourse = double(l.stones.size()) / l.courses;
    CHECK(perCourse > 2.0 * 500.0 / (p.stone.length + p.stone.joint) - 3.0);
    CHECK(perCourse < 2.0 * 500.0 / (p.stone.length + p.stone.joint) + 6.0);
    for (const Stone& s : l.stones)
    {
        CHECK(s.center.x > -1.0);
        CHECK(s.center.x < 501.0);
        CHECK(std::abs(s.center.z) < p.thickness * 0.5);
        CHECK(s.size.x >= std::max(2.0, p.stone.length * 0.2) - 1e-6);
        CHECK(std::abs(glm::determinant(s.frame) - 1.0) < 1e-9);
    }
}

TEST_CASE("masonry: running bond courses are offset by half a stone")
{
    WallParams p;
    p.jitter = 0.0;
    p.protrude = 0.0;
    const Layout l = layoutWall(line({{0, 0, 0}, {300, 0, 0}}), p);
    // The first outer-face stone at the start of each course: full on even courses, a bat on odd.
    for (int course = 0; course < 4; ++course)
    {
        const Stone* first = nullptr;
        for (const Stone& s : l.stones)
            if (s.course == course && s.face == Face::Outer && (!first || s.center.x < first->center.x))
                first = &s;
        REQUIRE(first);
        CHECK(first->role == (course % 2 ? Role::Bat : Role::Stretcher));
    }
}

TEST_CASE("masonry: every bond lays a clean wall")
{
    for (const std::string& name : bondNames())
    {
        CAPTURE(name);
        WallParams p;
        REQUIRE(parseBond(name, &p.bond));
        const Layout l = layoutWall(line({{0, 0, 0}, {320, 0, 0}, {320, 0, 260}, {-40, 0, 260}}), p);
        CHECK(!l.stones.empty());
        CHECK(countOverlaps(l.stones) == 0);
        std::set<Role> roles;
        for (const Stone& s : l.stones)
            roles.insert(s.role);
        if (p.bond == Bond::English || p.bond == Bond::Flemish || p.bond == Bond::Header || p.bond == Bond::GardenWall)
            CHECK(roles.count(Role::Header) == 1);
        if (p.bond == Bond::Running || p.bond == Bond::Stack)
            CHECK(roles.count(Role::Header) == 0);
    }
}

TEST_CASE("masonry: English bond alternates stretcher and header courses, Flemish mixes them in one course")
{
    WallParams p;
    p.bond = Bond::English;
    const Layout english = layoutWall(line({{0, 0, 0}, {400, 0, 0}}), p);
    for (const Stone& s : english.stones)
    {
        if (s.course % 2 == 0)
            CHECK(s.role != Role::Header);
        else
            CHECK(s.role != Role::Stretcher);
    }
    p.bond = Bond::Flemish;
    const Layout flemish = layoutWall(line({{0, 0, 0}, {400, 0, 0}}), p);
    int headers = 0, stretchers = 0;
    for (const Stone& s : flemish.stones)
        if (s.course == 0)
        {
            headers += s.role == Role::Header;
            stretchers += s.role == Role::Stretcher;
        }
    CHECK(headers > 5);
    CHECK(stretchers > 5);
}

TEST_CASE("masonry: corners are filled on every course")
{
    WallParams p;
    const Layout l = layoutWall(line({{0, 0, 0}, {400, 0, 0}, {400, 0, 300}}), p);
    CHECK(countOverlaps(l.stones) == 0);
    // The path turns left, so the outside of the corner is on the right.
    const glm::dvec2 outside(400.0 + p.thickness * 0.5, -p.thickness * 0.5);
    for (int course = 0; course < l.courses; ++course)
    {
        CAPTURE(course);
        double nearest = 1e300;
        for (const Stone& s : l.stones)
            if (s.course == course)
                nearest = std::min(nearest, glm::length(glm::dvec2(s.center.x, s.center.z) - outside));
        CHECK(nearest < p.stone.length);
    }
}

TEST_CASE("masonry: a closed square and a round tower close without overlaps")
{
    WallParams p;
    p.thickness = 40.0;
    const Layout square = layoutWall(line({{0, 0, 0}, {300, 0, 0}, {300, 0, 300}, {0, 0, 300}}, true), p);
    CHECK(!square.stones.empty());
    CHECK(countOverlaps(square.stones) == 0);

    const Layout tower = layoutWall(circle(150.0, 48), p);
    CHECK(!tower.stones.empty());
    CHECK(countOverlaps(tower.stones) == 0);
    for (const Stone& s : tower.stones)
    {
        const double r = glm::length(glm::dvec2(s.center.x, s.center.z));
        CHECK(r > 150.0 - p.thickness * 0.5 - 1.0);
        CHECK(r < 150.0 + p.thickness * 0.5 + 1.0);
    }
}

TEST_CASE("masonry: a wall thinner than a stone and a half is one stone thick")
{
    WallParams p;
    p.thickness = 12.0;
    const Layout l = layoutWall(line({{0, 0, 0}, {200, 0, 0}, {200, 0, 200}}), p);
    REQUIRE(!l.stones.empty());
    CHECK(countOverlaps(l.stones) == 0);
    for (const Stone& s : l.stones)
    {
        CHECK(s.face == Face::Through);
        CHECK(s.size.z == doctest::Approx(12.0));
    }
}

TEST_CASE("masonry: layout is the same every time")
{
    WallParams p;
    p.bond = Bond::Flemish;
    const Footprint f = line({{0, 0, 0}, {350, 0, 0}, {350, 0, 220}, {600, 0, 500}});
    const Layout a = layoutWall(f, p), b = layoutWall(f, p);
    REQUIRE(a.stones.size() == b.stones.size());
    for (size_t k = 0; k < a.stones.size(); ++k)
    {
        CHECK(a.stones[k].center == b.stones[k].center);
        CHECK(a.stones[k].size == b.stones[k].size);
        CHECK(a.stones[k].frame == b.stones[k].frame);
    }
    p.seed = 2;
    const Layout c = layoutWall(f, p);
    bool differs = c.stones.size() != a.stones.size();
    for (size_t k = 0; !differs && k < a.stones.size(); ++k)
        differs = a.stones[k].center != c.stones[k].center;
    CHECK(differs);
}

TEST_CASE("masonry: stone meshes are closed, outward and fill their box")
{
    for (Detail d : {Detail::Worn, Detail::Clean, Detail::Box})
    {
        StoneShape s;
        s.detail = d;
        s.seed = 3;
        const geo::Mesh m = stoneMesh(s);
        CAPTURE(int(d));
        CHECK(closedManifold(m));
        const double box = s.size.x * s.size.y * s.size.z;
        const double v = signedVolume(m);
        CHECK(v > 0.0);
        CHECK(v <= box + 1e-6);
        CHECK(v > box * (d == Detail::Worn ? 0.75 : 0.85));
        CHECK(m.cornerAttrs().has(geo::attr::uvName(0)));
        const geo::Bounds b = m.bounds();
        CHECK(b.max.x <= s.size.x * 0.5 + 1e-9);
        CHECK(b.max.y <= s.size.y * 0.5 + 1e-9);
        CHECK(b.max.z <= s.size.z * 0.5 + 1e-9);
        if (d == Detail::Box)
            CHECK(m.numFaces() == 6);
        else
            CHECK(m.numFaces() == 26);
    }
}

TEST_CASE("masonry: the core is closed and recessed behind the faces")
{
    WallParams p;
    for (const Footprint& f : {line({{0, 0, 0}, {400, 0, 0}, {400, 0, 300}}), line({{0, 0, 0}, {300, 0, 0}, {300, 0, 300}, {0, 0, 300}}, true)})
    {
        const geo::Mesh core = coreMesh(f, p);
        CHECK(closedManifold(core));
        CHECK(signedVolume(core) > 0.0);
        CHECK(core.bounds().max.y == doctest::Approx(p.height - p.stone.joint - p.recess));
    }
    const geo::Mesh straight = coreMesh(line({{0, 0, 0}, {400, 0, 0}}), p);
    CHECK(signedVolume(straight) == doctest::Approx(400.0 * (p.thickness - 2.0 * p.recess) * (p.height - p.stone.joint - p.recess)));
}

TEST_CASE("masonry: footprints round trip through loose edges")
{
    const std::vector<Footprint> in = {line({{0, 0, 0}, {100, 0, 0}, {100, 0, 50}}), circle(80.0, 10)};
    const std::vector<Footprint> out = footprintsFromEdges(edgesFromFootprints(in));
    REQUIRE(out.size() == 2);
    CHECK(!out[0].closed);
    CHECK(out[0].points.size() == 3);
    CHECK(out[1].closed);
    CHECK(out[1].points.size() == 10);
}

TEST_CASE("masonry: built walls share a few stone meshes")
{
    BuildOptions o;
    const Footprint f = line({{0, 0, 0}, {1000, 0, 0}, {1000, 0, 600}});
    const auto t0 = std::chrono::steady_clock::now();
    const Built b = buildWalls(std::span<const Footprint>(&f, 1), o);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE("16 m wall: " << b.stones.size() << " stones, " << b.meshes.size() << " meshes, " << ms << " ms");
    CHECK(b.stones.size() > 2000);
    CHECK(b.meshes.size() < b.stones.size() / 10);
    REQUIRE(b.reference.size() == b.stones.size());
    for (int r : b.reference)
        CHECK(size_t(r) < b.meshes.size());
}

TEST_CASE("masonry: masonryWall builds an editable wall and masonryInfo checks it")
{
    loadMasonry();
    run("newScene");
    const json w = run("masonryWall", {{"points", json::array({json::array({0, 0, 0}), json::array({300, 0, 0}), json::array({300, 0, 200})})},
                                       {"height", 150.0},
                                       {"bond", "flemish"}});
    CHECK(w["stones"].get<int64_t>() > 100);
    CHECK(w["courses"].get<int64_t>() == 20);
    const std::string mesh = w["mesh"].get<std::string>(), mod = w["modifier"].get<std::string>();

    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::Node* n = g.find(mesh);
    REQUIRE(n);
    const scene::MeshPtr out = n->as<scene::MeshPtr>(n->type().attrIndex("outMesh"));
    REQUIRE(out);
    CHECK(out->numFaces() > 1000);

    json info = run("masonryInfo", {{"args", {mesh}}});
    CHECK(info["checks"]["ok"].get<bool>());
    CHECK(info["stones"] == w["stones"]);
    CHECK(info["settings"]["bond"] == "flemish");

    run("setAttr", {{"args", {mod + ".bond", "english"}}});
    info = run("masonryInfo", {{"args", {mesh}}});
    CHECK(info["settings"]["bond"] == "english");
    CHECK(info["checks"]["overlaps"].get<int64_t>() == 0);

    CHECK_THROWS(run("masonryWall", {{"points", json::array({json::array({0, 0, 0}), json::array({100, 0, 0})})}, {"bond", "nonsense"}}));
}
