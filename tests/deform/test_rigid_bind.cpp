#include <doctest/doctest.h>

#include "TestUtil.h"

#include "deform/Deform.h"
#include "deform/RigidBind.h"
#include "geo/Primitives.h"
#include "scene/Deformers.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/Skin.h"

#include <glm/gtc/matrix_transform.hpp>

#include <filesystem>

using namespace lila;
using namespace lila::deform;
using json = nlohmann::json;
using namespace deformtest;

namespace
{
glm::dvec3 rwXf(const glm::dmat4& m, const glm::dvec3& p) { return glm::dvec3(m * glm::dvec4(p, 1.0)); }

/** A 0.8 cube every 1.0 along x and z, sitting 0.05 above y = 0. */
geo::Mesh rigidTiles(int nx, int nz)
{
    geo::Mesh out;
    for (int i = 0; i < nx; ++i)
        for (int k = 0; k < nz; ++k)
        {
            geo::Mesh c = geo::makeCube({.width = 0.8, .height = 0.8, .depth = 0.8});
            glm::dvec3 at((i - (nx - 1) * 0.5), 0.45, (k - (nz - 1) * 0.5));
            for (geo::Index p = 0; p < c.pointCapacity(); ++p)
                c.setPosition(p, c.position(p) + at);
            out.append(c);
        }
    return out;
}

/** Rolls the plane y = 0 up into a cylinder of radius r about the line x = 0, y = r, so the top of anything on it squeezes. */
glm::dvec3 rollUp(const glm::dvec3& p, double r)
{
    double a = p.x / r;
    return {(r - p.y) * std::sin(a), r - (r - p.y) * std::cos(a), p.z};
}

struct RigidSetup
{
    geo::Mesh driver = geo::makePlane({.width = 12, .depth = 12, .subdivX = 24, .subdivZ = 24});
    geo::Mesh pieces = rigidTiles(5, 3);
    std::vector<glm::dvec3> rest = pointsOf(pieces);
    std::vector<int32_t> ids = connectedRegions(pointAdjacency(pieces));
};

/** Deepest any piece's point sits inside another piece's rest box, carried by the transforms. */
double rigidDeepestOverlap(const RigidSetup& s, const RigidBindRig& rig, const std::vector<RigidFit>& t)
{
    double deepest = 0.0;
    for (size_t a = 0; a < rig.pieces(); ++a)
        for (size_t b = 0; b < rig.pieces(); ++b)
        {
            if (a == b)
                continue;
            glm::dvec3 lo(1e300), hi(-1e300);
            for (uint32_t k = rig.offsets[b]; k < rig.offsets[b + 1]; ++k)
            {
                lo = glm::min(lo, s.rest[rig.points[k]]);
                hi = glm::max(hi, s.rest[rig.points[k]]);
            }
            for (uint32_t k = rig.offsets[a]; k < rig.offsets[a + 1]; ++k)
            {
                glm::dvec3 p = t[a].apply(s.rest[rig.points[k]]);
                glm::dvec3 q = glm::transpose(t[b].rotation) * (p - t[b].to) + t[b].from;
                glm::dvec3 in = glm::min(q - lo, hi - q);
                deepest = std::max(deepest, std::min({in.x, in.y, in.z}));
            }
        }
    return deepest;
}
} // namespace

TEST_CASE("rigidBind carries pieces exactly with a rigid driver and keeps them rigid on a bending one")
{
    RigidSetup s;
    RigidBindRig rig = rigidBindRig(s.rest, s.ids, RigidShape::Box, 1.0);
    REQUIRE(rig.pieces() == 15);
    const geo::Mesh* drivers[] = {&s.driver};
    RigidBindBinding b = rigidBindSetup(drivers, s.rest, rig, {});
    REQUIRE(b.restNormal.size() == 15);
    CHECK(b.restNormal[0].y == doctest::Approx(1.0));
    CHECK(b.restHeight[0] == doctest::Approx(0.05).epsilon(1e-6));

    glm::dmat4 m = glm::rotate(glm::translate(glm::dmat4(1.0), glm::dvec3(3, -2, 5)), 0.8, glm::normalize(glm::dvec3(1, 2, -1)));
    std::vector<glm::dvec3> moved;
    for (const glm::dvec3& p : pointsOf(s.driver))
        moved.push_back(rwXf(m, p));
    std::span<const glm::dvec3> spans[] = {moved};
    std::vector<glm::dvec3> out = rigidBindApply(b, rig, spans, s.rest, {});
    for (size_t i = 0; i < out.size(); ++i)
        CHECK(near(out[i], rwXf(m, s.rest[i]), 1e-6));

    std::vector<glm::dvec3> rolled;
    for (const glm::dvec3& p : pointsOf(s.driver))
        rolled.push_back(rollUp(p, 3.0));
    spans[0] = rolled;
    RigidBindParams o;
    o.collisionIterations = 0;
    out = rigidBindApply(b, rig, spans, s.rest, o);
    for (size_t i = 0; i < out.size(); ++i)
        for (size_t j = i + 1; j < out.size(); ++j)
            if (s.ids[i] == s.ids[j])
                CHECK(glm::length(out[i] - out[j]) == doctest::Approx(glm::length(s.rest[i] - s.rest[j])).epsilon(1e-9));
    CHECK(rigidBindApply(b, rig, spans, s.rest, o) == out);
}

TEST_CASE("rigidBind collision pushes squeezed pieces apart, by sliding or lifting")
{
    RigidSetup s;
    RigidBindRig rig = rigidBindRig(s.rest, s.ids, RigidShape::Box, 1.0);
    const geo::Mesh* drivers[] = {&s.driver};
    RigidBindBinding b = rigidBindSetup(drivers, s.rest, rig, {});
    std::vector<glm::dvec3> rolled;
    for (const glm::dvec3& p : pointsOf(s.driver))
        rolled.push_back(rollUp(p, 2.0));
    std::span<const glm::dvec3> spans[] = {rolled};

    std::vector<RigidFit> t;
    RigidBindParams o;
    // Hard contacts and no reach limit, so overlaps are always cleared.
    o.reach = 0.0;
    o.collisionSoftness = 0.0;
    o.collisionIterations = 0;
    rigidBindApply(b, rig, spans, s.rest, o, &t);
    const double free = rigidDeepestOverlap(s, rig, t);
    CHECK(free > 0.004);
    o.collisionIterations = 200;
    o.collisionLift = 0.0;
    rigidBindApply(b, rig, spans, s.rest, o, &t);
    CHECK(rigidDeepestOverlap(s, rig, t) < free * 0.25);


    o.maxSlide = 1e-9;
    o.maxLift = 1e-9;
    o.collisionLift = 0.0;
    rigidBindApply(b, rig, spans, s.rest, o, &t);
    CHECK(rigidDeepestOverlap(s, rig, t) == doctest::Approx(free).epsilon(1e-3));
}

TEST_CASE("rigidBind collision lift raises the upper piece over the lower")
{
    geo::Mesh driver = geo::makePlane({.width = 12, .depth = 12, .subdivX = 24, .subdivZ = 24});
    geo::Mesh pieces = geo::makeCube({.width = 0.8, .height = 0.8, .depth = 0.8});
    for (geo::Index p = 0; p < pieces.pointCapacity(); ++p)
        pieces.setPosition(p, pieces.position(p) + glm::dvec3(0, 0.45, 0));
    geo::Mesh upper = geo::makeCube({.width = 0.8, .height = 0.8, .depth = 0.8});
    for (geo::Index p = 0; p < upper.pointCapacity(); ++p)
        upper.setPosition(p, upper.position(p) + glm::dvec3(0.9, 0.7, 0));
    pieces.append(upper);
    std::vector<glm::dvec3> rest = pointsOf(pieces);
    RigidBindRig rig = rigidBindRig(rest, connectedRegions(pointAdjacency(pieces)), RigidShape::Box, 1.0);
    REQUIRE(rig.pieces() == 2);
    const geo::Mesh* drivers[] = {&driver};
    RigidBindBinding b = rigidBindSetup(drivers, rest, rig, {});
    std::vector<glm::dvec3> squeezed;
    for (const glm::dvec3& p : pointsOf(driver))
        squeezed.push_back({p.x * 0.8, p.y, p.z});
    std::span<const glm::dvec3> spans[] = {squeezed};

    RigidBindParams o;
    o.collisionIterations = 0;
    std::vector<RigidFit> wrapped, t;
    rigidBindApply(b, rig, spans, rest, o, &wrapped);
    o.collisionIterations = 100;
    o.collisionLift = 1.0;
    rigidBindApply(b, rig, spans, rest, o, &t);
    CHECK(nearTight(t[0].to, wrapped[0].to));
    CHECK(t[1].to.y > wrapped[1].to.y + 0.3);
    CHECK(t[1].to.x == doctest::Approx(wrapped[1].to.x).epsilon(1e-3));

    o.collisionLift = 0.0;
    rigidBindApply(b, rig, spans, rest, o, &t);
    CHECK(t[1].to.y == doctest::Approx(wrapped[1].to.y));
    CHECK(t[1].to.x - t[0].to.x > wrapped[1].to.x - wrapped[0].to.x + 0.05);

    o.maxLift = 0.1;
    o.collisionLift = 1.0;
    rigidBindApply(b, rig, spans, rest, o, &t);
    CHECK(t[1].to.y == doctest::Approx(wrapped[1].to.y + 0.1).epsilon(1e-2));
}

TEST_CASE("rigidBind relaxation and rotation settings")
{
    RigidSetup s;
    RigidBindRig rig = rigidBindRig(s.rest, s.ids, RigidShape::Off, 1.0);
    CHECK(rig.restGap.size() == rig.nb.size());
    const geo::Mesh* drivers[] = {&s.driver};
    RigidBindBinding b = rigidBindSetup(drivers, s.rest, rig, {});
    std::vector<glm::dvec3> stretched;
    for (const glm::dvec3& p : pointsOf(s.driver))
        stretched.push_back({p.x * 1.5, p.y, p.z});
    std::span<const glm::dvec3> spans[] = {stretched};
    std::vector<RigidFit> t;
    RigidBindParams o;
    o.collideDriver = false;
    rigidBindApply(b, rig, spans, s.rest, o, &t);
    const double spread = glm::length(t[1 * 3].to - t[0].to);
    CHECK(spread == doctest::Approx(1.5).epsilon(1e-6));
    o.relaxation = 1.0;
    o.relaxIterations = 50;
    rigidBindApply(b, rig, spans, s.rest, o, &t);
    CHECK(glm::length(t[1 * 3].to - t[0].to) < spread - 0.1);
    for (const RigidFit& f : t)
        CHECK(std::abs(f.to.y - 0.45) < 2e-3);

    o = {};
    o.normalOffset = 2.0;
    o.rotationStrength = 0.0;
    std::vector<glm::dvec3> rolled;
    for (const glm::dvec3& p : pointsOf(s.driver))
        rolled.push_back(rollUp(p, 3.0));
    spans[0] = rolled;
    rigidBindApply(b, rig, spans, s.rest, o, &t);
    for (const RigidFit& f : t)
        CHECK(glm::length(glm::dvec3(f.rotation[0]) - glm::dvec3(1, 0, 0)) < 1e-12);
    o.rotationStrength = 1.0;
    std::vector<RigidFit> turned;
    rigidBindApply(b, rig, spans, s.rest, o, &turned);
    CHECK(glm::length(turned[0].to - t[0].to) < 1e-9);
    CHECK(turned[0].rotation[1][1] < 0.99);
}

namespace
{
using namespace lila::scene;

std::vector<glm::dvec3> wrapPoints(const std::string& mesh)
{
    Graph& g = Graph::current();
    g.evaluate();
    MeshPtr m = g.find(mesh)->as<MeshPtr>(MeshNode::kOutMesh);
    REQUIRE(m);
    return {m->positions().begin(), m->positions().end()};
}

/** Scales: three small cubes combined into one mesh over a plane driver. */
std::pair<std::string, std::string> rigidBindScene()
{
    run("newScene");
    std::string driver = run("polyPlane", {{"name", "Body"}, {"width", 20}, {"depth", 20}, {"subdivisionsX", 20}, {"subdivisionsZ", 20}})[0];
    json cubes = json::array();
    for (int i = 0; i < 3; ++i)
    {
        std::string c = run("polyCube", {{"width", 1.6}, {"height", 0.4}, {"depth", 1.6}})[0];
        run("move", {{"args", {c}}, {"to", v3(-2.0 + 2.0 * i, 0.3, 0)}});
        cubes.push_back(c);
    }
    std::string scales = run("polyCombine", {{"args", cubes}, {"name", "Scales"}}).get<std::string>();
    return {scales, driver};
}

void checkPiecesRigid(const std::vector<glm::dvec3>& rest, const std::vector<glm::dvec3>& now)
{
    REQUIRE(rest.size() == now.size());
    for (size_t i = 0; i < rest.size(); ++i)
        for (size_t j = i + 1; j < rest.size(); ++j)
            if (i / 8 == j / 8)
                CHECK(glm::length(now[i] - now[j]) == doctest::Approx(glm::length(rest[i] - rest[j])).epsilon(1e-6));
}
} // namespace

TEST_CASE("rigidBind command follows the driver, saves, rebinds and undoes")
{
    auto [scales, driver] = rigidBindScene();
    std::vector<glm::dvec3> rest = wrapPoints(scales);
    json made = run("rigidBind", {{"args", {scales, driver}}, {"params", {{"collisionLift", 1.0}}}});
    REQUIRE(made.size() == 1);
    const std::string d = made[0];
    CHECK(run("getAttr", {{"args", {d + ".collisionLift"}}}).get<double>() == doctest::Approx(1.0));
    CHECK(samePoints(wrapPoints(scales), rest, 1e-6));

    editPoints(driver, [](geo::Index, const glm::dvec3& p) { return rollUp(p, 6.0) - p; });
    std::vector<glm::dvec3> bent = wrapPoints(scales);
    CHECK(maxDistance(bent, rest) > 0.5);
    checkPiecesRigid(rest, bent);

    std::string path = (std::filesystem::temp_directory_path() / "lila_deform_tests" / "rigidbind.lilaa").string();
    run("saveScene", {{"args", {path}}});
    run("newScene");
    run("openScene", {{"args", {path}}});
    CHECK(maxDistance(wrapPoints(scales), bent) < 1e-6);

    run("rebindDeformer", {{"args", {d}}});
    CHECK(samePoints(wrapPoints(scales), rest, 1e-6));
    CHECK_THROWS(run("rigidBind", {{"args", {scales, driver}}, {"collisionShape", "blob"}}));
    CHECK_THROWS(run("rigidBind", {{"args", {scales}}}));
}

TEST_CASE("rigidBindToSkin gives each piece the skinned driver's weights under it")
{
    auto [scales, driver] = rigidBindScene();
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(-10, 0, 0)}, {"name", "bone_a"}});
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "bone_b"}});
    run("joint", {{"position", v3(10, 0, 0)}, {"name", "bone_c"}});
    run("skinCluster", {{"args", {"bone_a", driver}}});
    std::string d = run("rigidBind", {{"args", {scales, driver}}})[0];
    run("setAttr", {{"args", {"bone_b.rotateZ", 30.0}}});
    std::vector<glm::dvec3> wrapped = wrapPoints(scales);

    json skins = run("rigidBindToSkin", {{"args", {scales}}, {"maxInfluences", 4}});
    REQUIRE(skins.size() == 1);
    Graph& g = Graph::current();
    CHECK(g.find(d) == nullptr);
    NodeId sk = skin::find(g, g.find(scales)->id());
    REQUIRE(sk != kNoNode);
    WeightTable t = skin::table(g, sk);
    REQUIRE(t.points.size() == 24);
    for (size_t p = 0; p < t.points.size(); ++p)
    {
        CHECK(t.points[p] == t.points[p / 8 * 8]);
        double sum = 0.0;
        for (const auto& [j, w] : t.points[p])
            sum += w;
        CHECK(sum == doctest::Approx(1.0));
    }
    // Bound like the driver, so posed joints carry the pieces near where the wrap put them.
    std::vector<glm::dvec3> skinned = wrapPoints(scales);
    CHECK(maxDistance(skinned, wrapped) < 0.6);
    run("setAttr", {{"args", {"bone_b.rotateZ", 0.0}}});
    run("undo");
    run("undo");
    CHECK(g.find(d) != nullptr);
    CHECK(skin::find(g, g.find(scales)->id()) == kNoNode);
}

namespace
{
/** The demo arm: a tube bent by two linear-skinned bones at y = 0, covered in rings of tilted overlapping plates. */
struct ElbowScene
{
    geo::Mesh arm = geo::makeCylinder({.radius = 5, .height = 60, .subdivAxis = 24, .subdivHeight = 24});
    geo::Mesh plates;
    std::vector<glm::dvec3> rest;
    std::vector<int32_t> ids;

    ElbowScene()
    {
        for (int row = 0; row < 12; ++row)
            for (int k = 0; k < 12; ++k)
            {
                const double a = (k + 0.5 * (row % 2)) / 12.0 * 2.0 * glm::pi<double>();
                geo::Mesh p = geo::makeCube({.width = 3.2, .height = 5.0, .depth = 0.4});
                glm::dmat4 m = glm::translate(glm::dmat4(1.0), glm::dvec3(std::cos(a) * 5.6, -24 + row * 4.2, std::sin(a) * 5.6));
                m = glm::rotate(m, glm::radians(90.0) - a, glm::dvec3(0, 1, 0));
                m = glm::rotate(m, glm::radians(-12.0), glm::dvec3(1, 0, 0));
                for (geo::Index i = 0; i < p.pointCapacity(); ++i)
                    p.setPosition(i, rwXf(m, p.position(i)));
                plates.append(p);
            }
        rest = pointsOf(plates);
        ids = connectedRegions(pointAdjacency(plates));
    }

    std::vector<glm::dvec3> bent(double degrees) const
    {
        std::vector<glm::dvec3> out;
        const glm::dmat4 r = glm::rotate(glm::dmat4(1.0), glm::radians(degrees), glm::dvec3(0, 0, 1));
        for (const glm::dvec3& p : pointsOf(arm))
        {
            double w = glm::clamp((p.y + 6.0) / 12.0, 0.0, 1.0);
            w = w * w * (3.0 - 2.0 * w);
            out.push_back(p * (1.0 - w) + rwXf(r, p) * w);
        }
        return out;
    }
};
} // namespace

TEST_CASE("rigidBind keeps scales near the surface and moving smoothly through a collapsing elbow")
{
    ElbowScene s;
    RigidBindRig rig = rigidBindRig(s.rest, s.ids, RigidShape::Box, 1.0);
    REQUIRE(rig.pieces() == 144);
    const geo::Mesh* drivers[] = {&s.arm};
    RigidBindBinding b = rigidBindSetup(drivers, s.rest, rig, {});
    RigidBindParams o;
    o.collisionLift = 1.0;
    o.collisionIterations = 8;
    RigidBindParams bare = o;
    bare.collisionIterations = 0;
    bare.collideDriver = false;

    std::vector<RigidFit> prev, prevBare;
    double farthest = 0.0, pop = 0.0;
    for (double deg = 0.0; deg <= 120.0; deg += 1.0)
    {
        std::vector<glm::dvec3> d = s.bent(deg);
        std::span<const glm::dvec3> spans[] = {d};
        std::vector<RigidFit> t, base;
        rigidBindApply(b, rig, spans, s.rest, o, &t);
        rigidBindApply(b, rig, spans, s.rest, bare, &base);
        for (size_t i = 0; i < t.size(); ++i)
        {
            // Collision and the push off the surface stay within reach of where the surface put the piece.
            farthest = std::max(farthest, glm::length(t[i].to - base[i].to) / (rig.radius[i] * o.reach));
            if (!prev.empty())
                pop = std::max(pop, glm::length((t[i].to - prev[i].to) - (base[i].to - prevBare[i].to)));
        }
        prev = t;
        prevBare = base;
    }
    CHECK(farthest <= 1.0 + 1e-9);
    // Plates are about 3 by 5: per degree of bend, none moves more than a third of its width beyond the surface's own motion.
    CHECK(pop < 1.0);
}
