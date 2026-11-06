// SPDX-License-Identifier: Apache-2.0
// Modified for Lila (2026).
// Feather de-intersection (Plume: feathers laid over one another by a rank and off the body) and
// instances bent along their strands (Instance's deform alongStrand).

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

#include <nlohmann/json.hpp>

#if GROOMLAB_HAS_TBB
#include <tbb/task_arena.h>
#endif

#include "groomlab/curve.h"
#include "groomlab/graph.h"
#include "groomlab/groom.h"
#include "groomlab/instances.h"
#include "groomlab/operators.h"
#include "groomlab/scales.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

using json = nlohmann::json;

// ---- Plume ------------------------------------------------------------------------------------

// A 6 x 6 plane (normal +Y) with rows of shafts lying on it along +X, close enough that each
// feather's vane runs through its neighbours' (and its drooping barbs dip under the plane).
struct FeatherBed {
    MeshPtr scalp;
    StrandsPtr shafts;
    ExternalInputs ext;

    explicit FeatherBed(int columns = 7, int rows = 5) {
        scalp = test::makePlane(24, 6.0f);
        auto s = std::make_shared<StrandSet>();
        StrandId id = 1;
        for (int j = 0; j < rows; ++j) {
            for (int i = 0; i < columns; ++i) {
                const Vec3f root{0.6f + 0.45f * static_cast<float>(i), 0.0f, 1.0f + 0.4f * static_cast<float>(j)};
                std::vector<Vec3f> pts;
                for (int k = 0; k <= 10; ++k) pts.push_back(root + Vec3f{0.1f * static_cast<float>(k), 0.0f, 0.0f});
                s->addStrand(id++, pts, std::vector<float>(pts.size(), 0.01f), scalp->closestPoint(root).root);
            }
        }
        shafts = s;
        ext.set("scalp", scalp, 1);
    }

    StrandsPtr feathers() const {
        auto op = OperatorRegistry::instance().create(op::kPropagate);
        op->params().set("count", std::int64_t{12});
        op->params().set("segments", std::int64_t{6});
        op->params().set("length", 0.35);
        op->params().set("angle", 55.0);
        op->params().set("droop", 12.0);
        op->params().set("curl", 0.0);
        return run(*op, {shafts});
    }
    StrandsPtr run(const Operator& op, const std::vector<Value>& inputs) const {
        EvalContext ctx;
        ctx.externals = &ext;
        return valueAs<StrandsPtr>(op.evaluate(ctx, inputs));
    }
};

std::unique_ptr<Operator> plume(double offset = 0.02, double bodyOffset = 0.01) {
    auto op = OperatorRegistry::instance().create(op::kPlume);
    op->params().set("offset", offset);
    op->params().set("bodyOffset", bodyOffset);
    op->params().set("iterations", std::int64_t{6});
    return op;
}

// Each feather's strands (by featherId) and root (its shaft's first point).
struct Group {
    std::vector<std::size_t> strands;
    Vec3f root;
    float reach = 0.0f;
};
std::map<int, Group> groupsOf(const StrandSet& s) {
    std::map<int, Group> out;
    const auto& ids = *s.strandChannel("featherId");
    const auto& shaft = *s.strandChannel("isShaft");
    for (std::size_t k = 0; k < s.strandCount(); ++k) {
        Group& g = out[static_cast<int>(ids[k])];
        g.strands.push_back(k);
        if (shaft[k] > 0.5f) g.root = s.strandPoints(k)[0];
    }
    for (auto& [id, g] : out) {
        for (const std::size_t k : g.strands) {
            for (std::size_t i = 0; i < s.strandPointCount(k); ++i) g.reach = std::max(g.reach, (s.strandPoints(k)[i] - g.root).length());
        }
    }
    return out;
}

float pointSegment(const Vec3f& p, const Vec3f& a, const Vec3f& b) {
    const Vec3f ab = b - a;
    const float l2 = ab.dot(ab);
    const float t = l2 > 0.0f ? std::clamp((p - a).dot(ab) / l2, 0.0f, 1.0f) : 0.0f;
    return (p - (a + ab * t)).length();
}

// The smallest gap between a feather's points (past its root zone: a tenth of its reach) and the
// strands of every feather under it (`lower(a, b)`: a lies under b).
template <class Lower>
float minGap(const StrandSet& s, const Lower& lower) {
    const auto groups = groupsOf(s);
    float gap = std::numeric_limits<float>::max();
    for (const auto& [b, gb] : groups) {
        for (const auto& [a, ga] : groups) {
            if (a == b || !lower(ga, a, gb, b) || (ga.root - gb.root).length() > ga.reach + gb.reach) continue;
            for (const std::size_t sb : gb.strands) {
                const Vec3f* pb = s.strandPoints(sb);
                for (std::size_t i = 0; i < s.strandPointCount(sb); ++i) {
                    if ((pb[i] - gb.root).length() < 0.1f * gb.reach) continue;
                    for (const std::size_t sa : ga.strands) {
                        const Vec3f* pa = s.strandPoints(sa);
                        for (std::size_t j = 1; j < s.strandPointCount(sa); ++j) gap = std::min(gap, pointSegment(pb[i], pa[j - 1], pa[j]));
                    }
                }
            }
        }
    }
    return gap;
}

// The lowest point past the root zones (the plane is y = 0).
float lowestOffRoot(const StrandSet& s) {
    float lowest = std::numeric_limits<float>::max();
    for (const auto& [id, g] : groupsOf(s)) {
        for (const std::size_t k : g.strands) {
            for (std::size_t i = 0; i < s.strandPointCount(k); ++i) {
                const Vec3f& p = s.strandPoints(k)[i];
                if ((p - g.root).length() >= 0.1f * g.reach) lowest = std::min(lowest, p.y);
            }
        }
    }
    return lowest;
}

// The bed's order with rankBy rootDistance's automatic axis (against the feathers' way, -X):
// feathers rooted nearer -X lie on top; equal ranks by featherId.
bool underInBed(const Group& a, int ia, const Group& b, int ib) {
    if (a.root.x != b.root.x) return -a.root.x < -b.root.x;
    return ia < ib;
}

TEST(Plume, LaysEachFeatherOverTheOnesUnderItAndOffTheBody) {
    const FeatherBed bed;
    const StrandsPtr before = bed.feathers();
    const auto op = plume(0.02, 0.01);
    const StrandsPtr after = bed.run(*op, {before});
    std::string error;
    ASSERT_TRUE(after->validate(&error)) << error;
    ASSERT_EQ(after->strandCount(), before->strandCount());
    EXPECT_EQ(after->ids(), before->ids());
    EXPECT_EQ(after->offsets(), before->offsets());
    // Before: vanes through one another, barbs under the skin.
    EXPECT_LT(minGap(*before, underInBed), 0.002f);
    EXPECT_LT(lowestOffRoot(*before), -0.01f);
    // After: each above the ones under it, by about the offset, and above the body.
    const float gap = minGap(*after, underInBed);
    EXPECT_GT(gap, 0.25f * 0.02f) << gap;
    EXPECT_GT(lowestOffRoot(*after), 0.5f * 0.01f);
    // Roots stay where they were (on the skin); a rigid turn keeps every length from the root.
    const auto groupsBefore = groupsOf(*before), groupsAfter = groupsOf(*after);
    for (const auto& [id, g] : groupsBefore) {
        EXPECT_EQ(groupsAfter.at(id).root, g.root);
        for (const std::size_t k : g.strands) {
            for (std::size_t i = 0; i < before->strandPointCount(k); ++i) {
                EXPECT_NEAR((after->strandPoints(k)[i] - g.root).length(), (before->strandPoints(k)[i] - g.root).length(), 1e-4f);
            }
        }
    }
    // The vane normals turned with their feathers: still across each barb's shaft.
    const auto* nx = after->strandChannel("vaneNormalX");
    const auto* ny = after->strandChannel("vaneNormalY");
    ASSERT_TRUE(nx && ny);
    bool tilted = false;
    for (std::size_t k = 0; k < after->strandCount(); ++k) tilted = tilted || ((*ny)[k] < 0.999f && std::fabs((*nx)[k]) > 1e-3f);
    EXPECT_TRUE(tilted);
}

TEST(Plume, BendingFeathersResolveToo) {
    const FeatherBed bed;
    const StrandsPtr before = bed.feathers();
    auto op = plume(0.02, 0.01);
    op->params().set("stiffness", 0.0);
    const StrandsPtr bent = bed.run(*op, {before});
    EXPECT_GT(minGap(*bent, underInBed), 0.2f * 0.02f);
    EXPECT_GT(lowestOffRoot(*bent), 0.5f * 0.01f);
    op->params().set("stiffness", 1.0);
    const StrandsPtr rigid = bed.run(*op, {before});
    // A bend turns the feather less near its root than at the contacts: not the rigid turn.
    EXPECT_NE(bent->positions(), rigid->positions());
}

TEST(Plume, TheRankDecidesWhichLiesOnTop) {
    // Two feathers crossing in an X on the plane; a channel ranks them.
    const MeshPtr scalp = test::makePlane(16, 4.0f);
    auto shafts = std::make_shared<StrandSet>();
    const Vec3f roots[2] = {{1.0f, 0.0f, 1.0f}, {2.4f, 0.0f, 1.0f}};
    const Vec3f ways[2] = {Vec3f{1.0f, 0.0f, 1.0f}.normalized(), Vec3f{-1.0f, 0.0f, 1.0f}.normalized()};
    for (int f = 0; f < 2; ++f) {
        std::vector<Vec3f> pts;
        for (int k = 0; k <= 10; ++k) pts.push_back(roots[f] + ways[f] * (0.16f * static_cast<float>(k)));
        shafts->addStrand(static_cast<StrandId>(f + 1), pts, std::vector<float>(pts.size(), 0.01f), scalp->closestPoint(roots[f]).root);
    }
    std::vector<float>& rank = shafts->addStrandChannel("rank", 0.0f);
    ExternalInputs ext;
    ext.set("scalp", scalp, 1);
    EvalContext ctx;
    ctx.externals = &ext;
    auto propagate = OperatorRegistry::instance().create(op::kPropagate);
    propagate->params().set("count", std::int64_t{16});
    propagate->params().set("length", 0.3);
    propagate->params().set("droop", 0.0);
    auto op = plume(0.03, 0.0);
    op->params().set("rankBy", std::string("channel"));
    op->params().set("rankChannel", std::string("rank"));
    // The height of each feather's shaft (the shaft keeps its ID: feather f's is f + 1) where the two
    // cross (x = 1.7).
    auto shaftHeightAtCrossing = [](const StrandSet& s, int feather) {
        const auto& isShaft = *s.strandChannel("isShaft");
        for (std::size_t k = 0; k < s.strandCount(); ++k) {
            if (s.ids()[k] != static_cast<StrandId>(feather + 1) || isShaft[k] < 0.5f) continue;
            const Vec3f* p = s.strandPoints(k);
            for (std::size_t i = 1; i < s.strandPointCount(k); ++i) {
                if ((p[i - 1].x - 1.7f) * (p[i].x - 1.7f) <= 0.0f) return 0.5f * (p[i - 1].y + p[i].y);
            }
        }
        return -1.0f;
    };
    for (const int top : {0, 1}) {
        rank[0] = top == 0 ? 1.0f : 0.0f;
        rank[1] = top == 1 ? 1.0f : 0.0f;
        const StrandsPtr feathers = valueAs<StrandsPtr>(propagate->evaluate(ctx, {StrandsPtr(shafts)}));
        const StrandsPtr out = valueAs<StrandsPtr>(op->evaluate(ctx, {feathers}));
        const float upper = shaftHeightAtCrossing(*out, top), lower = shaftHeightAtCrossing(*out, 1 - top);
        EXPECT_GT(upper, lower + 0.02f) << "top " << top;
        EXPECT_EQ(lower, 0.0f) << "the lowest feather stays on the plane";  // (bodyOffset 0)
        // invert turns the order over.
        op->params().set("invert", true);
        const StrandsPtr inverted = valueAs<StrandsPtr>(op->evaluate(ctx, {feathers}));
        op->params().set("invert", false);
        EXPECT_GT(shaftHeightAtCrossing(*inverted, 1 - top), shaftHeightAtCrossing(*inverted, top) + 0.02f) << "top " << top;
    }
}

TEST(Plume, OffIsTheInputAsItCame) {
    const FeatherBed bed;
    const StrandsPtr before = bed.feathers();
    auto op = plume();
    op->params().set("strength", 0.0);
    EXPECT_EQ(bed.run(*op, {before}), before);  // nothing turned: the input itself
    op->params().set("strength", 1.0);
    op->params().set("iterations", std::int64_t{0});
    EXPECT_EQ(bed.run(*op, {before}), before);
    // Strands that are not feathers pass through.
    EXPECT_EQ(bed.run(*op, {bed.shafts}), bed.shafts);
    op->params().set("iterations", std::int64_t{4});
    EXPECT_EQ(bed.run(*op, {bed.shafts}), bed.shafts);
}

TEST(Plume, TheSameOnAnyNumberOfThreads) {
    const FeatherBed bed(9, 6);
    const StrandsPtr before = bed.feathers();
    auto op = plume();
    op->params().set("stiffness", 0.5);
    const StrandsPtr many = bed.run(*op, {before});
    StrandsPtr one;
#if GROOMLAB_HAS_TBB
    tbb::task_arena single(1);
    single.execute([&] { one = bed.run(*op, {before}); });
#else
    one = bed.run(*op, {before});
#endif
    ASSERT_EQ(one->pointCount(), many->pointCount());
    EXPECT_EQ(std::memcmp(one->positions().data(), many->positions().data(), many->pointCount() * sizeof(Vec3f)), 0);
    const StrandsPtr again = bed.run(*op, {before});
    EXPECT_EQ(std::memcmp(again->positions().data(), many->positions().data(), many->pointCount() * sizeof(Vec3f)), 0);
}

TEST(Plume, AnEditOfTheFeathersInOnePlaceLeavesTheOthersAsTheyWere) {
    // 20 x 20 feathers on a plane, pointing about +X (their ways turning slowly across it): the
    // automatic order lays the ones rooted farther back (toward -X) over the ones ahead. The edit
    // turns the ones rooted before x = 3 to +Z or -Z and makes them half again as long. The feathers
    // rooted past x = 5.9 (out of reach of those, and under them: what they lie over is farther
    // ahead) come out bit for bit the same: what lies over what is decided among neighbours (the
    // automatic order's way is each pair's), and how deep a feather is pushed out of the body goes
    // by its own length.
    const MeshPtr scalp = test::makePlane(48, 12.0f);
    ExternalInputs ext;
    ext.set("scalp", scalp, 1);
    EvalContext ctx;
    ctx.externals = &ext;
    auto propagate = OperatorRegistry::instance().create(op::kPropagate);
    propagate->params().set("count", std::int64_t{12});
    propagate->params().set("segments", std::int64_t{6});
    propagate->params().set("length", 0.35);
    propagate->params().set("angle", 55.0);
    propagate->params().set("droop", 12.0);
    propagate->params().set("curl", 0.0);
    auto bed = [&](float edited) {  // the edited feathers' way along Z (0: none)
        auto s = std::make_shared<StrandSet>();
        StrandId id = 1;
        for (int j = 0; j < 20; ++j) {
            for (int i = 0; i < 20; ++i) {
                const Vec3f root{0.6f + 0.45f * static_cast<float>(i), 0.0f, 1.0f + 0.4f * static_cast<float>(j)};
                const float turn = 0.25f * std::sin(0.7f * root.x + 0.3f * root.z);
                Vec3f way{std::cos(turn), 0.0f, std::sin(turn)};
                float length = 1.0f;
                if (edited != 0.0f && root.x < 3.0f) {
                    way = {0.0f, 0.0f, edited};
                    length = 1.5f;
                }
                std::vector<Vec3f> pts;
                for (int k = 0; k <= 10; ++k) pts.push_back(root + way * (0.1f * length * static_cast<float>(k)));
                s->addStrand(id++, pts, std::vector<float>(pts.size(), 0.01f), scalp->closestPoint(root).root);
            }
        }
        return valueAs<StrandsPtr>(propagate->evaluate(ctx, {StrandsPtr(s)}));
    };
    const auto op = plume(0.02, 0.01);
    const StrandsPtr feathers = bed(0.0f);
    for (const float edit : {1.0f, -1.0f}) {
        for (const bool bend : {false, true}) {
            op->params().set("stiffness", bend ? 0.3 : 1.0);
            const StrandsPtr before = valueAs<StrandsPtr>(op->evaluate(ctx, {feathers}));
            const StrandsPtr after = valueAs<StrandsPtr>(op->evaluate(ctx, {bed(edit)}));
            ASSERT_EQ(before->ids(), after->ids());
            std::size_t far = 0, moved = 0, turned = 0;
            for (const auto& [id, g] : groupsOf(*before)) {
                if (!(g.root.x > 5.9f)) continue;
                ++far;
                bool same = true, lifted = false;
                for (const std::size_t k : g.strands) {
                    const std::size_t begin = before->strandBegin(k), n = before->strandPointCount(k);
                    same = same && std::memcmp(before->positions().data() + begin, after->positions().data() + begin,
                                               n * sizeof(Vec3f)) == 0;
                    lifted = lifted || std::memcmp(before->positions().data() + begin, feathers->positions().data() + begin,
                                                   n * sizeof(Vec3f)) != 0;
                }
                moved += same ? 0 : 1;
                turned += lifted ? 1 : 0;
            }
            std::printf("[ PLUME    ] %s, edited to %+.0fZ: of %zu far feathers %zu turned by Plume, %zu changed by the edit\n",
                        bend ? "bending" : "rigid", edit, far, turned, moved);
            EXPECT_GT(far, 150u);
            EXPECT_GT(turned, far / 2);  // (the feathers do stack)
            EXPECT_EQ(moved, 0u);
        }
    }
}

// Scatter on a sphere, laid down (Direction), feathers (Propagate), Plume: through a groom.
json birdGraph(double density, bool plumed, double affect = 1.0) {
    json nodes = json::array({
        {{"name", "scalp"}, {"type", "Scalp"}},
        {{"name", "shafts"}, {"type", "Scatter"}, {"params", {{"density", density}, {"length", 0.6}, {"seed", 3}}}},
        {{"name", "lay"}, {"type", "Direction"}, {"params", {{"inclination", 84.0}, {"along", "direction"}}}},
        {{"name", "feathers"}, {"type", "Propagate"}, {"params", {{"count", 14}, {"segments", 5}, {"length", 0.22}, {"droop", 30.0}}}},
        {{"name", "output"}, {"type", "Output"}},
    });
    json links = json::array({
        {{"from", "scalp"}, {"to", "shafts"}, {"input", "scalp"}},
        {{"from", "shafts"}, {"to", "lay"}, {"input", "strands"}},
        {{"from", "lay"}, {"to", "feathers"}, {"input", "shafts"}},
    });
    if (plumed) {
        nodes.push_back({{"name", "plume"}, {"type", "Plume"}, {"params", {{"offset", 0.01}, {"bodyOffset", 0.005}, {"affect", affect}}}});
        links.push_back({{"from", "feathers"}, {"to", "plume"}, {"input", "strands"}});
        links.push_back({{"from", "plume"}, {"to", "output"}, {"input", "strands"}});
    } else {
        links.push_back({{"from", "feathers"}, {"to", "output"}, {"input", "strands"}});
    }
    return {{"version", 1}, {"nodes", nodes}, {"connections", links}, {"output", "output"}};
}

// ---- Instance deform --------------------------------------------------------------------------

// A prototype: a ribbon along +Y from 0 to 1, `rows` rows of two vertices at x = -0.5 and 0.5.
InstancePrototype ribbon(int rows = 9) {
    InstancePrototype p;
    for (int r = 0; r < rows; ++r) {
        const float y = static_cast<float>(r) / static_cast<float>(rows - 1);
        p.points.push_back({-0.5f, y, 0.0f});
        p.points.push_back({0.5f, y, 0.0f});
        p.normals.push_back({0.0f, 0.0f, 1.0f});
        p.normals.push_back({0.0f, 0.0f, 1.0f});
    }
    for (int r = 0; r + 1 < rows; ++r) {
        const auto a = static_cast<std::uint32_t>(2 * r);
        for (const std::uint32_t v : {a, a + 1, a + 2, a + 1, a + 3, a + 2}) p.triangles.push_back(v);
    }
    p.signature = 0x51B60;
    p.found = true;
    return p;
}

// A strand bent into a quarter circle of radius 1 in the XY plane (from the origin up +Y, over to
// +X), many points: its arc length is pi / 2.
std::vector<Vec3f> quarterCircle(int points = 41) {
    std::vector<Vec3f> pts;
    for (int i = 0; i < points; ++i) {
        const float a = 1.5707963f * static_cast<float>(i) / static_cast<float>(points - 1);
        pts.push_back({1.0f - std::cos(a), std::sin(a), 0.0f});
    }
    return pts;
}

// The point `d` along a polyline from its first point.
Vec3f alongPolyline(const std::vector<Vec3f>& pts, float d) {
    for (std::size_t i = 1; i < pts.size(); ++i) {
        const float len = (pts[i] - pts[i - 1]).length();
        if (d <= len || i + 1 == pts.size()) return pts[i - 1] + (pts[i] - pts[i - 1]) * (len > 0.0f ? d / len : 0.0f);
        d -= len;
    }
    return pts.back();
}

TEST(InstanceDeform, VerticesFollowTheStrandByArcLength) {
    StrandSet strands;
    const std::vector<Vec3f> curve = quarterCircle();
    strands.addStrand(5, curve, std::vector<float>(curve.size(), 0.1f));
    const float length = arcLength(curve.data(), curve.size());
    const InstancePrototype proto = ribbon();
    auto lookup = [&](const std::string& name) -> const InstancePrototype* { return name == "ribbon" ? &proto : nullptr; };
    auto set = std::make_shared<InstanceSet>();
    set->prototypes = {"ribbon"};
    InstanceSet::Instance inst;
    inst.strand = 5;
    inst.id = 50;
    inst.t = 0.2f;  // starts a fifth of the way along
    inst.fitStrand = true;
    inst.scale = 0.6f;  // 60% of the strand's length along it
    inst.aspect = 0.1f;
    inst.deform = true;
    set->instances = {inst};
    strands.setInstances(set);
    ASSERT_TRUE(set->hasMeshes());
    EXPECT_FALSE(set->hasScales());
    const auto placed = set->resolve(strands);
    ASSERT_EQ(placed.size(), 1u);
    EXPECT_TRUE(placed[0].meshed);
    EXPECT_NEAR(placed[0].distance, 0.2f * length, 1e-4f);
    const auto mesh = buildScaleMesh(strands, nullptr, lookup);
    ASSERT_TRUE(mesh);
    ASSERT_EQ(mesh->scales.size(), 1u);
    const ScaleMesh::Scale& sc = mesh->scales[0];
    ASSERT_EQ(sc.vertexCount, proto.points.size());
    const float along = 0.6f * length, across = along * 0.1f;
    for (std::size_t v = 0; v < sc.vertexCount; ++v) {
        const Vec3f& q = proto.points[v];
        const Vec3f& w = mesh->points[sc.firstVertex + v];
        // The middle of each row lies on the strand where its arc length says; the row's ends
        // are half the width across it, in the strand's plane's normal (no twist).
        const Vec3f onStrand = alongPolyline(curve, 0.2f * length + q.y * along);
        EXPECT_NEAR((w - onStrand).length(), 0.5f * across, 2e-3f) << v;
        if (v % 2 == 1) {  // the row's pair: its middle on the strand
            const Vec3f mid = (w + mesh->points[sc.firstVertex + v - 1]) * 0.5f;
            EXPECT_LT((mid - onStrand).length(), 2e-3f) << v;
        }
    }
    // No twist: on a strand in a plane, each row's width axis keeps its angle to the plane, and
    // every normal its own.
    const Vec3f firstRow = mesh->points[sc.firstVertex + 1] - mesh->points[sc.firstVertex];
    const float firstNormal = mesh->normals[sc.firstVertex].z;
    for (std::size_t v = 1; v < sc.vertexCount; v += 2) {
        const Vec3f row = mesh->points[sc.firstVertex + v] - mesh->points[sc.firstVertex + v - 1];
        EXPECT_NEAR(row.z, firstRow.z, 1e-3f * across + 1e-6f) << v;
        EXPECT_NEAR(row.length(), across, 1e-4f) << v;
        EXPECT_NEAR(mesh->normals[sc.firstVertex + v].z, firstNormal, 1e-3f) << v;
    }
}

TEST(InstanceDeform, OnAStraightStrandItIsTheRigidPlacement) {
    StrandSet strands;
    const std::vector<Vec3f> line = test::straightCurve({0.2f, 0.0f, 0.1f}, Vec3f{0.3f, 1.0f, 0.2f}.normalized(), 8);
    strands.addStrand(9, line, std::vector<float>(line.size(), 0.1f));
    const InstancePrototype proto = ribbon(5);
    auto lookup = [&](const std::string&) -> const InstancePrototype* { return &proto; };
    auto set = std::make_shared<InstanceSet>();
    set->prototypes = {"ribbon"};
    InstanceSet::Instance inst;
    inst.strand = 9;
    inst.id = 90;
    inst.t = 0.25f;
    inst.scale = 0.3f;
    inst.offset = 0.05f;
    inst.spin = 0.7f;
    inst.deform = true;
    set->instances = {inst};
    strands.setInstances(set);
    const auto mesh = buildScaleMesh(strands, nullptr, lookup);
    ASSERT_TRUE(mesh);
    const InstanceSet::Placed p = set->resolve(strands)[0];
    for (std::size_t v = 0; v < proto.points.size(); ++v) {
        const Vec3f& q = proto.points[v];
        const Vec3f rigid = p.origin + p.x * q.x + p.y * q.y + p.z * q.z;
        EXPECT_LT((mesh->points[v] - rigid).length(), 1e-4f) << v;
    }
}

TEST(InstanceDeform, NoneIsAsBefore) {
    // deform none: the same instances, hash and placements as an Instance node always made; the
    // mesh builder makes nothing of them.
    const MeshPtr plane = test::makePlane(4, 2.0f);
    auto strands = std::make_shared<StrandSet>();
    for (int i = 0; i < 6; ++i) {
        const Vec3f root{0.3f * static_cast<float>(i) + 0.1f, 0.0f, 0.5f};
        strands->addStrand(static_cast<StrandId>(i + 1), test::straightCurve(root, {0, 1, 0}, 6), std::vector<float>(7, 0.05f),
                           plane->closestPoint(root).root);
    }
    ExternalInputs ext;
    ext.set("scalp", plane, 1);
    EvalContext ctx;
    ctx.externals = &ext;
    auto op = OperatorRegistry::instance().create(op::kInstance);
    op->params().set("probability", 1.0);
    op->params().set("count", std::int64_t{2});
    const StrandsPtr none = valueAs<StrandsPtr>(op->evaluate(ctx, {StrandsPtr(strands)}));
    for (const auto& i : none->instances()->instances) {
        EXPECT_FALSE(i.deform);
        EXPECT_FALSE(i.meshed());
    }
    EXPECT_FALSE(none->instances()->hasMeshes());
    EXPECT_FALSE(buildScaleMesh(*none, plane));
    for (const auto& p : none->instances()->resolve(*none)) EXPECT_FALSE(p.meshed);
    op->params().set("deform", std::string("alongStrand"));
    const StrandsPtr bent = valueAs<StrandsPtr>(op->evaluate(ctx, {StrandsPtr(strands)}));
    ASSERT_EQ(bent->instances()->size(), none->instances()->size());
    EXPECT_NE(bent->instances()->contentHash(), none->instances()->contentHash());
    for (std::size_t k = 0; k < bent->instances()->size(); ++k) {
        const auto& a = none->instances()->instances[k];
        const auto& b = bent->instances()->instances[k];
        EXPECT_TRUE(b.deform);
        EXPECT_EQ(a.id, b.id);
        EXPECT_EQ(a.t, b.t);
        EXPECT_EQ(a.scale, b.scale);
    }
    // A host's instancer skips them (a mesh of their own), the mesh builder makes them.
    EXPECT_TRUE(bent->instances()->hasMeshes());
    const InstancePrototype proto = ribbon();
    const auto mesh = buildScaleMesh(*bent, plane, [&](const std::string&) -> const InstancePrototype* { return &proto; });
    ASSERT_TRUE(mesh);
    EXPECT_EQ(mesh->scales.size(), bent->instances()->size());
}

TEST(InstanceDeform, FacesTheFeathersVaneAndTakesAnotherAxis) {
    // A barb of a feather lying on the plane: bent along it, +Z faces the vane's normal (+Y).
    const MeshPtr plane = test::makePlane(8, 2.0f);
    auto shaft = std::make_shared<StrandSet>();
    std::vector<Vec3f> pts;
    for (int k = 0; k <= 10; ++k) pts.push_back({0.5f + 0.08f * static_cast<float>(k), 0.0f, 1.0f});
    shaft->addStrand(1, pts, std::vector<float>(pts.size(), 0.01f), plane->closestPoint(pts[0]).root);
    ExternalInputs ext;
    ext.set("scalp", plane, 1);
    EvalContext ctx;
    ctx.externals = &ext;
    auto propagate = OperatorRegistry::instance().create(op::kPropagate);
    propagate->params().set("count", std::int64_t{4});
    propagate->params().set("droop", 0.0);
    propagate->params().set("curl", 0.0);
    propagate->params().set("keepShaft", false);
    const StrandsPtr barbs = valueAs<StrandsPtr>(propagate->evaluate(ctx, {StrandsPtr(shaft)}));
    auto instance = OperatorRegistry::instance().create(op::kInstance);
    instance->params().set("probability", 1.0);
    instance->params().set("tMin", 0.0);
    instance->params().set("tMax", 0.0);
    instance->params().set("spinRandom", 0.0);
    instance->params().set("deform", std::string("alongStrand"));
    const StrandsPtr out = valueAs<StrandsPtr>(instance->evaluate(ctx, {barbs}));
    for (const auto& p : out->instances()->resolve(*out, plane.get())) EXPECT_GT(p.z.normalized().y, 0.999f);
    // deformAxis z: the prototype's +Z laid along the strand (a ribbon along Z bends as one along Y).
    InstancePrototype alongZ = ribbon();
    for (Vec3f& q : alongZ.points) q = {q.z, q.x, q.y};  // (x, y, z) -> (z, x, y): y's role is now z's
    for (Vec3f& n : alongZ.normals) n = {n.z, n.x, n.y};
    const InstancePrototype alongY = ribbon();
    instance->params().set("deformAxis", std::string("z"));
    const StrandsPtr zOut = valueAs<StrandsPtr>(instance->evaluate(ctx, {barbs}));
    const auto zMesh = buildScaleMesh(*zOut, plane, [&](const std::string&) -> const InstancePrototype* { return &alongZ; });
    const auto yMesh = buildScaleMesh(*out, plane, [&](const std::string&) -> const InstancePrototype* { return &alongY; });
    ASSERT_TRUE(zMesh && yMesh);
    ASSERT_EQ(zMesh->points.size(), yMesh->points.size());
    for (std::size_t v = 0; v < yMesh->points.size(); ++v) EXPECT_LT((zMesh->points[v] - yMesh->points[v]).length(), 1e-5f) << v;
}

}  // namespace
}  // namespace groomlab
