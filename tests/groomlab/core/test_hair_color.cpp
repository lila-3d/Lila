// SPDX-License-Identifier: Apache-2.0
// Modified for Lila (2026).
// HairColor, param by param: every param changes what the node writes (the RGB, per strand and per
// point, the melanin, the highlight channels) and what the hosts read of the look
// (GroomSession::hairLook); hue, saturation, melanin and value randomness vary each strand in both
// modes, shared per clump by clumpShare; the hair colors' model (melaninColor is the color whose
// absorption is the pigment's, varyHairColor's HSL turns).

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "groomlab/display.h"
#include "groomlab/export_view.h"
#include "groomlab/groom.h"
#include "groomlab/hair_color.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

using Rgb = std::array<float, 3>;
using nlohmann::json;

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

// Scalp -> Scatter -> Length -> Clump (12 clumps, not pulling) -> HairColor(params) -> Output.
GroomData colorGroom(const json& params, bool clumped = true) {
    const std::string upstream = clumped ? "clump" : "length";
    const json graph = {
        {"version", 1},
        {"output", "out"},
        {"nodes",
         {{{"name", "scalp"}, {"type", "Scalp"}},
          {{"name", "scatter"}, {"type", "Scatter"}, {"params", {{"density", 500.0}}}},
          {{"name", "length"}, {"type", "Length"}, {"params", {{"keep", 1.0}}}},
          {{"name", "clump"}, {"type", "Clump"}, {"params", {{"count", 12}, {"strength", 0.0}}}},
          {{"name", "color"}, {"type", "HairColor"}, {"params", params}},
          {{"name", "resolution"}, {"type", "Resample"}, {"params", {{"segments", 6}}}},
          {{"name", "out"}, {"type", "Output"}}}},
        {"connections",
         {{{"from", "scalp"}, {"to", "scatter"}, {"input", "scalp"}},
          {{"from", "scatter"}, {"to", "length"}, {"input", "strands"}},
          {{"from", "length"}, {"to", "clump"}, {"input", "strands"}},
          {{"from", upstream}, {"to", "color"}, {"input", "strands"}},
          {{"from", "color"}, {"to", "out"}, {"input", "strands"}}}}};
    GroomData d;
    d.scalp = tintedPlane();
    d.graphJson = graph.dump();
    return d;
}

Rgb strandColor(const StrandSet& s, std::size_t i) {
    return {(*s.strandChannel("colorR"))[i], (*s.strandChannel("colorG"))[i], (*s.strandChannel("colorB"))[i]};
}

// Everything HairColor writes, flattened: the RGB per strand and per point, the melanin and
// redness, the highlight channels (a channel's absence counts as a difference against its
// presence).
std::map<std::string, std::vector<float>> written(const StrandSet& s) {
    std::map<std::string, std::vector<float>> out;
    for (const char* name : {"colorR", "colorG", "colorB", kMelaninChannel, kMelaninRednessChannel, kSpecularChannel,
                             kRoughnessChannel, kTiltChannel}) {
        if (const std::vector<float>* v = s.strandChannel(name)) out[std::string("strand.") + name] = *v;
    }
    for (const char* name : {"colorR", "colorG", "colorB"}) {
        if (const std::vector<float>* v = s.pointChannel(name)) out[std::string("point.") + name] = *v;
    }
    return out;
}

// The largest change between two results, relative to the values (dark hair colors are small
// numbers); 1 for a channel only one has.
float change(const StrandSet& a, const StrandSet& b) {
    const auto x = written(a), y = written(b);
    float worst = 0.0f;
    for (const auto& [name, values] : x) {
        const auto it = y.find(name);
        if (it == y.end() || it->second.size() != values.size()) return 1.0f;
        for (std::size_t i = 0; i < values.size(); ++i) {
            const float scale = std::max({std::abs(values[i]), std::abs(it->second[i]), 1e-3f});
            worst = std::max(worst, std::abs(values[i] - it->second[i]) / scale);
        }
    }
    for (const auto& [name, values] : y) {
        if (!x.count(name)) return 1.0f;
    }
    return worst;
}

// HSL hue (turns) and saturation of a color.
std::array<float, 2> hueSaturation(const Rgb& c) {
    const float hi = std::max({c[0], c[1], c[2]}), lo = std::min({c[0], c[1], c[2]}), d = hi - lo;
    if (d < 1e-7f) return {0.0f, 0.0f};
    float h;
    if (hi == c[0]) h = (c[1] - c[2]) / d;
    else if (hi == c[1]) h = 2.0f + (c[2] - c[0]) / d;
    else h = 4.0f + (c[0] - c[1]) / d;
    h /= 6.0f;
    if (h < 0.0f) h += 1.0f;
    const float l = 0.5f * (hi + lo);
    return {h, d / std::max(1.0f - std::abs(2.0f * l - 1.0f), 1e-7f)};
}

// ---- The color model ----------------------------------------------------------------------------

// A melanin's color is the hair color whose absorption (Chiang's mapping: Arnold's base color, the
// viewport's hairColorSigma) is the pigment's: so the RGB renders as aiStandardHair's melanin.
TEST(HairColorModel, AMelaninsColorAbsorbsAsThePigment) {
    for (const float m : {0.0f, 0.1f, 0.35f, 0.6f, 0.9f, 1.0f}) {
        for (const float r : {0.0f, 0.3f, 1.0f}) {
            const Rgb sigma = hairMelaninAbsorption(m, r), back = hairColorAbsorption(melaninColor(m, r));
            for (int k = 0; k < 3; ++k) EXPECT_NEAR(back[k], sigma[k], 1e-3f * std::max(1.0f, sigma[k])) << m << " " << r << " " << k;
        }
    }
    EXPECT_EQ(melaninColor(0.0f, 0.5f), (Rgb{1.0f, 1.0f, 1.0f}));  // no pigment: white
    const Rgb brown = melaninColor(0.35f, 0.2f), dyed = melaninColor(0.35f, 0.2f, {1.0f, 0.5f, 0.25f});
    // The dye's absorption adds to the pigment's (aiStandardHair's melanin and base color).
    const Rgb sum = hairColorAbsorption(dyed), pigment = hairMelaninAbsorption(0.35f, 0.2f),
              tint = hairColorAbsorption({1.0f, 0.5f, 0.25f});
    for (int k = 0; k < 3; ++k) EXPECT_NEAR(sum[k], pigment[k] + tint[k], 1e-3f * sum[k]);
    EXPECT_FLOAT_EQ(dyed[0], brown[0]);
    EXPECT_LT(dyed[1], brown[1]);
    EXPECT_EQ(melaninColor(0.0f, 0.3f, {0.6f, 0.3f, 0.15f})[1], dyedHairColor({1.0f, 1.0f, 1.0f}, {0.6f, 0.3f, 0.15f})[1]);
    EXPECT_GT(brown[0], brown[1]);
    EXPECT_GT(brown[1], brown[2]);
    EXPECT_LT(melaninColor(0.9f, 0.2f)[0], melaninColor(0.3f, 0.2f)[0]);  // more melanin: darker
    EXPECT_GT(melaninColor(0.4f, 1.0f)[0] / melaninColor(0.4f, 1.0f)[1], brown[0] / brown[1]);  // redness: redder
}

// The base color a hair BSDF takes against the appearance: what the hair looks like (measured with
// kick), for what stores or draws a color without a hair BSDF. Medium brown looks a medium brown,
// blond light, black near black with its sheen, white white; the inverse gives the base color back.
TEST(HairColorModel, TheAppearanceIsWhatTheHairLooksLike) {
    auto srgb = [](float c) { return c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; };
    const Rgb brown = melaninColor(0.35f, 0.2f), shown = hairAppearanceColor(brown);
    EXPECT_LT(brown[0], 0.06f);  // the base color is dark
    // A medium brown on screen (sRGB about 145, 108, 66).
    EXPECT_NEAR(255.0f * srgb(shown[0]), 145.0f, 15.0f);
    EXPECT_NEAR(255.0f * srgb(shown[1]), 108.0f, 15.0f);
    EXPECT_NEAR(255.0f * srgb(shown[2]), 66.0f, 15.0f);
    EXPECT_GT(shown[0], shown[1]);
    EXPECT_GT(shown[1], shown[2]);
    const Rgb blond = hairAppearanceColor(melaninColor(0.15f, 0.2f)), black = hairAppearanceColor(melaninColor(1.0f, 0.1f));
    EXPECT_GT(blond[2], 0.3f);
    EXPECT_LT(black[0], 0.05f);
    EXPECT_GE(black[0], kAppearanceSheen - 1e-6f);
    EXPECT_NEAR(hairAppearanceColor({1.0f, 1.0f, 1.0f})[1], 1.0f, 1e-5f);
    for (const Rgb& c : {brown, melaninColor(0.15f, 0.2f), Rgb{0.6f, 0.35f, 0.2f}, Rgb{0.9f, 0.9f, 0.9f}}) {
        const Rgb back = hairBaseOfAppearance(hairAppearanceColor(c));
        for (int k = 0; k < 3; ++k) EXPECT_NEAR(back[k], c[k], 2e-3f * std::max(c[k], 0.05f)) << k;
    }
    // Lighter than the base color, the order of colors kept.
    for (const float c : {0.001f, 0.01f, 0.1f, 0.5f}) {
        EXPECT_GT(hairAppearanceColor({c, c, c})[0], c);
        EXPECT_LT(hairAppearanceColor({c, c, c})[0], hairAppearanceColor({2.0f * c, 0.0f, 0.0f})[0]);
    }
}

// HSL turns, as PxrHairColor varies its dye: the hue turns with the lightness kept, the
// saturation scales, grey and white stay neutral, the value scales it all.
TEST(HairColorModel, VaryHairColorTurnsTheHueAndScalesTheSaturationInHsl) {
    const Rgb c{0.6f, 0.3f, 0.1f};
    const Rgb turned = varyHairColor(c, 0.25f, 1.0f, 1.0f);
    EXPECT_NEAR(hueSaturation(turned)[0], hueSaturation(c)[0] + 0.25f, 1e-4f);
    EXPECT_NEAR(std::max({turned[0], turned[1], turned[2]}) + std::min({turned[0], turned[1], turned[2]}), 0.7f, 1e-5f);
    const Rgb flat = varyHairColor(c, 0.0f, 0.5f, 1.0f);
    EXPECT_NEAR(hueSaturation(flat)[1], 0.5f * hueSaturation(c)[1], 1e-4f);
    EXPECT_NEAR(hueSaturation(flat)[0], hueSaturation(c)[0], 1e-4f);
    for (const Rgb& neutral : {Rgb{1.0f, 1.0f, 1.0f}, Rgb{0.4f, 0.4f, 0.4f}}) {
        EXPECT_EQ(varyHairColor(neutral, 0.3f, 1.8f, 1.0f), neutral);
    }
    const Rgb darker = varyHairColor(c, 0.0f, 1.0f, 0.5f);
    for (int k = 0; k < 3; ++k) EXPECT_FLOAT_EQ(darker[k], c[k] * 0.5f);
    EXPECT_EQ(varyHairColor(c, 0.0f, 1.0f, 1.0f), c);
    EXPECT_EQ(varyHairColor(c, 1.0f, 1.0f, 1.0f)[0], varyHairColor(c, 0.0f, 1.0f, 1.0f)[0]);  // a whole turn
}

// ---- Every param --------------------------------------------------------------------------------

// ---- Randomness ---------------------------------------------------------------------------------

}  // namespace
}  // namespace groomlab
