// SPDX-License-Identifier: Apache-2.0
// Modified for Lila (2026).
// Viewport geometry (display.h): the buffers a host draws, on a fixture groom with every kind of
// item (strands in each color mode, ribbons, dots, guides, tubes, selection, parts, flow, cards,
// instances, maps on the scalp) and the images hosts make textures of, against the golden record
// of what Maya's viewport built before the builders moved to core (tests/data/golden/
// display_buffers.json, recorded once the core's output equalled the old Maya code's, run side by
// side, byte for byte; checked byte for byte where the floating point is MSVC's, counts
// everywhere). And the builder: what rebuilds when.

#include <gtest/gtest.h>

#include <bitset>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>

#include <nlohmann/json.hpp>

#include "groomlab/display.h"
#include "groomlab/export_view.h"
#include "groomlab/feathers.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/hair_color.h"
#include "groomlab/parts.h"
#include "groomlab/random.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;

const char* kGraph = R"({
  "version": 1,
  "nodes": [
    {"name": "scalp", "type": "Scalp"},
    {"name": "guides", "type": "Guides"},
    {"name": "scatter", "type": "Scatter", "params": {"density": 160.0}},
    {"name": "interpolate", "type": "Interpolate"},
    {"name": "clump", "type": "Clump", "params": {"count": 30, "strength": 0.6}},
    {"name": "width", "type": "Width"},
    {"name": "instance", "type": "Instance", "params": {"probability": 0.2, "prototype": "box", "seed": 3}},
    {"name": "cards", "type": "Cards", "params": {"segments": 4, "maxCards": 30}},
    {"name": "output", "type": "Output"}
  ],
  "connections": [
    {"from": "scalp", "to": "scatter", "input": "scalp"},
    {"from": "scatter", "to": "interpolate", "input": "strands"},
    {"from": "guides", "to": "interpolate", "input": "guides"},
    {"from": "scalp", "to": "interpolate", "input": "scalp"},
    {"from": "interpolate", "to": "clump", "input": "strands"},
    {"from": "clump", "to": "width", "input": "strands"},
    {"from": "width", "to": "instance", "input": "strands"},
    {"from": "instance", "to": "cards", "input": "strands"},
    {"from": "cards", "to": "output", "input": "strands"}
  ],
  "output": "output"
})";

// A sphere with UVs and a color set.
MeshPtr fixtureScalp() {
    const auto sphere = test::makeSphere(16, 32);
    auto mesh = std::make_shared<ScalpMesh>(sphere->points(), sphere->triangles());
    std::vector<ScalpMesh::UV> uvs;
    std::vector<ScalpMesh::Color> colors;
    for (const auto& tri : mesh->triangles()) {
        for (const std::int32_t v : tri) {
            const Vec3f& p = mesh->points()[static_cast<std::size_t>(v)];
            uvs.push_back({0.5f + 0.5f * p.x, 0.5f + 0.5f * p.z});
            colors.push_back({0.5f + 0.5f * p.x, 0.5f + 0.5f * p.y, 0.5f - 0.5f * p.z, 1.0f});
        }
    }
    mesh->setCornerUVs(std::move(uvs));
    mesh->setCornerColors("colorSet1", std::move(colors));
    return mesh;
}

std::string fixtureHierarchy(const MeshPtr& scalp) {
    GuideHierarchy h;
    const GroupId primary = h.addGroup("primary");
    TubeProfile tube;
    tube.width = {0.15f, 0.1f};
    std::vector<GuideId> ids;
    for (int i = 0; i < 8; ++i) {
        const float a = 6.2831853f * static_cast<float>(i) / 8.0f;  // (pm::: the same fixture on every platform)
        const Vec3f n = Vec3f{pm::cos(a) * 0.6f, 0.8f, pm::sin(a) * 0.6f}.normalized();
        ids.push_back(h.addGuide(primary, test::straightCurve(n, n * 0.8f + Vec3f{0.0f, 0.0f, 0.2f}, 6), tube));
    }
    ChildGenerationParams params;
    params.count = 3;
    h.generateChildren(primary, "children", params);
    h.setAttribute(ids[0], "freeze", 1.0f);
    h.setAttribute(ids[1], "len", 0.7f);
    h.setAttribute(ids[2], "len", 0.2f);
    h.evaluate(scalp.get());
    return h.toJson();
}

std::string fixtureParts(const ScalpMesh& scalp) {
    PartSet parts;
    parts.add("crown", {{0.1f, 1.0f, -0.4f}, {0.15f, 1.0f, 0.0f}, {0.2f, 0.95f, 0.4f}}, scalp);
    parts.add("side", {{-0.7f, 0.7f, -0.2f}, {-0.75f, 0.6f, 0.2f}}, scalp, 8);
    return parts.toJson();
}

std::string fixtureFlow(const ScalpMesh& scalp) {
    FlowSet flow;
    auto pin = [&](const Vec3f& at, const Vec3f& dir, int line, int seq) {
        FlowPin p = FlowSet::makePin(scalp, scalp.closestPoint(at), dir);
        p.line = line;
        p.seq = seq;
        flow.pins.push_back(p);
    };
    pin({0.0f, 0.2f, 1.0f}, {1.0f, 0.0f, 0.0f}, -1, 0);
    pin({-0.5f, 0.8f, 0.2f}, {0.0f, 0.0f, 1.0f}, 0, 0);
    pin({-0.5f, 0.75f, 0.4f}, {0.0f, 0.0f, 1.0f}, 0, 1);
    pin({-0.5f, 0.7f, 0.6f}, {0.0f, -0.3f, 1.0f}, 0, 2);
    pin({-0.5f, 0.5f, 0.8f}, {0.0f, -1.0f, 0.3f}, 0, 4);  // a gap: a run of its own
    FlowSink sink;
    sink.root = scalp.closestPoint({0.6f, 0.6f, 0.5f}).root;
    sink.angle = 70.0f;
    sink.radius = 0.4f;
    flow.sinks.push_back(sink);
    return flow.toJson();
}

// A box (per face-vertex points and normals) for the Instance node's "box".
InstancePrototype box() {
    InstancePrototype mesh;
    const Vec3f axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int a = 0; a < 3; ++a) {
        for (const float sign : {1.0f, -1.0f}) {
            const Vec3f n = axes[a] * sign, u = axes[(a + 1) % 3], v = axes[(a + 2) % 3];
            const auto base = static_cast<std::uint32_t>(mesh.points.size());
            for (const auto& [x, y] : {std::pair{-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}}) {
                mesh.points.push_back(n * 0.5f + u * (0.5f * x) + v * (0.5f * y) + Vec3f{0.0f, 0.5f, 0.0f});
                mesh.normals.push_back(n);
            }
            mesh.triangles.insert(mesh.triangles.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        }
    }
    mesh.signature = 42;
    mesh.found = true;
    return mesh;
}

struct Scenario {
    std::string name;
    DisplaySettings settings;
    bool stroke = false;
    bool roots = false;
    ScalpMapView map;
    bool mapTexture = false;
    bool noPrototypes = false;  // the instances' mesh is not in the host's scene: markers
};

std::string hex(std::uint64_t v) {
    char text[17];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(v));
    return text;
}

json record(const DisplayGeometry& g) {
    return json::array({g.vertexCount(), g.indexCount(), hex(g.digest())});
}

// Compares with the golden record: counts always, digests where the floating point is the one
// recorded (MSVC's math: test::kRecordedMath).
void expectRecorded(const json& expected, const json& actual, const std::string& where) {
    if (expected.is_null() || actual.is_null()) {
        EXPECT_EQ(expected.is_null(), actual.is_null()) << where;
        return;
    }
    if (expected.is_number()) {
        EXPECT_NEAR(expected.get<double>(), actual.get<double>(), 1e-4) << where;
        return;
    }
    ASSERT_EQ(expected.size(), actual.size()) << where;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (expected[i].is_string()) {
            if constexpr (test::kRecordedMath) EXPECT_EQ(expected[i], actual[i]) << where << ": the buffers differ from the recorded ones";
            continue;
        }
        EXPECT_EQ(expected[i], actual[i]) << where;
    }
}

void checkGolden(const std::string& name, const json& actual) {
    const fs::path file = fs::path(GROOMLAB_TEST_DATA) / "golden" / (name + ".json");
    const char* update = std::getenv("GROOMLAB_UPDATE_GOLDEN");
    if (update && std::string(update) == "1") {
        std::ofstream(file, std::ios::binary) << actual.dump(1) << "\n";
        return;
    }
    std::ifstream in(file, std::ios::binary);
    ASSERT_TRUE(in) << "no golden file " << file.generic_string() << " (run with GROOMLAB_UPDATE_GOLDEN=1)";
    const json expected = json::parse(in);
    ASSERT_EQ(expected.size(), actual.size());
    for (const auto& [scenario, items] : expected.items()) {
        ASSERT_TRUE(actual.contains(scenario)) << scenario;
        for (const auto& [item, value] : items.items()) {
            ASSERT_TRUE(actual[scenario].contains(item)) << scenario << "/" << item;
            expectRecorded(value, actual[scenario][item], scenario + "/" + item);
        }
    }
}

template <class T>
json recordBytes(const std::vector<T>& v) {
    return json::array({v.size(), hex(bufferDigest(v.data(), v.size() * sizeof(T)))});
}

// The root normals a host computing the frames on the GPU sends (writeRootNormals) are those the
// CPU's frames lean by: a lone point's tangent is its root normal; equal strands, equal keys.
TEST(Display, LinesRootNormalsAreTheFramesUp) {
    const auto scalp = groomlab::test::makeSphere(8, 16);
    auto strands = std::make_shared<StrandSet>();
    StrandRoot root;
    root.face = 5;
    root.u = root.v = 0.25f;
    strands->addStrand(1, {scalp->position(root)}, {}, root);  // one point: t = up
    strands->addStrand(2, {Vec3f{0, 0, 0}}, {}, StrandRoot{});  // unrooted: up
    strands->addStrand(3, {Vec3f{0, 0, 0}, Vec3f{0, 1, 0}, Vec3f{0, 2, 0.5f}}, {}, root);
    const StrandLines lines(strands, scalp, true, nullptr);
    std::vector<float> up(12), normals(15), tangents(15);
    const std::uint64_t key = lines.writeRootNormals(up.data());
    lines.writeFrames(normals.data(), tangents.data());
    const Vec3f n = scalp->normal(root);
    EXPECT_EQ((std::vector<float>{up[0], up[1], up[2], up[3]}), (std::vector<float>{n.x, n.y, n.z, 0.0f}));
    EXPECT_EQ((std::vector<float>{tangents[0], tangents[1], tangents[2]}), (std::vector<float>{n.x, n.y, n.z}));
    EXPECT_EQ((std::vector<float>{up[4], up[5], up[6]}), (std::vector<float>{0.0f, 1.0f, 0.0f}));
    std::vector<float> again(12);
    EXPECT_EQ(StrandLines(std::make_shared<StrandSet>(*strands), scalp, true, nullptr).writeRootNormals(again.data()), key);
    EXPECT_EQ(again, up);
    EXPECT_NE(key, 0u);
}

// Width as coverage: a pixel's share is the width projected to pixels (perspective: by the view
// depth; orthographic: clip w 1), clamped to [floor, 1]; no width (or behind the eye) is opaque.
TEST(Display, LineCoverageIsTheWidthInPixels) {
    // A 54.43 degree vertical field of view on a 1000-pixel-high viewport: P11 = 1 / tan(27.215).
    const float p11 = 1.0f / std::tan(27.215f * 3.14159265f / 180.0f);
    const float scale = linePixelScale(p11, 1000.0f);
    EXPECT_NEAR(scale, 0.5f * 1000.0f * p11, 1e-3f);
    // 1000 px / (2 tan(fov/2) d) per unit: at d = 100, a 0.1 wide strand is 0.97 px.
    const float expected = 0.1f * 1000.0f / (2.0f * std::tan(27.215f * 3.14159265f / 180.0f) * 100.0f);
    EXPECT_NEAR(lineCoverage(0.1f, 1.0f, scale, 100.0f), expected, 1e-4f);
    EXPECT_NEAR(lineCoverage(0.05f, 2.0f, scale, 100.0f), expected, 1e-4f);  // the scale multiplies
    EXPECT_NEAR(lineCoverage(0.1f, 1.0f, scale, 200.0f), expected * 0.5f, 1e-4f);  // twice as far: half
    EXPECT_EQ(lineCoverage(1.0f, 1.0f, scale, 100.0f), 1.0f);  // wider than a pixel: solid
    EXPECT_EQ(lineCoverage(0.0f, 1.0f, scale, 100.0f), kLineCoverageFloor);  // a tip of width 0
    EXPECT_EQ(lineCoverage(kNoLineWidth, 1.0f, scale, 100.0f), 1.0f);
    EXPECT_EQ(lineCoverage(0.01f, 1.0f, scale, 0.0f), 1.0f);
    EXPECT_EQ(lineCoverage(0.0f, 1.0f, scale, 1.0f, 0.25f), 0.25f);
    // Orthographic, 20 units high: P11 = 2 / 20; 50 px a unit on 1000 px, at any depth (w 1).
    EXPECT_NEAR(lineCoverage(0.01f, 1.0f, linePixelScale(0.1f, 1000.0f), 1.0f), 0.5f, 1e-5f);
}

// A thinned preview's strand covers what the 1 / density strands it stands for would together:
// 1 - (1 - c)^(1 / density), never more than the pixel; the coverage itself at density 1.
TEST(Display, LineCoverageAtDensityStandsForTheThinnedStrands) {
    EXPECT_EQ(lineCoverageAtDensity(0.3f, 1.0f), 0.3f);
    EXPECT_EQ(lineCoverageAtDensity(0.3f, 0.0f), 0.3f);  // unknown: as it is
    EXPECT_NEAR(lineCoverageAtDensity(0.05f, 0.1f), 1.0f - std::pow(0.95f, 10.0f), 1e-6f);
    EXPECT_NEAR(lineCoverageAtDensity(0.2f, 0.5f), 1.0f - 0.8f * 0.8f, 1e-6f);
    EXPECT_EQ(lineCoverageAtDensity(1.0f, 0.1f), 1.0f);
    float last = 0.0f;
    for (float c = 0.05f; c <= 1.0f; c += 0.05f) {
        for (float d : {0.01f, 0.1f, 0.35f}) {
            const float k = lineCoverageAtDensity(c, d);
            EXPECT_GE(k, c);
            EXPECT_LE(k, 1.0f);
        }
        EXPECT_GE(lineCoverageAtDensity(c, 0.1f), last);  // more coverage, more of it
        last = lineCoverageAtDensity(c, 0.1f);
    }
}

// The hashed coverage: thresholds uniform over strands and stretches, the same for the same
// fragment, a pixel's strands kept on their own (n of them cover 1 - (1 - c)^n of it, alone or
// over a multisampled pixel's samples), few kept fragments changing as the view zooms a little.
TEST(Display, LineHashedCoverageAccumulatesAndHolds) {
    // Uniform: ten bins over 40000 fragments (strands x stretches, multisampled), each within 5% of a tenth.
    std::array<int, 10> bins{};
    const int count = 40000;
    for (int i = 0; i < count; ++i) {
        const float t = lineHashThreshold(static_cast<std::uint32_t>(i % kLineIdCount), (static_cast<float>(i / kLineIdCount) + 0.5f) / 64.0f,
                                          1.0f / 90.0f, 8);  // between two powers of two: a blend
        ASSERT_GT(t, 0.0f);
        ASSERT_LE(t, 1.0f);
        ++bins[std::min(9, static_cast<int>(t * 10.0f))];
    }
    for (int b : bins) EXPECT_NEAR(b, count / 10, count / 200) << "a bin of the thresholds";
    // The same fragment, the same threshold; a stretch's fragments share theirs.
    EXPECT_EQ(lineHashThreshold(17, 0.4f, 1.0f / 64.0f, 8), lineHashThreshold(17, 0.4f, 1.0f / 64.0f, 8));
    EXPECT_EQ(lineHashThreshold(17, 0.4f, 1.0f / 64.0f, 8), lineHashThreshold(17, 0.4f + 0.2f / 64.0f, 1.0f / 64.0f, 8));
    EXPECT_NE(lineHashThreshold(17, 0.4f, 1.0f / 64.0f, 8), lineHashThreshold(18, 0.4f, 1.0f / 64.0f, 8));
    // A pixel crossed by n strands (their numbers and places along them at random) of coverage c:
    // its covered share over many such pixels, kept or not (one sample) or by 8 samples.
    std::uint64_t seed = 1;
    auto next = [&seed]() {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<float>(seed >> 40) / static_cast<float>(1 << 24);
    };
    for (const auto& [n, c] : {std::pair<int, float>{1, 0.3f}, {8, 0.1f}, {30, 0.05f}, {60, 0.12f}}) {
        double single = 0.0, sampled = 0.0;
        const int pixels = 4000;
        for (int p = 0; p < pixels; ++p) {
            bool any = false;
            std::uint32_t mask = 0;
            for (int s = 0; s < n; ++s) {
                const auto strand = static_cast<std::uint32_t>(next() * kLineIdCount);
                const float u = next(), uPerPixel = 1.0f / (40.0f + 100.0f * next());
                any = any || lineSampleMask(c, lineHashThreshold(strand, u, uPerPixel, 1), strand, 1) != 0u;
                mask |= lineSampleMask(c, lineHashThreshold(strand, u, uPerPixel, 8), strand, 8);
            }
            single += any ? 1.0 : 0.0;
            sampled += std::bitset<32>(mask).count() / 8.0;
        }
        const double expected = 1.0 - std::pow(1.0 - c, n);
        EXPECT_NEAR(single / pixels, expected, 0.03) << n << " strands of coverage " << c << ", one sample";
        EXPECT_NEAR(sampled / pixels, expected, 0.03) << n << " strands of coverage " << c << ", 8 samples";
    }
    // A strand's samples: coverage x samples of them on average, every one at coverage 1, none
    // below its threshold with one sample.
    double samples = 0.0;
    for (int i = 0; i < 4000; ++i) {
        const auto strand = static_cast<std::uint32_t>(i % kLineIdCount);
        samples += std::bitset<32>(lineSampleMask(0.3f, lineHashThreshold(strand, next(), 0.01f, 8), strand, 8)).count();
    }
    EXPECT_NEAR(samples / 4000.0, 0.3 * 8.0, 0.1);
    EXPECT_EQ(lineSampleMask(1.0f, 1.0f, 5, 8), 0xFFu);
    EXPECT_EQ(lineSampleMask(1.0f, 1.0f, 5, 1), 1u);
    EXPECT_EQ(lineSampleMask(0.2f, 0.3f, 5, 1), 0u);
    EXPECT_EQ(lineSampleMask(0.31f, 0.3f, 5, 1), 1u);
    // One sample a pixel: the coverage itself kept (hashed, opaque) up to kLineHashedBelow, every
    // fragment kept (blended at its coverage) from kLineBlendedAbove, smoothly between; the share
    // times the alpha it is drawn with (coverage / share) is the coverage.
    EXPECT_EQ(lineKeepShare(0.05f, 0.05f), 0.05f);
    EXPECT_EQ(lineKeepShare(kLineHashedBelow, kLineHashedBelow), kLineHashedBelow);
    EXPECT_EQ(lineKeepShare(kLineBlendedAbove, kLineBlendedAbove), 1.0f);
    EXPECT_EQ(lineKeepShare(1.0f, 1.0f), 1.0f);
    float share = 0.0f;
    for (float c = 0.01f; c <= 1.0f; c += 0.01f) {
        EXPECT_GE(lineKeepShare(c, c), c);
        EXPECT_GE(lineKeepShare(c, c), share);  // more coverage, more kept
        share = lineKeepShare(c, c);
        EXPECT_NEAR(share * (c / share), c, 1e-6f);
    }
    // A thinned preview from afar: its strands' own coverage low, the density's taking it up: hashed.
    const float thinned = lineCoverageAtDensity(0.05f, 0.25f);
    EXPECT_GT(thinned, kLineHashedBelow);
    EXPECT_EQ(lineKeepShare(thinned, 0.05f), thinned);
    // Zooming in by 1%: few of the kept fragments of coverage 0.5 change (the pattern holds).
    int flipped = 0;
    for (int i = 0; i < count; ++i) {
        const auto strand = static_cast<std::uint32_t>(i % kLineIdCount);
        const float u = (static_cast<float>(i / kLineIdCount) + 0.5f) / 40.0f;
        const bool before = lineHashThreshold(strand, u, 1.0f / 100.0f, 8) <= 0.5f;
        const bool after = lineHashThreshold(strand, u, 1.0f / 101.0f, 8) <= 0.5f;
        flipped += before != after ? 1 : 0;
    }
    EXPECT_LT(flipped, count / 50);
}

// One sample a pixel: stretches of about four pixels (short dashes, not scattered dots; not whole
// strands, which strands sharing a number would keep or drop together): the threshold a
// multisampled target takes at a quarter of the pixels a unit of u spans, the same over a
// stretch's four pixels, changing from stretch to stretch.
TEST(Display, LineHashThresholdStretchesFourPixelsOnOneSample) {
    for (std::uint32_t strand : {0u, 5u, 17u, 1023u}) {
        for (float u : {0.1f, 0.37f, 0.9f}) {
            for (float uPerPixel : {1.0f / 7.0f, 1.0f / 90.0f, 1.0f / 3000.0f}) {
                EXPECT_NEAR(lineHashThreshold(strand, u, uPerPixel, 1), lineHashThreshold(strand, u, 4.0f * uPerPixel, 8), 1e-5f) << strand;
                EXPECT_EQ(lineHashThreshold(strand, u, uPerPixel, 0), lineHashThreshold(strand, u, uPerPixel, 1));  // (0: one too)
            }
        }
    }
    // At 64 pixels a unit of u (a power of two: no blend), 16 stretches of four pixels.
    int differ = 0;
    for (int i = 0; i < 64; ++i) {
        const float u = (static_cast<float>(i) + 0.5f) / 64.0f;
        const float t = lineHashThreshold(17, u, 1.0f / 64.0f, 1);
        if (i % 4 != 0) EXPECT_EQ(t, lineHashThreshold(17, (static_cast<float>(i - i % 4) + 0.5f) / 64.0f, 1.0f / 64.0f, 1)) << i;
        else if (i > 0) differ += t != lineHashThreshold(17, (static_cast<float>(i - 4) + 0.5f) / 64.0f, 1.0f / 64.0f, 1) ? 1 : 0;
    }
    EXPECT_GE(differ, 12);
}

TEST(Display, ParsesGroupColorsAndHiddenGroups) {
    const auto colors = parseGuideGroupColors("a:#ff0000 b:#00ff80 bad:#12 c:#zz0000 d#ffffff");
    ASSERT_EQ(colors.size(), 2u);
    EXPECT_FLOAT_EQ(colors.at("a")[0], 1.0f);
    EXPECT_FLOAT_EQ(colors.at("b")[1], 1.0f);
    EXPECT_NEAR(colors.at("b")[2], 0.2158605f, 1e-6f);  // sRGB 0x80 in linear
    EXPECT_EQ(parseHiddenGuideGroups(" a,b  c,"), (std::set<std::string>{"a", "b", "c"}));
}

TEST(Display, GuideColorHexIsTheParsersReverse) {
    EXPECT_EQ(guideColorHex({1.0f, 0.0f, 0.2158605f}), "#ff0080");
    EXPECT_EQ(guideColorHex({-1.0f, 2.0f, 0.0f}), "#00ff00");  // clamped
    for (const std::string hex : {"#3d7eff", "#000000", "#ffffff", "#808080", "#12ab9c"}) {
        EXPECT_EQ(guideColorHex(parseGuideGroupColors("g:" + hex).at("g")), hex);
    }
}

// The smallest hue distance (around the circle) between two automatic colors.
float hueApart(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    auto hue = [](const std::array<float, 3>& c) {
        const float hi = std::max({c[0], c[1], c[2]}), lo = std::min({c[0], c[1], c[2]}), range = hi - lo;
        float h = hi == c[0] ? (c[1] - c[2]) / range : hi == c[1] ? 2.0f + (c[2] - c[0]) / range : 4.0f + (c[0] - c[1]) / range;
        h /= 6.0f;
        return h - std::floor(h);
    };
    const float d = std::abs(hue(a) - hue(b));
    return std::min(d, 1.0f - d);
}

// Guides were colored by level unless asked: a groom's child group shared every other child
// group's color. Now every group, child groups too, is its own hue: a parent and its children
// and siblings well apart, however many groups were made between them.
TEST(Display, EveryGuideGroupHasItsOwnColor) {
    GuideHierarchy h;
    const GroupId primary = h.addGroup("primary");
    const GroupId children = h.addGroup("children", primary);
    const GroupId bangs = h.addGroup("bangs");
    const GroupId bangKids = h.addGroup("bangKids", bangs);
    std::vector<GroupId> filler;  // groups made between a parent and its next child
    for (int i = 0; i < 4; ++i) filler.push_back(h.addGroup("filler" + std::to_string(i)));
    // 8 ids after their parents: golden-ratio steps alone would land 0.056 from the parent's hue.
    const GroupId late = h.addGroup("late", primary);
    const GroupId grandchild = h.addGroup("grandchild", children);
    ASSERT_EQ(late, primary + 8);
    ASSERT_EQ(grandchild, children + 8);
    for (const float seed : {0.0f, guideHueSeed("hair"), guideHueSeed("brows"), guideHueSeed("groom1"), 0.37f}) {
        const auto colors = autoGuideColors(h, seed);
        ASSERT_EQ(colors.size(), h.groups().size());
        for (const auto& [id, c] : colors) {
            EXPECT_NEAR(std::max({c[0], c[1], c[2]}), kGuideValue, 1e-5f);  // soft but colored, never gray
            EXPECT_NEAR(std::max({c[0], c[1], c[2]}) - std::min({c[0], c[1], c[2]}), kGuideValue * kGuideSaturation, 1e-5f);
        }
        auto apart = [&](GroupId a, GroupId b) { return hueApart(colors.at(a), colors.at(b)); };
        // Parents and their children.
        for (const auto& [parent, child] : std::vector<std::pair<GroupId, GroupId>>{
                 {primary, children}, {primary, late}, {bangs, bangKids}, {children, grandchild}}) {
            EXPECT_GE(apart(parent, child), 0.12f) << "seed " << seed << ": group " << child << " near its parent";
        }
        // Siblings.
        EXPECT_GE(apart(children, late), 0.12f) << seed;
        EXPECT_GE(apart(primary, bangs), 0.12f) << seed;
        // The common case, the first group and its first child: as far apart as a golden step.
        EXPECT_GE(apart(primary, children), 0.3f) << seed;
    }
    // Deterministic, and a new group leaves the others' colors as they were.
    const auto before = autoGuideColors(h, 0.25f);
    EXPECT_EQ(before, autoGuideColors(h, 0.25f));
    h.addGroup("newest", bangs);
    const auto after = autoGuideColors(h, 0.25f);
    for (const auto& [id, c] : before) EXPECT_EQ(after.at(id), c) << id;
    // The seed turns every hue: two grooms' first groups differ.
    EXPECT_GE(hueApart(autoGuideColors(h, guideHueSeed("groom1")).at(primary), autoGuideColors(h, guideHueSeed("groom2")).at(primary)), 0.1f);
}

}  // namespace
}  // namespace groomlab
