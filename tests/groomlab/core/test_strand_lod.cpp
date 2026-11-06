// SPDX-License-Identifier: Apache-2.0
// Screen-space level of detail for strand lines (strand_lod.h, docs/research/clump_lod.md): the
// view's level, the groups, the selection's rules, nested subsets, determinism, hysteresis and the
// impostors, on synthetic clumps whose answers are known.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <set>

#include "groomlab/display.h"
#include "groomlab/random.h"
#include "groomlab/strand_lod.h"

namespace groomlab {
namespace {

// `clumps` clumps of `members` strands of `points` points, each clump a tight bundle (members
// within `spread` of its axis) of length 10 standing on a 20 x 20 square, widths 0.01 tapering to
// a quarter, a clumpId channel (none: `channel` false).
StrandsPtr bundles(std::size_t clumps, std::size_t members, std::size_t points = 9, float spread = 0.05f, bool channel = true,
                   bool widths = true) {
    auto s = std::make_shared<StrandSet>();
    std::vector<std::size_t> counts(clumps * members, points);
    s->resize(counts);
    std::vector<Vec3f>& p = s->mutablePositions();
    std::vector<float>& w = s->mutableWidths();
    std::vector<StrandId>& ids = s->mutableIds();
    std::vector<float>* clumpId = channel ? &s->addStrandChannel("clumpId", -1.0f) : nullptr;
    const std::size_t side = static_cast<std::size_t>(std::ceil(std::sqrt(static_cast<double>(clumps))));
    for (std::size_t c = 0; c < clumps; ++c) {
        const Vec3f base(20.0f * static_cast<float>(c % side) / side, 0.0f, 20.0f * static_cast<float>(c / side) / side);
        for (std::size_t m = 0; m < members; ++m) {
            const std::size_t i = c * members + m;
            ids[i] = hash64(i * 7 + 3);
            Rng rng(ids[i]);
            const Vec3f offset(rng.uniform(-spread, spread), 0.0f, rng.uniform(-spread, spread));
            for (std::size_t k = 0; k < points; ++k) {
                const float u = static_cast<float>(k) / static_cast<float>(points - 1);
                p[s->strandBegin(i) + k] = base + offset * (1.0f - 0.5f * u) + Vec3f(0.0f, 10.0f * u, 0.0f);
                w[s->strandBegin(i) + k] = 0.01f * (1.0f - 0.75f * u);
            }
            if (clumpId) (*clumpId)[i] = static_cast<float>(c);
        }
    }
    if (!widths) s->mutableWidths().assign(s->pointCount(), 1.0f), s->mutableWidths().resize(0);
    return s;
}

// A view `pixels` a unit everywhere (orthographic).
StrandLodView at(float pixels) { return lodView({0, 0, 0}, {0, 0, -1}, pixels, true, {0, 0, 0}); }
StrandLodView atLevel(int level) {
    StrandLodView v;
    v.level = level;
    v.pixelScale = lodLevelPixels(level);
    v.orthographic = true;
    return v;
}

std::set<std::uint32_t> drawn(const StrandLod::Selection& sel) {
    std::set<std::uint32_t> out;
    for (const auto& list : sel.strands) out.insert(list.begin(), list.end());
    return out;
}

TEST(StrandLod, ClassesAreQuarterOctaves) {
    EXPECT_FLOAT_EQ(lodClassExponent(0), 1.0f);
    EXPECT_FLOAT_EQ(lodClassExponent(4), 2.0f);
    EXPECT_NEAR(lodClassExponent(1), std::pow(2.0f, 0.25f), 1e-6f);
    EXPECT_FLOAT_EQ(lodClassExponent(-3), 1.0f);  // clamped
}

TEST(StrandLod, TheViewIsTheScaleWhereEachPointComesClosestOrbiting) {
    const Vec3f center(0, 0, 0);
    // 100 units away (900 pixels a unit at clip w 1): 9 pixels a unit at the center.
    const StrandLodView view = lodView({0, 0, 100}, {0, 0, -1}, 900.0f, false, center);
    EXPECT_EQ(view.level, lodLevel(9.0f));
    // A point 10 from the center comes to 90 (as near as the level's quantization says).
    EXPECT_NEAR(view.pixelsAt(10.0f), 900.0f / (900.0f / lodLevelPixels(view.level) - 10.0f), 1e-3f);
    EXPECT_GT(view.pixelsAt(10.0f), view.pixelsAt(0.0f));
    EXPECT_TRUE(std::isinf(view.pixelsAt(200.0f)));  // it may come to the eye
    // Orbiting at that distance: the same view.
    for (int a = 0; a < 12; ++a) {
        const float t = 0.5236f * static_cast<float>(a);
        const Vec3f eye(100.0f * std::sin(t), 30.0f * std::cos(2.0f * t), 100.0f * std::cos(t));
        const Vec3f look = center - eye;
        EXPECT_EQ(lodView(eye * (100.0f / eye.length()), look, 900.0f, false, center), view);
    }
    // Orthographic: the scale itself everywhere; the center behind the eye: none.
    const StrandLodView flat = lodView({0, 0, 100}, {0, 0, -1}, 7.0f, true, center);
    EXPECT_EQ(flat.pixelsAt(0.0f), flat.pixelsAt(50.0f));
    EXPECT_EQ(lodView({0, 0, -5}, {0, 0, -1}, 900.0f, false, center).level, kNoLodLevel);
    EXPECT_EQ(lodLevel(std::numeric_limits<float>::infinity()), kNoLodLevel);
}

// A random rotation: its columns the camera's right, up and back (forward = -back).
struct Frame {
    Vec3f right, up, back;
};
Frame randomFrame(Rng& rng) {
    Vec3f a(rng.uniform(-1.0f, 1.0f), rng.uniform(-1.0f, 1.0f), rng.uniform(-1.0f, 1.0f));
    Vec3f b(rng.uniform(-1.0f, 1.0f), rng.uniform(-1.0f, 1.0f), rng.uniform(-1.0f, 1.0f));
    const Vec3f back = a.normalized();
    const Vec3f right = (b - back * b.dot(back)).normalized();
    return {right, back.cross(right), back};
}

TEST(StrandLod, ThePivotAtTheCenterIsTheCentersDepth) {
    const Vec3f center(3, 5, -2);
    Rng rng(11);
    StrandLodView a, b;
    for (int i = 0; i < 50; ++i) {
        const Frame f = randomFrame(rng);
        const Vec3f eye = center + f.back * rng.uniform(20.0f, 200.0f) + f.right * rng.uniform(-5.0f, 5.0f);
        a = lodView(eye, f.back * -1.0f, 900.0f, false, center, a);
        b = lodView(eye, f.back * -1.0f, 900.0f, false, center, center, b);
        EXPECT_EQ(a, b);
        EXPECT_EQ(a.level, a.farLevel);  // (the center's depth both nearest and farthest)
    }
}

TEST(StrandLod, AnOrbitAboutAnyPivotKeepsTheView) {
    // Pivots at the center, inside the sphere, on it, outside it toward the camera and beyond the
    // center; the camera turning about each at two distances, with the pivot on its axis (Maya's
    // center of interest, the app's target) or anywhere in the view (Maya's tumble pivot): every
    // view of an orbit the same, every point of the sphere's ball judged no nearer than it comes.
    const Vec3f center(3, 5, -2);
    const float radius = 10.0f, pixelScale = 900.0f;
    const float quantization = 0.75f / kLodLevelsPerOctave + 1e-4f;  // (log2: the levels' hysteresis at most)
    Rng rng(7);
    for (const Vec3f offset : {Vec3f(0, 0, 0), Vec3f(4, 1, 0), Vec3f(10, 0, 0), Vec3f(0, 3, 25), Vec3f(-6, 0, -30)}) {
        const Vec3f pivot = center + offset;
        const float e = offset.length();
        for (const float distance : {60.0f, 150.0f}) {
            for (const bool onAxis : {true, false}) {
                // The pivot's place in the camera's frame: on its axis, or off it (its depth `distance`).
                const Vec3f inView = onAxis ? Vec3f(0, 0, -distance) : Vec3f(0.2f * distance, -0.15f * distance, -distance);
                const float nearest = distance - e, farthest = distance + e;  // (the center's depth as it turns)
                StrandLodView first, previous;
                float closest = 0.0f;
                for (int step = 0; step < 200; ++step) {
                    const Frame f = randomFrame(rng);
                    const Vec3f eye = pivot + f.right * -inView.x + f.up * -inView.y + f.back * -inView.z;
                    const Vec3f forward = f.back * -1.0f;
                    const StrandLodView view = lodView(eye, forward, pixelScale, false, center, pivot, previous);
                    if (step == 0) first = view;
                    EXPECT_EQ(view, first) << "pivot offset " << offset.x << "," << offset.y << "," << offset.z << " distance " << distance
                                           << (onAxis ? " on axis" : " off axis") << " step " << step;
                    previous = view;
                    ASSERT_NE(view.level, kNoLodLevel);
                    // The view: the center's nearest and farthest depths, quantized.
                    EXPECT_LE(std::fabs(std::log2(view.pixelsAt(0.0f) * nearest / pixelScale)), quantization);
                    EXPECT_LE(std::fabs(std::log2(view.pixelsAtFarthest(0.0f) * farthest / pixelScale)), quantization);
                    // A point of the ball (r about the center), its nearest along the line of sight:
                    // never nearer than the center's nearest less r; the center never farther than its farthest.
                    const float r = rng.uniform(0.0f, radius);
                    const Vec3f toward = center - eye;
                    const Vec3f x = center - toward.normalized() * r;
                    const float depth = (x - eye).dot(forward);
                    EXPECT_GE(depth, (nearest - r) * (1.0f - 1e-5f) - 1e-3f);
                    EXPECT_LE(toward.dot(forward), farthest * (1.0f + 1e-5f) + 1e-3f);
                    closest = std::max(closest, (nearest - r) / depth);
                }
                EXPECT_GT(closest, 0.9f);  // (random turns come near the bound)
            }
        }
    }
}

TEST(StrandLod, ADollyChangesTheViewInSteps) {
    const Vec3f center(0, 0, 0), pivot(4, 2, 1);
    const Vec3f forward = Vec3f(0.3f, -0.2f, -1.0f).normalized();
    StrandLodView previous;
    int changes = 0, first = kNoLodLevel;
    for (float distance = 100.0f; distance >= 50.0f; distance *= 0.995f) {
        const StrandLodView view = lodView(pivot - forward * distance, forward, 900.0f, false, center, pivot, previous);
        ASSERT_NE(view.level, kNoLodLevel);
        if (first == kNoLodLevel) first = view.level;
        if (previous.level != kNoLodLevel && view.level != previous.level) {
            EXPECT_EQ(view.level, previous.level + 1);  // nearer: finer, a level at a time
            ++changes;
        }
        previous = view;
    }
    // The center's nearest depth from 100 - e to 50 - e: about 8 log2 of their ratio levels.
    const float e = pivot.length();
    const int expected = static_cast<int>(std::lround(8.0 * std::log2((100.0 - e) / (50.0 - e))));
    EXPECT_NEAR(changes, expected, 1);
    EXPECT_NEAR(previous.level - first, expected, 1);
}

TEST(StrandLod, APivotTheCenterMayComeToTheEyeAboutHasNoLevel) {
    // The camera turning about a point 5 in front of it, the strands 50 away: they may come beside
    // it (to its plane) as it turns: nothing thinned.
    const Vec3f eye(0, 0, 50), forward(0, 0, -1), center(0, 0, 0);
    EXPECT_EQ(lodView(eye, forward, 900.0f, false, center, Vec3f(0, 0, 45)).level, kNoLodLevel);
    EXPECT_NE(lodView(eye, forward, 900.0f, false, center, Vec3f(0, 0, 10)).level, kNoLodLevel);
    // Orthographic: the scale itself, whatever the pivot.
    const StrandLodView flat = lodView(eye, forward, 7.0f, true, center, Vec3f(0, 0, 45));
    EXPECT_EQ(flat.level, lodLevel(7.0f));
    EXPECT_EQ(flat.farLevel, flat.level);
}

TEST(StrandLod, LevelsKeepTheirValueWithinThreeQuartersOfALevel) {
    const int level = lodLevel(10.0f);
    EXPECT_EQ(level, static_cast<int>(std::lround(8.0 * std::log2(10.0))));
    EXPECT_NEAR(lodLevelPixels(level), 10.0f, 10.0f * 0.05f);
    const float step = std::exp2(1.0f / 8.0f);
    // Wobbling by half a level either way keeps it; a level and a half moves it.
    EXPECT_EQ(lodLevel(lodLevelPixels(level) * std::pow(step, 0.6f), level), level);
    EXPECT_EQ(lodLevel(lodLevelPixels(level) / std::pow(step, 0.6f), level), level);
    EXPECT_EQ(lodLevel(lodLevelPixels(level) * std::pow(step, 1.5f), level), level + 2);  // (round(L + 1.5) from L + 1.5)
    EXPECT_NE(lodLevel(lodLevelPixels(level) / std::pow(step, 1.0f), level), level);
}

TEST(StrandLod, BandInkIsTheFibersOwnWhenThinAndSaturatesWhenDense) {
    EXPECT_EQ(lodBandInk(0.0f), 0.0f);
    EXPECT_NEAR(lodBandInk(1e-4f), 1e-4f, 1e-8f);
    EXPECT_NEAR(lodBandInk(0.05f), 0.05f, 0.05f * 0.02f);  // thin: about a (a little less: overlap)
    EXPECT_LT(lodBandInk(0.05f), 0.05f);
    float last = 0.0f;
    for (float a = 0.01f; a < 1e4f; a *= 1.7f) {
        const float ink = lodBandInk(a);
        EXPECT_GT(ink, last);
        EXPECT_LE(ink, a * 1.0001f);  // never more than the fibers' own widths
        last = ink;
    }
    // Dense: about 2 sqrt(2 ln(a / sqrt(2 pi))) (the band opaque where a phi > 1).
    const float a = 1e4f;
    EXPECT_NEAR(lodBandInk(a), 2.0f * std::sqrt(2.0f * std::log(a / 2.5066f)), 0.6f);
}

TEST(StrandLod, StrandsAreGroupedByTheirClumpOrByCellsOfTheirRoots) {
    StrandLod lod;
    lod.setStrands(bundles(16, 50), "clumpId");
    EXPECT_EQ(lod.groupCount(), 16u);
    EXPECT_TRUE(lod.clumped());
    EXPECT_TRUE(lod.hasWidths());
    StrandLod cells;
    cells.setStrands(bundles(16, 50, 9, 0.05f, false), "clumpId");
    EXPECT_FALSE(cells.clumped());
    EXPECT_GE(cells.groupCount(), 1u);
    EXPECT_LE(cells.groupCount(), 800u);
    // The bounding sphere holds every point.
    const StrandsPtr s = bundles(16, 50);
    lod.setStrands(s, "clumpId");
    for (const Vec3f& p : s->positions()) EXPECT_LE((p - lod.center()).length(), lod.radius() * 1.0001f);
}

TEST(StrandLod, CloseViewsDrawEverythingAndFarViewsThin) {
    StrandLod lod;
    lod.setStrands(bundles(16, 400), "clumpId");
    StrandLodSettings settings;
    settings.impostors = false;
    // A unit 2000 pixels: strands of 0.01 cover 20 pixels, nothing thinned.
    EXPECT_FALSE(lod.select(at(2000.0f), settings).active);
    // A unit 2 pixels: 0.02 of a pixel each; thinned.
    const StrandLod::Selection& far = lod.select(at(2.0f), settings);
    ASSERT_TRUE(far.active);
    EXPECT_LT(far.drawnStrands, far.totalStrands / 4);
    EXPECT_EQ(far.drawnSegments, far.drawnStrands * 8);
    // No view (no level), or strands without widths: nothing thinned.
    EXPECT_FALSE(lod.select(StrandLodView{}, settings).active);
    StrandLod opaque;
    opaque.setStrands(bundles(4, 100, 9, 0.05f, true, false), "clumpId");
    EXPECT_FALSE(opaque.hasWidths());
    EXPECT_FALSE(opaque.select(at(2.0f), settings).active);
}

TEST(StrandLod, TheDrawnStrandsStandForTheGroupToAStrand) {
    const StrandsPtr s = bundles(9, 300);
    StrandLod lod;
    lod.setStrands(s, "clumpId");
    StrandLodSettings settings;
    settings.impostors = false;
    for (const float pixels : {50.0f, 8.0f, 3.0f, 1.0f, 0.4f}) {
        const StrandLod::Selection& sel = lod.select(at(pixels), settings);
        if (!sel.active) continue;
        // Per clump: the exponents of its drawn strands summed, against its members.
        std::vector<double> weight(9, 0.0);
        double most = 1.0;
        for (int k = 0; k < kLodClassCount; ++k) {
            for (const std::uint32_t i : sel.strands[k]) weight[i / 300] += lodClassExponent(k);
            if (!sel.strands[k].empty()) most = std::max<double>(most, lodClassExponent(k));
        }
        for (const double w : weight) EXPECT_NEAR(w, 300.0, 0.5 * most + 1e-3) << pixels;
    }
}

TEST(StrandLod, FewerStrandsAreASubsetOfMore) {
    StrandLod lod;
    lod.setStrands(bundles(12, 500), "clumpId");
    StrandLodSettings settings;
    settings.impostors = false;
    std::set<std::uint32_t> previous;
    bool first = true;
    // Zooming out, level by level: each selection's strands within the last one's.
    for (int level = lodLevel(400.0f); level >= lodLevel(0.3f); --level) {
        const StrandLod::Selection& sel = lod.select(atLevel(level), settings);
        const std::set<std::uint32_t> now = sel.active ? drawn(sel) : std::set<std::uint32_t>{};
        if (sel.active && !first && !previous.empty()) {
            EXPECT_TRUE(std::includes(previous.begin(), previous.end(), now.begin(), now.end())) << level;
        }
        if (sel.active) {
            previous = now;
            first = false;
        }
    }
    EXPECT_FALSE(first);
}

TEST(StrandLod, TheSameInputsGiveTheSameSelection) {
    const StrandsPtr s = bundles(10, 300);
    StrandLod a, b;
    a.setStrands(s, "clumpId");
    b.setStrands(std::make_shared<StrandSet>(*s), "clumpId");
    StrandLodSettings settings;
    for (const float pixels : {30.0f, 4.0f, 1.0f, 0.2f}) {
        const StrandLod::Selection& x = a.select(at(pixels), settings);
        const StrandLod::Selection& y = b.select(at(pixels), settings);
        EXPECT_EQ(x.active, y.active);
        EXPECT_EQ(x.key, y.key);
        EXPECT_EQ(x.impostorKey, y.impostorKey);
        for (int k = 0; k < kLodClassCount; ++k) EXPECT_EQ(x.strands[k], y.strands[k]);
        EXPECT_EQ(x.impostorGroups, y.impostorGroups);
    }
}

TEST(StrandLod, MovedStrandsKeepTheirGroupsAndASmallChangeKeepsTheSelection) {
    const StrandsPtr s = bundles(10, 300);
    StrandLod lod;
    lod.setStrands(s, "clumpId");
    StrandLodSettings settings;
    settings.impostors = false;
    const int level = lodLevel(3.0f);
    const StrandLod::Selection before = lod.select(atLevel(level), settings);
    ASSERT_TRUE(before.active);
    const std::uint64_t serial = lod.groupingSerial();
    // The same strands nudged (a brush dab): the groups stay, the selection too.
    auto moved = std::make_shared<StrandSet>(*s);
    for (Vec3f& p : moved->mutablePositions()) p = p + Vec3f(0.001f, 0.0f, 0.0f);
    lod.setStrands(moved, "clumpId");
    EXPECT_EQ(lod.groupingSerial(), serial);
    EXPECT_EQ(lod.select(atLevel(level), settings).key, before.key);
    // Other strands (other IDs): grouped again.
    lod.setStrands(bundles(11, 300), "clumpId");
    EXPECT_NE(lod.groupingSerial(), serial);
}

TEST(StrandLod, SegmentsAreTheClassesStrandsSegments) {
    const StrandsPtr s = bundles(6, 200, 5);
    StrandLod lod;
    lod.setStrands(s, "clumpId");
    StrandLodSettings settings;
    settings.impostors = false;
    const StrandLod::Selection& sel = lod.select(at(1.5f), settings);
    ASSERT_TRUE(sel.active);
    EXPECT_EQ(sel.blendSegments, 0u);  // (0.015 of a pixel: hashed alone)
    std::array<std::size_t, kLodClassCount + 2> firsts{};
    const std::vector<std::uint32_t> all = lod.classSegments(firsts);
    EXPECT_EQ(all.size(), 2 * sel.drawnSegments);
    EXPECT_EQ(firsts[kLodClassCount + 1], firsts[kLodClassCount]);
    for (int k = 0; k < kLodClassCount; ++k) {
        ASSERT_EQ(firsts[k + 1] - firsts[k], 2 * lod.segmentCount(k));
        std::size_t at = firsts[k];
        for (const std::uint32_t i : sel.strands[k]) {
            for (std::size_t p = 0; p + 1 < s->strandPointCount(i); ++p) {
                EXPECT_EQ(all[at++], s->strandBegin(i) + p);
                EXPECT_EQ(all[at++], s->strandBegin(i) + p + 1);
            }
        }
        EXPECT_EQ(at, firsts[k + 1]);
    }
}

TEST(StrandLod, OnlyTheSlotsWhoseSegmentsChangedMoveTheirKeys) {
    // Clumps of different sizes (their classes differ), zoomed out a level at a time: a slot's key
    // stays exactly while its segments do, and moves when they change.
    auto s = std::make_shared<StrandSet>(*bundles(12, 500, 7));
    std::vector<float>& clumpId = *s->mutableStrandChannel("clumpId");
    for (std::size_t i = 0, c = 0, end = 60; i < clumpId.size(); ++i) {  // clumps of 60, 140, 220, ... strands
        if (i == end) end += 60 + 80 * ++c;
        clumpId[i] = static_cast<float>(c);
    }
    StrandLod lod;
    lod.setStrands(s, "clumpId");
    StrandLodSettings settings;
    settings.impostors = false;
    std::array<std::vector<std::uint32_t>, kLodSlotCount> lastLists;
    std::array<std::uint64_t, kLodSlotCount> lastKeys{};
    int kept = 0, moved = 0;
    for (int level = lodLevel(60.0f); level >= lodLevel(0.5f); --level) {
        const StrandLod::Selection& sel = lod.select(atLevel(level), settings);
        if (!sel.active) continue;
        std::array<std::size_t, kLodClassCount + 2> firsts{};
        const std::vector<std::uint32_t> all = lod.classSegments(firsts);
        for (int slot = 0; slot < kLodSlotCount; ++slot) {
            const std::size_t segments = slot < kLodClassCount ? lod.segmentCount(slot) : lod.segmentCount(0, kLodBlended);
            EXPECT_EQ(sel.slotSegments[slot], segments);
            EXPECT_EQ(firsts[slot + 1] - firsts[slot], 2 * segments);
            EXPECT_EQ(sel.slotKeys[slot] == 0, segments == 0);
            const std::vector<std::uint32_t> list(all.begin() + static_cast<std::ptrdiff_t>(firsts[slot]),
                                                  all.begin() + static_cast<std::ptrdiff_t>(firsts[slot + 1]));
            if (segments > 0 && lastKeys[slot] != 0) {
                EXPECT_EQ(sel.slotKeys[slot] == lastKeys[slot], list == lastLists[slot]) << "slot " << slot << " level " << level;
                (list == lastLists[slot] ? kept : moved) += 1;
            }
            lastLists[slot] = list;
            lastKeys[slot] = sel.slotKeys[slot];
        }
    }
    EXPECT_GT(kept, 0);
    EXPECT_GT(moved, 0);
}

TEST(StrandLod, SegmentsAreWrittenInChunksAsInOne) {
    // Many strands of uneven point counts (more than a chunk of them in a class): each class's
    // segments in strand order, every one once.
    auto s = std::make_shared<StrandSet>();
    std::vector<std::size_t> counts(20000);
    for (std::size_t i = 0; i < counts.size(); ++i) counts[i] = 2 + (hash64(i) % 9);
    s->resize(counts);
    std::vector<float>& clumpId = s->addStrandChannel("clumpId", -1.0f);
    for (std::size_t i = 0; i < counts.size(); ++i) {
        s->mutableIds()[i] = hash64(i * 5 + 1);
        clumpId[i] = static_cast<float>(i / 400);
        for (std::size_t k = 0; k < counts[i]; ++k) {
            const float u = static_cast<float>(k) / static_cast<float>(counts[i] - 1);
            s->mutablePositions()[s->strandBegin(i) + k] = Vec3f(static_cast<float>(i % 141) * 0.1f, 5.0f * u, static_cast<float>(i / 141) * 0.1f);
            s->mutableWidths()[s->strandBegin(i) + k] = 0.01f * (1.0f - 0.75f * u);
        }
    }
    StrandLod lod;
    lod.setStrands(s, "clumpId");
    StrandLodSettings settings;
    settings.impostors = false;
    for (const float pixels : {300.0f, 60.0f, 8.0f}) {
        const StrandLod::Selection& sel = lod.select(at(pixels), settings);
        if (!sel.active) continue;
        std::array<std::size_t, kLodClassCount + 2> firsts{};
        const std::vector<std::uint32_t> all = lod.classSegments(firsts);
        std::set<std::uint32_t> starts;
        for (int k = 0; k < kLodClassCount; ++k) {
            std::size_t at = firsts[k];
            for (const std::uint32_t i : sel.strands[k]) {
                for (std::size_t p = 0; p + 1 < s->strandPointCount(i); ++p) {
                    const auto start = static_cast<std::uint32_t>(s->strandBegin(i) + p);
                    if (k == 0 && sel.blendSegments > 0 && (at >= firsts[1] || all[at] != start)) continue;  // (in the blended list)
                    ASSERT_LT(at + 1, all.size());
                    EXPECT_EQ(all[at], start);
                    EXPECT_EQ(all[at + 1], start + 1);
                    at += 2;
                }
            }
            EXPECT_EQ(at, firsts[k + 1]) << pixels << " class " << k;
        }
        for (std::size_t j = 0; j < all.size(); j += 2) EXPECT_TRUE(starts.insert(all[j]).second);
        EXPECT_EQ(starts.size(), sel.drawnSegments);
    }
}

TEST(StrandLod, SegmentsThatMayBlendAreAListOfTheirOwn) {
    // Widths 0.01 tapering to 0.0025 at 40 pixels a unit: 0.4 to 0.1 of a pixel, all in the blended
    // range; at 400, 4 to 1 (opaque) but the tips' last segment.
    const StrandsPtr s = bundles(4, 50, 5);
    StrandLod lod;
    lod.setStrands(s, "clumpId");
    StrandLodSettings settings;
    settings.impostors = false;
    settings.error = 0.01f;  // (nothing thinned)
    const StrandLod::Selection& close = lod.select(at(40.0f), settings);
    ASSERT_TRUE(close.active);  // nothing thinned (strands wide enough), but they blend
    EXPECT_EQ(close.thinnedGroups, 0u);
    EXPECT_EQ(close.blendSegments, close.drawnSegments);
    EXPECT_EQ(lod.segmentCount(0, kLodOpaque), 0u);  // (every segment in the list, drawn twice)
    EXPECT_EQ(lod.segmentCount(0, kLodBlended), close.drawnSegments);
    // Opaque everywhere: nothing to split, nothing to do.
    EXPECT_FALSE(lod.select(at(4000.0f), settings).active);
    // Off: no list (here, nothing to do at all).
    settings.splitBlended = false;
    EXPECT_FALSE(lod.select(at(40.0f), settings).active);
    // Some segments blend: each segment once, in the class or in the list.
    settings.splitBlended = true;
    const StrandLod::Selection& some = lod.select(at(140.0f), settings);  // 1.4 to 0.35: the tips blend
    ASSERT_TRUE(some.active);
    EXPECT_GT(some.blendSegments, 0u);
    EXPECT_LT(some.blendSegments, some.drawnSegments);
    std::array<std::size_t, kLodClassCount + 2> firsts{};
    const std::vector<std::uint32_t> all = lod.classSegments(firsts);
    EXPECT_EQ(all.size(), 2 * some.drawnSegments);
    std::set<std::uint32_t> starts;
    for (std::size_t j = 0; j < all.size(); j += 2) {
        EXPECT_EQ(all[j + 1], all[j] + 1);
        EXPECT_TRUE(starts.insert(all[j]).second);
    }
    EXPECT_EQ(starts.size(), s->strandCount() * 4);
    for (std::size_t j = firsts[kLodClassCount]; j < firsts[kLodClassCount + 1]; j += 2) {
        // A blendable segment reaches below a pixel at one end at least, above a tenth at one.
        EXPECT_LT(std::min(s->widths()[all[j]], s->widths()[all[j + 1]]) * 140.0f, 1.0f);
        EXPECT_GT(std::max(s->widths()[all[j]], s->widths()[all[j + 1]]) * 140.0f, kLineHashedBelow);
    }
}

TEST(StrandLod, TightClumpsFarAwayAreOneLineOfTheirInk) {
    // 64 clumps of 200 strands within 0.05 of their axes: at 2 pixels a unit they spread 0.1 pixel.
    const StrandsPtr s = bundles(64, 200, 9, 0.05f);
    StrandLod lod;
    lod.setStrands(s, "clumpId");
    StrandLodSettings settings;
    const StrandLod::Selection& sel = lod.select(at(2.0f), settings);
    ASSERT_TRUE(sel.active);
    EXPECT_EQ(sel.impostorGroups.size(), 64u);
    EXPECT_EQ(sel.drawnStrands, 0u);
    const StrandsPtr axes = lod.impostors();
    ASSERT_TRUE(axes);
    ASSERT_EQ(axes->strandCount(), 64u);
    // Each point as wide as the band's ink: about the members' widths together while they overlap
    // little, never more; along the clump's axis (the members' mean).
    for (std::size_t j = 0; j < axes->strandCount(); ++j) {
        const std::size_t b = axes->strandBegin(j);
        const float root = axes->widths()[b];
        EXPECT_GT(root, 0.0f);
        EXPECT_LE(root, 200.0f * 0.01f * 1.0001f);
        EXPECT_NEAR(axes->positions()[b + axes->strandPointCount(j) - 1].y, 10.0f, 1e-3f);
    }
    EXPECT_EQ(lod.impostors(), axes);  // kept while the selection stands
    // Their colors: each point the members' mean.
    auto colors = std::make_shared<StrandColors>();
    colors->strands = s;
    for (std::size_t i = 0; i < s->strandCount(); ++i) colors->colors.push_back({static_cast<float>(i % 2), 0.5f, 0.0f});
    const auto mean = lod.impostorColors(*colors);
    ASSERT_TRUE(mean);
    EXPECT_EQ(mean->vertexCount(), axes->pointCount());
    for (std::size_t v = 0; v < axes->pointCount(); ++v) {
        const auto c = mean->at(0, v);
        EXPECT_NEAR(c[0], 0.5f, 0.15f);
        EXPECT_NEAR(c[1], 0.5f, 1e-5f);
    }
    // Without impostors the same clumps are thinned lines.
    settings.impostors = false;
    const StrandLod::Selection& lines = lod.select(at(2.0f), settings);
    EXPECT_TRUE(lines.impostorGroups.empty());
    EXPECT_GT(lines.drawnStrands, 0u);
}

TEST(StrandLod, ThePreviewsDensityCountsEachStrandForMore) {
    StrandLod lod;
    lod.setStrands(bundles(9, 400), "clumpId");
    StrandLodSettings full, preview;
    full.impostors = preview.impostors = false;
    preview.density = 0.25f;  // each strand already stands for four
    const std::size_t atFull = lod.select(at(6.0f), full).drawnStrands;
    StrandLod again;
    again.setStrands(bundles(9, 400), "clumpId");
    const std::size_t atPreview = again.select(at(6.0f), preview).drawnStrands;
    EXPECT_GT(atPreview, atFull);  // each kept strand widened from a wider one: more kept for the same error
}

}  // namespace
}  // namespace groomlab
