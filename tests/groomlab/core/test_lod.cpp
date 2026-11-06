// SPDX-License-Identifier: Apache-2.0
// Render LOD: distance thinning with width compensation, and camera culling.

#include <gtest/gtest.h>

#include <cmath>

#include "groomlab/lod.h"

namespace groomlab {
namespace {

// A field of short vertical strands on the ground in front of the camera
// (the camera at the origin, looking down -z): x in [-50, 50], depth 1-200.
StrandSet field() {
    StrandSet s;
    StrandId id = 0;
    for (int zi = 1; zi <= 200; ++zi) {
        for (int xi = -50; xi <= 50; xi += 2) {
            const Vec3f root(float(xi), -1.0f, -float(zi));
            s.addStrand(id++, {root, root + Vec3f(0, 0.5f, 0), root + Vec3f(0, 1.0f, 0)}, {0.01f, 0.01f, 0.005f});
        }
    }
    return s;
}

TEST(Lod, CullsOutsideTheView) {
    const StrandSet s = field();
    LodCamera cam;  // identity: at the origin, looking down -z
    cam.tanHalfFovX = cam.tanHalfFovY = 1.0f;  // 90 degrees
    LodSettings settings;
    settings.frustum = true;
    settings.margin = 0.0f;
    const LodPlan plan = planLod(s, cam, settings);
    EXPECT_GT(plan.culled, 0u);
    EXPECT_EQ(plan.thinned, 0u);
    EXPECT_EQ(plan.kept.size() + plan.culled, s.strandCount());
    for (const std::size_t i : plan.kept) {
        const Vec3f& root = s.strandPoints(i)[0];
        EXPECT_LE(std::abs(root.x), -root.z + 1e-4f) << i;  // inside the 90-degree wedge
    }
    // A wider margin keeps more.
    settings.margin = 0.5f;
    EXPECT_GT(planLod(s, cam, settings).kept.size(), plan.kept.size());
    // Behind the camera: nothing.
    cam.matrix[2][2] = -1.0f;  // looking down +z
    cam.matrix[0][0] = -1.0f;
    settings.margin = 0.0f;
    EXPECT_EQ(planLod(s, cam, settings).kept.size(), 0u);
}

TEST(Lod, ThinsWithDistanceAndKeepsCoverage) {
    const StrandSet s = field();
    LodCamera cam;
    LodSettings settings;
    settings.distance = true;
    settings.nearDistance = 20.0f;
    settings.farDistance = 100.0f;
    settings.minDensity = 0.2f;
    const LodPlan plan = planLod(s, cam, settings);
    const StrandSet out = applyLod(s, plan);
    ASSERT_EQ(out.strandCount(), plan.kept.size());
    std::size_t nearTotal = 0, nearKept = 0, farTotal = 0, farKept = 0;
    double farCoverage = 0.0, farCoverageBefore = 0.0;
    for (std::size_t i = 0; i < s.strandCount(); ++i) {
        const float d = s.strandPoints(i)[0].length();
        if (d < 19.0f) ++nearTotal;
        if (d > 110.0f) {
            ++farTotal;
            farCoverageBefore += s.widths()[s.strandBegin(i)];
        }
    }
    for (std::size_t k = 0; k < out.strandCount(); ++k) {
        const float d = out.strandPoints(k)[0].length();
        if (d < 19.0f) {
            ++nearKept;
            EXPECT_FLOAT_EQ(out.widths()[out.strandBegin(k)], 0.01f);  // untouched up close
        }
        if (d > 110.0f) {
            ++farKept;
            farCoverage += out.widths()[out.strandBegin(k)];
        }
    }
    EXPECT_EQ(nearKept, nearTotal);
    EXPECT_NEAR(double(farKept) / double(farTotal), 0.2, 0.03);
    EXPECT_NEAR(farCoverage / farCoverageBefore, 1.0, 0.15);  // wider survivors cover as much

    // The plan is by stable ID: another motion key (moved points) keeps the same strands.
    StrandSet moved = s;
    for (Vec3f& p : moved.mutablePositions()) p += Vec3f(0.3f, 0.0f, 0.0f);
    const StrandSet key2 = applyLod(moved, plan);
    ASSERT_EQ(key2.strandCount(), out.strandCount());
    for (std::size_t k = 0; k < out.strandCount(); ++k) EXPECT_EQ(key2.ids()[k], out.ids()[k]);
}

}  // namespace
}  // namespace groomlab
