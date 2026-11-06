// SPDX-License-Identifier: Apache-2.0
// Modified for Lila (2026).
// Render overrides (render_overrides.h): widths scaled, each strand scaled about its root, nothing
// changed by the defaults, and the density a share of the groom's own strands.

#include <gtest/gtest.h>

#include <cmath>
#include <set>

#include "groomlab/groom.h"
#include "groom/GroomEvaluator.h"
#include "groomlab/render_overrides.h"
#include "test_helpers.h"

using lila::groom::GroomEvaluator;

namespace groomlab {
namespace {

GroomData sphereGroom() {
    GroomData data;
    data.scalp = test::makeSphere(12, 24);
    data.graphJson = R"({"version": 1, "nodes": [
    {"name": "scalp", "type": "Scalp"},
    {"name": "scatter", "type": "Scatter", "params": {"density": 80.0, "length": 0.5}},
    {"name": "noise", "type": "Noise", "params": {"amount": 0.1}},
    {"name": "width", "type": "Width", "params": {"width": 0.01}},
    {"name": "output", "type": "Output"}],
    "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
                    {"from": "scatter", "to": "noise", "input": "strands"},
                    {"from": "noise", "to": "width", "input": "strands"},
                    {"from": "width", "to": "output", "input": "strands"}], "output": "output"})";
    return data;
}

TEST(RenderOverrides, TheDefaultsChangeNothing) {
    const StrandsPtr strands = GroomEvaluator(sphereGroom()).evaluate(1.0f, 0.0);
    EXPECT_EQ(applyRenderOverrides(strands, RenderOverrides{}), strands);
    EXPECT_EQ(RenderOverrides{}.densityTimes(0.5f), 0.5f);
    RenderOverrides odd;
    odd.density = 3.0f;
    EXPECT_EQ(odd.densityTimes(1.0f), 1.0f);  // a render never adds strands
    odd.density = std::nanf("");
    EXPECT_EQ(odd.densityTimes(1.0f), 1.0f);
}

TEST(RenderOverrides, WidthAndLengthScale) {
    const StrandsPtr strands = GroomEvaluator(sphereGroom()).evaluate(1.0f, 0.0);
    RenderOverrides o;
    o.widthScale = 2.0f;
    o.lengthScale = 0.5f;
    const StrandsPtr out = applyRenderOverrides(strands, o);
    ASSERT_NE(out, strands);
    ASSERT_EQ(out->strandCount(), strands->strandCount());
    EXPECT_EQ(out->ids(), strands->ids());
    for (std::size_t i = 0; i < out->widths().size(); ++i) EXPECT_FLOAT_EQ(out->widths()[i], 2.0f * strands->widths()[i]);
    for (std::size_t s = 0; s < out->strandCount(); ++s) {
        const Vec3f* a = strands->strandPoints(s);
        const Vec3f* b = out->strandPoints(s);
        EXPECT_EQ(b[0], a[0]);  // the root stays
        for (std::size_t k = 1; k < out->strandPointCount(s); ++k) {
            EXPECT_LT((b[k] - (a[0] + (a[k] - a[0]) * 0.5f)).length(), 1e-6f);  // shape and all, at half size
        }
    }
    // The input is untouched.
    EXPECT_EQ(GroomEvaluator(sphereGroom()).evaluate(1.0f, 0.0)->contentHash(), strands->contentHash());
}

TEST(RenderOverrides, DensityKeepsTheGroomsOwnStrands) {
    GroomEvaluator groom(sphereGroom());
    const StrandsPtr full = groom.evaluate(1.0f, 0.0);
    RenderOverrides o;
    o.density = 0.3f;
    const StrandsPtr thinned = groom.evaluate(o.densityTimes(1.0f), 0.0);
    EXPECT_LT(thinned->strandCount(), full->strandCount() / 2);
    EXPECT_GT(thinned->strandCount(), full->strandCount() / 6);
    std::set<StrandId> ids(full->ids().begin(), full->ids().end());
    for (const StrandId id : thinned->ids()) EXPECT_TRUE(ids.count(id));
}

}  // namespace
}  // namespace groomlab
