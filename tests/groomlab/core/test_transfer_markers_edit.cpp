// SPDX-License-Identifier: Apache-2.0
// The Transfer Groom windows' marker editor (transfer_markers_edit.h): two point lists paired by
// index, deletes that take the partner along, symmetry groups, undo and redo, the pairs a transfer
// takes, and the ray pick both hosts' views share.

#include <gtest/gtest.h>

#include "groomlab/transfer_markers_edit.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

MarkerPoint at(float x, float y = 0.0f, float z = 0.0f) { return MarkerPoint{StrandRoot{}, Vec3f{x, y, z}, 0}; }

std::vector<float> xs(const TransferMarkerEditor& e, MarkerSide side) {
    std::vector<float> out;
    for (const MarkerPoint& p : e.points(side)) out.push_back(p.point.x);
    return out;
}

constexpr MarkerSide L = MarkerSide::Old;
constexpr MarkerSide R = MarkerSide::New;

TEST(TransferMarkerEdit, PointsPairByIndexInAnyOrder) {
    TransferMarkerEditor e;
    EXPECT_EQ(e.summary(), "No markers");
    // Three on the left first, then two on the right: no alternation needed.
    EXPECT_EQ(e.add(L, at(1)), 0u);
    EXPECT_EQ(e.add(L, at(2)), 1u);
    EXPECT_EQ(e.add(L, at(3)), 2u);
    EXPECT_EQ(e.pairCount(), 0u);
    EXPECT_EQ(e.pendingCount(), 3u);
    EXPECT_EQ(e.add(R, at(10)), 0u);
    EXPECT_EQ(e.add(R, at(20)), 1u);
    EXPECT_EQ(e.pairCount(), 2u);
    EXPECT_EQ(e.pendingCount(), 1u);
    EXPECT_TRUE(e.matched(L, 1));
    EXPECT_FALSE(e.matched(L, 2));  // drawn hollow
    EXPECT_TRUE(e.matched(R, 1));
    EXPECT_EQ(e.summary(), "2 pairs, 1 unmatched");
    const auto pairs = e.pairs();
    ASSERT_EQ(pairs.size(), 2u);
    EXPECT_FLOAT_EQ(pairs[1].oldPoint.x, 2.0f);
    EXPECT_FLOAT_EQ(pairs[1].newPoint.x, 20.0f);
    e.add(R, at(30));
    EXPECT_EQ(e.summary(), "3 pairs");
}

TEST(TransferMarkerEdit, DeleteTakesThePartnerAndRenumbers) {
    TransferMarkerEditor e;
    for (float x : {1.0f, 2.0f, 3.0f, 4.0f}) e.add(L, at(x));
    for (float x : {10.0f, 20.0f, 30.0f}) e.add(R, at(x));
    // Pair 2 deleted from the right: the left's 2 goes too, and 3 and 4 move up.
    EXPECT_EQ(e.remove(R, 1), std::vector<std::size_t>{1});
    EXPECT_EQ(xs(e, L), (std::vector<float>{1, 3, 4}));
    EXPECT_EQ(xs(e, R), (std::vector<float>{10, 30}));
    EXPECT_EQ(e.summary(), "2 pairs, 1 unmatched");
    // An unmatched point deletes alone.
    e.remove(L, 2);
    EXPECT_EQ(xs(e, L), (std::vector<float>{1, 3}));
    EXPECT_EQ(xs(e, R), (std::vector<float>{10, 30}));
    EXPECT_THROW(e.remove(L, 5), std::out_of_range);
}

TEST(TransferMarkerEdit, UndoAndRedoWalkTheEdits) {
    TransferMarkerEditor e;
    e.add(L, at(1));
    e.add(R, at(10));
    e.add(L, at(2));
    EXPECT_EQ(e.undoDepth(), 3u);
    e.remove(L, 0);
    EXPECT_EQ(e.pairCount(), 0u);
    ASSERT_TRUE(e.undo());
    EXPECT_EQ(xs(e, L), (std::vector<float>{1, 2}));
    EXPECT_EQ(xs(e, R), (std::vector<float>{10}));
    ASSERT_TRUE(e.undo());
    EXPECT_EQ(xs(e, L), (std::vector<float>{1}));
    ASSERT_TRUE(e.redo());
    EXPECT_EQ(xs(e, L), (std::vector<float>{1, 2}));
    // An edit after an undo drops the redo steps.
    e.undo();
    e.add(R, at(20));
    EXPECT_FALSE(e.canRedo());
    EXPECT_FALSE(e.redo());
    // A drag: its first move is a step, the rest join it.
    e.add(L, at(5));
    const std::size_t depth = e.undoDepth();
    e.move(L, 1, at(6));
    e.move(L, 1, at(7), false);
    e.move(L, 1, at(8), false);
    EXPECT_EQ(e.undoDepth(), depth + 1);
    e.undo();
    EXPECT_EQ(xs(e, L), (std::vector<float>{1, 5}));
    // Reset (a window opening) clears the history; setPairs is a step.
    e.reset({TransferMarker{{}, {1, 0, 0}, {}, {2, 0, 0}}});
    EXPECT_FALSE(e.canUndo());
    EXPECT_EQ(e.pairCount(), 1u);
    e.setPairs({});
    EXPECT_EQ(e.pairCount(), 0u);
    e.undo();
    EXPECT_EQ(e.pairCount(), 1u);
    while (e.canUndo()) e.undo();
    EXPECT_FALSE(e.undo());
}

TEST(TransferMarkerEdit, SymmetryAddsTheMirrorAndDeletesBoth) {
    TransferMarkerEditor e;
    e.add(L, at(0.5f));
    EXPECT_EQ(e.addMirrored(L, at(1, 2), at(-1, 2), 1e-4f), 1u);
    EXPECT_EQ(xs(e, L), (std::vector<float>{0.5f, 1, -1}));
    EXPECT_NE(e.point(L, 1).group, 0u);
    EXPECT_EQ(e.point(L, 1).group, e.point(L, 2).group);
    // A point on the mirror plane adds alone.
    e.addMirrored(L, at(0, 3), at(0, 3), 1e-4f);
    EXPECT_EQ(e.count(L), 4u);
    EXPECT_EQ(e.point(L, 3).group, 0u);
    // The right side mirrored too, then both mirror points' pairs go with one delete.
    e.add(R, at(10));
    e.addMirrored(R, at(11), at(-11), 1e-4f);
    e.add(R, at(12));
    EXPECT_EQ(e.removalSet(L, 2), (std::vector<std::size_t>{1, 2}));
    EXPECT_EQ(e.remove(L, 2), (std::vector<std::size_t>{1, 2}));
    EXPECT_EQ(xs(e, L), (std::vector<float>{0.5f, 0}));
    EXPECT_EQ(xs(e, R), (std::vector<float>{10, 12}));
    // One step: undo brings all four back.
    e.undo();
    EXPECT_EQ(e.count(L), 4u);
    EXPECT_EQ(e.count(R), 4u);
    // A moved mirror point keeps its group.
    e.move(L, 2, at(-1.5f, 2));
    EXPECT_EQ(e.point(L, 2).group, e.point(L, 1).group);
    EXPECT_EQ(mirrorX({1, 2, 3}), Vec3f(-1, 2, 3));
    EXPECT_EQ(mirrorX({1, 2, 3}, 5.0f), Vec3f(9, 2, 3));
}

TEST(TransferMarkerEdit, RequirementsByMethod) {
    EXPECT_EQ(markerRequirement(TransferMethod::Closest, 0), "");
    EXPECT_EQ(markerRequirement(TransferMethod::UV, 0), "");
    EXPECT_EQ(markerRequirement(TransferMethod::MarkersAlign, 2), "Markers: align needs 3 or more pairs (2 placed)");
    EXPECT_EQ(markerRequirement(TransferMethod::MarkersAlign, 3), "");
    EXPECT_EQ(markerRequirement(TransferMethod::MarkersWarp, 3), "Markers: warp needs 4 or more pairs (3 placed)");
    EXPECT_EQ(markerRequirement(TransferMethod::MarkersWarp, 4), "");
}

TEST(TransferMarkerEdit, PickAndSnapOnAMesh) {
    const auto sphere = test::makeSphere(16, 32, 1.0f);
    MarkerPoint hit;
    ASSERT_TRUE(pickMarkerPoint(*sphere, {0.3f, 0.2f, 5.0f}, {0, 0, -1}, &hit));
    EXPECT_GE(hit.root.face, 0);
    EXPECT_NEAR(hit.point.z, 0.93f, 0.02f);  // the near side, sqrt(1 - 0.13)
    EXPECT_LT((sphere->position(hit.root) - hit.point).length(), 1e-5f);
    EXPECT_FALSE(pickMarkerPoint(*sphere, {3, 0, 5}, {0, 0, -1}, &hit));
    MarkerPoint snapped;
    ASSERT_TRUE(snapMarkerPoint(*sphere, mirrorX(hit.point) * 1.2f, &snapped));
    EXPECT_NEAR(snapped.point.length(), 1.0f, 0.02f);
    EXPECT_LT(snapped.point.x, 0.0f);
}

}  // namespace
}  // namespace groomlab
