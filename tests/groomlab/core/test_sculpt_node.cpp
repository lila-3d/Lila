// SPDX-License-Identifier: Apache-2.0
// Modified for Lila (2026).
// The Sculpt node (sculpt.h): brushed edits of generated guides kept as relative forms, by guide
// ID, in the groom; strokes on the node's output through the stroke system.

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <random>

#include <gtest/gtest.h>

#include "groomlab/curve.h"
#include "groomlab/graph_edit.h"
#include "groomlab/groom.h"
#include "groomlab/groom_file.h"
#include "groomlab/random.h"
#include "groomlab/sculpt.h"
#include "groomlab/stroke.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

constexpr float kPi = 3.14159265f;

// Rotates p about the X axis through `pivot` by `angle` radians.
Vec3f turnAboutX(const Vec3f& p, const Vec3f& pivot, float angle) {
    const Vec3f d = p - pivot;
    const float c = std::cos(angle), s = std::sin(angle);
    return pivot + Vec3f(d.x, d.y * c - d.z * s, d.y * s + d.z * c);
}

// A guide rooted at `root` on the plane (normal +Y), `segments` segments of `length` in all,
// bent toward +Z by `bend` radians spread evenly along it (0: straight up).
Guide planeGuide(std::uint64_t id, const Vec3f& root, int segments, float length, float bend, const ScalpMesh& plane) {
    Guide g;
    g.id = id;
    g.root = plane.closestPoint(root).root;
    Vec3f p = root;
    g.points.push_back(p);
    for (int i = 0; i < segments; ++i) {
        const float a = bend * (static_cast<float>(i) + 0.5f) / static_cast<float>(segments);
        p = p + Vec3f(0.0f, std::cos(a), std::sin(a)) * (length / static_cast<float>(segments));
        g.points.push_back(p);
    }
    return g;
}

// The guide with its last `tip` segments turned `angle` about X at the point they start from: a hook.
std::vector<Vec3f> hooked(const std::vector<Vec3f>& pts, std::size_t tip, float angle) {
    std::vector<Vec3f> out = pts;
    const std::size_t from = pts.size() - 1 - tip;
    for (std::size_t k = from + 1; k < pts.size(); ++k) out[k] = turnAboutX(pts[k], pts[from], angle);
    return out;
}

float angleBetween(const Vec3f& a, const Vec3f& b) {
    return std::acos(std::clamp(a.normalized().dot(b.normalized()), -1.0f, 1.0f));
}

float maxDistance(const std::vector<Vec3f>& a, const std::vector<Vec3f>& b) {
    float d = 0.0f;
    for (std::size_t k = 0; k < std::min(a.size(), b.size()); ++k) d = std::max(d, (a[k] - b[k]).length());
    return d;
}

TEST(SculptNode, ARecordedEditGivesTheSculptedShapeBack) {
    const auto plane = test::makePlane(8, 2.0f);
    const Guide g = planeGuide(7, {1.0f, 0.0f, 1.0f}, 15, 1.0f, 0.8f, *plane);
    // A hook near the tip, a comb at the root (the whole guide turned about the root), longer.
    std::vector<Vec3f> sculpted = hooked(g.points, 4, 0.6f);
    for (Vec3f& p : sculpted) p = turnAboutX(p, g.points[0], -0.3f);
    for (std::size_t k = 1; k < sculpted.size(); ++k) sculpted[k] = sculpted[0] + (sculpted[k] - sculpted[0]) * 1.2f;
    for (const float strength : {1.0f, 0.5f, 1.7f}) {
        const GuideSculpt edit = recordSculpt(g, sculpted, strength, plane.get());
        ASSERT_EQ(edit.samples.size(), g.points.size() - 1);
        EXPECT_LT(maxDistance(applySculpt(g, edit, strength, plane.get()), sculpted), 1e-5f) << strength;
        // Strength 0: the input, to the bit.
        EXPECT_EQ(applySculpt(g, edit, 0.0f, plane.get()), g.points);
    }
    EXPECT_THROW(recordSculpt(g, sculpted, 0.0f, plane.get()), std::invalid_argument);
    // An edit of nothing is none.
    EXPECT_TRUE(recordSculpt(g, g.points, 1.0f, plane.get()).identity());
}

TEST(SculptNode, AHookKeepsItsRelativeFormWhenTheInputChanges) {
    const auto plane = test::makePlane(8, 2.0f);
    const Guide straight = planeGuide(3, {1.0f, 0.0f, 1.0f}, 15, 1.0f, 0.0f, *plane);
    const float hook = 30.0f * kPi / 180.0f;
    const GuideSculpt edit = recordSculpt(straight, hooked(straight.points, 4, hook), 1.0f, plane.get());
    // The input made again upstream: bent, half as long again, with another point count.
    for (const int segments : {15, 22, 9}) {
        const Guide bent = planeGuide(3, {1.0f, 0.0f, 1.0f}, segments, 1.5f, 1.1f, *plane);
        const std::vector<Vec3f> out = applySculpt(bent, edit, 1.0f, plane.get());
        ASSERT_EQ(out.size(), bent.points.size());
        float total = 0.0f, along = 0.0f;
        for (std::size_t k = 0; k + 1 < bent.points.size(); ++k) total += (bent.points[k + 1] - bent.points[k]).length();
        for (std::size_t k = 0; k + 1 < bent.points.size(); ++k) {
            const Vec3f in = bent.points[k + 1] - bent.points[k];
            const Vec3f mine = out[k + 1] - out[k];
            const float t = (along + 0.5f * in.length()) / total;
            along += in.length();
            // Lengths kept; the bend the input has, plus the hook past 11/15 of the length.
            EXPECT_NEAR(mine.length(), in.length(), 1e-5f);
            EXPECT_NEAR(out[k + 1].x, 1.0f, 1e-5f) << "stays in the bend's plane";
            if (t > 12.0f / 15.0f) EXPECT_NEAR(angleBetween(mine, in), hook, 2e-3f) << segments << " " << t;
            if (t < 10.0f / 15.0f) EXPECT_NEAR(angleBetween(mine, in), 0.0f, 2e-3f) << segments << " " << t;
        }
    }
    // Turned rigidly with its root (a scalp turned with it): the result turns the same way.
    const Guide bent = planeGuide(3, {1.0f, 0.0f, 1.0f}, 15, 1.0f, 0.5f, *plane);
    std::vector<Vec3f> turnedPoints;
    for (const Vec3f& p : plane->points()) turnedPoints.push_back(turnAboutX(p, {1.0f, 0.0f, 1.0f}, 0.7f));
    const auto turnedPlane = std::make_shared<const ScalpMesh>(turnedPoints, plane->triangles());
    Guide turned = bent;
    for (Vec3f& p : turned.points) p = turnAboutX(p, {1.0f, 0.0f, 1.0f}, 0.7f);
    std::vector<Vec3f> expected = applySculpt(bent, edit, 1.0f, plane.get());
    for (Vec3f& p : expected) p = turnAboutX(p, {1.0f, 0.0f, 1.0f}, 0.7f);
    EXPECT_LT(maxDistance(applySculpt(turned, edit, 1.0f, turnedPlane.get()), expected), 1e-5f);
}

TEST(SculptNode, EditsFollowIdsAndWaitForMissingOnes) {
    const auto plane = test::makePlane(8, 2.0f);
    auto set = std::make_shared<GuideSet>();
    for (std::uint64_t id = 1; id <= 5; ++id) {
        set->guides().push_back(planeGuide(id * 1000003u, {0.2f + 0.3f * static_cast<float>(id), 0.0f, 1.0f}, 12, 0.8f, 0.3f, *plane));
    }
    SculptEdits edits;
    const Guide& second = set->guides()[1];
    edits.guides[second.id] = recordSculpt(second, hooked(second.points, 3, 0.5f), 1.0f, plane.get());
    edits.guides[999] = edits.guides[second.id];  // an ID the input does not have: dormant
    const GuidesPtr out = applySculpts(set, edits, 1.0f, plane.get());
    // Shuffled: each guide gets what its ID gets.
    auto shuffled = std::make_shared<GuideSet>(*set);
    std::mt19937 rng(4);
    std::shuffle(shuffled->guides().begin(), shuffled->guides().end(), rng);
    const GuidesPtr outShuffled = applySculpts(shuffled, edits, 1.0f, plane.get());
    for (const Guide& g : outShuffled->guides()) {
        const auto it = std::find_if(out->guides().begin(), out->guides().end(), [&](const Guide& o) { return o.id == g.id; });
        ASSERT_NE(it, out->guides().end());
        EXPECT_EQ(it->points, g.points);
        const auto in = std::find_if(set->guides().begin(), set->guides().end(), [&](const Guide& o) { return o.id == g.id; });
        if (g.id == second.id) {
            EXPECT_GT(maxDistance(g.points, in->points), 0.05f);
        } else {
            EXPECT_EQ(g.points, in->points);  // untouched guides as they came
        }
    }
    // The missing ID comes back: its edit applies again.
    Guide back = planeGuide(999, {1.5f, 0.0f, 0.5f}, 12, 0.8f, 0.3f, *plane);
    auto withBack = std::make_shared<GuideSet>(*set);
    withBack->guides().push_back(back);
    const GuidesPtr outBack = applySculpts(withBack, edits, 1.0f, plane.get());
    EXPECT_GT(maxDistance(outBack->guides().back().points, back.points), 0.05f);
    // No edit reaches a set: the set itself.
    SculptEdits elsewhere;
    elsewhere.guides[999] = edits.guides[second.id];
    EXPECT_EQ(applySculpts(set, elsewhere, 1.0f, plane.get()), set);
}

TEST(SculptNode, ScaleInvariantAndDeterministic) {
    const auto plane = test::makePlane(8, 2.0f);
    const Guide g = planeGuide(11, {1.0f, 0.0f, 1.0f}, 15, 1.0f, 0.6f, *plane);
    const GuideSculpt edit = recordSculpt(g, hooked(g.points, 5, 0.7f), 1.0f, plane.get());
    // The same at 100 times the size: the scene and the guide scaled, the edit as it was.
    std::vector<Vec3f> big;
    for (const Vec3f& p : plane->points()) big.push_back(p * 100.0f);
    const auto bigPlane = std::make_shared<const ScalpMesh>(big, plane->triangles());
    Guide bigGuide = g;
    for (Vec3f& p : bigGuide.points) p = p * 100.0f;
    const std::vector<Vec3f> small = applySculpt(g, edit, 1.0f, plane.get());
    const std::vector<Vec3f> large = applySculpt(bigGuide, edit, 1.0f, bigPlane.get());
    for (std::size_t k = 0; k < small.size(); ++k) EXPECT_LT((large[k] * 0.01f - small[k]).length(), 1e-5f);
    // Recorded at that size: the same edit.
    const GuideSculpt bigEdit = recordSculpt(bigGuide, hooked(bigGuide.points, 5, 0.7f), 1.0f, bigPlane.get());
    ASSERT_EQ(bigEdit.samples.size(), edit.samples.size());
    for (std::size_t k = 0; k < edit.samples.size(); ++k) {
        EXPECT_NEAR(bigEdit.samples[k].ax, edit.samples[k].ax, 1e-5f);
        EXPECT_NEAR(bigEdit.samples[k].ay, edit.samples[k].ay, 1e-5f);
        EXPECT_NEAR(bigEdit.samples[k].scale, edit.samples[k].scale, 1e-5f);
    }
    EXPECT_EQ(applySculpt(g, edit, 1.0f, plane.get()), small);  // to the bit, every time
}

TEST(SculptNode, TextsRoundTripRenameCopyAndClear) {
    const auto plane = test::makePlane(8, 2.0f);
    const Guide g = planeGuide(42, {1.0f, 0.0f, 1.0f}, 10, 1.0f, 0.4f, *plane);
    SculptSet set;
    set.nodes["sculpt1"].guides[std::uint64_t{18446744073709551557ull}] = recordSculpt(g, hooked(g.points, 3, 0.4f), 1.0f, plane.get());
    const std::string text = set.toJson();
    EXPECT_EQ(SculptSet::fromJson(text).nodes, set.nodes);  // floats exactly, 64-bit IDs whole
    EXPECT_TRUE(SculptSet::fromJson("").empty());
    EXPECT_THROW(SculptSet::fromJson("{nope"), std::invalid_argument);
    const std::string renamed = renameSculptNode(text, "sculpt1", "shape");
    EXPECT_EQ(sculptedNodes(renamed), std::vector<std::string>{"shape"});
    EXPECT_EQ(SculptSet::fromJson(renamed).nodes.at("shape"), set.nodes.at("sculpt1"));
    EXPECT_EQ(renameSculptNode(text, "other", "x"), text);
    const std::string copied = copySculptNode(text, "sculpt1", "sculpt2");
    EXPECT_EQ(sculptedNodes(copied), (std::vector<std::string>{"sculpt1", "sculpt2"}));
    EXPECT_EQ(sculptedNodes(clearSculptNode(copied, "sculpt1")), std::vector<std::string>{"sculpt2"});

    // A compound renamed or copied takes its inner nodes' edits along ("compound1/sculpt1"), not
    // those of a node whose name only starts the same ("compound10/...").
    SculptSet inner;
    inner.nodes["compound1/sculpt1"] = set.nodes.at("sculpt1");
    inner.nodes["compound1/deep/sculpt1"] = set.nodes.at("sculpt1");
    inner.nodes["compound10/sculpt1"] = set.nodes.at("sculpt1");
    const std::string grouped = inner.toJson();
    EXPECT_EQ(sculptedNodes(renameSculptNode(grouped, "compound1", "shaping")),
              (std::vector<std::string>{"compound10/sculpt1", "shaping/deep/sculpt1", "shaping/sculpt1"}));
    EXPECT_EQ(sculptedNodes(copySculptNode(grouped, "compound1/deep", "deep")),
              (std::vector<std::string>{"compound1/deep/sculpt1", "compound1/sculpt1", "compound10/sculpt1", "deep/sculpt1"}));
    // Moves in order: grouped, then the compound renamed.
    EXPECT_EQ(sculptedNodes(moveSculptNodes(text, {{"sculpt1", "compound1/sculpt1"}, {"compound1", "shaping"}})),
              std::vector<std::string>{"shaping/sculpt1"});
    EXPECT_EQ(moveSculptNodes(text, {{"other", "x"}}), text);
}

// ---- Through the graph and a session ------------------------------------------------------------

// Sparse hand guides -> an even Scatter + Interpolate -> ToGuides -> Sculpt -> Interpolate (the
// render's strands), on a unit sphere. `sculpt`: the Sculpt node's name.
std::string workflowGraph(const std::string& sculpt = "sculpt1", double strength = 1.0) {
    return R"({"output": "output", "nodes": [
      {"name": "scalp", "type": "Scalp"},
      {"name": "sparse", "type": "Guides"},
      {"name": "even", "type": "Scatter", "params": {"density": 6.0, "randomness": 0.0, "length": 0.5, "previewAll": true}},
      {"name": "dense", "type": "Interpolate"},
      {"name": "toGuides", "type": "ToGuides"},
      {"name": ")" + sculpt + R"(", "type": "Sculpt", "params": {"strength": )" + std::to_string(strength) + R"(}},
      {"name": "roots", "type": "Scatter", "params": {"density": 60.0}},
      {"name": "hair", "type": "Interpolate"},
      {"name": "output", "type": "Output"}],
     "connections": [
      {"from": "scalp", "to": "even", "input": "scalp"},
      {"from": "even", "to": "dense", "input": "strands"},
      {"from": "sparse", "to": "dense", "input": "guides"},
      {"from": "scalp", "to": "dense", "input": "scalp"},
      {"from": "dense", "to": "toGuides", "input": "strands"},
      {"from": "toGuides", "to": ")" + sculpt + R"(", "input": "guides"},
      {"from": "scalp", "to": ")" + sculpt + R"(", "input": "scalp"},
      {"from": "scalp", "to": "roots", "input": "scalp"},
      {"from": "roots", "to": "hair", "input": "strands"},
      {"from": ")" + sculpt + R"(", "to": "hair", "input": "guides"},
      {"from": "scalp", "to": "hair", "input": "scalp"},
      {"from": "hair", "to": "output", "input": "strands"}]})";
}

// A few hand guides over the upper half of the unit sphere, leaning by `lean` toward +Z.
std::string sparseGuides(const ScalpMesh& scalp, float lean) {
    GuideHierarchy h;
    const GroupId g = h.addGroup("primary");
    for (int i = 0; i < 12; ++i) {
        const float a = 2.0f * kPi * static_cast<float>(i) / 12.0f;
        const Vec3f n = Vec3f(0.6f * std::cos(a), 0.75f, 0.6f * std::sin(a)).normalized();
        const Vec3f dir = (n + Vec3f(0.0f, 0.0f, lean)).normalized();
        h.addGuide(g, test::straightCurve(n, dir * 0.6f, 10));
    }
    h.evaluate(&scalp);
    return h.toJson();
}

const Guide* byId(const GuideSet& set, std::uint64_t id) {
    for (const Guide& g : set.guides()) {
        if (g.id == id) return &g;
    }
    return nullptr;
}

// Per segment: the angle between the output's and the input's segment, and their length ratio.
std::vector<std::pair<float, float>> relativeForm(const Guide& in, const Guide& out) {
    std::vector<std::pair<float, float>> form;
    for (std::size_t k = 0; k + 1 < in.points.size(); ++k) {
        const Vec3f a = in.points[k + 1] - in.points[k], b = out.points[k + 1] - out.points[k];
        form.emplace_back(angleBetween(a, b), b.length() / a.length());
    }
    return form;
}

TEST(SculptNode, TheEditableGuidesDrawInTheirToGuidesNodesColor) {
    // ToGuides tags the guides it makes with its color; a Sculpt node's copy keeps it, and the
    // editable guides (sculptHierarchy) draw in it, not the hand guides' blue.
    GuideSet guides;
    guides.add(7, {{0, 1, 0}, {0, 2, 0}});
    EXPECT_FALSE(guides.hasDisplayColor());
    EXPECT_FALSE(sculptHierarchy(guides, "sculpt1", nullptr)->groups().front().hasColor);
    guides.setDisplayColor({0.9f, 0.2f, 0.1f});
    const std::uint64_t hash = guides.contentHash();
    const auto copied = std::make_shared<GuideSet>(guides);
    EXPECT_TRUE(copied->hasDisplayColor());
    const auto shown = sculptHierarchy(*copied, "sculpt1", nullptr);
    ASSERT_TRUE(shown->groups().front().hasColor);
    EXPECT_FLOAT_EQ(shown->groups().front().color[0], 0.9f);
    EXPECT_FLOAT_EQ(shown->groups().front().color[2], 0.1f);
    GuideSet plain;
    plain.add(7, {{0, 1, 0}, {0, 2, 0}});
    EXPECT_EQ(plain.contentHash(), hash);  // display only
}

}  // namespace
}  // namespace groomlab
