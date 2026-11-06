// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include "groomlab/random.h"

namespace groomlab {
namespace {

TEST(Random, Hash64IsDeterministicAndMixes) {
    EXPECT_EQ(hash64(42), hash64(42));
    EXPECT_NE(hash64(42), hash64(43));
}

TEST(Random, UnitFloatInRange) {
    for (std::uint64_t i = 0; i < 10000; ++i) {
        const float f = unitFloat(hash64(i));
        EXPECT_GE(f, 0.0f);
        EXPECT_LT(f, 1.0f);
    }
}

TEST(Random, RngSameSeedSameSequence) {
    Rng a(7), b(7), c(8);
    for (int i = 0; i < 100; ++i) {
        const auto va = a.nextU64();
        EXPECT_EQ(va, b.nextU64());
        EXPECT_NE(va, c.nextU64());
    }
}

TEST(Random, DensityIsMonotonic) {
    // A strand kept at a low density must also be kept at every higher density,
    // so lowering preview density never reshuffles surviving strands.
    for (std::uint64_t id = 0; id < 5000; ++id) {
        if (keepAtDensity(id, 0.1f)) {
            EXPECT_TRUE(keepAtDensity(id, 0.5f));
            EXPECT_TRUE(keepAtDensity(id, 1.0f));
        }
    }
}

TEST(Random, DensityKeepsExpectedFraction) {
    int kept = 0;
    const int n = 100000;
    for (int id = 0; id < n; ++id) kept += keepAtDensity(static_cast<std::uint64_t>(id), 0.25f) ? 1 : 0;
    EXPECT_NEAR(static_cast<double>(kept) / n, 0.25, 0.01);
}

TEST(Random, FullDensityKeepsEverything) {
    for (std::uint64_t id = 0; id < 1000; ++id) EXPECT_TRUE(keepAtDensity(id, 1.0f));
}

}  // namespace
}  // namespace groomlab
