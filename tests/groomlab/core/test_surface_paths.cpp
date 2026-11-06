// SPDX-License-Identifier: Apache-2.0
// Paths along the skin (surface_paths.h) and what Interpolate makes of them: its frame transfer
// and followSurface.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "groomlab/curve.h"
#include "groomlab/graph.h"
#include "groomlab/guide_set.h"
#include "groomlab/operator_state.h"
#include "groomlab/operators.h"
#include "groomlab/random.h"
#include "groomlab/strand_set.h"
#include "groomlab/surface_paths.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

constexpr float kPi = 3.14159265f;

// Strands rooted on `scalp` at the closest points to `roots`: `points` points straight out along
// the normal, 0.3 long (the incoming shape Interpolate replaces).
StrandsPtr strandsOn(const ScalpMesh& scalp, const std::vector<Vec3f>& roots, int points = 8) {
    auto strands = std::make_shared<StrandSet>();
    for (std::size_t s = 0; s < roots.size(); ++s) {
        const SurfacePoint p = scalp.closestPoint(roots[s]);
        strands->addStrand(static_cast<StrandId>(s + 1), test::straightCurve(p.position, p.normal * 0.3f, points - 1), {}, p.root);
    }
    return strands;
}

// Straight guides from (the closest scalp points to) each root along its direction.
GuidesPtr guidesOn(const ScalpMesh& scalp, const std::vector<std::pair<Vec3f, Vec3f>>& rootsAndDirections) {
    auto guides = std::make_shared<GuideSet>();
    std::uint64_t id = 1;
    for (const auto& [root, direction] : rootsAndDirections) {
        const SurfacePoint p = scalp.closestPoint(root);
        Guide& g = guides->add(id++, test::straightCurve(p.position, direction, 6));
        g.root = p.root;
    }
    return guides;
}

StrandsPtr interpolate(const Operator& op, const StrandsPtr& strands, const GuidesPtr& guides, const MeshPtr& scalp) {
    ExternalInputs ext;
    EvalContext ctx;
    ctx.externals = &ext;
    return valueAs<StrandsPtr>(op.evaluate(ctx, {Value(strands), Value(guides), Value(scalp)}));
}

// (Not keeping clearance: these tests measure the frame transfer itself, where strands lying on the
// skin would sit kClearanceFloor above it.)
std::unique_ptr<Operator> interpolateOp(bool followSurface) {
    auto op = OperatorRegistry::instance().create(op::kInterpolate);
    op->params().set("followSurface", followSurface);
    op->params().set("keepClearance", false);
    return op;
}

Vec3f tipOffset(const StrandSet& s, std::size_t i) {
    const Vec3f* p = s.strandPoints(i);
    return p[s.strandPointCount(i) - 1] - p[0];
}

bool sameBits(const StrandSet& a, const StrandSet& b) {
    return a.positions().size() == b.positions().size() &&
           std::memcmp(a.positions().data(), b.positions().data(), a.positions().size() * sizeof(Vec3f)) == 0;
}

// The fin's two faces (z = -0.05 and +0.05, 2 high, 2 deep) and the points of a grid on each,
// below `top`.
const float kFinThickness = 0.1f;
std::vector<Vec3f> finGrid(float side, float spacing, float top) {
    std::vector<Vec3f> out;
    for (float x = 0.5f * spacing; x < 2.0f; x += spacing) {
        for (float y = 0.5f * spacing; y < top; y += spacing) out.push_back({x, y, side * 0.5f * kFinThickness});
    }
    return out;
}

// The path from fronts' source 0 to 	o.
SurfacePath pathTo(const SurfaceFronts& fronts, const SurfacePoint& to) {
    const Vec3f d = to.position - fronts.front(0)->position;
    return fronts.path(0, SurfacePathEnd(to.root, to.position), d.dot(d));
}

// ---- Paths ----------------------------------------------------------------------------------------

TEST(SurfacePaths, OnAPlaneEveryPathIsTheStraightLine) {
    const auto plane = test::makePlane(20, 2.0f);
    const SurfaceGraph graph(*plane);
    const SurfacePoint source = plane->closestPoint({1.03f, 0.0f, 0.97f});
    const auto front = surfaceFront(graph, source.root, 0.7f);
    ASSERT_GT(front->reached.size(), 50u);
    for (const SurfaceFront::Reached& r : front->reached) {
        EXPECT_EQ(r.anchor, 0u) << "a bend on a plane";
        EXPECT_EQ(r.distance, (graph.position(r.vertex) - source.position).length());
    }
    const SurfaceFronts fronts(sharedSurfaceGraph(*plane), {source.root}, {0.7f}, nullptr);
    Rng rng(5);
    for (int i = 0; i < 200; ++i) {
        const SurfacePoint p = plane->closestPoint({1.0f + rng.uniform(-0.5f, 0.5f), 0.0f, 1.0f + rng.uniform(-0.5f, 0.5f)});
        const SurfacePath path = pathTo(fronts, p);
        EXPECT_EQ(path.kind, SurfacePath::Kind::Straight);
    }
    // Past the limit (and its edges): not reached.
    const SurfacePoint far = plane->closestPoint({1.9f, 0.0f, 1.9f});
    EXPECT_EQ(pathTo(fronts, far).kind, SurfacePath::Kind::Beyond);
}

TEST(SurfacePaths, GoRoundAFoldNotThroughIt) {
    // From one face of a thin flap to the point just behind it on the other: over the top.
    const MeshPtr fin = test::makeFin(2.0f, kFinThickness, 1.0f, 0.05f);
    const SurfacePoint from = fin->closestPoint({0.5f, 0.5f, -0.05f});
    const SurfacePoint to = fin->closestPoint({0.5f, 0.5f, 0.05f});
    const SurfaceFronts fronts(sharedSurfaceGraph(*fin), {from.root}, {std::numeric_limits<float>::infinity()}, nullptr);
    const SurfacePath path = pathTo(fronts, to);
    ASSERT_EQ(path.kind, SurfacePath::Kind::Bent);
    const float around = 2.0f * 1.5f + kPi * 0.5f * kFinThickness;  // up, over, down
    EXPECT_NEAR(path.distance, around, 0.03f * around);
    // Over the rim the two faces' normals face apart: the frame comes over it with its twist.
    EXPECT_GT(std::abs(path.twist), 0.1f);
    // Up the same face: the straight line.
    const SurfacePoint up = fin->closestPoint({0.6f, 1.2f, -0.05f});
    EXPECT_EQ(pathTo(fronts, up).kind, SurfacePath::Kind::Straight);
}

TEST(SurfacePaths, GoRoundAGapInTheMesh) {
    // A plane with a slot cut into it (an ear cut out of a scalp): across the slot, round its end.
    const auto plane = test::makePlane(40, 1.0f);
    std::vector<ScalpMesh::Triangle> kept;
    for (const ScalpMesh::Triangle& t : plane->triangles()) {
        const Vec3f c = (plane->points()[t[0]] + plane->points()[t[1]] + plane->points()[t[2]]) * (1.0f / 3.0f);
        if (std::abs(c.x - 0.5f) < 0.03f && c.z < 0.7f) continue;
        kept.push_back(t);
    }
    const auto slotted = std::make_shared<const ScalpMesh>(plane->points(), kept);
    const SurfacePoint from = slotted->closestPoint({0.4f, 0.0f, 0.2f});
    const SurfacePoint to = slotted->closestPoint({0.6f, 0.0f, 0.2f});
    const SurfaceFronts fronts(sharedSurfaceGraph(*slotted), {from.root}, {2.0f}, nullptr);
    const SurfacePath path = pathTo(fronts, to);
    ASSERT_EQ(path.kind, SurfacePath::Kind::Bent);
    const float around = 2.0f * std::hypot(0.1f, 0.5f);  // to the slot's end and back
    EXPECT_GT(path.distance, 0.9f * around);
    EXPECT_LT(path.distance, 1.15f * around);
}

TEST(SurfacePaths, ASeamIsNoEdge) {
    // The plane's vertices split along x = 0.5 (as a scan's UV seams split them): one surface still.
    const auto plane = test::makePlane(20, 1.0f);
    std::vector<Vec3f> points = plane->points();
    std::vector<ScalpMesh::Triangle> triangles = plane->triangles();
    std::vector<std::int32_t> copyOf(points.size(), -1);
    for (ScalpMesh::Triangle& t : triangles) {
        const Vec3f c = (points[t[0]] + points[t[1]] + points[t[2]]) * (1.0f / 3.0f);
        if (c.x < 0.5f) continue;
        for (std::int32_t& v : t) {
            if (points[v].x != 0.5f) continue;
            if (copyOf[v] < 0) {
                copyOf[v] = static_cast<std::int32_t>(points.size());
                points.push_back(points[v]);
            }
            v = copyOf[v];
        }
    }
    const auto split = std::make_shared<const ScalpMesh>(points, triangles);
    EXPECT_EQ(SurfaceGraph(*split).vertexCount(), plane->pointCount());
    const SurfacePoint from = split->closestPoint({0.35f, 0.0f, 0.5f});
    const SurfaceFronts fronts(sharedSurfaceGraph(*split), {from.root}, {0.5f}, nullptr);
    for (const float x : {0.45f, 0.5f, 0.55f, 0.65f}) {
        const SurfacePoint to = split->closestPoint({x, 0.0f, 0.52f});
        EXPECT_EQ(pathTo(fronts, to).kind, SurfacePath::Kind::Straight) << x;
    }
    // Interpolate across the seam: as on the plane in one piece.
    std::vector<Vec3f> roots;
    for (int i = 0; i < 40; ++i) roots.push_back({0.3f + 0.01f * static_cast<float>(i), 0.0f, 0.5f});
    std::vector<std::pair<Vec3f, Vec3f>> guides;
    for (int i = 0; i < 6; ++i) {
        for (int j = 0; j < 3; ++j) {
            const float x = 0.2f + 0.12f * static_cast<float>(i), z = 0.38f + 0.12f * static_cast<float>(j);
            guides.push_back({{x, 0.0f, z}, {x - 0.5f, 0.6f, 0.1f * static_cast<float>(j)}});
        }
    }
    const auto whole = interpolate(*interpolateOp(true), strandsOn(*plane, roots), guidesOn(*plane, guides), plane);
    const auto cut = interpolate(*interpolateOp(true), strandsOn(*split, roots), guidesOn(*split, guides), split);
    ASSERT_TRUE(whole && cut);
    EXPECT_TRUE(sameBits(*whole, *cut));
}

// ---- Interpolate: followSurface -------------------------------------------------------------------

TEST(Interpolate, FollowingTheSurfaceKeepsAFoldsSidesApart) {
    // A thin flap, guides on both faces: those on the -z face stand straight out of it, those on
    // the +z face are combed along x. Each face's strands take their own face's guides along
    // the skin; by the straight line the faces are 0.1 apart and blend.
    const MeshPtr fin = test::makeFin(2.0f, kFinThickness, 2.0f, 0.05f);
    std::vector<std::pair<Vec3f, Vec3f>> guides;
    for (const Vec3f& p : finGrid(-1.0f, 0.25f, 1.6f)) guides.push_back({p, {0.0f, 0.0f, -0.8f}});
    for (const Vec3f& p : finGrid(1.0f, 0.25f, 1.6f)) guides.push_back({p, {0.8f, 0.0f, 0.0f}});
    std::vector<Vec3f> roots = finGrid(1.0f, 0.07f, 1.4f);  // on the +z face, below the rim
    const StrandsPtr strands = strandsOn(*fin, roots);
    const GuidesPtr g = guidesOn(*fin, guides);
    const StrandsPtr along = interpolate(*interpolateOp(true), strands, g, fin);
    const StrandsPtr straight = interpolate(*interpolateOp(false), strands, g, fin);
    ASSERT_TRUE(along && straight);
    float worstAlong = 0.0f, worstStraight = 0.0f;
    for (std::size_t s = 0; s < strands->strandCount(); ++s) {
        worstAlong = std::max(worstAlong, std::abs(tipOffset(*along, s).z));
        worstStraight = std::max(worstStraight, std::abs(tipOffset(*straight, s).z));
        EXPECT_NEAR(tipOffset(*along, s).x, 0.8f, 0.02f) << s;
    }
    EXPECT_LT(worstAlong, 1e-3f) << "the other face's guides reached through the flap";
    EXPECT_GT(worstStraight, 0.2f) << "the straight line should have blended the faces (the test's premise)";
}

TEST(Interpolate, FollowingTheSurfaceIsTheStraightLineOnASmoothScalp) {
    // On a plane and on a sphere with guides close enough that no path bends: the same strands,
    // to the bit, as by the straight line.
    Rng rng(31);
    {
        const auto plane = test::makePlane(30, 1.0f);
        std::vector<std::pair<Vec3f, Vec3f>> guides;
        for (int i = 0; i < 40; ++i) {
            guides.push_back({{rng.uniform(0.05f, 0.95f), 0.0f, rng.uniform(0.05f, 0.95f)},
                              {rng.uniform(-0.3f, 0.3f), 0.5f, rng.uniform(-0.3f, 0.3f)}});
        }
        std::vector<Vec3f> roots;
        for (int i = 0; i < 3000; ++i) roots.push_back({rng.uniform(0.0f, 1.0f), 0.0f, rng.uniform(0.0f, 1.0f)});
        const StrandsPtr strands = strandsOn(*plane, roots);
        const GuidesPtr g = guidesOn(*plane, guides);
        const auto on = interpolate(*interpolateOp(true), strands, g, plane);
        const auto off = interpolate(*interpolateOp(false), strands, g, plane);
        ASSERT_TRUE(on && off);
        EXPECT_TRUE(sameBits(*on, *off)) << "plane";
    }
    {
        const MeshPtr sphere = test::makeSphere(40, 80, 1.0f);
        std::vector<std::pair<Vec3f, Vec3f>> guides;
        for (int i = 0; i < 150; ++i) {
            const Vec3f n = Vec3f(rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)).normalized();
            guides.push_back({n, n * 0.5f + Vec3f(0.1f, -0.2f, 0.0f)});
        }
        std::vector<Vec3f> roots;
        for (int i = 0; i < 5000; ++i) roots.push_back(Vec3f(rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)).normalized());
        const StrandsPtr strands = strandsOn(*sphere, roots);
        const GuidesPtr g = guidesOn(*sphere, guides);
        const auto on = interpolate(*interpolateOp(true), strands, g, sphere);
        const auto off = interpolate(*interpolateOp(false), strands, g, sphere);
        ASSERT_TRUE(on && off);
        EXPECT_TRUE(sameBits(*on, *off)) << "sphere";
    }
}

TEST(Interpolate, WithoutAScalpTheStraightLine) {
    // No scalp: followSurface changes nothing.
    const auto plane = test::makePlane(10, 1.0f);
    std::vector<Vec3f> roots;
    for (int i = 0; i < 100; ++i) roots.push_back({0.01f * static_cast<float>(i), 0.0f, 0.5f});
    const StrandsPtr strands = strandsOn(*plane, roots);
    const GuidesPtr g = guidesOn(*plane, {{{0.2f, 0, 0.5f}, {0, 1, 0}}, {{0.5f, 0, 0.4f}, {0.3f, 1, 0}}, {{0.8f, 0, 0.6f}, {0, 1, 0.3f}}});
    const auto on = interpolate(*interpolateOp(true), strands, g, nullptr);
    const auto off = interpolate(*interpolateOp(false), strands, g, nullptr);
    ASSERT_TRUE(on && off);
    EXPECT_TRUE(sameBits(*on, *off));
}

// ---- Interpolate: the frame transfer --------------------------------------------------------------

TEST(Interpolate, AGuidesFrameComesRoundAFoldTheSkinsWay) {
    // One guide on the -z face of a thin flap near its top, combed along x (along the rim). Its
    // strands on the +z face get it carried over the rim: still along +x. Turned straight from
    // one face's normal to the other's (they face apart: a half turn about whatever axis), it
    // came out along -x.
    const MeshPtr fin = test::makeFin(2.0f, kFinThickness, 2.0f, 0.05f);
    const GuidesPtr g = guidesOn(*fin, {{{1.0f, 1.8f, -0.05f}, {0.8f, 0.0f, 0.0f}}});
    const StrandsPtr strands = strandsOn(*fin, finGrid(1.0f, 0.1f, 1.95f));
    for (const bool followSurface : {true, false}) {
        const auto out = interpolate(*interpolateOp(followSurface), strands, g, fin);
        ASSERT_TRUE(out);
        for (std::size_t s = 0; s < out->strandCount(); ++s) {
            const Vec3f tip = tipOffset(*out, s);
            EXPECT_NEAR(tip.x, 0.8f, 1e-3f) << s << (followSurface ? " following" : " straight");
            EXPECT_NEAR(tip.z, 0.0f, 1e-3f) << s;
        }
    }
}

// The angle (degrees) between a and b.
float degreesBetween(const Vec3f& a, const Vec3f& b) {
    return std::acos(std::clamp(a.normalized().dot(b.normalized()), -1.0f, 1.0f)) * 180.0f / kPi;
}

TEST(Interpolate, AGuideCombedAlongTheSkinStaysCombedSo) {
    // Guides lying along the skin, combed down the meridians of a sphere and round the tube of a
    // torus: the strands between them lie along the same lines (within a few degrees, what
    // guides a ring apart give), and in the skin's tangent plane.
    const MeshPtr sphere = test::makeSphere(48, 96, 1.0f);
    std::vector<std::pair<Vec3f, Vec3f>> guides;
    auto south = [](const Vec3f& n) { return (Vec3f(0, -1, 0) - n * -n.y).normalized(); };
    for (int i = 0; i < 12; ++i) {
        for (int j = 1; j < 6; ++j) {
            const float phi = 2.0f * kPi * static_cast<float>(i) / 12.0f, theta = kPi * static_cast<float>(j) / 6.0f;
            const Vec3f n(std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi));
            guides.push_back({n, south(n) * 0.4f});
        }
    }
    std::vector<Vec3f> roots;
    for (int i = 0; i < 400; ++i) {
        const float phi = 2.0f * kPi * static_cast<float>(i) / 400.0f, theta = 0.6f + 1.9f * static_cast<float>(i % 50) / 50.0f;
        roots.push_back({std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi)});
    }
    const StrandsPtr strands = strandsOn(*sphere, roots);
    const auto out = interpolate(*interpolateOp(true), strands, guidesOn(*sphere, guides), sphere);
    ASSERT_TRUE(out);
    float worst = 0.0f;
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        const Vec3f root = out->strandPoints(s)[0];
        const Vec3f first = out->strandPoints(s)[1] - root;
        worst = std::max(worst, degreesBetween(first, south(root.normalized())));
    }
    EXPECT_LT(worst, 4.0f) << "sphere";

    // A torus (tube radius 0.4 round a circle of 1, in the XZ plane): guides combed round the tube.
    std::vector<Vec3f> points;
    std::vector<ScalpMesh::Triangle> triangles;
    constexpr int kAround = 96, kTube = 32;
    auto torusAt = [](float u, float v) {
        return Vec3f((1.0f + 0.4f * std::cos(v)) * std::cos(u), 0.4f * std::sin(v), (1.0f + 0.4f * std::cos(v)) * std::sin(u));
    };
    for (int i = 0; i < kAround; ++i) {
        for (int j = 0; j < kTube; ++j) points.push_back(torusAt(2.0f * kPi * i / kAround, 2.0f * kPi * j / kTube));
    }
    auto at = [](int i, int j) { return ((i + kAround) % kAround) * kTube + (j + kTube) % kTube; };
    for (int i = 0; i < kAround; ++i) {
        for (int j = 0; j < kTube; ++j) {
            triangles.push_back({at(i, j), at(i, j + 1), at(i + 1, j)});
            triangles.push_back({at(i, j + 1), at(i + 1, j + 1), at(i + 1, j)});
        }
    }
    const auto torus = std::make_shared<const ScalpMesh>(points, triangles);
    auto roundTube = [](const Vec3f& p) {  // d/dv of torusAt: round the tube
        const float u = std::atan2(p.z, p.x);
        const Vec3f o = (p - Vec3f(std::cos(u), 0.0f, std::sin(u))).normalized();  // (cos v cos u, sin v, cos v sin u)
        const float cosV = o.x * std::cos(u) + o.z * std::sin(u);
        return Vec3f(-o.y * std::cos(u), cosV, -o.y * std::sin(u)).normalized();
    };
    guides.clear();
    for (int i = 0; i < 24; ++i) {
        for (int j = 0; j < 8; ++j) {
            const Vec3f p = torusAt(2.0f * kPi * i / 24.0f, 2.0f * kPi * (j + 0.5f) / 8.0f);
            guides.push_back({p, roundTube(p) * 0.3f});
        }
    }
    roots.clear();
    Rng rng(4);
    for (int i = 0; i < 600; ++i) roots.push_back(torusAt(rng.uniform(0.0f, 2.0f * kPi), rng.uniform(0.0f, 2.0f * kPi)));
    const StrandsPtr ring = strandsOn(*torus, roots);
    const auto combed = interpolate(*interpolateOp(true), ring, guidesOn(*torus, guides), torus);
    ASSERT_TRUE(combed);
    worst = 0.0f;
    for (std::size_t s = 0; s < combed->strandCount(); ++s) {
        const Vec3f root = combed->strandPoints(s)[0];
        worst = std::max(worst, degreesBetween(combed->strandPoints(s)[1] - root, roundTube(root)));
    }
    EXPECT_LT(worst, 4.0f) << "torus";
}

// ---- Interpolate: what is kept --------------------------------------------------------------------

TEST(Interpolate, PathsAreKeptWhileOnlyShapesChangeAndMadeAgainForMovedRoots) {
    const MeshPtr fin = test::makeFin(2.0f, kFinThickness, 2.0f, 0.05f, true, 1.5f);
    std::vector<std::pair<Vec3f, Vec3f>> guides;
    for (const Vec3f& p : finGrid(-1.0f, 0.3f, 1.8f)) guides.push_back({p, {0.0f, 0.2f, -0.8f}});
    for (const Vec3f& p : finGrid(1.0f, 0.3f, 1.8f)) guides.push_back({p, {0.8f, 0.3f, 0.0f}});
    for (int i = 0; i < 6; ++i) {
        for (int j = 0; j < 4; ++j) {
            const float x = 0.15f + 0.33f * static_cast<float>(i), z = (j < 2 ? -1.2f : 0.3f) + 0.4f * static_cast<float>(j % 2);
            guides.push_back({{x, 0.0f, z}, {0.1f * static_cast<float>(j), 0.6f, 0.0f}});
        }
    }
    std::vector<Vec3f> roots = finGrid(1.0f, 0.06f, 1.9f);
    const std::vector<Vec3f> other = finGrid(-1.0f, 0.06f, 1.9f);
    roots.insert(roots.end(), other.begin(), other.end());
    Rng rng(8);
    for (int i = 0; i < 3000; ++i) roots.push_back({rng.uniform(0.0f, 2.0f), 0.0f, rng.uniform(-1.5f, 1.5f)});
    const StrandsPtr strands = strandsOn(*fin, roots);
    GuidesPtr g = guidesOn(*fin, guides);
    const auto kept = interpolateOp(true);
    ASSERT_TRUE(interpolate(*kept, strands, g, fin));
    // Reshaped guides (a comb): no binding, no path made again.
    {
        auto reshaped = std::make_shared<GuideSet>(*g);
        for (Guide& guide : reshaped->guides()) {
            for (std::size_t k = 1; k < guide.points.size(); ++k) guide.points[k].x += 0.05f * static_cast<float>(k);
        }
        const std::size_t fronts = surfaceFrontCount();
        const auto out = interpolate(*kept, strands, reshaped, fin);
        EXPECT_EQ(surfaceFrontCount(), fronts);
        const auto fresh = interpolate(*interpolateOp(true), strands, reshaped, fin);
        ASSERT_TRUE(out && fresh);
        EXPECT_TRUE(sameBits(*out, *fresh));
        g = reshaped;
    }
    // Two guide roots moved (one on each face): the paths of those, and of the guides whose
    // reach that changed, are made again; the strands come out as a fresh Interpolate makes them.
    for (const std::size_t which : {3u, 50u}) {
        auto moved = std::make_shared<GuideSet>(*g);
        Guide& guide = moved->guides()[which];
        const Vec3f at = fin->position(guide.root) + Vec3f(0.07f, 0.05f, 0.0f);
        const SurfacePoint p = fin->closestPoint(at);
        for (Vec3f& q : guide.points) q += p.position - fin->position(guide.root);
        guide.root = p.root;
        const std::size_t fronts = surfaceFrontCount();
        const auto out = interpolate(*kept, strands, moved, fin);
        const std::size_t made = surfaceFrontCount() - fronts;
        EXPECT_GE(made, 1u);
        EXPECT_LT(made, moved->size() / 4) << "most paths made again for one moved root";
        const auto fresh = interpolate(*interpolateOp(true), strands, moved, fin);
        ASSERT_TRUE(out && fresh);
        EXPECT_TRUE(sameBits(*out, *fresh)) << which;
        g = moved;
    }
}

// ---- Interpolate: keepClearance -------------------------------------------------------------------

// A head-like scalp: a sphere of radius 10 (y up) with an ear (a lobe 2.5 high, about 2.5 wide)
// standing out of its side and the back of the skull bulging 1 out.
MeshPtr headWithAnEar() {
    const auto sphere = test::makeSphere(96, 192, 10.0f);
    auto direction = [](float polarDegrees, float azimuthDegrees) {
        const float t = polarDegrees * kPi / 180.0f, a = azimuthDegrees * kPi / 180.0f;
        return Vec3f(std::sin(t) * std::cos(a), std::cos(t), std::sin(t) * std::sin(a));
    };
    const Vec3f ear = direction(80.0f, 40.0f), back = direction(60.0f, -60.0f);
    auto bump = [](const Vec3f& n, const Vec3f& at, float height, float width) {
        const float a = std::acos(std::clamp(n.dot(at), -1.0f, 1.0f)) / width;
        return height * std::exp(-a * a);
    };
    std::vector<Vec3f> points;
    for (const Vec3f& p : sphere->points()) {
        const Vec3f n = p.normalized();
        points.push_back(n * (10.0f + bump(n, ear, 2.5f, 0.12f) + bump(n, back, 1.0f, 0.35f)));
    }
    return std::make_shared<const ScalpMesh>(points, sphere->triangles());
}

// The height of p above `scalp` (along the closest point's normal): negative inside it.
float heightAbove(const ScalpMesh& scalp, const Vec3f& p) {
    const SurfacePoint at = scalp.closestPoint(p);
    return (p - at.position).dot(at.normal);
}

std::unique_ptr<Operator> clearanceOp(bool keepClearance) {
    auto op = OperatorRegistry::instance().create(op::kInterpolate);
    op->params().set("keepClearance", keepClearance);
    return op;
}

TEST(Interpolate, KeepingClearanceKeepsStrandsOutOfTheScalp) {
    // One guide lying on the crown (0.05 above the skin), combed over to +x, then hanging down: its
    // shape carried to every strand of the top of a head with an ear and a bulging back. Carried as
    // it is, strands crossing the ear or the bulge go into them; keeping clearance, the roots stay
    // and the lengths are kept, and with strands of 17 points (the walk asks every point) no point is
    // inside the scalp. Strands of 24 points ask every other point (interpolateClearanceStep): the
    // ear is narrower than two of their segments, and a point between two asked ones neither of
    // which was lifted, moved by their blend, may dip into it a little (a soft target: a few
    // points in a thousand, shallow).
    const MeshPtr head = headWithAnEar();
    std::vector<Vec3f> along;
    for (int i = 0; i <= 40; ++i) {  // over the skin, from the crown to 100 degrees down the +x side
        const float t = 100.0f * kPi / 180.0f * static_cast<float>(i) / 40.0f;
        const SurfacePoint at = head->closestPoint(Vec3f(std::sin(t), std::cos(t), 0.0f) * 10.0f);
        along.push_back(i == 0 ? at.position : at.position + at.normal * 0.05f);
    }
    for (int i = 1; i <= 12; ++i) along.push_back(along[40] + Vec3f(0.0f, -0.5f * static_cast<float>(i), 0.0f));
    auto guides = std::make_shared<GuideSet>();
    Guide& g = guides->add(1, along);
    g.root = head->closestPoint(along[0]).root;
    Rng rng(12);
    std::vector<Vec3f> roots;
    while (roots.size() < 3000) {  // the top of the head, 55 degrees round the crown
        const Vec3f n = Vec3f(rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1));
        if (n.length() > 1.0f || n.length() < 0.1f || n.normalized().y < std::cos(55.0f * kPi / 180.0f)) continue;
        roots.push_back(n.normalized() * 10.0f);
    }
    for (const int count : {17, 24}) {
        SCOPED_TRACE(count);
        const StrandsPtr strands = strandsOn(*head, roots, count);
        const auto off = interpolate(*clearanceOp(false), strands, guides, head);
        const auto on = interpolate(*clearanceOp(true), strands, guides, head);
        ASSERT_TRUE(off && on);
        std::size_t inside[2] = {0, 0}, moved = 0;
        float deepest[2] = {0.0f, 0.0f}, lengthsApart = 0.0f;
        for (std::size_t s = 0; s < strands->strandCount(); ++s) {
            const std::size_t n = on->strandPointCount(s);
            const Vec3f* a = off->strandPoints(s);
            const Vec3f* b = on->strandPoints(s);
            EXPECT_EQ(std::memcmp(&a[0], &b[0], sizeof(Vec3f)), 0) << "the root of strand " << s << " moved";
            bool any = false;
            for (std::size_t j = 0; j < n; ++j) {
                for (int k = 0; k < 2 && j > 0; ++k) {  // (the root is on the skin, a rounding either side)
                    const float h = heightAbove(*head, (k ? b : a)[j]);
                    if (h < 0.0f) ++inside[k];
                    deepest[k] = std::min(deepest[k], h);
                }
                any = any || std::memcmp(&a[j], &b[j], sizeof(Vec3f)) != 0;
            }
            moved += any ? 1 : 0;
            const float length = arcLength(a, n);
            lengthsApart = std::max(lengthsApart, std::abs(arcLength(b, n) - length) / length);
        }
        std::printf("[ clearance ] %zu strands x %d points from one guide: points inside the scalp %zu (deepest %.3f) carried "
                    "as they are, %zu (deepest %.4f) keeping clearance; %zu strands lifted, lengths at most %.4f%% apart\n",
                    strands->strandCount(), count, inside[0], deepest[0], inside[1], deepest[1], moved, 100.0f * lengthsApart);
        EXPECT_GT(inside[0], 500u) << "carried as they are, strands should cross the ear and the bulge (the test's premise)";
        if (count <= 17) {
            EXPECT_EQ(inside[1], 0u);
        } else {
            EXPECT_LT(inside[1], strands->strandCount() * static_cast<std::size_t>(count) / 1000);
            EXPECT_GT(deepest[1], -0.1f);
        }
        EXPECT_LT(lengthsApart, 0.01f);
    }
    const StrandsPtr strands = strandsOn(*head, roots, 24);

    // A brush dab (the guide combed a little), on the node that made the strands: as a fresh one.
    const auto kept = clearanceOp(true);
    ASSERT_TRUE(interpolate(*kept, strands, guides, head));
    auto combed = std::make_shared<GuideSet>(*guides);
    for (std::size_t k = 41; k < combed->guides()[0].points.size(); ++k) combed->guides()[0].points[k].z += 0.2f;
    const auto dab = interpolate(*kept, strands, combed, head);
    const auto fresh = interpolate(*clearanceOp(true), strands, combed, head);
    ASSERT_TRUE(dab && fresh);
    EXPECT_TRUE(sameBits(*dab, *fresh));
}

// The distance from p to the polyline `pts` (n points).
float distanceToPolyline(const Vec3f* pts, std::size_t n, const Vec3f& p) {
    float best = (p - pts[0]).length();
    for (std::size_t j = 1; j < n; ++j) {
        const Vec3f d = pts[j] - pts[j - 1];
        const float t = std::clamp((p - pts[j - 1]).dot(d) / std::max(d.dot(d), 1e-12f), 0.0f, 1.0f);
        best = std::min(best, (p - (pts[j - 1] + d * t)).length());
    }
    return best;
}

TEST(Interpolate, KeepingClearanceDrapesOverAnEarAndComesBack) {
    // Two guides running down the side of the head 0.3 above the skin, in front of the ear and
    // behind it (25 degrees round from it either way, where the skin has no ear), then hanging:
    // their shape blended into strands rooted above the ear goes through the ear. Keeping
    // clearance, those strands lie on the ear (lifted just onto it, not to the guides' 0.3 above
    // it), and well below it they are back on their shaped path (not carried on, standing off the
    // head by the ear's height); no point is inside the skin, the roots stay and the lengths are
    // kept. Strands of 25 points (every other point asked) and of 41 (every third), walked one by
    // one where they touch the ear. (Over the ear's crest a point between two on the skin may stand
    // a little off it: the strand's segments are a large part of the ear's width.)
    const MeshPtr head = headWithAnEar();
    auto direction = [](float polarDegrees, float azimuthDegrees) {
        const float t = polarDegrees * kPi / 180.0f, a = azimuthDegrees * kPi / 180.0f;
        return Vec3f(std::sin(t) * std::cos(a), std::cos(t), std::sin(t) * std::sin(a));
    };
    auto guides = std::make_shared<GuideSet>();
    for (const float azimuth : {15.0f, 65.0f}) {
        std::vector<Vec3f> along;
        for (int i = 0; i <= 32; ++i) {  // over the skin from 30 to 110 degrees down
            const SurfacePoint at = head->closestPoint(direction(30.0f + 2.5f * static_cast<float>(i), azimuth) * 10.0f);
            along.push_back(i == 0 ? at.position : at.position + at.normal * 0.3f);
        }
        for (int i = 1; i <= 16; ++i) along.push_back(along[32] + Vec3f(0.0f, -0.5f * static_cast<float>(i), 0.0f));
        Guide& g = guides->add(guides->size() + 1, along);
        g.root = head->closestPoint(along[0]).root;
    }
    Rng rng(4);
    std::vector<Vec3f> roots;
    for (int i = 0; i < 300; ++i) roots.push_back(direction(rng.uniform(28.0f, 32.0f), rng.uniform(37.0f, 43.0f)) * 10.0f);
    for (const int count : {25, 41}) {
        SCOPED_TRACE(count);
        const StrandsPtr strands = strandsOn(*head, roots, count);
        const auto off = interpolate(*clearanceOp(false), strands, guides, head);
        const auto on = interpolate(*clearanceOp(true), strands, guides, head);
        ASSERT_TRUE(off && on);
        std::size_t inside = 0, overEar = 0, below = 0, crossing = 0;
        float deepest = 0.0f, highestOnEar = 0.0f, farthestBelow = 0.0f, lengthsApart = 0.0f;
        double onEar = 0.0, offBelow = 0.0;  // summed: the heights over the ear, the distances below it
        for (std::size_t s = 0; s < strands->strandCount(); ++s) {
            const std::size_t n = on->strandPointCount(s);
            const Vec3f* a = off->strandPoints(s);
            const Vec3f* b = on->strandPoints(s);
            EXPECT_EQ(std::memcmp(&a[0], &b[0], sizeof(Vec3f)), 0) << "the root of strand " << s << " moved";
            bool crosses = false;
            for (std::size_t j = 1; j < n; ++j) {
                const float h = heightAbove(*head, b[j]);
                if (h < 0.0f) ++inside;
                deepest = std::min(deepest, h);
                if (heightAbove(*head, a[j]) < -0.05f) {  // carried, this point is in the ear: walked, on it
                    crosses = true;
                    ++overEar;
                    onEar += h;
                    highestOnEar = std::max(highestOnEar, h);
                }
                if (a[j].y < -8.0f) {  // well below the ear (its lowest edge is at about y = -1.5)
                    ++below;
                    const float apart = distanceToPolyline(a, n, b[j]);
                    offBelow += apart;
                    farthestBelow = std::max(farthestBelow, apart);
                }
            }
            crossing += crosses ? 1 : 0;
            const float length = arcLength(a, n);
            lengthsApart = std::max(lengthsApart, std::abs(arcLength(b, n) - length) / length);
        }
        const double meanOnEar = onEar / static_cast<double>(std::max<std::size_t>(overEar, 1));
        const double meanBelow = offBelow / static_cast<double>(std::max<std::size_t>(below, 1));
        std::printf("[ clearance ] %zu strands x %d points over an ear: %zu crossing it; %zu points that were in it %.4f above "
                    "it on average (at most %.4f); %zu points well below it %.4f off their shaped path on average (at "
                    "most %.4f); %zu inside the scalp (deepest %.4f); lengths at most %.4f%% apart\n",
                    strands->strandCount(), count, crossing, overEar, meanOnEar, highestOnEar, below, meanBelow, farthestBelow,
                    inside, deepest, 100.0f * lengthsApart);
        EXPECT_GT(crossing, strands->strandCount() * 9 / 10) << "the test's premise: carried, the strands cross the ear";
        EXPECT_GT(below, strands->strandCount() * 4);
        EXPECT_LT(meanOnEar, 0.05) << "lying on the ear (the guides float 0.3 above the skin)";
        EXPECT_LT(highestOnEar, 0.25f);
        EXPECT_LT(meanBelow, 0.05) << "back on the shaped path below the ear (it stands 2.5 out)";
        EXPECT_LT(farthestBelow, 0.2f);
        EXPECT_EQ(inside, 0u);
        EXPECT_LT(lengthsApart, 0.01f);
    }
}

// A sphere of radius 10 and the same sphere with its vertices beyond x = 7.5 pushed out by 6%
// (an edit of one side of a scalp, as a new rest pose: no deformation).
std::pair<MeshPtr, MeshPtr> sphereAndEdited() {
    const MeshPtr sphere = test::makeSphere(64, 128, 10.0f);
    std::vector<Vec3f> points = sphere->points();
    for (Vec3f& p : points) {
        if (p.x > 7.5f) p = p * 1.06f;
    }
    return {sphere, std::make_shared<const ScalpMesh>(points, sphere->triangles())};
}

// `strands` rooted on `scalp` again: each root's point and normal there, the same shape.
StrandsPtr rootedOn(const ScalpMesh& scalp, const StrandSet& strands) {
    auto out = std::make_shared<StrandSet>();
    for (std::size_t s = 0; s < strands.strandCount(); ++s) {
        const StrandRoot& root = strands.roots()[s];
        const Vec3f* p = strands.strandPoints(s);
        std::vector<Vec3f> pts(p, p + strands.strandPointCount(s));
        const Vec3f move = scalp.position(root) - pts[0];
        for (Vec3f& q : pts) q += move;
        out->addStrand(strands.ids()[s], pts, {}, root);
    }
    return out;
}

TEST(Interpolate, KeepingClearanceIsLocalToTheScalp) {
    // Guides combed down over a sphere's upper half, strands all over it: keeping clearance on the
    // sphere and on the sphere with its +x side edited, the strands rooted on the other side (roots
    // below x = -2.5) come out the same to the bit, as they do not keeping clearance. The field's
    // grid is the world's, and the clearance distances (and the paths' slack, by the mean edge)
    // go by sizes rounded to an eighth of an octave: nothing there depends on how far the other
    // side reaches.
    const auto [sphere, edited] = sphereAndEdited();
    auto guides = std::make_shared<GuideSet>();
    std::uint64_t id = 1;
    for (const float polar : {20.0f, 45.0f, 70.0f}) {
        for (int k = 0; k < 12; ++k) {
            const float a = 2.0f * kPi * (static_cast<float>(k) + (polar == 45.0f ? 0.5f : 0.0f)) / 12.0f;
            std::vector<Vec3f> along;
            for (int i = 0; i <= 8; ++i) {  // over the skin, 40 degrees down, then hanging
                const float t = (polar + 5.0f * static_cast<float>(i)) * kPi / 180.0f;
                const Vec3f n(std::sin(t) * std::cos(a), std::cos(t), std::sin(t) * std::sin(a));
                along.push_back(n * (i == 0 ? 10.0f : 10.05f));
            }
            for (int i = 1; i <= 8; ++i) along.push_back(along[8] + Vec3f(0.0f, -0.6f * static_cast<float>(i), 0.0f));
            Guide& g = guides->add(id++, along);
            g.root = sphere->closestPoint(along[0]).root;
            g.points[0] = sphere->position(g.root);
        }
    }
    Rng rng(5);
    std::vector<Vec3f> roots;
    while (roots.size() < 6000) {
        const Vec3f n = Vec3f(rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1));
        if (n.length() > 1.0f || n.length() < 0.1f || n.normalized().y < -0.3f) continue;
        roots.push_back(n.normalized() * 10.0f);
    }
    const StrandsPtr strands = strandsOn(*sphere, roots, 16);
    const auto before = interpolate(*clearanceOp(true), strands, guides, sphere);
    const auto after = interpolate(*clearanceOp(true), rootedOn(*edited, *strands), guides, edited);
    ASSERT_TRUE(before && after);
    std::size_t far = 0, differ = 0, lifted = 0, differOff = 0;
    float worst = 0.0f;
    const auto off = interpolate(*clearanceOp(false), strands, guides, sphere);
    const auto offEdited = interpolate(*clearanceOp(false), rootedOn(*edited, *strands), guides, edited);
    for (std::size_t s = 0; s < strands->strandCount(); ++s) {
        if (strands->strandPoints(s)[0].x >= -2.5f) continue;
        ++far;
        const std::size_t n = strands->strandPointCount(s);
        const Vec3f* a = before->strandPoints(s);
        const Vec3f* b = after->strandPoints(s);
        if (std::memcmp(a, b, n * sizeof(Vec3f)) != 0) ++differ;
        if (std::memcmp(a, off->strandPoints(s), n * sizeof(Vec3f)) != 0) ++lifted;
        if (std::memcmp(off->strandPoints(s), offEdited->strandPoints(s), n * sizeof(Vec3f)) != 0) ++differOff;
        for (std::size_t j = 0; j < n; ++j) worst = std::max(worst, (a[j] - b[j]).length());
    }
    std::printf("[ clearance ] one side of the scalp edited: %zu of %zu strands on the other side differ (worst %.2e; not keeping "
                "clearance %zu); %zu of them lifted\n", differ, far, worst, differOff, lifted);
    EXPECT_GT(far, 1000u);
    // (A fifth of them or so: only points that would sink under the skin are lifted, not every
    // point lower than its guides are, 0.05 above the skin, as keeping clearance first did.)
    EXPECT_GT(lifted, far / 8) << "the test's premise: keeping clearance lifts strands there";
    EXPECT_EQ(differOff, 0u);
    EXPECT_EQ(differ, 0u);
}

TEST(Interpolate, KeepingClearanceLeavesAStrandClearOfTheSkinAlone) {
    // A dome on a plane: strands on its top, one guide on the flat rising at 45 degrees. On the
    // dome the guide's shape stands clear of the skin, so no point is below the clearance: the
    // strands are the same to the bit with the option and without.
    const auto plane = test::makePlane(90, 6.0f);
    std::vector<Vec3f> points = plane->points();
    for (Vec3f& p : points) {
        const float r2 = (p.x - 3.0f) * (p.x - 3.0f) + (p.z - 3.0f) * (p.z - 3.0f);
        p.y = r2 < 1.5f * 1.5f ? std::max(0.0f, std::sqrt(1.5f * 1.5f - r2) - 0.9f) : 0.0f;
    }
    const auto dome = std::make_shared<const ScalpMesh>(points, plane->triangles());
    const GuidesPtr g = guidesOn(*dome, {{{1.0f, 0.0f, 1.0f}, Vec3f(1.0f, 1.0f, 0.0f).normalized() * 1.5f}});
    Rng rng(3);
    std::vector<Vec3f> roots;
    for (int i = 0; i < 500; ++i) roots.push_back({3.0f + rng.uniform(-0.25f, 0.25f), 1.0f, 3.0f + rng.uniform(-0.25f, 0.25f)});
    const StrandsPtr strands = strandsOn(*dome, roots, 16);
    const auto off = interpolate(*clearanceOp(false), strands, g, dome);
    const auto on = interpolate(*clearanceOp(true), strands, g, dome);
    ASSERT_TRUE(off && on);
    EXPECT_TRUE(sameBits(*off, *on));
}

}  // namespace
}  // namespace groomlab
