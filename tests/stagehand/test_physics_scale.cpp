#include <Decompose.h>
#include <RigidScene.h>

#include "TestUtil.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace physics = lila::physics;
using stagehandtest::box;

namespace
{
/** A closed box's twelve triangles, outward facing. */
void boxTriangles(std::vector<glm::dvec3>& t, const glm::dvec3& lo, const glm::dvec3& hi)
{
    auto c = [&](int k) { return glm::dvec3(k & 1 ? hi.x : lo.x, k & 2 ? hi.y : lo.y, k & 4 ? hi.z : lo.z); };
    const int quads[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
    for (const auto& q : quads)
        for (int k : {0, 1, 2, 0, 2, 3})
            t.push_back(c(q[k]));
}

/** An arch 200 wide, 240 tall and 60 deep: two legs 40 wide and a lintel across them, its opening 120 wide and 200 high. */
std::vector<glm::dvec3> archTriangles()
{
    std::vector<glm::dvec3> t;
    boxTriangles(t, {-100, 0, -30}, {-60, 200, 30});
    boxTriangles(t, {60, 0, -30}, {100, 200, 30});
    boxTriangles(t, {-100, 200, -30}, {100, 240, 30});
    return t;
}

double totalHullVolume(const std::vector<std::vector<glm::dvec3>>& parts)
{
    double v = 0.0;
    for (const auto& p : parts)
        v += physics::hullVolume(p);
    return v;
}
} // namespace

TEST_CASE("physics: a body kilometres out rests on the ground as precisely as one at the origin")
{
    const glm::dvec3 far(250000.0, 0.0, -180000.0);
    physics::Settings st;
    st.ground = true;
    st.origin = far;
    physics::RigidScene s(st);
    const int b = s.add(box(far + glm::dvec3(0, 50, 0), {10, 10, 10}));
    const physics::SettleResult r = s.settleUntilRest();
    CHECK(r.atRest);
    CHECK_FALSE(r.capped);
    CHECK(std::abs(s.pose(b).position.x - far.x) < 0.1);
    CHECK(s.pose(b).position.y == doctest::Approx(10.0).epsilon(0.02));
    CHECK(s.penetration(b) < 0.5);
    REQUIRE(s.contacts().size() == 1);
    CHECK(glm::length(s.contacts()[0].point - glm::dvec3(far.x, 0.0, far.z)) < 20.0);
    const auto hit = s.raycast(far + glm::dvec3(0, 500, 0), {0, -1, 0}, 1000.0);
    REQUIRE(hit);
    CHECK(hit->body == b);
    CHECK(hit->point.y == doctest::Approx(20.0).epsilon(0.02));
}

TEST_CASE("physics: the ground reaches far past a kilometre")
{
    physics::Settings st;
    st.ground = true;
    physics::RigidScene s(st);
    const int b = s.add(box({5e6, 30, 0}, {10, 10, 10}));
    s.settleUntilRest();
    CHECK(s.pose(b).position.y > 5.0);
}

TEST_CASE("physics: settleUntilRest stops at rest, and says when the cap stopped it")
{
    physics::Settings st;
    st.ground = true;
    physics::RigidScene s(st);
    s.add(box({0, 2000, 0}, {10, 10, 10}));
    physics::SettleOptions o;
    o.cap = 0.25;
    physics::SettleResult r = s.settleUntilRest(o);
    CHECK(r.capped);
    CHECK_FALSE(r.atRest);
    CHECK(r.seconds == doctest::Approx(0.25).epsilon(0.1));
    r = s.settleUntilRest();
    CHECK(r.atRest);
    CHECK_FALSE(r.capped);
}

TEST_CASE("physics: shapes with one key are built once")
{
    physics::clearShapeCache();
    auto pts = std::make_shared<std::vector<glm::dvec3>>();
    for (int k = 0; k < 8; ++k)
        pts->push_back(glm::dvec3(k & 1 ? 10 : -10, k & 2 ? 5 : -5, k & 4 ? 3 : -3));
    for (int run = 0; run < 3; ++run)
    {
        physics::RigidScene s;
        for (int k = 0; k < 10; ++k)
        {
            physics::BodyDesc b;
            b.shape.kind = physics::Shape::Kind::Hull;
            b.shape.points = pts;
            b.shape.key = 0x1234;
            b.position = {k * 30.0, 10, 0};
            s.add(b);
        }
    }
    CHECK(physics::shapeCacheSize() == 1);
    physics::BodyDesc other;
    other.shape.kind = physics::Shape::Kind::Hull;
    other.shape.points = pts;
    other.shape.key = 0x1234;
    other.density = 2400.0; // a different mass is another shape
    physics::RigidScene s;
    s.add(other);
    CHECK(physics::shapeCacheSize() == 2);
}

TEST_CASE("physics: an arch decomposes into parts that leave its opening free")
{
    const std::vector<glm::dvec3> tris = archTriangles();
    const std::vector<std::vector<glm::dvec3>> parts = physics::decompose(tris);
    CHECK(parts.size() >= 3);
    CHECK(parts.size() <= 16);
    const double solid = 2 * 40.0 * 200 * 60 + 200.0 * 40 * 60;
    const double wrapped = physics::hullVolume(tris);
    CHECK(wrapped == doctest::Approx(200.0 * 240 * 60).epsilon(0.01));
    const double parted = totalHullVolume(parts);
    CHECK(parted < solid * 1.35);
    CHECK(parted > solid * 0.8);
    CHECK(physics::decompose(tris) == parts);

    // Dropped over a post that fits its opening, the arch lands on the ground around it instead of on top of it.
    physics::Settings st;
    st.ground = true;
    physics::RigidScene s(st);
    s.add(box({0, 75, 0}, {20, 75, 20}, physics::Role::Static));
    physics::BodyDesc arch;
    arch.shape.kind = physics::Shape::Kind::Compound;
    for (const auto& p : parts)
        arch.shape.parts.push_back(std::make_shared<const std::vector<glm::dvec3>>(p));
    arch.position = {0, 40, 0};
    arch.density = 2400.0;
    const int a = s.add(arch);
    s.settleUntilRest();
    CHECK(s.pose(a).position.y < 5.0);
    CHECK(s.penetration(a) < 1.0);

    // As one hull it can't: it sits on the post.
    physics::RigidScene one(st);
    one.add(box({0, 75, 0}, {20, 75, 20}, physics::Role::Static));
    physics::BodyDesc hull;
    hull.shape.kind = physics::Shape::Kind::Hull;
    hull.shape.points = std::make_shared<const std::vector<glm::dvec3>>(tris);
    hull.position = {0, 160, 0};
    const int h = one.add(hull);
    one.settleUntilRest();
    CHECK(one.pose(h).position.y > 140.0);
}

TEST_CASE("physics: convex meshes of a shape close up and enclose it")
{
    physics::Shape b;
    b.kind = physics::Shape::Kind::Box;
    b.halfExtents = {10, 20, 30};
    b.center = {0, 20, 0};
    const auto boxMesh = physics::convexMeshes(b);
    REQUIRE(boxMesh.size() == 1);
    CHECK(boxMesh[0].points.size() == 8);
    CHECK(boxMesh[0].faces.size() >= 6);
    CHECK(physics::hullVolume(boxMesh[0].points) == doctest::Approx(20.0 * 40 * 60).epsilon(0.01));
    physics::Shape c;
    c.kind = physics::Shape::Kind::Compound;
    for (const auto& p : physics::decompose(archTriangles()))
        c.parts.push_back(std::make_shared<const std::vector<glm::dvec3>>(p));
    CHECK(physics::convexMeshes(c).size() == c.parts.size());
    physics::Shape m;
    m.kind = physics::Shape::Kind::Mesh;
    CHECK(physics::convexMeshes(m).empty());
}

TEST_CASE("physics: a heightfield is ground with hills, kilometres from the origin")
{
    // 2 km square of 10 m cells, a slope rising 1 m per 10 m along X.
    const int n = 201;
    auto heights = std::make_shared<std::vector<float>>(size_t(n) * n);
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x)
            (*heights)[size_t(z) * n + size_t(x)] = float(x * 100.0);
    const glm::dvec3 terrain(300000.0, 0.0, 300000.0);
    physics::Settings st;
    st.origin = terrain + glm::dvec3(100000, 0, 100000);
    physics::RigidScene s(st);
    physics::BodyDesc hf;
    hf.role = physics::Role::Static;
    hf.shape.kind = physics::Shape::Kind::Heightfield;
    hf.shape.heights = heights;
    hf.shape.samplesX = hf.shape.samplesZ = n;
    hf.shape.cell = 1000.0;
    hf.position = terrain;
    s.add(hf);
    // Halfway across, the ground is 100 m up.
    const glm::dvec3 at = terrain + glm::dvec3(100000, 0, 100000);
    const auto hit = s.raycast(at + glm::dvec3(0, 20000, 0), {0, -1, 0}, 40000.0);
    REQUIRE(hit);
    CHECK(hit->body == 0);
    CHECK(hit->point.y == doctest::Approx(10000.0).epsilon(0.002));
    const int b = s.add(box(at + glm::dvec3(0, 10100, 0), {50, 20, 50}, physics::Role::Dynamic, 2400.0));
    s.settleUntilRest();
    CHECK(s.pose(b).position.y > 10000.0);
    CHECK(s.pose(b).position.y < 10100.0);
    CHECK(s.penetration(b) < 1.0);
}

namespace
{
/** An irregular convex lump about radius across: a rock or a chunk of rubble. */
std::shared_ptr<const std::vector<glm::dvec3>> lump(double radius, uint32_t seed)
{
    auto pts = std::make_shared<std::vector<glm::dvec3>>();
    uint32_t x = seed * 2654435761u + 1u;
    auto next = [&] {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        return double(x % 10000) / 10000.0;
    };
    for (int k = 0; k < 14; ++k)
    {
        const glm::dvec3 d = glm::normalize(glm::dvec3(next() - 0.5, next() - 0.5, next() - 0.5) + glm::dvec3(1e-6));
        pts->push_back(d * radius * (0.7 + 0.3 * next()) * glm::dvec3(1.0, 0.6, 0.9));
    }
    return pts;
}

struct StoneRun
{
    bool atRest = false;
    double seconds = 0.0;
    double height = 0.0;  // where the stone came to rest, cm
    double drift = 0.0;   // how far it moved in three more seconds awake, cm
    double sink = 0.0;    // deepest overlap of anything, cm
    double lowestDebris = 1e300;
};

/** Thirty chunks of rubble 8 to 30 cm across, then a boulder about 4 m across and some ten tonnes dropped onto them. */
StoneRun stoneOnRubble(const physics::Settings& st)
{
    physics::RigidScene s(st);
    const glm::dvec3 site = st.origin;
    std::vector<int> debris;
    for (int k = 0; k < 30; ++k)
    {
        physics::BodyDesc b;
        b.shape.kind = physics::Shape::Kind::Hull;
        b.shape.points = lump(4.0 + (k % 6) * 2.2, uint32_t(k + 1));
        b.position = site + glm::dvec3((k % 6) * 45.0 - 112.0, 20.0 + (k / 6) * 3.0, (k / 6) * 40.0 - 80.0);
        b.density = 2400.0;
        debris.push_back(s.add(b));
    }
    s.settleUntilRest();
    physics::BodyDesc stone;
    stone.shape.kind = physics::Shape::Kind::Hull;
    stone.shape.points = lump(220.0, 99);
    stone.position = site + glm::dvec3(0, 200.0, 0);
    stone.density = 2400.0;
    const int st0 = s.add(stone);
    const physics::SettleResult r = s.settleUntilRest();
    StoneRun out;
    out.atRest = r.atRest;
    out.seconds = r.seconds;
    out.height = s.pose(st0).position.y;
    const physics::Pose rested = s.pose(st0);
    for (int d : debris)
        out.lowestDebris = std::min(out.lowestDebris, s.pose(d).position.y);
    for (int b = 0; b < int(s.size()); ++b)
        out.sink = std::max(out.sink, s.penetration(b));
    s.wake(st0);
    for (int k = 0; k < 180; ++k)
        s.step(1.0 / 60.0);
    out.drift = glm::length(s.pose(st0).position - rested.position);
    return out;
}
} // namespace

TEST_CASE("physics: a boulder of ten tonnes rests on small rubble kilometres out without jitter or sinking")
{
    physics::Settings st;
    st.ground = true;
    st.origin = {200000.0, 0.0, -150000.0};
    st.collisionSteps = 2;
    st.velocitySteps = 12;
    st.maxMassRatio = 50.0;
    {
        physics::RigidScene probe(st);
        physics::BodyDesc b;
        b.shape.kind = physics::Shape::Kind::Hull;
        b.shape.points = lump(220.0, 99);
        b.density = 2400.0;
        probe.add(b);
        CHECK(probe.mass(0) > 9000.0);
    }
    // The same boulder on bare ground, for how high the rubble should hold it.
    double onGround = 0.0;
    {
        physics::RigidScene bare(st);
        physics::BodyDesc b;
        b.shape.kind = physics::Shape::Kind::Hull;
        b.shape.points = lump(220.0, 99);
        b.position = st.origin + glm::dvec3(0, 200.0, 0);
        b.density = 2400.0;
        bare.add(b);
        bare.settleUntilRest();
        onGround = bare.pose(0).position.y;
    }
    const StoneRun r = stoneOnRubble(st);
    CHECK(r.atRest);
    // On the rubble, not crushed through it to the ground.
    CHECK(r.height > onGround + 4.0);
    CHECK(r.sink < 2.0);
    CHECK(r.lowestDebris > 1.0);
    // Woken and run on, it stays put.
    CHECK(r.drift < 1.0);

    physics::Settings plain;
    plain.ground = true;
    plain.origin = st.origin;
    const StoneRun p = stoneOnRubble(plain);
    // Without the mass ratio limit the rubble is crushed into the ground.
    CHECK(p.sink > r.sink);
}

TEST_CASE("physics: a scene too big for its body cap says so")
{
    physics::Settings st;
    st.maxBodies = 20;
    physics::RigidScene s(st);
    int added = 0;
    try
    {
        for (int k = 0; k < 40; ++k, ++added)
            s.add(box({k * 30.0, 10, 0}, {5, 5, 5}));
    }
    catch (const std::invalid_argument& e)
    {
        CHECK(std::string(e.what()).find("too many bodies") != std::string::npos);
    }
    CHECK(added < 20);
    CHECK(physics::maxBodiesLimit() >= 65535);
}
