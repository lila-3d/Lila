// SPDX-License-Identifier: Apache-2.0
// Modified for Lila (2026).
// Guide attribute layers as maps: density before strands exist, and after Interpolate.

#include <gtest/gtest.h>

#include "groomlab/graph_io.h"
#include "groomlab/groom.h"
#include "groomlab/guide_hierarchy.h"
#include "test_helpers.h"
#include "groom/GroomEvaluator.h"

using lila::groom::GroomEvaluator;

namespace groomlab {
namespace {

// Primaries over the unit plane; "density" is 1 on the left half (x < 0.5), 0 on the right.
std::string layeredHierarchy(const ScalpMesh& plane) {
    GuideHierarchy h;
    const GroupId g = h.addGroup("primary");
    for (float x = 0.1f; x < 1.0f; x += 0.2f) {
        for (float z = 0.1f; z < 1.0f; z += 0.2f) {
            const GuideId id = h.addGuide(g, test::straightCurve({x, 0, z}, {0, 0.5f, 0}, 4));
            h.setAttribute(id, "density", x < 0.5f ? 1.0f : 0.0f);
            h.setAttribute(id, "len", x < 0.5f ? 0.2f : 1.0f);
        }
    }
    h.evaluate(&plane);
    EXPECT_EQ(h.attributeNames(), (std::vector<std::string>{"density", "len"}));
    return h.toJson();
}

std::string graph(bool connectGuides, const char* source = "guide") {
    std::string g = std::string(R"j({"version": 1, "output": "out", "nodes": [
        {"name": "scalp", "type": "Scalp"}, {"name": "guides", "type": "Guides"},
        {"name": "scatter", "type": "Scatter", "params": {"density": {")j") + source + R"j(": "density", "max": 2000.0}}},
        {"name": "interp", "type": "Interpolate"},
        {"name": "length", "type": "Length", "params": {"keep": {"guide": "len"}}},
        {"name": "out", "type": "Output"}],
      "connections": [
        {"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "scatter", "to": "interp", "input": "strands"}, {"from": "guides", "to": "interp", "input": "guides"},
        {"from": "scalp", "to": "interp", "input": "scalp"}, {"from": "interp", "to": "length", "input": "strands"},
        {"from": "length", "to": "out", "input": "strands"})j";
    if (connectGuides) g += R"j(, {"from": "guides", "to": "scatter", "input": "guides"})j";
    return g + "]}";
}

TEST(Layers, DensityAndLengthFromGuideLayers) {
    const auto plane = test::makePlane(16);
    GroomData d;
    d.scalp = plane;
    d.hierarchyJson = layeredHierarchy(*plane);
    d.graphJson = graph(true);
    const StrandsPtr s = GroomEvaluator(d).evaluate(1.0f, 0.0);
    ASSERT_GT(s->strandCount(), 300u);
    std::size_t right = 0;
    float leftLength = 0.0f;
    std::size_t nLeft = 0;
    for (std::size_t i = 0; i < s->strandCount(); ++i) {
        const Vec3f* p = s->strandPoints(i);
        const float len = (p[s->strandPointCount(i) - 1] - p[0]).length();
        if (p[0].x > 0.62f) ++right;
        if (p[0].x < 0.3f) {
            leftLength += len;
            ++nLeft;
        }
    }
    EXPECT_LT(right, s->strandCount() / 50);  // painted out: (almost) no strands on the right
    ASSERT_GT(nLeft, 0u);
    EXPECT_NEAR(leftLength / nLeft, 0.5f * 0.2f, 0.02f);  // length layer read from the strand channel

    // A layer no guide carries: a clear error, not silently 0.
    {
        GroomData missing = d;
        GuideHierarchy h = GuideHierarchy::fromJson(d.hierarchyJson);
        h.removeAttribute("density");
        missing.hierarchyJson = h.toJson();
        EXPECT_THROW(GroomEvaluator(missing).evaluate(1.0f, 0.0), GraphError);
    }

    // Scatter without its guides input: a clear error on the node.
    d.graphJson = graph(false);
    try {
        GroomEvaluator(d).evaluate(1.0f, 0.0);
        FAIL();
    } catch (const GraphError& e) {
        EXPECT_NE(std::string(e.what()).find("connect them"), std::string::npos);
    }
}

TEST(Layers, AnAttributeMapBeforeStrandsReadsTheGuideLayer) {
    // Painted values live on the guides; an attribute map on Scatter (no
    // strands yet) reads the guide layer of that name, like a guide map.
    const auto plane = test::makePlane(16);
    GroomData d;
    d.scalp = plane;
    d.hierarchyJson = layeredHierarchy(*plane);
    d.graphJson = graph(true, "guide");
    const std::size_t viaGuide = GroomEvaluator(d).evaluate(1.0f, 0.0)->strandCount();
    d.graphJson = graph(true, "attribute");
    EXPECT_EQ(GroomEvaluator(d).evaluate(1.0f, 0.0)->strandCount(), viaGuide);

    d.graphJson = graph(false, "attribute");
    try {
        GroomEvaluator(d).evaluate(1.0f, 0.0);
        FAIL();
    } catch (const GraphError& e) {
        EXPECT_NE(std::string(e.what()).find("painted on the guides"), std::string::npos) << e.what();
    }
}

TEST(Layers, RemoveALayer) {
    const auto plane = test::makePlane(4);
    GuideHierarchy h = GuideHierarchy::fromJson(layeredHierarchy(*plane));
    h.removeAttribute("len");
    EXPECT_EQ(h.attributeNames(), (std::vector<std::string>{"density"}));
    h.renameAttribute("density", "fur");
    EXPECT_EQ(h.attributeNames(), (std::vector<std::string>{"fur"}));
    EXPECT_FLOAT_EQ(h.guides().front().attributes.at("fur"), 1.0f);
    h.setAttribute(h.guides().front().id, "other", 0.5f);
    EXPECT_THROW(h.renameAttribute("fur", "other"), std::invalid_argument);
}

}  // namespace
}  // namespace groomlab
