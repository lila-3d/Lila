// SPDX-License-Identifier: Apache-2.0
// Modified for Lila (2026).
// A scalp that moves: meshes made by withPoints (the same topology, the closest-point tree
// refitted rather than built again) answer as meshes built from scratch, and evaluations on a
// deforming scalp give the same strands either way. Following the scalp (follow_scalp.h): the
// groom at rest carried root by root.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

#include "groomlab/display.h"
#include "groomlab/follow_scalp.h"
#include "groomlab/groom_file.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/random.h"
#include "groomlab/strand_geometry.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

// A frame of a deforming head: squashed, twisted about Y and moved, a little more each frame.
std::vector<Vec3f> deformedPoints(const ScalpMesh& rest, int frame) {
    std::vector<Vec3f> points = rest.points();
    const float f = static_cast<float>(frame);
    for (Vec3f& p : points) {
        const float a = 0.05f * f * (1.0f + p.y);
        const float c = std::cos(a), s = std::sin(a);
        p = Vec3f(p.x * c - p.z * s, p.y * (1.0f - 0.01f * f), p.x * s + p.z * c) + Vec3f(0.02f * f, 0.0f, -0.01f * f);
    }
    return points;
}

// The same mesh built from scratch: points, triangles, UVs, color sets and rest pose.
ScalpMesh rebuilt(const ScalpMesh& like, std::vector<Vec3f> points) {
    ScalpMesh mesh(std::move(points), like.triangles());
    if (like.hasUVs()) mesh.setCornerUVs(like.cornerUVs());
    for (const std::string& name : like.colorSetNames()) mesh.setCornerColors(name, *like.cornerColors(name));
    if (like.hasRest()) mesh.setRestPoints(like.restPoints());
    return mesh;
}

std::shared_ptr<ScalpMesh> headWithEverything() {
    const auto sphere = test::makeSphere(24, 48, 1.0f);
    auto head = std::make_shared<ScalpMesh>(*sphere);
    std::vector<ScalpMesh::UV> uvs;
    std::vector<ScalpMesh::Color> colors;
    for (std::size_t c = 0; c < head->triangleCount() * 3; ++c) {
        const Vec3f& p = head->points()[static_cast<std::size_t>(head->triangles()[c / 3][c % 3])];
        uvs.push_back({0.5f + 0.5f * p.x, 0.5f + 0.5f * p.y});
        colors.push_back({p.z * 0.5f + 0.5f, 0.25f, 0.75f, 1.0f});
    }
    head->setCornerUVs(std::move(uvs));
    head->setCornerColors("mask", std::move(colors));
    head->setRestPoints(sphere->points());
    return head;
}

void expectSamePoint(const SurfacePoint& a, const SurfacePoint& b) {
    EXPECT_EQ(a.root.face, b.root.face);
    EXPECT_EQ(a.root.u, b.root.u);
    EXPECT_EQ(a.root.v, b.root.v);
    EXPECT_EQ(a.position, b.position);
    EXPECT_EQ(a.normal, b.normal);
}

TEST(ScalpRefit, WithPointsMatchesAMeshBuiltFromScratch) {
    const auto head = headWithEverything();
    (void)head->closestPoint({0.0f, 2.0f, 0.0f});  // its tree is built: the next frames refit it
    ScalpMesh previous = *head;
    for (int frame = 1; frame <= 4; ++frame) {
        const std::vector<Vec3f> points = deformedPoints(*head, frame);
        const ScalpMesh moved = previous.withPoints(points);  // a chain of refits, frame after frame
        const ScalpMesh fresh = rebuilt(*head, points);
        EXPECT_EQ(moved.contentHash(), fresh.contentHash());
        EXPECT_EQ(moved.surfaceHash(), fresh.surfaceHash());
        EXPECT_EQ(moved.topologyHash(), fresh.topologyHash());
        EXPECT_EQ(moved.restSurfaceHash(), fresh.restSurfaceHash());
        EXPECT_EQ(moved.restPoseHash(), fresh.restPoseHash());
        EXPECT_EQ(moved.colorSetHash("mask"), fresh.colorSetHash("mask"));
        EXPECT_EQ(moved.totalArea(), fresh.totalArea());
        EXPECT_TRUE(moved.samePointsLayout(fresh));
        EXPECT_TRUE(moved.samePointsLayout(*head));
        EXPECT_FALSE(fresh.treeRefitted());
        EXPECT_TRUE(moved.treeRefitted());

        // Queries: random points near and far, every vertex pushed out along its normal (nearest
        // a shared vertex: exact ties between faces), edge midpoints, and rays through vertices.
        Rng rng(static_cast<std::uint64_t>(frame));
        std::vector<Vec3f> probes;
        for (int i = 0; i < 400; ++i) {
            probes.push_back(Vec3f(rng.uniform(-1.6f, 1.6f), rng.uniform(-1.6f, 1.6f), rng.uniform(-1.6f, 1.6f)));
        }
        for (std::size_t v = 0; v < points.size(); v += 3) {
            probes.push_back(points[v] * 1.1f);
            probes.push_back(points[v]);
        }
        for (std::size_t t = 0; t < moved.triangleCount(); t += 7) {
            const auto& tri = moved.triangles()[t];
            probes.push_back((points[static_cast<std::size_t>(tri[0])] + points[static_cast<std::size_t>(tri[1])]) * 0.5f * 1.05f);
        }
        for (const Vec3f& p : probes) {
            expectSamePoint(moved.closestPoint(p), fresh.closestPoint(p));
            expectSamePoint(moved.closestPoint(p, 0.05f), fresh.closestPoint(p, 0.05f));
            float lowA = 0, lowB = 0;
            expectSamePoint(moved.closestPointBounded(p, 0.2f, &lowA), fresh.closestPointBounded(p, 0.2f, &lowB));
            if (std::isfinite(lowA) && fresh.closestPoint(p, 0.2f).root.face >= 0) EXPECT_EQ(lowA, lowB);
        }
        for (std::size_t v = 0; v < points.size(); v += 5) {
            const Vec3f origin = points[v] * 3.0f;  // straight through the vertex: ties between its faces
            SurfacePoint a, b;
            float ta = 0, tb = 0;
            const bool hitA = moved.intersectRay(origin, points[v] - origin, &a, &ta);
            const bool hitB = fresh.intersectRay(origin, points[v] - origin, &b, &tb);
            ASSERT_EQ(hitA, hitB);
            if (hitA) {
                expectSamePoint(a, b);
                EXPECT_EQ(ta, tb);
            }
        }
        previous = moved;
    }
}

TEST(ScalpRefit, AMeshNeverQueriedHandsNoTreeOnAndItsCopiesBuildTheirOwn) {
    const auto head = headWithEverything();
    const ScalpMesh moved = head->withPoints(deformedPoints(*head, 2));
    EXPECT_FALSE(moved.treeRefitted());  // the source never built a tree: this one builds its own
    const ScalpMesh next = moved.withPoints(deformedPoints(*head, 3));
    EXPECT_TRUE(next.treeRefitted());
    EXPECT_THROW((void)head->withPoints({Vec3f{}}), std::invalid_argument);
}

const char* kDeformingGraph = R"({
  "version": 1,
  "nodes": [
    {"name": "scalp", "type": "Scalp"},
    {"name": "guides", "type": "Guides"},
    {"name": "scatter", "type": "Scatter", "params": {"density": 400.0}},
    {"name": "interpolate", "type": "Interpolate"},
    {"name": "comb", "type": "Gravity", "params": {"strength": 0.7}},
    {"name": "collide", "type": "Collide", "params": {"maxDepth": 0.05}},
    {"name": "output", "type": "Output"}
  ],
  "connections": [
    {"from": "scalp", "to": "scatter", "input": "scalp"},
    {"from": "scatter", "to": "interpolate", "input": "strands"},
    {"from": "guides", "to": "interpolate", "input": "guides"},
    {"from": "scalp", "to": "interpolate", "input": "scalp"},
    {"from": "interpolate", "to": "comb", "input": "strands"},
    {"from": "comb", "to": "collide", "input": "strands"},
    {"from": "scalp", "to": "collide", "input": "scalp"},
    {"from": "collide", "to": "output", "input": "strands"}
  ],
  "output": "output"
})";

std::string guidesJson(const ScalpMesh& scalp) {
    GuideHierarchy h;
    const GroupId primary = h.addGroup("primary");
    Rng rng(9);
    for (int i = 0; i < 24; ++i) {
        const Vec3f n = Vec3f(rng.uniform(-1, 1), rng.uniform(0.2f, 1), rng.uniform(-1, 1)).normalized();
        h.addGuide(primary, test::straightCurve(n, n * 0.6f + Vec3f(0.1f, -0.2f, 0), 8));
    }
    h.evaluate(&scalp);
    ChildGenerationParams p;
    p.count = 3;
    h.generateChildren(primary, "children", p);
    h.evaluate(&scalp);
    return h.toJson();
}

// ---- Following the scalp ----------------------------------------------------------------------

const char* kLookGraph = R"({
  "version": 1,
  "nodes": [
    {"name": "scalp", "type": "Scalp"},
    {"name": "guides", "type": "Guides"},
    {"name": "scatter", "type": "Scatter", "params": {"density": 300.0}},
    {"name": "interpolate", "type": "Interpolate"},
    {"name": "clump", "type": "Clump", "params": {"count": 30, "strength": 0.6}},
    {"name": "noise", "type": "Noise", "params": {"amplitude": 0.03}},
    {"name": "width", "type": "Width"},
    {"name": "output", "type": "Output"}
  ],
  "connections": [
    {"from": "scalp", "to": "scatter", "input": "scalp"},
    {"from": "scatter", "to": "interpolate", "input": "strands"},
    {"from": "guides", "to": "interpolate", "input": "guides"},
    {"from": "scalp", "to": "interpolate", "input": "scalp"},
    {"from": "interpolate", "to": "clump", "input": "strands"},
    {"from": "clump", "to": "noise", "input": "strands"},
    {"from": "noise", "to": "width", "input": "strands"},
    {"from": "width", "to": "output", "input": "strands"}
  ],
  "output": "output"
})";

// A rigid motion: a turn about an axis, then a move.
struct Rigid {
    Vec3f axis = Vec3f(0.3f, 1.0f, 0.2f).normalized();
    float angle = 0.7f;
    Vec3f move{0.4f, -0.2f, 1.1f};
    Vec3f operator()(const Vec3f& p) const {
        const float c = std::cos(angle), s = std::sin(angle);
        const Vec3f& k = axis;
        return p * c + k.cross(p) * s + k * (k.dot(p) * (1.0f - c)) + move;
    }
};

GroomData followData(const char* graph) {
    const auto head = headWithEverything();
    GroomData data;
    data.graphJson = graph;
    data.hierarchyJson = guidesJson(*head);
    data.scalp = head;
    return data;
}

float maxDistance(const StrandSet& a, const StrandSet& b) {
    float worst = 0.0f;
    for (std::size_t i = 0; i < a.positions().size(); ++i) worst = std::max(worst, (a.positions()[i] - b.positions()[i]).length());
    return worst;
}

}  // namespace
}  // namespace groomlab
