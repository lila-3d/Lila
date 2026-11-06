// SPDX-License-Identifier: Apache-2.0
// Modified for Lila (2026).
// Moving a groom to another scalp (GH-8): guides, children, layers and parts.

#include <cmath>

#include <gtest/gtest.h>

#include "groomlab/flow.h"
#include "groomlab/groom.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/parts.h"
#include "test_helpers.h"
#include "groom/GroomEvaluator.h"

using lila::groom::GroomEvaluator;

namespace groomlab {
namespace {

float curveLength(const std::vector<Vec3f>& pts) {
    float len = 0.0f;
    for (std::size_t i = 1; i < pts.size(); ++i) len += (pts[i] - pts[i - 1]).length();
    return len;
}

// Primaries standing out of the upper half of a unit sphere, a "fur" layer, children.
GuideHierarchy furryHead(const ScalpMesh& head) {
    GuideHierarchy h;
    const GroupId g = h.addGroup("primary");
    for (int i = 0; i < 8; ++i) {
        const float a = 0.4f + 0.12f * i;
        const Vec3f n{std::sin(a) * std::cos(1.7f * i), std::cos(a), std::sin(a) * std::sin(1.7f * i)};
        const GuideId id = h.addGuide(g, test::straightCurve(n, n * 0.6f, 6));
        h.setAttribute(id, "fur", i * 0.125f);
    }
    h.evaluate(&head);
    ChildGenerationParams params;
    params.count = 3;
    h.generateChildren(g, "children", params);
    h.evaluate(&head);
    return h;
}

TEST(Transfer, GuidesReRootOnANewScalpKeepingShapeAndLayers) {
    const auto head = test::makeSphere(24, 48, 1.0f);
    const auto sculpt = test::makeSphere(17, 31, 1.1f);  // another tessellation, a bit bigger
    GuideHierarchy h = furryHead(*head);
    const GuideHierarchy before = h;

    h.transfer(*head, *sculpt);
    ASSERT_EQ(h.guides().size(), before.guides().size());
    for (std::size_t i = 0; i < h.guides().size(); ++i) {
        const HierarchyGuide& a = before.guides()[i];
        const HierarchyGuide& b = h.guides()[i];
        EXPECT_EQ(a.id, b.id);
        EXPECT_EQ(a.attributes, b.attributes);
        EXPECT_NEAR(b.points.front().length(), 1.1f, 0.01f) << "root on the new scalp";
        EXPECT_NEAR(curveLength(b.points), curveLength(a.points), 0.02f);
        if (!b.isChild()) {
            // Still standing straight out: the shape turned with the normal.
            const Vec3f dir = (b.points.back() - b.points.front()).normalized();
            EXPECT_GT(dir.dot(b.points.front().normalized()), 0.99f);
            EXPECT_LT((b.points.front().normalized() - a.points.front().normalized()).length(), 0.02f);
        }
    }

    // Evaluates on the new scalp with the layer intact.
    GroomData d;
    d.scalp = sculpt;
    d.hierarchyJson = h.toJson();
    d.graphJson = R"j({"version": 1, "output": "out", "nodes": [
        {"name": "scalp", "type": "Scalp"}, {"name": "guides", "type": "Guides"},
        {"name": "scatter", "type": "Scatter", "params": {"density": {"guide": "fur", "max": 300.0}}},
        {"name": "interp", "type": "Interpolate"}, {"name": "out", "type": "Output"}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "guides", "to": "scatter", "input": "guides"},
        {"from": "scatter", "to": "interp", "input": "strands"}, {"from": "guides", "to": "interp", "input": "guides"},
        {"from": "scalp", "to": "interp", "input": "scalp"}, {"from": "interp", "to": "out", "input": "strands"}]})j";
    EXPECT_GT(GroomEvaluator(d).evaluate(1.0f, 0.0)->strandCount(), 50u);
}

TEST(Transfer, FitMovesAndScalesOntoAScalpElsewhere) {
    const auto head = test::makeSphere(24, 48, 1.0f);
    std::vector<Vec3f> moved;
    const auto big = test::makeSphere(24, 48, 2.0f);
    for (const Vec3f& p : big->points()) moved.push_back(p + Vec3f{5, 0, 0});
    const auto other = std::make_shared<const ScalpMesh>(head->withPoints(moved));
    GuideHierarchy h = furryHead(*head);
    const GuideHierarchy before = h;

    const ScalpFit fit = ScalpFit::boundingBoxes(*head, *other);
    EXPECT_NEAR(fit.scale, 2.0f, 1e-4f);
    h.transfer(*head, *other, fit);
    for (std::size_t i = 0; i < h.guides().size(); ++i) {
        const HierarchyGuide& a = before.guides()[i];
        const HierarchyGuide& b = h.guides()[i];
        EXPECT_LT((b.points.front() - (Vec3f{5, 0, 0} + a.points.front() * 2.0f)).length(), 0.05f);
        EXPECT_NEAR(curveLength(b.points), 2.0f * curveLength(a.points), 0.05f);
    }
}

TEST(Transfer, PartsFollow) {
    const auto head = test::makeSphere(24, 48, 1.0f);
    const auto sculpt = test::makeSphere(17, 31, 1.1f);
    PartSet parts;
    std::vector<Vec3f> line;
    for (int i = 0; i <= 10; ++i) line.push_back({0.0f, std::cos(0.1f * i - 0.5f), std::sin(0.1f * i - 0.5f)});
    parts.parts.push_back(PartSet::fromPoints("crown", line, *head));

    const PartSet moved = parts.transferred(*head, *sculpt);
    ASSERT_EQ(moved.parts.size(), 1u);
    EXPECT_EQ(moved.parts[0].name, "crown");
    ASSERT_EQ(moved.parts[0].points.size(), parts.parts[0].points.size());
    for (std::size_t i = 0; i < moved.parts[0].points.size(); ++i) {
        const Vec3f a = head->position(parts.parts[0].points[i]);
        const Vec3f b = sculpt->position(moved.parts[0].points[i]);
        EXPECT_NEAR(b.length(), 1.1f, 0.01f);
        EXPECT_LT((b.normalized() - a.normalized()).length(), 0.02f);
    }
}

TEST(Transfer, FlowFollowsKeepingDirections) {
    const auto head = test::makeSphere(24, 48, 1.0f);
    const auto sculpt = test::makeSphere(17, 31, 1.1f);
    FlowSet flow;
    const Vec3f at = Vec3f(0.3f, 0.9f, 0.2f).normalized();
    FlowPin pin = FlowSet::makePin(*head, head->closestPoint(at), Vec3f(0.0f, 0.0f, 1.0f));
    pin.line = 2;
    pin.seq = 5;
    flow.pins.push_back(pin);
    FlowSink sink;
    sink.root = head->closestPoint(Vec3f(0.0f, 1.0f, 0.0f)).root;
    sink.radius = 0.4f;
    flow.sinks.push_back(sink);

    const FlowSet moved = flow.transferred(*head, *sculpt, ScalpFit{{0, 0, 0}, {0, 0, 0}, 1.1f});
    ASSERT_EQ(moved.pins.size(), 1u);
    ASSERT_EQ(moved.sinks.size(), 1u);
    EXPECT_EQ(moved.pins[0].line, 2);
    EXPECT_EQ(moved.pins[0].seq, 5);
    EXPECT_LT((sculpt->position(moved.pins[0].root).normalized() - at).length(), 0.02f);
    EXPECT_NEAR(moved.sinks[0].radius, 0.44f, 1e-5f);
    // The same world direction on both scalps (the tangent part of +z there).
    const FlowField before(flow, *head), after(moved, *sculpt);
    EXPECT_GT(before.directions()[0].dot(after.directions()[0]), 0.99f);
}

}  // namespace
}  // namespace groomlab
