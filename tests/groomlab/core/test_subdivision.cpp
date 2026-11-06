// SPDX-License-Identifier: Apache-2.0
// Modified for Lila (2026).
// Catmull-Clark subdivision of the scalp and colliders (subdivision.h): the rules against
// positions worked out by hand, UVs and color sets, polygons from triangles, the session's scalp
// refined (roots on the refined surface, stable while it deforms, match render), groom files,
// and the time a refinement takes.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <set>

#include <nlohmann/json.hpp>

#include "groomlab/display.h"
#include "groomlab/groom_file.h"
#include "groomlab/flow.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/operator_state.h"
#include "groomlab/parts.h"
#include "groomlab/stroke.h"
#include "groomlab/subdivision.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

// A mesh of quads (and other polygons), each fan-triangulated, its polygons recorded as Maya's
// getTriangles counts them.
std::shared_ptr<ScalpMesh> polygonMesh(std::vector<Vec3f> points, const std::vector<std::vector<std::int32_t>>& faces) {
    std::vector<ScalpMesh::Triangle> tris;
    std::vector<std::int32_t> counts;
    for (const auto& f : faces) {
        for (std::size_t k = 1; k + 1 < f.size(); ++k) tris.push_back({f[0], f[k], f[k + 1]});
        counts.push_back(static_cast<std::int32_t>(f.size() - 2));
    }
    auto mesh = std::make_shared<ScalpMesh>(std::move(points), std::move(tris));
    mesh->setPolygons(std::move(counts));
    return mesh;
}

std::shared_ptr<ScalpMesh> cube() {
    return polygonMesh({{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1}, {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}},
                       {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {3, 7, 6, 2}, {0, 4, 7, 3}, {1, 2, 6, 5}});
}

bool near(const Vec3f& a, const Vec3f& b, float tol = 1e-5f) { return (a - b).length() <= tol; }

bool hasPoint(const std::vector<Vec3f>& points, const Vec3f& p) {
    for (const Vec3f& q : points) {
        if (near(p, q)) return true;
    }
    return false;
}

// A closed head-like sphere of quads with triangle fans at the poles: rings x segments faces,
// welded (one vertex per place), UVs with a seam down one side (u 0 and 1 on the same vertices).
std::shared_ptr<ScalpMesh> quadSphere(int rings, int segments, float radius = 10.0f) {
    std::vector<Vec3f> points{{0.0f, radius, 0.0f}};
    for (int r = 1; r < rings; ++r) {
        const float theta = 3.14159265f * static_cast<float>(r) / static_cast<float>(rings);
        for (int s = 0; s < segments; ++s) {
            const float phi = 6.2831853f * static_cast<float>(s) / static_cast<float>(segments);
            points.push_back({radius * std::sin(theta) * std::cos(phi), radius * std::cos(theta), radius * std::sin(theta) * std::sin(phi)});
        }
    }
    points.push_back({0.0f, -radius, 0.0f});
    const auto south = static_cast<std::int32_t>(points.size() - 1);
    auto at = [segments](int r, int s) { return 1 + (r - 1) * segments + (s % segments); };
    std::vector<std::vector<std::int32_t>> faces;
    std::vector<std::vector<ScalpMesh::UV>> faceUVs;
    auto uv = [&](int r, int s) { return ScalpMesh::UV{static_cast<float>(s) / segments, 1.0f - static_cast<float>(r) / rings}; };
    for (int s = 0; s < segments; ++s) {
        faces.push_back({0, at(1, s + 1), at(1, s)});
        faceUVs.push_back({{(s + 0.5f) / segments, 1.0f}, uv(1, s + 1), uv(1, s)});
    }
    for (int r = 1; r + 1 < rings; ++r) {
        for (int s = 0; s < segments; ++s) {
            faces.push_back({at(r, s), at(r, s + 1), at(r + 1, s + 1), at(r + 1, s)});
            faceUVs.push_back({uv(r, s), uv(r, s + 1), uv(r + 1, s + 1), uv(r + 1, s)});
        }
    }
    for (int s = 0; s < segments; ++s) {
        faces.push_back({south, at(rings - 1, s), at(rings - 1, s + 1)});
        faceUVs.push_back({{(s + 0.5f) / segments, 0.0f}, uv(rings - 1, s), uv(rings - 1, s + 1)});
    }
    auto mesh = polygonMesh(std::move(points), faces);
    std::vector<ScalpMesh::UV> corners;
    for (const auto& f : faceUVs) {
        for (std::size_t k = 1; k + 1 < f.size(); ++k) corners.insert(corners.end(), {f[0], f[k], f[k + 1]});
    }
    mesh->setCornerUVs(std::move(corners));
    return mesh;
}

TEST(Subdivision, CubeOnceAndTwice) {
    const auto c = cube();
    // Once: corners to 5/9, edge points (P0 + P1 + F0 + F1) / 4, face points the face centers.
    const ScalpMesh once = SubdivisionPlan::of(*c, 1)->refine(*c);
    EXPECT_EQ(once.pointCount(), 8u + 12u + 6u);
    EXPECT_EQ(once.triangleCount(), 24u * 2u);
    EXPECT_EQ(once.subdivisionLevel(), 1);
    EXPECT_EQ(once.cageFaceCount(), 6u);
    const float a = 5.0f / 9.0f;
    for (int v = 0; v < 8; ++v) {
        const Vec3f& cagePoint = c->points()[v];
        EXPECT_TRUE(near(once.points()[v], cagePoint * a)) << v;
    }
    EXPECT_TRUE(hasPoint(once.points(), {0.75f, 0.75f, 0.0f}));
    EXPECT_TRUE(hasPoint(once.points(), {-0.75f, 0.0f, 0.75f}));
    EXPECT_TRUE(hasPoint(once.points(), {1.0f, 0.0f, 0.0f}));
    EXPECT_TRUE(hasPoint(once.points(), {0.0f, -1.0f, 0.0f}));
    // Twice: the corner (valence 3) to (F + R + P) / 3 = 55/108; a face point of the corner's quad
    // to its corners' mean (5/9 + 3/4 + 1 + 3/4, 5/9 + 3/4, 5/9 + 3/4) / 4.
    const ScalpMesh twice = SubdivisionPlan::of(*c, 2)->refine(*c);
    EXPECT_EQ(twice.pointCount(), 26u + 48u + 24u);
    EXPECT_EQ(twice.triangleCount(), 96u * 2u);
    const float b = 55.0f / 108.0f;
    EXPECT_TRUE(near(twice.points()[6], {b, b, b}));
    EXPECT_TRUE(near(twice.points()[0], {-b, -b, -b}));
    EXPECT_TRUE(hasPoint(twice.points(), {55.0f / 72.0f, 47.0f / 144.0f, 47.0f / 144.0f}));
    // Level by level and in one go agree.
    const auto plan2 = SubdivisionPlan::build(*c, 2);
    EXPECT_EQ(plan2->refinePoints(c->points()), twice.points());
    // The cube's triangles as faces (no polygons): another, triangle-based surface.
    auto triangles = std::make_shared<ScalpMesh>(c->points(), c->triangles());
    EXPECT_EQ(SubdivisionPlan::of(*triangles, 1)->cageFaceCount(), 12u);
    EXPECT_FALSE(near(SubdivisionPlan::of(*triangles, 1)->refinePoints(triangles->points())[6], {a, a, a}));
}

TEST(Subdivision, NgonsAndTriangles) {
    // A pentagon and a triangle sharing an edge: one face each, a quad per corner.
    const auto mesh = polygonMesh({{0, 0, 0}, {2, 0, 0}, {3, 1.5f, 0}, {1, 3, 0}, {-1, 1.5f, 0}, {1, -2, 0}},
                                  {{0, 1, 2, 3, 4}, {0, 5, 1}});
    const auto plan = SubdivisionPlan::of(*mesh, 1);
    EXPECT_EQ(plan->cageFaceCount(), 2u);
    EXPECT_EQ(plan->triangles().size(), (5u + 3u) * 2u);
    EXPECT_EQ(plan->pointCount(), 6u + 7u + 2u);
    // The pentagon's face point is its corners' mean.
    EXPECT_TRUE(hasPoint(plan->refinePoints(mesh->points()), {1.0f, 1.2f, 0.0f}));
    // Polygons that are not one border (a polygon of two separate triangles) stay triangles.
    auto split = std::make_shared<ScalpMesh>(mesh->points(), std::vector<ScalpMesh::Triangle>{{0, 1, 2}, {3, 4, 5}});
    split->setPolygons({2});
    EXPECT_EQ(SubdivisionPlan::of(*split, 1)->cageFaceCount(), 2u);
    EXPECT_THROW(split->setPolygons({3}), std::invalid_argument);
}

// A 2 x 2 grid of quads, open: (x, y) in 0..2, z 0 but the middle of the bottom edge (0.5) and
// the center (1).
std::shared_ptr<ScalpMesh> openGrid() {
    std::vector<Vec3f> points;
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 3; ++x) points.push_back({static_cast<float>(x), static_cast<float>(y), x == 1 && y == 1 ? 1.0f : x == 1 && y == 0 ? 0.5f : 0.0f});
    }
    auto at = [](int x, int y) { return y * 3 + x; };
    std::vector<std::vector<std::int32_t>> faces;
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) faces.push_back({at(x, y), at(x + 1, y), at(x + 1, y + 1), at(x, y + 1)});
    }
    return polygonMesh(std::move(points), faces);
}

TEST(Subdivision, OpenGridBoundaryAndCorners) {
    const auto grid = openGrid();
    const std::vector<Vec3f> p = SubdivisionPlan::of(*grid, 1)->refinePoints(grid->points());
    // Corners (one face) stay; a boundary vertex takes the crease rule (a + 6p + b) / 8 along
    // the boundary; a boundary edge's point is its midpoint.
    EXPECT_TRUE(near(p[0], {0, 0, 0}));
    EXPECT_TRUE(near(p[8], {2, 2, 0}));
    EXPECT_TRUE(near(p[1], {1, 0, 0.375f}));
    EXPECT_TRUE(hasPoint(p, {0.5f, 0.0f, 0.25f}));
    // The center: the smooth rule, (F + R + 2P) / 4.
    EXPECT_TRUE(near(p[4], {1, 1, 2.4375f / 4.0f}));
}

TEST(Subdivision, UVsPinCornersAndSeams) {
    // The grid's UVs: (x, y) / 2 but the center at (0.6, 0.6). The center's UV is smoothed, the
    // straight UV border stays, the corners (a UV with one face) are pinned.
    auto grid = openGrid();
    std::vector<ScalpMesh::UV> uvs;
    for (const auto& t : grid->triangles()) {
        for (const std::int32_t v : t) {
            const Vec3f& q = grid->points()[v];
            uvs.push_back(v == 4 ? ScalpMesh::UV{0.6f, 0.6f} : ScalpMesh::UV{q.x * 0.5f, q.y * 0.5f});
        }
    }
    grid->setCornerUVs(uvs);
    const ScalpMesh refined = SubdivisionPlan::of(*grid, 1)->refine(*grid);
    ASSERT_TRUE(refined.hasUVs());
    auto uvOfVertex = [&](const ScalpMesh& m, std::int32_t v) {
        for (std::size_t t = 0; t < m.triangleCount(); ++t) {
            for (int k = 0; k < 3; ++k) {
                if (m.triangles()[t][k] == v) return m.cornerUVs()[t * 3 + k];
            }
        }
        return ScalpMesh::UV{-1.0f, -1.0f};
    };
    EXPECT_NEAR(uvOfVertex(refined, 4)[0], 0.55625f, 1e-6f);
    EXPECT_NEAR(uvOfVertex(refined, 4)[1], 0.55625f, 1e-6f);
    EXPECT_NEAR(uvOfVertex(refined, 0)[0], 0.0f, 1e-7f);
    EXPECT_NEAR(uvOfVertex(refined, 1)[0], 0.5f, 1e-6f);
    EXPECT_NEAR(uvOfVertex(refined, 1)[1], 0.0f, 1e-6f);

    // A seam: two quads sharing an edge, each its own UV island. The surface is smooth across the
    // edge; the UVs are not mixed across it (each island's own UVs, pinned at its corners).
    auto strip = polygonMesh({{0, 0, 0}, {1, 0, 0.5f}, {2, 0, 0}, {0, 1, 0}, {1, 1, 0.5f}, {2, 1, 0}}, {{0, 1, 4, 3}, {1, 2, 5, 4}});
    strip->setCornerUVs({{0, 0}, {0.5f, 0}, {0.5f, 1}, {0, 0}, {0.5f, 1}, {0, 1},
                         {10, 0}, {10.5f, 0}, {10.5f, 1}, {10, 0}, {10.5f, 1}, {10, 1}});
    const ScalpMesh s = SubdivisionPlan::of(*strip, 1)->refine(*strip);
    // The shared edge's point is the smooth one, (P0 + P1 + F0 + F1) / 4.
    EXPECT_TRUE(hasPoint(s.points(), {1.0f, 0.5f, 0.375f}));
    std::size_t left = 0, right = 0;
    for (std::size_t c = 0; c < s.cornerUVs().size(); ++c) {
        const float u = s.cornerUVs()[c][0];
        EXPECT_TRUE((u >= -1e-6f && u <= 0.5f + 1e-6f) || (u >= 10.0f - 1e-6f && u <= 10.5f + 1e-6f)) << u;
        // The seam's point: on the left island's border (u 0.5) or the right's (u 10), v 0.5.
        if (near(s.points()[s.triangles()[c / 3][c % 3]], {1.0f, 0.5f, 0.375f})) {
            EXPECT_NEAR(s.cornerUVs()[c][1], 0.5f, 1e-6f);
            (u < 5.0f ? left : right) += 1;
            EXPECT_NEAR(u, u < 5.0f ? 0.5f : 10.0f, 1e-6f);
        }
    }
    EXPECT_GT(left, 0u);
    EXPECT_GT(right, 0u);
}

TEST(Subdivision, ColorSetsAreLinear) {
    auto c = cube();
    // One set by vertex (a painted map), one by face (a face-varying set).
    std::vector<ScalpMesh::Color> byVertex, byFace;
    for (std::size_t t = 0; t < c->triangleCount(); ++t) {
        for (const std::int32_t v : c->triangles()[t]) {
            byVertex.push_back({static_cast<float>(v) / 7.0f, 1.0f - static_cast<float>(v) / 7.0f, 0.5f, 1.0f});
            byFace.push_back({static_cast<float>(t / 2) / 5.0f, 0.0f, 0.0f, 1.0f});
        }
    }
    c->setCornerColors("map", byVertex);
    c->setCornerColors("faces", byFace);
    for (int levels = 1; levels <= 2; ++levels) {
        const ScalpMesh refined = SubdivisionPlan::of(*c, levels)->refine(*c);
        const auto* map = refined.cornerColors("map");
        const auto* faces = refined.cornerColors("faces");
        ASSERT_TRUE(map && faces);
        // A cage vertex keeps its value; every corner is the bilinear mix of its face's: each
        // refined face's corners stay within its cage face's values, face colors stay per face.
        for (std::size_t corner = 0; corner < map->size(); ++corner) {
            const std::int32_t v = refined.triangles()[corner / 3][corner % 3];
            if (v < 8) EXPECT_NEAR((*map)[corner][0], static_cast<float>(v) / 7.0f, 1e-6f);
            EXPECT_NEAR((*map)[corner][0] + (*map)[corner][1], 1.0f, 1e-6f);
            const std::size_t cageFace = (corner / 3) / (2 * (levels == 1 ? 4 : 16));
            EXPECT_NEAR((*faces)[corner][0], static_cast<float>(cageFace) / 5.0f, 1e-6f);
        }
    }
    // A refined vertex map reads the same as the color set made from the refined values.
    const auto plan = SubdivisionPlan::of(*c, 1);
    std::vector<float> values(8);
    for (int v = 0; v < 8; ++v) values[v] = static_cast<float>(v) / 7.0f;
    const std::vector<float> refined = plan->refineVertexValues(values, 1);
    const ScalpMesh mesh = plan->refine(*c);
    const auto* map = mesh.cornerColors("map");
    for (std::size_t corner = 0; corner < map->size(); ++corner) {
        EXPECT_NEAR((*map)[corner][0], refined[mesh.triangles()[corner / 3][corner % 3]], 1e-6f);
    }
}

TEST(Subdivision, MovedPointsReuseThePlan) {
    const auto head = quadSphere(16, 24);
    MeshPtr cage = head;
    const MeshPtr first = subdivideScalp(cage, 2);
    EXPECT_EQ(SubdivisionPlan::of(*cage, 2).get(), SubdivisionPlan::of(*cage, 2).get());
    std::vector<Vec3f> moved = head->points();
    for (Vec3f& p : moved) p = p * 1.1f + Vec3f{0.0f, 1.0f, 0.0f};
    const MeshPtr next = std::make_shared<const ScalpMesh>(head->withPoints(moved));
    const MeshPtr fast = subdivideScalp(next, 2, cage, first);
    const MeshPtr full = subdivideScalp(next, 2);
    EXPECT_EQ(fast->points(), full->points());
    EXPECT_EQ(fast->cornerUVs(), full->cornerUVs());
    EXPECT_EQ(fast->contentHash(), full->contentHash());
    EXPECT_EQ(fast->topologyHash(), first->topologyHash());
}

const char* kGraph = R"({
  "version": 1,
  "nodes": [
    {"name": "scalp", "type": "Scalp"SCALP},
    {"name": "guides", "type": "Guides"},
    {"name": "scatter", "type": "Scatter", "params": {"density": 2.0}},
    {"name": "interpolate", "type": "Interpolate"},
    {"name": "output", "type": "Output"}
  ],
  "connections": [
    {"from": "scalp", "to": "scatter", "input": "scalp"},
    {"from": "scatter", "to": "interpolate", "input": "strands"},
    {"from": "guides", "to": "interpolate", "input": "guides"},
    {"from": "scalp", "to": "interpolate", "input": "scalp"},
    {"from": "interpolate", "to": "output", "input": "strands"}
  ],
  "output": "output"
})";

std::string graphWith(const std::string& subdivision) {
    std::string g = kGraph;
    const std::string params = subdivision.empty() ? "" : R"(, "params": {"subdivision": ")" + subdivision + "\"}";
    g.replace(g.find("SCALP"), 5, params);
    return g;
}

std::string guidesOn(const ScalpMesh& scalp) {
    GuideHierarchy h;
    const GroupId primary = h.addGroup("primary");
    TubeProfile tube;
    tube.width = {1.5f, 1.0f};
    for (int i = 0; i < 8; ++i) {
        const float a = 6.2831853f * static_cast<float>(i) / 8.0f;
        const Vec3f n = Vec3f{std::cos(a) * 0.6f, 0.8f, std::sin(a) * 0.6f}.normalized();
        h.addGuide(primary, test::straightCurve(n * 10.0f, n * 4.0f, 6), tube);
    }
    h.evaluate(&scalp);
    return h.toJson();
}

// Every strand's root on `surface` (its root's place there, and nothing of it nearer).
void expectRootsOn(const StrandSet& strands, const ScalpMesh& surface) {
    ASSERT_GT(strands.strandCount(), 50u);
    for (std::size_t s = 0; s < strands.strandCount(); ++s) {
        const StrandRoot& root = strands.roots()[s];
        ASSERT_TRUE(surface.contains(root));
        const Vec3f first = strands.positions()[strands.offsets()[s]];
        EXPECT_LT((surface.position(root) - first).length(), 1e-4f) << s;
        EXPECT_LT((surface.closestPoint(first).position - first).length(), 1e-4f) << s;
    }
}

TEST(Subdivision, GroomFilesKeepPolygonsAndTheLevel) {
    auto head = quadSphere(6, 8);
    head->setRenderSubdivision(3, "smooth mesh preview, level 3");
    GroomFile file;
    file.data.graphJson = graphWith("");
    file.data.scalp = head;
    const GroomFile back = groomFileFromJson(groomFileToJson(file));
    ASSERT_TRUE(back.data.scalp);
    EXPECT_EQ(back.data.scalp->polygons(), head->polygons());
    EXPECT_EQ(back.data.scalp->renderSubdivision(), 3);
    EXPECT_EQ(back.data.scalp->renderSubdivisionFrom(), "smooth mesh preview, level 3");
    GroomFile turned = back;
    convertUpAxis(turned, "Z");
    EXPECT_EQ(turned.data.scalp->polygons(), head->polygons());
    EXPECT_EQ(turned.data.scalp->renderSubdivision(), 3);
    // Without either, the file says nothing of them.
    file.data.scalp = quadSphere(6, 8);
    auto plain = std::make_shared<ScalpMesh>(file.data.scalp->points(), file.data.scalp->triangles());
    file.data.scalp = plain;
    const nlohmann::json written = nlohmann::json::parse(groomFileToJson(file));
    EXPECT_FALSE(written["scalp"].contains("polygons"));
    EXPECT_FALSE(written["scalp"].contains("subdivision"));
}

// ---- Parts and flow on a subdivided scalp ---------------------------------------------------------
// Stored bound to the cage whatever the level (parts.h, flow.h), carried onto the refined scalp
// through the plan's parametrization (SubdivisionPlan::refinedRoot) and back (cageRoot).

// Cage points at random, spread over every triangle.
std::vector<StrandRoot> someRoots(const ScalpMesh& mesh, std::size_t perTriangle = 3) {
    std::vector<StrandRoot> roots;
    std::uint64_t state = 12345;
    auto next = [&] {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<float>(state >> 40) / static_cast<float>(1ull << 24);
    };
    for (std::size_t t = 0; t < mesh.triangleCount(); ++t) {
        for (std::size_t k = 0; k < perTriangle; ++k) {
            float u = next(), v = next();
            if (u + v > 1.0f) {
                u = 1.0f - u;
                v = 1.0f - v;
            }
            roots.push_back({static_cast<std::int32_t>(t), u, v});
        }
    }
    // The corners and edges too (the seams between faces and quads).
    roots.push_back({0, 0.0f, 0.0f});
    roots.push_back({0, 1.0f, 0.0f});
    roots.push_back({0, 0.5f, 0.5f});
    roots.push_back({0, 0.0f, 0.5f});
    return roots;
}

TEST(SubdivisionBinding, CagePointsGoThereAndBack) {
    const std::vector<std::shared_ptr<ScalpMesh>> meshes{
        quadSphere(8, 10), cube(),
        polygonMesh({{0, 0, 0}, {2, 0, 0}, {3, 1.5f, 0.3f}, {1, 3, 0}, {-1, 1.5f, -0.2f}, {1, -2, 0}}, {{0, 1, 2, 3, 4}, {0, 5, 1}}),
        std::make_shared<ScalpMesh>(*test::makeSphere(6, 8, 3.0f))};  // no polygons: every triangle a face
    for (std::size_t m = 0; m < meshes.size(); ++m) {
        const ScalpMesh& cage = *meshes[m];
        for (int levels = 1; levels <= kMaxSubdivisionLevels; ++levels) {
            const auto plan = SubdivisionPlan::of(cage, levels);
            const ScalpMesh refined = plan->refine(cage);
            for (const StrandRoot& r : someRoots(cage)) {
                const StrandRoot on = plan->refinedRoot(r);
                ASSERT_TRUE(refined.contains(on)) << m << " " << levels;
                EXPECT_GE(on.u, 0.0f);
                EXPECT_GE(on.v, 0.0f);
                EXPECT_LE(on.u + on.v, 1.0f + 1e-6f);
                const StrandRoot back = plan->cageRoot(on);
                ASSERT_TRUE(cage.contains(back));
                EXPECT_LT((cage.position(back) - cage.position(r)).length(), 2e-5f) << m << " " << levels << " " << r.face;
            }
            for (const StrandRoot& r : someRoots(refined, 1)) {
                const StrandRoot back = plan->cageRoot(r);
                ASSERT_TRUE(cage.contains(back));
                EXPECT_LT((refined.position(plan->refinedRoot(back)) - refined.position(r)).length(), 2e-5f) << m << " " << levels;
            }
            // Off either mesh: off the other.
            EXPECT_EQ(plan->refinedRoot({-1, 0.0f, 0.0f}).face, -1);
            EXPECT_EQ(plan->refinedRoot({static_cast<std::int32_t>(cage.triangleCount()), 0.0f, 0.0f}).face, -1);
            EXPECT_EQ(plan->cageRoot({static_cast<std::int32_t>(refined.triangleCount()), 0.0f, 0.0f}).face, -1);
        }
    }
}

TEST(SubdivisionBinding, AFlatEvenGridKeepsItsPoints) {
    // Catmull-Clark leaves a flat, evenly spaced grid where it is (only finer), so every cage point
    // lands exactly where it was: the parametrization is the grid's own.
    std::vector<Vec3f> points;
    for (int y = 0; y < 5; ++y) {
        for (int x = 0; x < 5; ++x) points.push_back({static_cast<float>(x), 0.0f, static_cast<float>(y)});
    }
    std::vector<std::vector<std::int32_t>> faces;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) faces.push_back({y * 5 + x, y * 5 + x + 1, (y + 1) * 5 + x + 1, (y + 1) * 5 + x});
    }
    const auto grid = polygonMesh(points, faces);
    for (int levels = 1; levels <= kMaxSubdivisionLevels; ++levels) {
        const auto plan = SubdivisionPlan::of(*grid, levels);
        const ScalpMesh refined = plan->refine(*grid);
        ASSERT_EQ(refinementPlan(*grid, refined), plan);
        for (const StrandRoot& r : someRoots(*grid)) {
            EXPECT_LT((refined.position(plan->refinedRoot(r)) - grid->position(r)).length(), 1e-5f) << levels;
        }
    }
    // A mesh not refined from this cage has no plan.
    EXPECT_EQ(refinementPlan(*grid, *grid), nullptr);
    EXPECT_EQ(refinementPlan(*grid, SubdivisionPlan::of(*quadSphere(4, 6), 1)->refine(*quadSphere(4, 6))), nullptr);
}

Vec3f onSphere(float radius, float thetaDegrees, float phiDegrees) {
    const float t = thetaDegrees * 0.0174532925f, p = phiDegrees * 0.0174532925f;
    return {radius * std::sin(t) * std::cos(p), radius * std::cos(t), radius * std::sin(t) * std::sin(p)};
}

float degreesBetween(const Vec3f& a, const Vec3f& b) {
    return std::acos(std::clamp(a.normalized().dot(b.normalized()), -1.0f, 1.0f)) * 57.29578f;
}

// A part over the crown and flow arrows and a whorl beside it, authored on the cage.
void partsAndFlowOn(const ScalpMesh& cage, GroomData& data) {
    PartSet parts;
    std::vector<Vec3f> line;
    for (int k = 0; k <= 20; ++k) line.push_back(onSphere(10.0f, 12.0f + 2.5f * static_cast<float>(k), 37.0f));
    parts.add("crown", line, cage);
    data.partsJson = parts.toJson();
    FlowSet flow;
    for (int k = 0; k < 6; ++k) {
        const Vec3f at = onSphere(10.0f, 30.0f + 9.0f * static_cast<float>(k), 100.0f + 13.0f * static_cast<float>(k));
        flow.pins.push_back(FlowSet::makePin(cage, cage.closestPoint(at), Vec3f{0.3f, -1.0f, 0.2f * static_cast<float>(k)}));
    }
    FlowSink sink;
    sink.root = cage.closestPoint(onSphere(10.0f, 40.0f, 230.0f)).root;
    sink.radius = 3.0f;
    flow.sinks.push_back(sink);
    data.flowJson = flow.toJson();
}

struct Overlays {
    std::vector<std::vector<Vec3f>> lines;
    std::vector<Vec3f> pins, directions, sinks;
    std::vector<Vec3f> field;  // the field at sample points, on this scalp
    StrandsPtr strands;
};

// Within `tol` of each other, point by point (a cage point and where it refines to: the refined
// control mesh lies inside the cage's facets on a convex head, by up to about the sagitta of an
// edge, and Catmull-Clark evens the spacing out along the surface).
void expectNear(const std::vector<Vec3f>& a, const std::vector<Vec3f>& b, float tol, const std::string& what) {
    ASSERT_EQ(a.size(), b.size()) << what;
    for (std::size_t i = 0; i < a.size(); ++i) EXPECT_LT((a[i] - b[i]).length(), tol) << what << " " << i;
}

void expectSameStrands(const StrandSet& a, const StrandSet& b) {
    ASSERT_EQ(a.strandCount(), b.strandCount());
    EXPECT_EQ(a.positions(), b.positions());
    for (std::size_t s = 0; s < a.strandCount(); ++s) {
        EXPECT_EQ(a.roots()[s].face, b.roots()[s].face);
        EXPECT_EQ(a.roots()[s].u, b.roots()[s].u);
        EXPECT_EQ(a.roots()[s].v, b.roots()[s].v);
    }
}

// ---- The same groom at every level (smooth preview on or off, match render) -------------------

// Scatter (by a painted color set) -> a Clump picking 40 centers of its own -> another picking 25.
std::string layoutGraph(const std::string& subdivision, const std::string& scatterParams) {
    return R"({"version": 1, "nodes": [
        {"name": "scalp", "type": "Scalp", "params": {"subdivision": ")" + subdivision + R"("}},
        {"name": "scatter", "type": "Scatter", "params": {)" + scatterParams + R"(}},
        {"name": "auto", "type": "Clump", "params": {"count": 40, "clumpChannel": "clumpId"}},
        {"name": "own", "type": "Clump", "params": {"clumpBy": "count", "count": 25, "clumpChannel": "ownClump"}},
        {"name": "output", "type": "Output"}],
      "connections": [
        {"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "scatter", "to": "auto", "input": "strands"},
        {"from": "auto", "to": "own", "input": "strands"},
        {"from": "own", "to": "output", "input": "strands"}],
      "output": "output"})";
}

struct Layout {
    StrandsPtr scattered, strands;
    GuidesPtr centers;
    std::vector<std::size_t> chosen;
};

double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// A head of about 10,000 faces: the plan (built once per topology) and a frame's refinement
// (the plan's weights over new points, the refined mesh moved: what a deforming scalp costs).
TEST(Budget, SubdivisionRefine) {
    const auto head = quadSphere(100, 100);
    ASSERT_GT(head->polygons().size(), 9900u);
    const double budgets[] = {0.0, 25.0, 80.0, 320.0};  // ms per frame, levels 1 to 3
    for (int levels = 1; levels <= 3; ++levels) {
        auto t0 = std::chrono::steady_clock::now();
        const auto plan = SubdivisionPlan::build(*head, levels);
        const double buildMs = msSince(t0);
        MeshPtr cage = head;
        MeshPtr refined = std::make_shared<const ScalpMesh>(plan->refine(*head));
        refined->closestPoint({0.0f, 20.0f, 0.0f});  // the tree a frame refits
        double best = 1e9, bestPoints = 1e9;
        for (int frame = 1; frame <= 4; ++frame) {
            std::vector<Vec3f> points = head->points();
            for (Vec3f& p : points) p = p + Vec3f{0.0f, 0.01f * frame * p.y, 0.0f};
            const MeshPtr next = std::make_shared<const ScalpMesh>(cage->withPoints(points));
            t0 = std::chrono::steady_clock::now();
            const std::vector<Vec3f> moved = plan->refinePoints(points);
            bestPoints = std::min(bestPoints, msSince(t0));
            t0 = std::chrono::steady_clock::now();
            const MeshPtr out = subdivideScalp(next, levels, cage, refined);
            best = std::min(best, msSince(t0));
            cage = next;
            refined = out;
        }
        std::printf("[ budget ] Catmull-Clark x%d on %zu faces: %zu triangles, plan %.1f ms, points %.1f ms, frame %.1f ms\n",
                    levels, plan->cageFaceCount(), plan->triangles().size(), buildMs, bestPoints, best);
        EXPECT_LT(best, budgets[levels]) << "level " << levels;
    }
}

}  // namespace
}  // namespace groomlab
