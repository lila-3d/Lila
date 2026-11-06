// SPDX-License-Identifier: Apache-2.0
// Modified for Lila (2026).
// Guide attribute layers as maps: density before strands exist, and after Interpolate.

#include <gtest/gtest.h>

#include "groomlab/graph_io.h"
#include "groomlab/groom.h"
#include "groomlab/guide_hierarchy.h"
#include "test_helpers.h"

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
