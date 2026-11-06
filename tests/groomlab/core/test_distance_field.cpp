// SPDX-License-Identifier: Apache-2.0
// Modified for Lila (2026).
// The signed distance field Collide asks (distance_field.h): its samples against the mesh's own
// closest points, and Collide's field method against its exact one on the app's lookdev head.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <sstream>
#include <string>

#include "groomlab/distance_field.h"
#include "groomlab/graph.h"
#include "groomlab/groom.h"
#include "groomlab/operators.h"
#include "groomlab/random.h"
#include "groomlab/simulation.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

std::string readText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in) << path;
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// The points and triangles of an OBJ of triangles ("v x y z", "f a/t b/t c/t").
std::shared_ptr<const ScalpMesh> readObj(const std::string& path) {
    std::istringstream in(readText(path));
    std::vector<Vec3f> points;
    std::vector<ScalpMesh::Triangle> triangles;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream words(line);
        std::string kind;
        words >> kind;
        if (kind == "v") {
            Vec3f p;
            words >> p.x >> p.y >> p.z;
            points.push_back(p);
        } else if (kind == "f") {
            ScalpMesh::Triangle t{};
            std::string corner;
            for (int k = 0; k < 3 && words >> corner; ++k) t[k] = std::stoi(corner.substr(0, corner.find('/'))) - 1;
            triangles.push_back(t);
        }
    }
    return std::make_shared<const ScalpMesh>(std::move(points), std::move(triangles));
}

std::shared_ptr<const ScalpMesh> headScalp() { return readObj(std::string(GROOMLAB_TEST_DATA) + "/app/head_scalp.obj"); }

// The exact height Collide reads at p: along the closest point's normal (no bound).
float exactHeight(const ScalpMesh& mesh, const Vec3f& p) {
    const SurfacePoint sp = mesh.closestPoint(p);
    return (p - sp.position).dot(sp.normal);
}

TEST(DistanceField, SamplesTheMeshsHeightsAndNormals) {
    // A sphere of radius 10 (the bench scalp), points within 0.2 above its surface and 0.1 under.
    const auto sphere = test::makeSphere(60, 120, 10.0f);
    const float cell = MeshDistanceField::cellSizeFor(*sphere, 0.08f);
    EXPECT_GT(cell, 0.0f);
    EXPECT_EQ(cell, std::exp2(std::round(std::log2(cell))));  // a power of two
    const MeshDistanceField field(sphere, cell, MeshDistanceField::reachFor(30.0f));
    Rng rng(7);
    double worstHeight = 0.0, worstTurn = 0.0;
    int answered = 0;
    for (int i = 0; i < 4000; ++i) {
        const Vec3f dir = Vec3f(rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)).normalized();
        const Vec3f p = dir * (10.0f + rng.uniform(-0.1f, 0.2f));
        float h = 0.0f;
        Vec3f n;
        if (!field.sample(p, &h, &n)) continue;
        ++answered;
        const SurfacePoint sp = sphere->closestPoint(p);
        worstHeight = std::max(worstHeight, static_cast<double>(std::fabs(h - (p - sp.position).dot(sp.normal))));
        worstTurn = std::max(worstTurn, static_cast<double>(std::acos(std::min(1.0f, n.dot(sp.normal)))));
    }
    std::printf("[ field ] sphere, cell %.4f: %d of 4000 answered, worst height %.2e, worst normal %.2e rad\n", cell, answered,
                worstHeight, worstTurn);
    EXPECT_GT(answered, 3600);  // a smooth closed surface: almost every point is the field's
    EXPECT_LT(worstHeight, 2e-3);
    EXPECT_LT(worstTurn, 0.03);
    const MeshDistanceField::Stats stats = field.stats();
    EXPECT_GT(stats.fineBricks, 0u);
    EXPECT_GT(stats.queries, 0u);
}

TEST(DistanceField, PushesAsTheMeshDoesOrAsks) {
    // Collide's push from the field: kept where the mesh keeps the point, moved to the offset where
    // it moves it, else it asks. On an open cap (the app's head scalp), around it and under it.
    const auto head = headScalp();
    const float offset = 0.08f, maxDepth = 30.0f;
    const auto field = sharedDistanceField(head, MeshDistanceField::cellSizeFor(*head, offset),
                                           MeshDistanceField::reachFor(maxDepth));
    EXPECT_EQ(field, sharedDistanceField(head, field->cellSize(), field->reach()));  // kept, by the surface
    Rng rng(3);
    int kept = 0, moved = 0, asked = 0;
    double worst = 0.0, sum = 0.0;
    for (int i = 0; i < 20000; ++i) {
        // Around the head (an ellipsoid cap about 16 cm across), most near the scalp.
        const SurfacePoint on = head->closestPoint(Vec3f(rng.uniform(-9, 9), rng.uniform(-2, 12), rng.uniform(-9, 9)));
        const Vec3f p = on.position + on.normal * rng.uniform(-0.1f, 1.0f) +
                        Vec3f(rng.uniform(-0.3f, 0.3f), rng.uniform(-0.3f, 0.3f), rng.uniform(-0.3f, 0.3f));
        Vec3f delta;
        const SurfacePoint sp = head->closestPoint(p);
        const float h = (p - sp.position).dot(sp.normal);
        switch (field->push(p, offset, maxDepth, maxDepth, &delta)) {
            case MeshDistanceField::Push::Kept:
                ++kept;
                EXPECT_GE(h, offset - 1e-3f) << "kept a point the mesh pushes";
                break;
            case MeshDistanceField::Push::Moved: {
                ++moved;
                const double apart = (delta - sp.normal * (offset - h)).length();
                worst = std::max(worst, apart);
                sum += apart;
                break;
            }
            case MeshDistanceField::Push::Ask: ++asked; break;
        }
    }
    std::printf("[ field ] head, cell %.4f: kept %d, moved %d, asked %d; pushes apart from the mesh's: worst %.2e, mean %.2e\n",
                field->cellSize(), kept, moved, asked, worst, moved ? sum / moved : 0.0);
    EXPECT_GT(kept + moved, 15000);
    EXPECT_GT(moved, 1500);
    EXPECT_LT(worst, 0.05 * offset);
    EXPECT_LT(sum / std::max(moved, 1), 0.005 * offset);
}

TEST(DistanceField, AnswersAloneAsTheMeshDoes) {
    // Probe::fieldOnly: never asks. Where the field would ask, its answer from the closest faces it
    // holds nearby is the mesh's (the same push, to rounding) for almost every point. On the head
    // (an open cap), around it and under it.
    const auto head = headScalp();
    const float offset = 0.08f, maxDepth = 30.0f;
    const auto field = sharedDistanceField(head, MeshDistanceField::cellSizeFor(*head, offset), MeshDistanceField::reachFor(maxDepth));
    Rng rng(11);
    int asked = 0, same = 0;
    double worst = 0.0;
    for (int i = 0; i < 40000; ++i) {
        const SurfacePoint on = head->closestPoint(Vec3f(rng.uniform(-9, 9), rng.uniform(-2, 12), rng.uniform(-9, 9)));
        const Vec3f p = on.position + on.normal * rng.uniform(-3.0f, 1.0f) +
                        Vec3f(rng.uniform(-0.5f, 0.5f), rng.uniform(-0.5f, 0.5f), rng.uniform(-0.5f, 0.5f));
        Vec3f delta;
        if (field->push(p, offset, maxDepth, maxDepth, &delta) != MeshDistanceField::Push::Ask) continue;
        ++asked;
        MeshDistanceField::Probe alone;
        alone.fieldOnly = true;
        Vec3f byField = p, byMesh = p;
        const MeshDistanceField::Push push = field->push(p, offset, maxDepth, maxDepth, &delta, &alone);
        ASSERT_NE(push, MeshDistanceField::Push::Ask);
        if (push == MeshDistanceField::Push::Moved) byField += delta;
        pushOutOf(*head, nullptr, byMesh, offset, maxDepth, maxDepth);
        const double apart = (byField - byMesh).length();
        worst = std::max(worst, apart);
        same += apart <= 1e-5 * (1.0 + p.length());
    }
    std::printf("[ field ] head: %d points the field asks about; answered alone as the mesh does: %d (worst %.2e apart)\n", asked,
                same, worst);
    EXPECT_GT(asked, 500);
    EXPECT_GE(same, asked - asked / 200);
}

TEST(DistanceField, TriangleBoundsHoldTheClosestPointsMade) {
    // The sphere a triangle's bound gives holds the point closestPointOnFace makes (rounded) for
    // every p within its reach, whatever the triangle: well shaped, thin, a sliver, tiny far from
    // the origin; none for a degenerate one. Points around it, near its edges and corners, and as
    // far as the bound holds.
    Rng rng(29);
    int checked = 0, bounded = 0;
    for (int t = 0; t < 4000; ++t) {
        const float scale = std::exp2(rng.uniform(-8.0f, 4.0f));
        const Vec3f at(rng.uniform(-1, 1) * std::exp2(rng.uniform(0.0f, 10.0f)), rng.uniform(-200, 200), rng.uniform(-200, 200));
        const Vec3f a = at, b = at + Vec3f(rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)) * scale;
        Vec3f c = at + Vec3f(rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)) * scale;
        if (t % 4 == 1) c = a + (b - a) * rng.uniform(-0.5f, 1.5f) + Vec3f(rng.uniform(-1, 1), 0, 0) * (scale * 1e-4f);  // a sliver
        if (t % 4 == 2) c = a + (b - a) * 0.5f;  // degenerate (to rounding)
        const ScalpMesh mesh({a, b, c}, {{0, 1, 2}});
        const MeshDistanceField::TriangleBound bound = MeshDistanceField::triangleBound(a, b, c);
        if (t % 4 == 2 && (b - a).cross(c - a).length() == 0.0f) EXPECT_EQ(bound.within, -1.0f);
        if (!(bound.within > 0.0f)) continue;
        ++bounded;
        const float within = std::sqrt(bound.within);
        for (int i = 0; i < 64; ++i) {
            Vec3f p;
            const int kind = i % 4;
            if (kind == 0) {  // anywhere it holds
                const Vec3f along = Vec3f(rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)).normalized();
                p = bound.center + along * within * rng.uniform(0.0f, 1.0f);
            } else {  // near an edge or a corner, a little off the triangle's plane
                const Vec3f& from = kind == 1 ? a : kind == 2 ? b : c;
                const Vec3f& to = kind == 1 ? b : kind == 2 ? c : a;
                p = from + (to - from) * rng.uniform(-0.01f, 1.01f) +
                    Vec3f(rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)) * scale * std::exp2(rng.uniform(-20.0f, 2.0f));
            }
            if ((p - bound.center).dot(p - bound.center) > bound.within) continue;
            const Vec3f q = mesh.closestPointOnFace(p, 0).position;
            const double dx = static_cast<double>(q.x) - bound.center.x, dy = static_cast<double>(q.y) - bound.center.y,
                         dz = static_cast<double>(q.z) - bound.center.z;
            ++checked;
            ASSERT_LE(std::sqrt(dx * dx + dy * dy + dz * dz), static_cast<double>(bound.reach)) << "triangle " << t << ", point " << i;
        }
    }
    EXPECT_GT(bounded, 1500);
    EXPECT_GT(checked, 50000);
}

// A push with Probe::fieldOnly answered from MeshDistanceField::tables alone, as look.slang's
// clearancePush reads them (the same steps in C++): what a GPU's copy of the field answers.
struct TablesPush {
    const std::vector<std::uint32_t>& w;
    float beside = 0.0f;
    std::uint32_t word(std::size_t i) const { return w[i]; }
    float real(std::size_t i) const {
        float f;
        std::memcpy(&f, &w[i], 4);
        return f;
    }
    Vec3f vec(std::size_t i) const { return {real(i), real(i + 1), real(i + 2)}; }
    static float fromHalf(std::uint32_t h) {
        if (h >= 0x7C00u) return std::numeric_limits<float>::infinity();
        const std::uint32_t exponent = (h >> 10) & 0x1Fu, mantissa = h & 0x3FFu;
        if (exponent == 0) return static_cast<float>(mantissa) * (1.0f / 16777216.0f);
        const std::uint32_t bits = ((exponent + 112u) << 23) | (mantissa << 13);
        float v;
        std::memcpy(&v, &bits, 4);
        return v;
    }
    static Vec3f fromOctahedral(int a, int b) {
        const float u = static_cast<float>(a) * (1.0f / 32767.0f), v = static_cast<float>(b) * (1.0f / 32767.0f);
        Vec3f n(u, v, 1.0f - std::fabs(u) - std::fabs(v));
        if (n.z < 0.0f) {
            n.x = (1.0f - std::fabs(v)) * (u >= 0.0f ? 1.0f : -1.0f);
            n.y = (1.0f - std::fabs(u)) * (v >= 0.0f ? 1.0f : -1.0f);
        }
        return n;
    }
    bool besideAt(float h, float d) const { return beside > 0.0f && d > beside && 2.0f * std::fabs(h) < d; }
    MeshDistanceField::Push answerNear(const ScalpMesh& mesh, const Vec3f& p, const std::uint32_t* seeds, int count, float offset,
                                       float maxDepth, float searchRadius, MeshDistanceField::Probe& probe, Vec3f* delta) const {
        const std::size_t tris = word(MeshDistanceField::kTableTriangles), start = word(MeshDistanceField::kTableRingStart),
                          rings = word(MeshDistanceField::kTableRings);
        float best = std::numeric_limits<float>::max();
        SurfacePoint at;
        std::uint32_t atFace = MeshDistanceField::kTableNone;
        auto scan = [&](std::uint32_t around) {
            bool nearer = false;
            for (int c = 0; c < 3; ++c) {
                const std::uint32_t v =
                    word(tris + around * MeshDistanceField::kTableTriangleWords + MeshDistanceField::kTableTriangleCorners + c);
                for (std::uint32_t k = word(start + v); k < word(start + v + 1); ++k) {
                    const SurfacePoint sp = mesh.closestPointOnFace(p, word(rings + k));
                    const float d = (sp.position - p).dot(sp.position - p);
                    if (d < best) {
                        best = d;
                        at = sp;
                        atFace = word(rings + k);
                        nearer = true;
                    }
                }
            }
            return nearer;
        };
        for (int i = 0; i < count; ++i) {
            const std::uint32_t seed = seeds[i];
            bool again = seed == MeshDistanceField::kTableNone;
            for (int j = 0; j < i; ++j) again = again || seeds[j] == seed;
            if (again) continue;
            scan(seed);
        }
        for (int step = 0; step < MeshDistanceField::kAnswerSteps && atFace != MeshDistanceField::kTableNone; ++step) {
            if (!scan(atFace)) break;
        }
        if (at.root.face < 0 || !(best < searchRadius * searchRadius)) {
            probe.learnt(-std::numeric_limits<float>::infinity(), searchRadius);
            return MeshDistanceField::Push::Kept;
        }
        at.normal = mesh.normal(at.root);
        const float h = (p - at.position).dot(at.normal), d = (p - at.position).length();
        probe.learnt(h, d);
        if (h >= offset || h <= -maxDepth || besideAt(h, d)) return MeshDistanceField::Push::Kept;
        *delta = at.normal * (offset - h);
        return MeshDistanceField::Push::Moved;
    }
    MeshDistanceField::Push push(const ScalpMesh& mesh, const Vec3f& p, float offset, float maxDepth, float searchRadius,
                                 MeshDistanceField::Probe& probe, Vec3f* delta) const {
        using F = MeshDistanceField;
        constexpr float inf = std::numeric_limits<float>::infinity();
        const float inv = real(F::kTableInverseCell);
        const float g[3] = {p.x * inv - 0.5f, p.y * inv - 0.5f, p.z * inv - 0.5f};  // (the grid half a cell off the world's)
        int local[3];
        float f[3], lo[3];
        std::uint32_t dims[3];
        for (int a = 0; a < 3; ++a) {
            dims[a] = word(F::kTableDims + a);
            const int origin = static_cast<int>(word(F::kTableOriginRegion + a));
            lo[a] = static_cast<float>(origin * 16);
            const float hi = static_cast<float>((origin + static_cast<int>(dims[a])) * 16);
            if (!(g[a] >= lo[a] && g[a] < hi)) {
                probe.learnt(-inf, searchRadius);
                return F::Push::Kept;
            }
            const float whole = std::floor(g[a]);
            local[a] = static_cast<int>(whole - lo[a]);
            f[a] = std::clamp(g[a] - whole, 0.0f, 1.0f);
        }
        auto settles = [&](float h, float d) {
            probe.learnt(h, std::max(d, 0.0f));
            return d > searchRadius || h >= offset;
        };
        const int brick[3] = {local[0] / 4, local[1] / 4, local[2] / 4}, region[3] = {brick[0] / 4, brick[1] / 4, brick[2] / 4};
        const std::size_t r = word(F::kTableRegions) + (region[0] + dims[0] * (region[1] + dims[1] * region[2])) * 4;
        if (word(r) != 0 && settles(real(r + 1), real(r + 2))) return F::Push::Kept;
        const std::uint32_t base = word(r + 3);
        if (base == F::kTableNone) return F::Push::Kept;
        const std::size_t b = word(F::kTableBricks) +
                              (base + (brick[0] - region[0] * 4) + (brick[1] - region[1] * 4) * 4 + (brick[2] - region[2] * 4) * 16) * 4;
        const std::uint32_t nodeBase = word(b + 3);
        if (settles(real(b + 1), real(b + 2))) return F::Push::Kept;
        std::uint32_t seeds[8];
        std::fill(seeds, seeds + 8, F::kTableNone);
        if (nodeBase == F::kTableNone) {  // a far brick: its middle's face and the 7 beside it on p's side
            seeds[0] = word(b);
            int step[3];
            for (int a = 0; a < 3; ++a) step[a] = local[a] - brick[a] * 4 >= 2 ? 1 : -1;
            for (int k = 1; k < 8; ++k) {
                const int nb[3] = {brick[0] + ((k & 1) ? step[0] : 0), brick[1] + ((k & 2) ? step[1] : 0), brick[2] + ((k & 4) ? step[2] : 0)};
                bool inside = true;
                for (int a = 0; a < 3; ++a) inside = inside && nb[a] >= 0 && nb[a] < static_cast<int>(dims[a]) * 4;
                if (!inside) continue;
                const int nr[3] = {nb[0] / 4, nb[1] / 4, nb[2] / 4};
                const std::size_t rr = word(F::kTableRegions) + (nr[0] + dims[0] * (nr[1] + dims[1] * nr[2])) * 4;
                const bool same = nr[0] == region[0] && nr[1] == region[1] && nr[2] == region[2];
                if ((!same && word(rr) != 0) || word(rr + 3) == F::kTableNone) continue;
                seeds[k] = word(word(F::kTableBricks) +
                                (word(rr + 3) + (nb[0] - nr[0] * 4) + (nb[1] - nr[1] * 4) * 4 + (nb[2] - nr[2] * 4) * 16) * 4);
            }
            return answerNear(mesh, p, seeds, 8, offset, maxDepth, searchRadius, probe, delta);
        }
        probe.forget();
        const int cell = (local[0] - brick[0] * 4) + (local[1] - brick[1] * 4) * 5 + (local[2] - brick[2] * 4) * 25;
        const std::size_t nodes = word(F::kTableNodes) + nodeBase * 125 * 2, faces = word(F::kTableNodeFaces) + nodeBase * 125;
        const float cellSize = real(F::kTableCell), band = real(F::kTableBand), scale = real(F::kTableHeightScale);
        static const float apart[8] = {0.0f, 1.0f, 1.0f, 1.4142136f, 1.0f, 1.4142136f, 1.4142136f, 1.7320508f};
        std::uint32_t words[16];
        float h[8], lowest = inf;
        std::uint32_t nearestBits = 0xFFFFu, farthestBits = 0u;
        for (int k = 0; k < 8; ++k) {
            const int at = cell + (k & 1) + ((k >> 1) & 1) * 5 + ((k >> 2) & 1) * 25;
            words[2 * k] = word(nodes + at * 2);
            words[2 * k + 1] = word(nodes + at * 2 + 1);
            seeds[k] = word(faces + at);
            const int height = static_cast<std::int16_t>(words[2 * k] & 0xFFFFu);
            h[k] = height == 32767 ? inf : height == -32767 ? -inf : static_cast<float>(height) * scale;
            nearestBits = std::min(nearestBits, words[2 * k] >> 16);
            farthestBits = std::max(farthestBits, words[2 * k] >> 16);
            lowest = std::min(lowest, h[k]);
        }
        const float nearest = fromHalf(nearestBits), farthest = fromHalf(farthestBits), diagonal = 1.7320508f * cellSize;
        auto ask = [&] { return answerNear(mesh, p, seeds, 8, offset, maxDepth, searchRadius, probe, delta); };
        if (nearest > searchRadius + diagonal) return F::Push::Kept;
        if (farthest == inf) return ask();
        if (farthest > searchRadius - diagonal) {
            if (lowest >= offset + 2.0f * diagonal) {
                probe.heightLow = lowest - 2.0f * diagonal;
                return F::Push::Kept;
            }
            return ask();
        }
        if (lowest < -band || lowest < -maxDepth + 2.0f * diagonal) return ask();
        bool finite = true;
        for (const float x : h) finite = finite && x < inf;
        if (!finite) {
            if (lowest >= offset + 2.0f * diagonal) {
                probe.heightLow = lowest - 2.0f * diagonal;
                return F::Push::Kept;
            }
            return ask();
        }
        for (int k = 1; k < 8; ++k) {
            if (std::fabs(h[0] - h[k]) > 1.5f * apart[k] * cellSize) return ask();
        }
        auto lerp = [](float a, float c, float t) { return a + (c - a) * t; };
        const float height = lerp(lerp(lerp(h[0], h[1], f[0]), lerp(h[2], h[3], f[0]), f[1]),
                                  lerp(lerp(h[4], h[5], f[0]), lerp(h[6], h[7], f[0]), f[1]), f[2]);
        probe.heightLow = height;
        if (height >= offset) return F::Push::Kept;
        if (beside > 0.0f) {
            float d[8];
            for (int k = 0; k < 8; ++k) d[k] = fromHalf(words[2 * k] >> 16);
            const float distance = lerp(lerp(lerp(d[0], d[1], f[0]), lerp(d[2], d[3], f[0]), f[1]),
                                        lerp(lerp(d[4], d[5], f[0]), lerp(d[6], d[7], f[0]), f[1]), f[2]);
            if (besideAt(height, distance)) return F::Push::Kept;
        }
        Vec3f nv[8];
        for (int k = 0; k < 8; ++k) {
            nv[k] = fromOctahedral(static_cast<std::int16_t>(words[2 * k + 1] & 0xFFFFu), static_cast<std::int16_t>(words[2 * k + 1] >> 16));
        }
        const float first = nv[0].dot(nv[0]);
        for (int k = 1; k < 8; ++k) {
            const float d = nv[0].dot(nv[k]);
            if (d <= 0.0f || d * d < 0.999f * 0.999f * first * nv[k].dot(nv[k])) return ask();
        }
        auto lerpV = [](const Vec3f& a, const Vec3f& c, float t) { return a + (c - a) * t; };
        const Vec3f normal = lerpV(lerpV(lerpV(nv[0], nv[1], f[0]), lerpV(nv[2], nv[3], f[0]), f[1]),
                                   lerpV(lerpV(nv[4], nv[5], f[0]), lerpV(nv[6], nv[7], f[0]), f[1]), f[2])
                                 .normalized();
        *delta = normal * (offset - height);
        return F::Push::Moved;
    }
};

TEST(DistanceField, TablesAnswerAsTheFieldDoes) {
    // The field as tables (what a GPU holds), read as look.slang reads them: every push with
    // Probe::fieldOnly, at offsets up to the tables' and around a sphere (its poles' degenerate
    // triangles too) and the head (an open cap), answers as the field does, to the bit, and learns
    // the same.
    for (const bool onHead : {false, true}) {
        const auto mesh = onHead ? headScalp() : std::shared_ptr<const ScalpMesh>(test::makeSphere(96, 192, 10.0f));
        const float cap = onHead ? 0.6f : 1.1f, depth = 4.0f * cap, searchRadius = depth * 1.001f + 1e-6f;
        const float cell = onHead ? 0.0625f : 0.125f;
        const auto field = sharedDistanceField(mesh, cell, MeshDistanceField::reachFor(searchRadius), static_cast<int>(cap / cell) + 5);
        const MeshDistanceField::Tables tables = field->tables(cap, searchRadius);
        ASSERT_TRUE(tables);
        const TablesPush reader{*tables, 2.0f * cell};
        Rng rng(17);
        int moved = 0, asked = 0, differ = 0;
        for (int i = 0; i < 60000; ++i) {
            Vec3f p;
            if (i % 4 == 0) {  // about the poles
                p = Vec3f(rng.uniform(-0.5f, 0.5f), (i % 8 ? 1.0f : -1.0f) * rng.uniform(9.0f, 10.5f), rng.uniform(-0.5f, 0.5f));
            } else {
                const SurfacePoint on = mesh->closestPoint(Vec3f(rng.uniform(-11, 11), rng.uniform(-11, 12), rng.uniform(-11, 11)));
                p = on.position + on.normal * rng.uniform(-2.0f * cap, 1.5f * cap) +
                    Vec3f(rng.uniform(-0.3f, 0.3f), rng.uniform(-0.3f, 0.3f), rng.uniform(-0.3f, 0.3f));
            }
            const float offset = rng.uniform(0.0f, cap);
            MeshDistanceField::Probe a, b;
            a.fieldOnly = true;
            a.beside = b.beside = 2.0f * cell;
            Vec3f da, db;
            Vec3f d0;
            asked += field->push(p, offset, depth, searchRadius, &d0) == MeshDistanceField::Push::Ask;
            const MeshDistanceField::Push pa = field->push(p, offset, depth, searchRadius, &da, &a);
            const MeshDistanceField::Push pb = reader.push(*mesh, p, offset, depth, searchRadius, b, &db);
            moved += pa == MeshDistanceField::Push::Moved;
            const bool same = pa == pb && std::memcmp(&a.heightLow, &b.heightLow, 4) == 0 &&
                              std::memcmp(&a.distanceLow, &b.distanceLow, 4) == 0 &&
                              (pa != MeshDistanceField::Push::Moved || std::memcmp(&da, &db, sizeof da) == 0);
            if (!same && differ++ < 5) {
                std::printf("[ tables ] %s: (%.5f %.5f %.5f) offset %.4f: field %d (%.4g, %.4g), tables %d (%.4g, %.4g)\n",
                            onHead ? "head" : "sphere", p.x, p.y, p.z, offset, static_cast<int>(pa), a.heightLow, a.distanceLow,
                            static_cast<int>(pb), b.heightLow, b.distanceLow);
            }
        }
        std::printf("[ tables ] %s: %zu words; of 60000 pushes %d moved, %d the field alone would ask about; %d differ\n",
                    onHead ? "head" : "sphere", tables->size(), moved, asked, differ);
        EXPECT_GT(moved, 1000);
        EXPECT_GT(asked, 100);
        EXPECT_EQ(differ, 0);
    }
}

// The head graph's Collide (offset 0.08, maxDepth 30, 3 passes) by `method`, timed.
StrandsPtr headCollide(const MeshPtr& head, const StrandsPtr& in, const std::string& method, double* ms) {
    auto op = OperatorRegistry::instance().create(op::kCollide);
    op->params().set("offset", 0.08);
    op->params().set("maxDepth", 30.0);
    op->params().set("iterations", std::int64_t{3});
    op->params().set("method", method);
    const EvalContext ctx;
    const auto t0 = std::chrono::steady_clock::now();
    StrandsPtr out = valueAs<StrandsPtr>(op->evaluate(ctx, {in, head}));
    *ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return out;
}

struct Apart {
    double worst = 0.0, mean = 0.0;
    std::size_t over = 0;  // points more than 1e-3 apart
};
Apart apart(const StrandSet& a, const StrandSet& b) {
    Apart r;
    double sum = 0.0;
    for (std::size_t i = 0; i < a.pointCount(); ++i) {
        const double d = (a.positions()[i] - b.positions()[i]).length();
        r.worst = std::max(r.worst, d);
        sum += d;
        r.over += d > 1e-3;
    }
    r.mean = sum / static_cast<double>(std::max<std::size_t>(a.pointCount(), 1));
    return r;
}

// ---- A deforming scalp: Collide through the rest pose's field (RestPoseCollision) ----

// p turned about a unit axis through the origin by `angle` (Rodrigues).
Vec3f turned(const Vec3f& p, const Vec3f& axis, float angle) {
    const float c = std::cos(angle), s = std::sin(angle);
    return p * c + axis.cross(p) * s + axis * (axis.dot(p) * (1.0f - c));
}

// Two motions of space (the head's ~18 units across): a rigid one (a turn of `amount` x 35
// degrees about a tilted axis and a move), and a bend (about the x axis through the middle of
// the head by an angle growing along z: `amount` x 10 degrees at the front, as much the other
// way at the back: the scalp's curvature changes, which no rigid motion does).
Vec3f rigidly(const Vec3f& p, float amount) {
    const Vec3f axis = Vec3f(0.3f, 1.0f, 0.2f).normalized();
    return turned(p, axis, amount * 0.61f) + Vec3f(2.0f, -1.0f, 3.0f) * amount;
}
Vec3f bent(const Vec3f& p, float amount) {
    const Vec3f pivot(0.0f, 4.0f, 0.0f);
    return pivot + turned(p - pivot, Vec3f(1.0f, 0.0f, 0.0f), amount * 0.0194f * p.z);
}

// The head with its rest pose (its own points) on a frame moved by `motion`, and the strands
// moved the same way (strands that followed the scalp: lying along it there as at rest).
template <typename Motion>
MeshPtr movedHead(const std::shared_ptr<const ScalpMesh>& head, Motion motion) {
    auto rested = std::make_shared<ScalpMesh>(*head);
    rested->setRestPoints(head->points());
    std::vector<Vec3f> points = head->points();
    for (Vec3f& p : points) p = motion(p);
    return std::make_shared<const ScalpMesh>(rested->withPoints(std::move(points)));
}
template <typename Motion>
StrandsPtr movedStrands(const StrandsPtr& in, Motion motion) {
    auto out = std::make_shared<StrandSet>(*in);
    for (Vec3f& p : out->mutablePositions()) p = motion(p);
    return out;
}

// The same deformed mesh without its rest pose: what a field made for this frame alone answers
// (Collide's field for a deforming mesh without a rest pose, a collider's).
MeshPtr withoutRest(const MeshPtr& mesh) {
    auto bare = std::make_shared<ScalpMesh>(*mesh);
    bare->setRestPoints({});
    return bare;
}

// Points (not roots) under the offset by more than `slack` of it, as the mesh measures them (not
// past maxDepth), and how deep under the offset the deepest one is.
struct Under {
    std::size_t count = 0;
    float worst = 0.0f;
};
Under under(const ScalpMesh& mesh, const StrandSet& s, float slack) {
    Under u;
    for (std::size_t k = 0; k < s.strandCount(); ++k) {
        for (std::size_t i = 1; i < s.strandPointCount(k); ++i) {
            const float h = exactHeight(mesh, s.strandPoints(k)[i]);
            if (h <= -30.0f) continue;
            u.count += h < 0.08f * (1.0f - slack);
            u.worst = std::max(u.worst, 0.08f - h);
        }
    }
    return u;
}

TEST(Collide, FieldIsLocalToTheMesh) {
    // A sphere and the same sphere with its vertices beyond x = 7.5 pushed out by 6% (an edit of one
    // side, as a new rest pose): strands combed straight down through both (every one on the upper
    // half pushed out), those rooted on the other side (below x = -2.5) come out the same to the
    // bit. The field's grid is the world's: nothing there depends on how far the other side reaches.
    const MeshPtr sphere = test::makeSphere(64, 128, 10.0f);
    std::vector<Vec3f> points = sphere->points();
    for (Vec3f& p : points) {
        if (p.x > 7.5f) p = p * 1.06f;
    }
    const MeshPtr edited = std::make_shared<const ScalpMesh>(points, sphere->triangles());
    Rng rng(4);
    auto strandsOn = [&](const ScalpMesh& mesh, const std::vector<StrandRoot>& roots) {
        auto out = std::make_shared<StrandSet>();
        for (std::size_t s = 0; s < roots.size(); ++s) {
            const Vec3f p = mesh.position(roots[s]);
            std::vector<Vec3f> pts;
            for (int i = 0; i < 16; ++i) pts.push_back(p + Vec3f(0.0f, -0.4f * static_cast<float>(i), 0.0f));
            out->addStrand(static_cast<StrandId>(s + 1), pts, {}, roots[s]);
        }
        return out;
    };
    std::vector<StrandRoot> roots;
    while (roots.size() < 6000) {
        const Vec3f n = Vec3f(rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1));
        if (n.length() > 1.0f || n.length() < 0.1f || n.normalized().y < 0.0f) continue;
        roots.push_back(sphere->closestPoint(n.normalized() * 10.0f).root);
    }
    auto collide = [&](const MeshPtr& mesh) {
        auto op = OperatorRegistry::instance().create(op::kCollide);
        op->params().set("offset", 0.05);
        op->params().set("maxDepth", 3.0);
        const EvalContext ctx;
        return valueAs<StrandsPtr>(op->evaluate(ctx, {strandsOn(*mesh, roots), mesh}));
    };
    const StrandsPtr before = collide(sphere), after = collide(edited);
    ASSERT_TRUE(before && after);
    std::size_t far = 0, differ = 0;
    float worst = 0.0f;
    for (std::size_t s = 0; s < before->strandCount(); ++s) {
        if (before->strandPoints(s)[0].x >= -2.5f) continue;
        ++far;
        const std::size_t n = before->strandPointCount(s);
        const Vec3f* a = before->strandPoints(s);
        const Vec3f* b = after->strandPoints(s);
        if (std::memcmp(a, b, n * sizeof(Vec3f)) != 0) ++differ;
        for (std::size_t j = 0; j < n; ++j) worst = std::max(worst, (a[j] - b[j]).length());
    }
    std::printf("[ field ] one side of the mesh edited: %zu of %zu strands on the other side differ (worst %.2e)\n", differ, far,
                worst);
    EXPECT_GT(far, 1000u);
    EXPECT_EQ(differ, 0u);
}

// ---- The guide simulation through the rest field ----

// Every `step`th of the head's strands as guides (no root on the scalp: pinned by their first
// points, so the scalp, when they are given one, is asked as it is, as a collider is).
GuidesPtr headGuides(const StrandSet& strands, std::size_t step) {
    auto guides = std::make_shared<GuideSet>();
    for (std::size_t s = 0; s < strands.strandCount(); s += step) {
        const Vec3f* p = strands.strandPoints(s);
        guides->add(static_cast<std::uint64_t>(s + 1), std::vector<Vec3f>(p, p + strands.strandPointCount(s)));
    }
    return guides;
}
GuidesPtr movedGuides(const GuidesPtr& in, const std::function<Vec3f(const Vec3f&)>& motion) {
    auto out = std::make_shared<GuideSet>(*in);
    for (std::size_t g = 0; g < out->size(); ++g) {
        for (Vec3f& p : out->guides()[g].points) p = motion(p);
    }
    return out;
}

// The head moving by motion(frame / (frames - 1)) as the guides' collider (or their scalp), the
// guides moving with it: the solver started on frame 0, stepped to each next frame; each step
// timed (after the first) and the fields made then counted.
struct HeadSim {
    std::vector<Vec3f> points;  // after the last frame
    double msPerStep = 0.0;
    std::size_t madeAfterFirst = 0;
};
HeadSim simulateHead(const std::shared_ptr<const ScalpMesh>& head, const GuidesPtr& atRest, bool asScalp, bool rest,
                     const std::function<Vec3f(const Vec3f&, float)>& motion, int frames, float nudge = 0.0f) {
    SimSettings s;  // the node's defaults (cm: the head's units)
    s.collisionOffset = 0.08f;
    auto inputsAt = [&](int frame) {
        const float t = static_cast<float>(frame) / static_cast<float>(std::max(frames - 1, 1));
        auto m = [&](const Vec3f& p) { return motion(p, t); };
        MeshPtr mesh = movedHead(head, m);
        if (!rest) mesh = withoutRest(mesh);
        GuidesPtr guides = movedGuides(atRest, m);
        if (nudge != 0.0f) {
            auto n = std::make_shared<GuideSet>(*guides);
            for (std::size_t g = 0; g < n->size(); ++g) {
                for (std::size_t j = 1; j < n->guides()[g].points.size(); ++j) n->guides()[g].points[j].x += nudge;
            }
            guides = n;
        }
        SimInputs in;
        in.guides = guides;
        if (asScalp) {
            in.scalp = mesh;
        } else {
            in.colliders = {mesh};
        }
        return in;
    };
    GuideSolver solver;
    solver.start(s, 24.0, 0, inputsAt(0));
    HeadSim out;
    for (int f = 1; f < frames; ++f) {
        const SimInputs in = inputsAt(f);
        const std::size_t made = MeshDistanceField::madeCount();
        const auto t0 = std::chrono::steady_clock::now();
        solver.step(in, 1);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (f > 1) {
            out.msPerStep += ms / (frames - 2);
            out.madeAfterFirst += MeshDistanceField::madeCount() - made;
        }
    }
    out.points = solver.points();
    return out;
}

Apart apart(const std::vector<Vec3f>& a, const std::vector<Vec3f>& b) {
    Apart r;
    double sum = 0.0;
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
        const double d = (a[i] - b[i]).length();
        r.worst = std::max(r.worst, d);
        sum += d;
        r.over += d > 1e-3;
    }
    r.mean = sum / static_cast<double>(std::max<std::size_t>(a.size(), 1));
    return r;
}

}  // namespace
}  // namespace groomlab
