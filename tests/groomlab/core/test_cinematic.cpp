// SPDX-License-Identifier: Apache-2.0
// Modified for Lila (2026).
// Cinematic look nodes: HairColor (melanin and RGB), Flyaway and Tangle.

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <thread>

#include "groomlab/cards.h"
#include "groomlab/curve.h"
#include "groomlab/export_view.h"
#include "groomlab/graph_io.h"
#include "groomlab/hair_color.h"
#include "groomlab/groom.h"
#include "groomlab/map_ref.h"
#include "groomlab/operators.h"
#include "groomlab/presets.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

// ---------------------------------------------------------------------------
// HairColor as RGB: dye, a map (an image, a color set), randomness, tip color

std::string tempPath(const char* name) { return (std::filesystem::temp_directory_path() / name).generic_string(); }

// Binary PPM (RGB), rows top to bottom; rgb(x, y) in 0-255.
template <class F>
void writePpm(const std::string& path, int w, int h, F rgb) {
    std::ofstream f(path, std::ios::binary);
    f << "P6\n" << w << " " << h << "\n255\n";
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::array<int, 3> c = rgb(x, y);
            for (const int v : c) f.put(static_cast<char>(v));
        }
    }
}

// Unit plane with UVs (u = x, v = z) and a color set "tint": green where x > 0.5, else blue.
std::shared_ptr<const ScalpMesh> tintedPlane(int cells = 8) {
    const auto plane = test::makePlane(cells);
    auto mesh = std::make_shared<ScalpMesh>(plane->points(), plane->triangles());
    std::vector<ScalpMesh::UV> uvs;
    std::vector<ScalpMesh::Color> tint;
    for (const auto& tri : mesh->triangles()) {
        for (const std::int32_t v : tri) {
            const Vec3f& p = mesh->points()[v];
            uvs.push_back({p.x, p.z});
            tint.push_back(p.x > 0.5f ? ScalpMesh::Color{0.0f, 1.0f, 0.0f, 1.0f} : ScalpMesh::Color{0.0f, 0.0f, 1.0f, 1.0f});
        }
    }
    mesh->setCornerUVs(uvs);
    mesh->setCornerColors("tint", tint);
    return mesh;
}

std::array<float, 3> colorOf(const StrandSet& s, std::size_t i) {
    return {(*s.strandChannel("colorR"))[i], (*s.strandChannel("colorG"))[i], (*s.strandChannel("colorB"))[i]};
}

TEST(HairColorRgb, ExportPrefersExplicitColorOverMelanin) {
    StrandSet s;
    s.addStrand(1, {{0, 0, 0}, {0, 1, 0}}, {0.01f, 0.01f});
    s.addStrand(2, {{1, 0, 0}, {1, 1, 0}}, {0.01f, 0.01f});
    s.addStrandChannel(kMelaninChannel, 1.0f);  // black hair
    EXPECT_LT(strandColors(s)[0][0], 0.2f);
    s.addStrandChannel(kColorChannels[0], 0.9f);
    s.addStrandChannel(kColorChannels[1], 0.8f);
    EXPECT_LT(strandColors(s)[0][0], 0.2f);  // two of three channels: not a color yet
    s.addStrandChannel(kColorChannels[2], 0.7f);
    const std::vector<std::array<float, 3>> colors = strandColors(s);
    ASSERT_EQ(colors.size(), 2u);
    EXPECT_FLOAT_EQ(colors[1][0], 0.9f);
    EXPECT_FLOAT_EQ(colors[1][1], 0.8f);
    EXPECT_FLOAT_EQ(colors[1][2], 0.7f);
    const ExportView view = describeExport(s, nullptr);
    EXPECT_TRUE(view.hasColors());
    EXPECT_FLOAT_EQ(view.colors[0][2], 0.7f);
}

TEST(HairColorRgb, ExportViewHasPointColorsOnlyWithAllThreePointChannels) {
    StrandSet s;
    s.addStrand(1, {{0, 0, 0}, {0, 1, 0}}, {0.01f, 0.01f});
    s.addStrand(2, {{1, 0, 0}, {1, 1, 0}, {1, 2, 0}}, {0.01f, 0.01f, 0.01f});
    for (const char* name : kColorChannels) s.addStrandChannel(name, 0.4f);
    EXPECT_FALSE(describeExport(s, nullptr).hasPointColors());
    s.addPointChannel(kColorChannels[0], 0.2f);
    s.addPointChannel(kColorChannels[1], 0.3f);
    EXPECT_FALSE(describeExport(s, nullptr).hasPointColors());  // two of three: per strand still
    s.addPointChannel(kColorChannels[2], 0.5f);
    (*s.mutablePointChannel(kColorChannels[0]))[4] = 0.9f;
    const ExportView view = describeExport(s, nullptr);
    ASSERT_TRUE(view.hasPointColors());
    ASSERT_EQ(view.pointColors.size(), s.pointCount());
    EXPECT_FLOAT_EQ(view.pointColors[0][1], 0.3f);
    EXPECT_FLOAT_EQ(view.pointColors[4][0], 0.9f);
    EXPECT_FLOAT_EQ(view.pointColors[4][2], 0.5f);
    EXPECT_FLOAT_EQ(view.colors[1][0], 0.4f);  // the per-strand color stays
}

// Two straight strands 0.1 apart (100 points each, so a point sits near the span's
// middle), tangled at strength 1; `stick` says which of them stick (the fraction map),
// `clumps` their clumpId when given; `set` edits Tangle's params further.
std::array<StrandsPtr, 2> tangledPair(std::vector<float> stick, std::vector<float> clumps = {},
                                      const std::function<void(ParamSet&)>& set = {}) {
    auto in = std::make_shared<StrandSet>();
    for (int i = 0; i < 2; ++i) {
        in->addStrand(7 + i, test::straightCurve({0.4f + 0.1f * static_cast<float>(i), 0.0f, 0.5f}, {0, 1, 0}, 99));
    }
    in->addStrandChannel("stick") = std::move(stick);
    if (!clumps.empty()) in->addStrandChannel("clumpId") = std::move(clumps);
    auto tangle = OperatorRegistry::instance().create(op::kTangle);
    MapRef fraction;
    fraction.source = MapRef::Source::Attribute;
    fraction.name = "stick";
    tangle->params().set("fraction", fraction);
    tangle->params().set("strength", 1.0);
    tangle->params().set("radius", 0.2);
    if (set) set(tangle->params());
    EvalContext ctx;
    return {StrandsPtr(in), valueAs<StrandsPtr>(tangle->evaluate(ctx, {StrandsPtr(in)}))};
}

// The closest the first strand of `s` comes to the second's curve (a stuck strand keeps its
// length, so its points lag a little behind its partner's along the span).
float closest(const StrandSet& s) {
    float d = 1e9f;
    const Vec3f* other = s.strandPoints(1);
    for (std::size_t k = 0; k < s.strandPointCount(0); ++k) {
        const Vec3f p = s.strandPoints(0)[k];
        for (std::size_t j = 0; j + 1 < s.strandPointCount(1); ++j) {
            const Vec3f seg = other[j + 1] - other[j];
            const float t = std::clamp((p - other[j]).dot(seg) / std::max(1e-12f, seg.dot(seg)), 0.0f, 1.0f);
            d = std::min(d, (other[j] + seg * t - p).length());
        }
    }
    return d;
}

// How far the points of strand `s` moved from `in` to `out`, at most.
float movedMost(const StrandSet& in, const StrandSet& out, std::size_t s) {
    float most = 0.0f;
    for (std::size_t k = 0; k < in.strandPointCount(s); ++k) {
        most = std::max(most, (out.strandPoints(s)[k] - in.strandPoints(s)[k]).length());
    }
    return most;
}

TEST(Tangle, AtFullStrengthAStrandMeetsAPartnerThatStaysPut) {
    const auto [in, out] = tangledPair({1.0f, 0.0f});  // only the first sticks: all the way onto the second
    EXPECT_LT(closest(*out), 5e-3f);  // touching, within 5% of the gap (its length kept, it lags a little)
    for (std::size_t k = 0; k < 100; ++k) {
        EXPECT_LT((out->strandPoints(1)[k] - in->strandPoints(1)[k]).length(), 1e-7f);  // the partner stays
    }
    EXPECT_NEAR(arcLength(out->strandPoints(0), 100), arcLength(in->strandPoints(0), 100), 1e-4f);  // no stretch
}

TEST(Tangle, AtFullStrengthAMutualPairMeetsInTheMiddle) {
    const auto [in, out] = tangledPair({1.0f, 1.0f});  // each picks the other: both move halfway
    EXPECT_LT(closest(*out), 1e-3f);
    EXPECT_NEAR(std::max(movedMost(*in, *out, 0), movedMost(*in, *out, 1)), 0.05f, 1e-3f);
}

TEST(Tangle, PartnersComeFromTheStrandsOwnClump) {
    // Neighbours of two clumps never stick (they would bridge the gap between the clumps)...
    const auto [in, apart] = tangledPair({1.0f, 1.0f}, {3.0f, 4.0f});
    EXPECT_EQ(movedMost(*in, *apart, 0), 0.0f);
    EXPECT_EQ(movedMost(*in, *apart, 1), 0.0f);
    // ...unless asked, or when they share the clump.
    const auto across = tangledPair({1.0f, 1.0f}, {3.0f, 4.0f}, [](ParamSet& p) { p.set("clumpChannel", std::string()); })[1];
    EXPECT_LT(closest(*across), 1e-3f);
    const auto together = tangledPair({1.0f, 1.0f}, {3.0f, 3.0f})[1];
    EXPECT_LT(closest(*together), 1e-3f);
}

TEST(Tangle, APartnerFartherThanTheReachDoesNotPull) {
    const auto set = [](double reach) { return [reach](ParamSet& p) { p.set("reach", reach); }; };
    const auto [in, held] = tangledPair({1.0f, 0.0f}, {}, set(0.05));  // 0.1 apart: out of reach
    EXPECT_EQ(movedMost(*in, *held, 0), 0.0f);
    const auto unlimited = tangledPair({1.0f, 0.0f}, {}, set(0.0))[1];  // 0: no limit
    EXPECT_LT(closest(*unlimited), 5e-3f);
}

TEST(Tangle, WetHairStaysInItsClumpsWithoutStretching) {
    // The former wet_hair preset (tests/data/presets) on a ball: its tight clumps stuck together by
    // Tangle. Strands of neighbouring clumps once pulled tips across the gap, up to 7.5 times a segment's length.
    std::ifstream file(std::filesystem::u8path(std::string(GROOMLAB_TEST_DATA) + "/presets/wet_hair_tangle.json"));
    std::stringstream text;
    text << file.rdbuf();
    GraphDocument doc = loadGraphJson(presetGraphJson(text.str(), 20.0));
    doc.graph.node(doc.node("scatter")).params().set("density", 40.0);
    ExternalInputs externals;
    setGroomExternals(externals, test::makeSphere(24, 48, 8.0f), nullptr, nullptr);
    EvalContext ctx;
    ctx.externals = &externals;
    const StrandsPtr in = doc.graph.evaluateStrands(doc.node("clump"), ctx);
    const StrandsPtr out = doc.graph.evaluateStrands(doc.node("tangle"), ctx);
    const float reach = static_cast<float>(doc.graph.node(doc.node("tangle")).params().getDouble("reach", 0.25));
    std::size_t moved = 0;
    for (std::size_t s = 0; s < in->strandCount(); ++s) {
        const std::size_t n = in->strandPointCount(s);
        const Vec3f* a = in->strandPoints(s);
        const Vec3f* b = out->strandPoints(s);
        const float most = movedMost(*in, *out, s);
        if (most == 0.0f) continue;
        ++moved;
        EXPECT_LE(most, reach * 1.5f) << s;  // the reach, and the lengths kept along it
        for (std::size_t k = 1; k < n; ++k) {
            ASSERT_NEAR((b[k] - b[k - 1]).length(), (a[k] - a[k - 1]).length(), 1e-4f) << s << " " << k;
        }
    }
    EXPECT_GT(moved, in->strandCount() / 3);  // still matted
}

TEST(Cleanup, RampLinearFlagIsPartOfTheCacheKey) {
    // Toggling only a ramp's Linear flag must re-evaluate (it changes the curve).
    GroomData d;
    d.scalp = test::makePlane(8);
    d.graphJson = R"j({"version": 1, "output": "out", "nodes": [
        {"name": "scalp", "type": "Scalp"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 200.0}},
        {"name": "noise", "type": "Noise", "params": {"amplitude": 0.2, "profile": [[0, 0], [0.5, 1], [1, 0.3]]}},
        {"name": "out", "type": "Output"}, {"name": "resolution", "type": "Resample", "params": {"segments": 8}}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
                      {"from": "scatter", "to": "noise", "input": "strands"},
                      {"from": "noise", "to": "out", "input": "strands"}]})j";
    GraphDocument doc = loadGraphJson(d.graphJson);
    ExternalInputs externals;
    setGroomExternals(externals, d.scalp, nullptr, nullptr);
    EvalContext ctx;
    ctx.externals = &externals;
    const StrandsPtr smooth = doc.graph.evaluateStrands(doc.output, ctx);
    Ramp straight = doc.graph.node(doc.node("noise")).params().getRamp("profile");
    straight.straight = true;
    doc.graph.node(doc.node("noise")).params().set("profile", straight);
    const StrandsPtr linear = doc.graph.evaluateStrands(doc.output, ctx);
    EXPECT_NE(smooth->contentHash(), linear->contentHash());
}

TEST(Cleanup, AReloadedGraphReusesTheOldCache) {
    // Editing one param reloads the whole graph document (as Maya does);
    // only the edited node and what follows it may run again.
    auto graphWith = [](double amplitude) {
        return std::string(R"j({"version": 1, "output": "out", "nodes": [
            {"name": "scalp", "type": "Scalp"},
            {"name": "scatter", "type": "Scatter", "params": {"density": 300.0}},
            {"name": "length", "type": "Length", "params": {"keep": 0.8}},
            {"name": "noise", "type": "Noise", "params": {"amplitude": )j") + std::to_string(amplitude) + R"j(}},
            {"name": "out", "type": "Output"}, {"name": "resolution", "type": "Resample", "params": {"segments": 8}}],
          "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
                          {"from": "scatter", "to": "length", "input": "strands"},
                          {"from": "length", "to": "noise", "input": "strands"},
                          {"from": "noise", "to": "out", "input": "strands"}]})j";
    };
    const auto plane = test::makePlane(8);
    ExternalInputs externals;
    setGroomExternals(externals, plane, nullptr, nullptr);
    EvalContext ctx;
    ctx.externals = &externals;
    GraphDocument before = loadGraphJson(graphWith(0.05));
    before.graph.evaluateStrands(before.output, ctx);
    EXPECT_EQ(before.graph.evaluationCount(), 5u);

    GraphDocument after = loadGraphJson(graphWith(0.2));
    after.graph.adoptCache(before.graph);
    const StrandsPtr edited = after.graph.evaluateStrands(after.output, ctx);
    EXPECT_EQ(after.graph.evaluationCount(), 2u);  // noise and the output; scalp, scatter and length reused

    GraphDocument fresh = loadGraphJson(graphWith(0.2));
    EXPECT_EQ(fresh.graph.evaluateStrands(fresh.output, ctx)->contentHash(), edited->contentHash());
}

TEST(Cleanup, MergeKeepsBothSidesCards) {
    StrandSet a, b;
    a.addStrand(1, {{0, 0, 0}, {0, 1, 0}});
    b.addStrand(2, {{1, 0, 0}, {1, 1, 0}});
    b.addCardLayer(std::make_shared<CardMesh>());
    auto& channel = a.addStrandChannel("clumpId", -1.0f);
    channel[0] = 5.0f;
    StrandSet merged = a;
    merged.append(b);
    EXPECT_EQ(merged.strandCount(), 2u);
    EXPECT_EQ(merged.strandPointCount(1), 2u);
    EXPECT_EQ(merged.strandPoints(1)[1].y, 1.0f);
    EXPECT_EQ((*merged.strandChannel("clumpId"))[1], -1.0f);  // default for b's strand
    std::string error;
    EXPECT_TRUE(merged.validate(&error)) << error;
}


}  // namespace
}  // namespace groomlab
