// SPDX-License-Identifier: Apache-2.0
// Wet (wet_op.cpp): wet-hair clumping as an elastocapillary cascade. Small clumps form near the
// root and merge into bigger ones toward the tip, each nested in one of the level above; wetness 0
// is the strands as they came.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#if GROOMLAB_HAS_TBB
#include <tbb/task_arena.h>
#endif

#include "groomlab/graph.h"
#include "groomlab/operator_state.h"
#include "groomlab/operators.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

// Long straight hair on a plane `scale` x 4 units wide (about 3000 strands, 15 segments, 6 x scale
// long), through a Wet node with clumpSize `scale` (and `params` set on it).
struct WetBed {
    Graph g;
    ExternalInputs ext;
    NodeId scatter = 0, wet = 0;

    explicit WetBed(float scale = 1.0f) {
        ext.set("scalp", test::makePlane(8, 4.0f * scale), 1);
        const NodeId scalp = g.addNode(op::kScalp);
        scatter = g.addNode(op::kScatter);
        g.node(scatter).params().set("density", 190.0 / (static_cast<double>(scale) * scale));
        g.node(scatter).params().set("length", 6.0 * scale);
        const NodeId segments = g.addNode(op::kResample);  // unconnected: it only holds the segments
        g.node(segments).params().set("segments", std::int64_t{15});
        g.connect(scalp, scatter, 0);
        wet = g.addNode(op::kWet);
        g.connect(scatter, wet, 0);
        g.node(wet).params().set("clumpSize", 1.0 * scale);
    }
    StrandsPtr eval(NodeId id) {
        EvalContext ctx;
        ctx.externals = &ext;
        return g.evaluateStrands(id, ctx);
    }
    StrandsPtr input() { return eval(scatter); }
    StrandsPtr output() { return eval(wet); }
};

// The mean distance at point i of the strands from their clump's mean there, the clumps by
// `channel` (strands in none, -1, left out).
float clumpSpread(const StrandSet& s, const std::vector<float>& ids, std::size_t i) {
    std::map<float, std::pair<Vec3f, int>> sums;
    for (std::size_t k = 0; k < s.strandCount(); ++k) {
        if (ids[k] < 0.0f) continue;
        auto& [sum, n] = sums[ids[k]];
        sum += s.strandPoints(k)[i];
        ++n;
    }
    double total = 0.0;
    std::size_t count = 0;
    for (std::size_t k = 0; k < s.strandCount(); ++k) {
        if (ids[k] < 0.0f) continue;
        const auto& [sum, n] = sums[ids[k]];
        if (n < 2) continue;
        total += (s.strandPoints(k)[i] - sum / static_cast<float>(n)).length();
        ++count;
    }
    return count ? static_cast<float>(total / static_cast<double>(count)) : 0.0f;
}

TEST(Wet, DryIsTheStrandsAsTheyCame) {
    WetBed bed;
    bed.g.node(bed.wet).params().set("wetness", 0.0);
    const StrandsPtr in = bed.input();
    const StrandsPtr out = bed.output();
    ASSERT_TRUE(in && out);
    ASSERT_EQ(out->pointCount(), in->pointCount());
    EXPECT_EQ(out->ids(), in->ids());
    EXPECT_EQ(std::memcmp(out->positions().data(), in->positions().data(), in->pointCount() * sizeof(Vec3f)), 0);
    EXPECT_EQ(out->strandChannelNames(), in->strandChannelNames());
    EXPECT_EQ(out->pointChannelNames(), in->pointChannelNames());
}

TEST(Wet, ClumpsGrowFromRootToTip) {
    // Each level's clumps pull in (their strands' spread about the clump's mean halves) farther
    // along the strand than the level below: small clumps near the root, merged into bigger ones
    // toward the tip. The roots never move.
    WetBed bed;
    bed.g.node(bed.wet).params().set("wetness", 0.8);
    const StrandsPtr in = bed.input();
    const StrandsPtr out = bed.output();
    ASSERT_TRUE(in && out);
    ASSERT_GT(in->strandCount(), 2000u);
    const std::size_t n = out->uniformPointCount();
    ASSERT_EQ(n, 16u);
    const char* channels[] = {"clumpId2", "clumpId1", "clumpId"};  // the smallest first
    float previous = 0.0f;
    for (const char* name : channels) {
        const std::vector<float>* ids = out->strandChannel(name);
        ASSERT_TRUE(ids) << name;
        float engaged = 2.0f;  // the share of the strand where the spread first falls below half
        for (std::size_t i = 1; i < n; ++i) {
            const float before = clumpSpread(*in, *ids, i), after = clumpSpread(*out, *ids, i);
            ASSERT_GT(before, 0.0f);
            if (after < 0.5f * before) {
                engaged = static_cast<float>(i) / static_cast<float>(n - 1);
                break;
            }
        }
        EXPECT_LT(engaged, 1.01f) << name << " never clumps";
        EXPECT_GT(engaged, previous) << name << " clumps no farther out than the level below";
        previous = engaged;
        EXPECT_NEAR(clumpSpread(*out, *ids, 0), clumpSpread(*in, *ids, 0), 1e-6f) << name;
    }
    // At the tip the biggest clumps are tight.
    const std::vector<float>& big = *out->strandChannel("clumpId");
    EXPECT_LT(clumpSpread(*out, big, n - 1), 0.2f * clumpSpread(*in, big, n - 1));
    // Every point says how wet it is.
    const std::vector<float>* wetness = out->pointChannel("wetness");
    ASSERT_TRUE(wetness);
    EXPECT_FLOAT_EQ((*wetness)[0], 0.8f);
}

TEST(Wet, SmallClumpsNestInBigOnes) {
    // The strands of one clump are all in one clump of the level above, at every level.
    WetBed bed;
    bed.g.node(bed.wet).params().set("levels", std::int64_t{4});
    const StrandsPtr out = bed.output();
    ASSERT_TRUE(out);
    const char* channels[] = {"clumpId3", "clumpId2", "clumpId1", "clumpId"};
    for (int level = 0; level + 1 < 4; ++level) {
        const std::vector<float>* small = out->strandChannel(channels[level]);
        const std::vector<float>* big = out->strandChannel(channels[level + 1]);
        ASSERT_TRUE(small && big);
        std::map<float, float> parent;
        std::size_t clumped = 0;
        for (std::size_t s = 0; s < out->strandCount(); ++s) {
            if ((*small)[s] < 0.0f) continue;
            ++clumped;
            const auto [it, fresh] = parent.emplace((*small)[s], (*big)[s]);
            EXPECT_EQ(it->second, (*big)[s]) << channels[level] << " clump " << (*small)[s] << " spans two of "
                                             << channels[level + 1];
        }
        EXPECT_GT(clumped, out->strandCount() * 9 / 10) << channels[level];
        std::map<float, int> children;
        for (const auto& [child, p] : parent) ++children[p];
        EXPECT_GT(parent.size(), children.size()) << "no nesting: as many " << channels[level] << " as "
                                                  << channels[level + 1];
    }
}

TEST(Wet, ScaleInvariant) {
    // The same groom 100 times as big (plane, length, density and clumpSize fitted) makes the same
    // strands, 100 times as far apart.
    WetBed small(1.0f), big(100.0f);
    const StrandsPtr a = small.output();
    const StrandsPtr b = big.output();
    ASSERT_TRUE(a && b);
    ASSERT_EQ(a->ids(), b->ids());
    ASSERT_EQ(a->pointCount(), b->pointCount());
    float worst = 0.0f;
    for (std::size_t i = 0; i < a->pointCount(); ++i) worst = std::max(worst, (b->positions()[i] / 100.0f - a->positions()[i]).length());
    EXPECT_LT(worst, 1e-4f);
    EXPECT_EQ(*a->strandChannel("clumpId"), *b->strandChannel("clumpId"));
    EXPECT_EQ(*a->strandChannel("clumpId2"), *b->strandChannel("clumpId2"));
}

TEST(Wet, DeterministicOnAnyNumberOfThreads) {
    WetBed bed;
    const StrandsPtr many = bed.output();
    ASSERT_TRUE(many);
    StrandsPtr one;
#if GROOMLAB_HAS_TBB
    tbb::task_arena single(1);
    single.execute([&] {
        WetBed fresh;
        one = fresh.output();
    });
#else
    WetBed fresh;
    one = fresh.output();
#endif
    ASSERT_TRUE(one);
    ASSERT_EQ(one->pointCount(), many->pointCount());
    EXPECT_EQ(std::memcmp(one->positions().data(), many->positions().data(), many->pointCount() * sizeof(Vec3f)), 0);
    EXPECT_EQ(*one->strandChannel("clumpId"), *many->strandChannel("clumpId"));
    // A wetness change and back (the levels keep their picks) makes the same again.
    bed.g.node(bed.wet).params().set("wetness", 0.3);
    ASSERT_TRUE(bed.output());
    bed.g.node(bed.wet).params().set("wetness", 0.7);
    const StrandsPtr again = bed.output();
    EXPECT_EQ(std::memcmp(again->positions().data(), many->positions().data(), many->pointCount() * sizeof(Vec3f)), 0);
}

TEST(Wet, NoClumpChannelLeavesNoChannels) {
    WetBed bed;
    bed.g.node(bed.wet).params().set("clumpChannel", std::string());
    const StrandsPtr in = bed.input();
    const StrandsPtr out = bed.output();
    ASSERT_TRUE(in && out);
    EXPECT_EQ(out->strandChannelNames(), in->strandChannelNames());
    EXPECT_GT((out->positions().back() - in->positions().back()).length(), 0.0f);  // still clumped
}

TEST(Wet, KeepsItsLevelsForItsResult) {
    // What a host running the levels as Clumps reads (operator_state.h WetState): per level, the
    // largest first, its Clump's params (its profile wetLevelProfile's at the wetness) and center
    // choice, for the Wet's result alone; nothing at a wetness of 0.
    WetBed bed;
    bed.g.node(bed.wet).params().set("levels", std::int64_t{3});
    bed.g.node(bed.wet).params().set("wetness", 0.6);
    const StrandsPtr out = bed.output();
    ASSERT_TRUE(out);
    EXPECT_EQ(wetLevelCount(bed.g.node(bed.wet).params()), 3);
    std::shared_ptr<const WetState> state;
    ASSERT_TRUE(wetState(bed.g.node(bed.wet), out, &state));
    ASSERT_EQ(state->levels.size(), 3u);
    ASSERT_EQ(state->params.size(), 3u);
    EXPECT_FLOAT_EQ(state->wetness, 0.6f);
    std::size_t centers = 0;
    for (std::size_t j = 0; j < 3; ++j) {
        SCOPED_TRACE(j);
        EXPECT_TRUE(state->params[j].getRamp("profile") == wetLevelProfile(static_cast<std::int64_t>(j), 3, 0.6f));
        EXPECT_EQ(state->params[j].getString("centersFrom"), "pick");
        const ClumpState& level = state->levels[j];
        ASSERT_TRUE(level.assigned);
        EXPECT_EQ(level.assigned->size(), out->strandCount());
        EXPECT_FALSE(level.chosen.empty());   // picked by roots alone
        EXPECT_GT(level.centerGuides.size(), centers);  // smaller levels, more clumps
        centers = level.centerGuides.size();
    }
    EXPECT_FALSE(wetLevelProfile(0, 3, 0.6f) == wetLevelProfile(0, 3, 0.9f));
    EXPECT_FALSE(wetState(bed.g.node(bed.wet), bed.input(), &state));  // another result
    bed.g.node(bed.wet).params().set("wetness", 0.0);
    const StrandsPtr dry = bed.output();
    EXPECT_FALSE(wetState(bed.g.node(bed.wet), dry, &state));
}

}  // namespace
}  // namespace groomlab
