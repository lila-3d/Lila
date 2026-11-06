// SPDX-License-Identifier: Apache-2.0
// Modified for Lila (2026).
// M5 grooming: guide attributes into strands, attribute (clump) paint,
// selection masks, symmetry, guide selection and hierarchy tools.

#include <gtest/gtest.h>

#include <cmath>

#include "groomlab/brush.h"
#include "groomlab/graph_io.h"
#include "groomlab/groom.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/guide_selection.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

// Primaries on the +x half of a symmetric sphere, mirrored to -x, with children.
struct Scene {
    std::shared_ptr<const ScalpMesh> scalp = test::makeSphere(24, 48, 1.0f);
    GuideHierarchy h;
    GroupId primary = kNoGroup, children = kNoGroup;

    explicit Scene(bool withChildren = true) {
        primary = h.addGroup("primary");
        TubeProfile tube;
        tube.width = {0.15f, 0.1f};
        const float angles[] = {0.3f, 0.7f, 1.1f};
        for (const float a : angles) {
            const Vec3f n = Vec3f{std::sin(a), std::cos(a), 0.25f}.normalized();
            h.addGuide(primary, test::straightCurve(n, n * 0.7f, 8), tube);
        }
        h.evaluate(scalp.get());
        h.mirrorGroup(primary, 0, 0.0f, true);
        h.evaluate(scalp.get());
        if (withChildren) {
            ChildGenerationParams p;
            p.count = 3;
            children = h.generateChildren(primary, "children", p);
            h.evaluate(scalp.get());
        }
    }
    const HierarchyGuide& mirrorOf(const HierarchyGuide& g) const {
        const std::vector<GuideId> m = mirrorSelection(h, {g.id}, 0, 0.0f, 1e-3f);
        EXPECT_EQ(m.size(), 1u);
        return *h.guide(m.at(0));
    }
};

float maxDistance(const std::vector<Vec3f>& a, const std::vector<Vec3f>& b) {
    float d = 0.0f;
    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) d = std::max(d, (a[i] - b[i]).length());
    return d;
}

TEST(Mirror, MirrorGroupMakesASymmetricSet) {
    Scene s(false);
    ASSERT_EQ(s.h.guidesInGroup(s.primary).size(), 6u);
    for (const GuideId id : s.h.guidesInGroup(s.primary)) {
        const HierarchyGuide& g = *s.h.guide(id);
        const HierarchyGuide& m = s.mirrorOf(g);
        EXPECT_NE(m.id, g.id);
        std::vector<Vec3f> mirrored = g.points;
        for (Vec3f& p : mirrored) p = mirrorPoint(p, 0, 0.0f);
        EXPECT_LT(maxDistance(mirrored, m.points), 1e-4f);
    }
    // Mirroring again replaces the other side instead of adding to it.
    s.h.setAttribute(s.h.guidesInGroup(s.primary)[0], "clump", 0.5f);
    s.h.mirrorGroup(s.primary, 0, 0.0f, true);
    EXPECT_EQ(s.h.guidesInGroup(s.primary).size(), 6u);
    EXPECT_THROW(s.h.mirrorGroup(s.primary, 3, 0.0f, true), std::invalid_argument);
}

TEST(Mirror, MirroredGuidesTakeMirroredFrames) {
    // Both mirror tools give a copy the mirror image of its source's frames (frameUp mirrored),
    // so what hangs off the copies is laid out as the source's mirror image.
    Scene s;
    auto expectMirroredFrames = [&](const std::vector<GuideId>& created) {
        ASSERT_FALSE(created.empty());
        for (const GuideId id : created) {
            const HierarchyGuide& copy = *s.h.guide(id);
            const HierarchyGuide& src = s.mirrorOf(copy);
            EXPECT_LT((copy.frameUp - mirrorVector(src.frameUp, 0)).length(), 1e-6f) << id;
            const std::vector<Frame> a = copy.frames(), b = src.frames();
            ASSERT_EQ(a.size(), b.size());
            EXPECT_LT((a.back().normal - mirrorVector(b.back().normal, 0)).length(), 1e-4f) << id;
        }
    };
    expectMirroredFrames(s.h.mirrorGroup(s.primary, 0, 0.0f, true));
    s.h.evaluate(s.scalp.get());
    s.h.generateChildren(s.primary, "children", {});
    s.h.evaluate(s.scalp.get());
    expectMirroredFrames(s.h.mirrorAll(0, 0.0f, true));  // children too
}

TEST(Mirror, SymmetrySyncCopiesWholeTreesWithEditedChildren) {
    Scene s;
    ASSERT_EQ(s.h.guidesInGroup(s.children).size(), 18u);
    // Hand edits on the +x side: a child bent, a child locked, an attribute.
    GuideId bent = kNoGuide, locked = kNoGuide;
    for (const GuideId id : s.h.guidesInGroup(s.children)) {
        const HierarchyGuide& g = *s.h.guide(id);
        if (g.points.front().x <= 0.05f) continue;
        if (bent == kNoGuide) bent = id;
        else if (locked == kNoGuide) locked = id;
    }
    ASSERT_NE(locked, kNoGuide);
    std::vector<Vec3f> pts = s.h.guide(bent)->points;
    for (std::size_t k = 1; k < pts.size(); ++k) pts[k] = pts[k] + Vec3f{0.0f, 0.0f, 0.05f * float(k)};
    s.h.setPoints(bent, pts);
    s.h.lock(locked);
    s.h.setAttribute(bent, "clump", 0.75f);
    s.h.evaluate(s.scalp.get());

    const std::vector<GuideId> created = s.h.mirrorAll(0, 0.0f, true);
    s.h.evaluate(s.scalp.get());
    EXPECT_EQ(created.size(), 3u + 9u);  // the +x primaries and their children
    EXPECT_EQ(s.h.guidesInGroup(s.primary).size(), 6u);
    EXPECT_EQ(s.h.guidesInGroup(s.children).size(), 18u);
    // Every guide has a mirror image: the edited child too.
    for (const GuideId id : s.h.guidesInGroup(s.children)) {
        const HierarchyGuide& g = *s.h.guide(id);
        const HierarchyGuide& m = s.mirrorOf(g);
        std::vector<Vec3f> mirrored = g.points;
        for (Vec3f& p : mirrored) p = mirrorPoint(p, 0, 0.0f);
        EXPECT_LT(maxDistance(mirrored, m.points), 2e-3f) << "child " << id;
        EXPECT_EQ(m.parent != kNoGuide, true);
        EXPECT_EQ(s.h.guide(m.parent)->points.front().x * g.points.front().x < 0.0f, true);
    }
    const HierarchyGuide& bentMirror = s.mirrorOf(*s.h.guide(bent));
    EXPECT_FLOAT_EQ(bentMirror.attributes.at("clump"), 0.75f);
    EXPECT_TRUE(s.mirrorOf(*s.h.guide(locked)).locked);
    // Syncing the other way round keeps the counts.
    s.h.mirrorAll(0, 0.0f, false);
    EXPECT_EQ(s.h.guidesInGroup(s.children).size(), 18u);
    EXPECT_THROW(s.h.mirrorAll(5, 0.0f, true), std::invalid_argument);
}

TEST(Paint, AttributeBrushPaintsTowardTheValue) {
    Scene s(false);
    const HierarchyGuide& target = *s.h.guide(s.h.guidesInGroup(s.primary)[0]);
    BrushSettings b;
    b.mode = brushModeFromString("clump");
    b.radius = 0.3f;
    b.strength = 0.5f;
    b.value = 1.0f;
    const Vec3f at = target.points[2];
    applyBrushStroke(s.h, s.scalp.get(), b, {at});
    const float once = s.h.guide(target.id)->attributes.at("clump");
    EXPECT_GT(once, 0.0f);
    EXPECT_LE(once, 0.5f);
    for (int i = 0; i < 20; ++i) applyBrushStroke(s.h, s.scalp.get(), b, {at});
    EXPECT_NEAR(s.h.guide(target.id)->attributes.at("clump"), 1.0f, 0.05f);
    // Painting down, and guides out of reach are untouched.
    b.value = 0.0f;
    applyBrushStroke(s.h, s.scalp.get(), b, {at});
    EXPECT_LT(s.h.guide(target.id)->attributes.at("clump"), 1.0f);
    EXPECT_EQ(s.mirrorOf(target).attributes.count("clump"), 0u);
}

TEST(Paint, SelectionMaskLimitsTheBrush) {
    Scene s(false);
    const std::vector<GuideId> ids = s.h.guidesInGroup(s.primary);
    const Scene before(false);
    std::map<GuideId, float> mask{{ids[0], 1.0f}, {ids[1], 0.5f}};
    BrushSettings b;
    b.radius = 5.0f;  // reaches every guide
    b.mask = &mask;
    applyBrushStroke(s.h, s.scalp.get(), b, {{0, 1.5f, 0}, {0, 1.5f, 0.4f}});
    const float full = maxDistance(s.h.guide(ids[0])->points, before.h.guide(ids[0])->points);
    const float half = maxDistance(s.h.guide(ids[1])->points, before.h.guide(ids[1])->points);
    EXPECT_GT(full, 0.01f);
    EXPECT_GT(half, 0.0f);
    EXPECT_LT(half, full);
    for (std::size_t i = 2; i < ids.size(); ++i) {
        EXPECT_EQ(s.h.guide(ids[i])->points, before.h.guide(ids[i])->points);
    }
}

TEST(Symmetry, MirroredStrokesKeepAGroomSymmetric) {
    Scene s;
    BrushSettings b;
    b.radius = 0.5f;
    b.level = 0;
    b.mirrorAxis = 0;
    applyBrushStroke(s.h, s.scalp.get(), b, {{0.4f, 1.2f, 0.1f}, {0.4f, 1.2f, 0.5f}});  // +x side only
    bool moved = false;
    const Scene original;
    for (const GuideId id : s.h.guidesInGroup(s.primary)) {
        const HierarchyGuide& g = *s.h.guide(id);
        moved = moved || maxDistance(g.points, original.h.guide(id)->points) > 0.01f;
        std::vector<Vec3f> mirrored = g.points;
        for (Vec3f& p : mirrored) p = mirrorPoint(p, 0, 0.0f);
        EXPECT_LT(maxDistance(mirrored, s.mirrorOf(g).points), 1e-3f) << "guide " << id;
    }
    EXPECT_TRUE(moved);
}

TEST(Selection, ByLevelGroupLassoSoftAndMirror) {
    Scene s;
    EXPECT_EQ(guidesAt(s.h, 0).size(), 6u);
    EXPECT_EQ(guidesAt(s.h, 1).size(), 18u);
    EXPECT_EQ(guidesAt(s.h, -1).size(), 24u);
    EXPECT_EQ(guidesAt(s.h, 0, "children").size(), 18u);
    EXPECT_TRUE(guidesAt(s.h, 0, "missing").empty());

    // Looking down -y: view = (x, z). A lasso around x > 0 takes the +x primaries.
    const ViewProjector top = [](const Vec3f& p, std::array<float, 2>* v) {
        *v = {p.x, p.z};
        return p.y > 0.0f;
    };
    const std::vector<std::array<float, 2>> right = {{0.0f, -2.0f}, {2.0f, -2.0f}, {2.0f, 2.0f}, {0.0f, 2.0f}};
    const std::vector<GuideId> lassoed = guidesInLasso(s.h, guidesAt(s.h, 0), top, right);
    ASSERT_EQ(lassoed.size(), 3u);
    for (const GuideId id : lassoed) EXPECT_GT(s.h.guide(id)->points[0].x, 0.0f);
    EXPECT_TRUE(guidesInLasso(s.h, guidesAt(s.h, 0), top, {{0, 0}, {1, 0}}).empty());  // not a polygon

    const std::map<GuideId, float> soft = softSelection(s.h, {lassoed[0]}, 0.8f);
    EXPECT_EQ(soft.at(lassoed[0]), 1.0f);
    ASSERT_GT(soft.size(), 1u);
    for (const auto& [id, w] : soft) {
        EXPECT_EQ(s.h.guide(id)->group, s.primary);  // same group only
        EXPECT_GT(w, 0.0f);
        EXPECT_LE(w, 1.0f);
    }
    EXPECT_EQ(softSelection(s.h, lassoed, 0.0f).size(), 3u);

    const std::vector<GuideId> mirrored = mirrorSelection(s.h, lassoed, 0, 0.0f, 1e-3f);
    ASSERT_EQ(mirrored.size(), 3u);
    for (const GuideId id : mirrored) EXPECT_LT(s.h.guide(id)->points[0].x, 0.0f);
}

TEST(HierarchyTools, ReparentKeepsShapesAndFollowsTheNewParent) {
    Scene s;
    const std::vector<GuideId> primaries = s.h.guidesInGroup(s.primary);
    const GuideId child = s.h.childrenOf(primaries[0])[0];
    const std::vector<Vec3f> shape = s.h.guide(child)->points;
    s.h.reparent({child}, primaries[1]);
    s.h.evaluate(s.scalp.get());
    EXPECT_EQ(s.h.guide(child)->parent, primaries[1]);
    EXPECT_LT(maxDistance(s.h.guide(child)->points, shape), 1e-3f);

    // It now follows primaries[1], not primaries[0].
    std::vector<Vec3f> moved = s.h.guide(primaries[1])->points;
    for (std::size_t i = 1; i < moved.size(); ++i) moved[i] += Vec3f{0.0f, 0.0f, 0.2f} * static_cast<float>(i);
    s.h.setPoints(primaries[1], moved);
    s.h.evaluate(s.scalp.get());
    EXPECT_GT(maxDistance(s.h.guide(child)->points, shape), 0.1f);
    EXPECT_THROW(s.h.reparent({child}, child), std::invalid_argument);  // not in the parent group
}

TEST(HierarchyTools, PromoteAndDemoteKeepShapes) {
    Scene s;
    std::map<GuideId, std::vector<Vec3f>> shapes;
    for (const GuideId id : s.h.guidesInGroup(s.children)) shapes[id] = s.h.guide(id)->points;

    s.h.promoteGroup(s.children);
    s.h.evaluate(s.scalp.get());
    EXPECT_EQ(s.h.groupDepth(s.children), 0);
    for (const auto& [id, pts] : shapes) {
        EXPECT_FALSE(s.h.guide(id)->isChild());
        EXPECT_LT(maxDistance(s.h.guide(id)->points, pts), 2e-3f);
    }
    EXPECT_THROW(s.h.promoteGroup(s.children), std::invalid_argument);  // already a root group

    s.h.demoteGroup(s.children, s.primary);
    s.h.evaluate(s.scalp.get());
    EXPECT_EQ(s.h.groupDepth(s.children), 1);
    for (const auto& [id, pts] : shapes) {
        EXPECT_TRUE(s.h.guide(id)->followsParent());
        EXPECT_LT(maxDistance(s.h.guide(id)->points, pts), 5e-3f);
    }
    EXPECT_THROW(s.h.demoteGroup(s.children, s.children), std::invalid_argument);

    // Survives a save and load.
    const GuideHierarchy loaded = GuideHierarchy::fromJson(s.h.toJson());
    EXPECT_EQ(loaded.groupDepth(loaded.findGroup("children")), 1);
}

}  // namespace
}  // namespace groomlab
