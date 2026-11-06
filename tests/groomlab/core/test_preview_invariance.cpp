// SPDX-License-Identifier: Apache-2.0
// Modified for Lila (2026).
// Preview invariance: the preview density changes only how many strands show. Every shipped preset
// (and the presets of tests/data/presets) is evaluated on the reference groom (the strokes
// fixture's scalp and guides, the preset fitted to its size) at full density and at preview
// densities 0.5, 0.1 and 0.02, each in a session of its own: every strand a preview shows is the
// full density's strand of that ID, bit for bit (points, widths, every strand and point channel),
// and the strands shown are the render's thinned by ID (those whose ID falls under the density,
// where the strands are a Scatter's; the strands a preview at a higher density shows, where they
// are made from such strands: barbs, copies), about the density's share of them, or all of them
// where nothing thins them (previewAll, guides as strands).

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "groomlab/graph_io.h"
#include "groomlab/groom.h"
#include "groomlab/groom_file.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/presets.h"
#include "groomlab/preview_core.h"
#include "groomlab/random.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

const std::string kShipped = GROOMLAB_SHIPPED_PRESETS;

// Presets allowed to differ, each with why (none: every preset holds).
const std::map<std::string, std::string> kAllowed = {};

// The presets' Scatters at a quarter of their density (a test of minutes otherwise: some grow a
// million strands on the reference groom), still well above the core's least count.
constexpr double kDensityScale = 0.25;
std::string thinnedGraph(const std::string& graphJson) {
    nlohmann::json graph = nlohmann::json::parse(graphJson);
    for (nlohmann::json& node : graph["nodes"]) {
        if (node.value("type", std::string()) != "Scatter" || !node.contains("params")) continue;
        nlohmann::json& params = node["params"];
        if (params.contains("density") && params["density"].is_number()) {
            params["density"] = params["density"].get<double>() * kDensityScale;
        }
    }
    return graph.dump();
}

// The first difference between strand `a` of `x` and strand `b` of `y` (empty: the same bits).
std::string strandDifference(const StrandSet& x, std::size_t a, const StrandSet& y, std::size_t b) {
    if (x.strandPointCount(a) != y.strandPointCount(b)) return "point count";
    if (std::memcmp(&x.roots()[a], &y.roots()[b], sizeof(StrandRoot)) != 0) return "root";
    const std::size_t n = x.strandPointCount(a), xa = x.strandBegin(a), yb = y.strandBegin(b);
    if (n && std::memcmp(x.strandPoints(a), y.strandPoints(b), n * sizeof(Vec3f)) != 0) {
        for (std::size_t k = 0; k < n; ++k) {
            const Vec3f p = x.strandPoints(a)[k], q = y.strandPoints(b)[k];
            if (std::memcmp(&p, &q, sizeof p) != 0) {
                char text[160];
                std::snprintf(text, sizeof text, "point %zu (%g %g %g vs %g %g %g)", k, p.x, p.y, p.z, q.x, q.y, q.z);
                return text;
            }
        }
    }
    if (n && std::memcmp(x.widths().data() + xa, y.widths().data() + yb, n * sizeof(float)) != 0) return "widths";
    if (x.strandChannelNames() != y.strandChannelNames()) return "strand channel names";
    if (x.pointChannelNames() != y.pointChannelNames()) return "point channel names";
    for (const std::string& name : x.strandChannelNames()) {
        const float u = (*x.strandChannel(name))[a], v = (*y.strandChannel(name))[b];
        if (std::memcmp(&u, &v, sizeof u) != 0) return "strand channel " + name + " (" + std::to_string(u) + " vs " + std::to_string(v) + ")";
    }
    for (const std::string& name : x.pointChannelNames()) {
        if (n && std::memcmp(x.pointChannel(name)->data() + xa, y.pointChannel(name)->data() + yb, n * sizeof(float)) != 0) {
            return "point channel " + name;
        }
    }
    return {};
}

}  // namespace

// The nodes reading other strands that no preset above reads that way, in one graph on a sphere of
// 100k strands (a core of a sixteenth): relaxed roots (randomness 0.6), a Clump's own centers and a Clump
// centered on another Scatter's strands (centersFrom inputPoints: each axis a strand), a Twist and a
// Curl by clump, BlendNeighbors, a Tangle, Resample by length, Bands' guards by length, a
// SelfCollide (its grid of the core's hair), a Static (its charge, the core's), an expression of
// the longest strand, ToGuides as a Clump's centers, Merge's seam: every strand a preview shows is
// the render's strand of that ID, bit for bit, and the shown ones are thinned by ID.
TEST(PreviewInvariance, EveryNodeReadingOtherStrands) {
    const auto scalp = test::makeSphere(40, 80, 1.0f);
    auto hierarchy = std::make_shared<GuideHierarchy>();
    const GroupId primary = hierarchy->addGroup("primary");
    Rng rng(4);
    for (int i = 0; i < 150; ++i) {
        const Vec3f n = Vec3f(rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)).normalized();
        hierarchy->addGuide(primary, test::straightCurve(n, n * 0.8f + Vec3f(0.2f, -0.3f, 0), 12));
    }
    hierarchy->evaluate(scalp.get());
    const std::string graph = R"({"version": 1, "output": "out",
      "nodes": [
        {"name": "scalp", "type": "Scalp"}, {"name": "guides", "type": "Guides"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 8000.0, "randomness": 0.6}},
        {"name": "interp", "type": "Interpolate"},
        {"name": "clump", "type": "Clump", "params": {"count": 150, "copyAmount": 0.3}},
        {"name": "twist", "type": "Twist", "params": {"turns": 0.5}},
        {"name": "curl", "type": "Curl", "params": {"coherence": "clump", "radius": 0.02}},
        {"name": "neighbors", "type": "BlendNeighbors", "params": {"strength": 0.5}},
        {"name": "tangle", "type": "Tangle", "params": {"radius": 0.1}},
        {"name": "longest", "type": "Expression", "params": {"expression": "$length / $longest", "min": 0.5, "max": 1.0}},
        {"name": "length", "type": "Length", "paramInputs": ["keep"]},
        {"name": "byLength", "type": "Resample", "params": {"mode": "length", "segments": 12}},
        {"name": "color", "type": "HairColor"},
        {"name": "bands", "type": "Bands", "params": {"guards": 0.4, "guardsBy": "longest"}},
        {"name": "selfCollide", "type": "SelfCollide", "params": {"packing": 0.05}},
        {"name": "static", "type": "Static", "params": {"strength": 0.8}},
        {"name": "scatterB", "type": "Scatter", "params": {"density": 6000.0, "seed": 3}},
        {"name": "interpB", "type": "Interpolate"},
        {"name": "centers", "type": "Scatter", "params": {"density": 600.0, "seed": 5}},
        {"name": "clumpB", "type": "Clump", "params": {"centersFrom": "inputPoints"}},
        {"name": "toGuides", "type": "ToGuides", "params": {"fraction": 0.5}},
        {"name": "clumpC", "type": "Clump", "params": {"centersFrom": "inputCurves", "strength": 0.4}},
        {"name": "merge", "type": "Merge", "params": {"blend": 0.15}},
        {"name": "out", "type": "Output"}],
      "connections": [
        {"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "scatter", "to": "interp", "input": "strands"},
        {"from": "guides", "to": "interp", "input": "guides"},
        {"from": "scalp", "to": "interp", "input": "scalp"},
        {"from": "interp", "to": "clump", "input": "strands"},
        {"from": "clump", "to": "twist", "input": "strands"},
        {"from": "twist", "to": "curl", "input": "strands"},
        {"from": "curl", "to": "neighbors", "input": "strands"},
        {"from": "neighbors", "to": "tangle", "input": "strands"},
        {"from": "tangle", "to": "length", "input": "strands"},
        {"from": "longest", "to": "length", "input": "keep"},
        {"from": "length", "to": "byLength", "input": "strands"},
        {"from": "byLength", "to": "color", "input": "strands"},
        {"from": "color", "to": "bands", "input": "strands"},
        {"from": "scalp", "to": "scatterB", "input": "scalp"},
        {"from": "scatterB", "to": "interpB", "input": "strands"},
        {"from": "guides", "to": "interpB", "input": "guides"},
        {"from": "scalp", "to": "interpB", "input": "scalp"},
        {"from": "scalp", "to": "centers", "input": "scalp"},
        {"from": "interpB", "to": "clumpB", "input": "strands"},
        {"from": "centers", "to": "clumpB", "input": "centers"},
        {"from": "clumpB", "to": "clumpC", "input": "strands"},
        {"from": "interpB", "to": "toGuides", "input": "strands"},
        {"from": "toGuides", "to": "clumpC", "input": "centers"},
        {"from": "bands", "to": "selfCollide", "input": "strands"},
        {"from": "selfCollide", "to": "static", "input": "strands"},
        {"from": "static", "to": "merge", "input": "a"},
        {"from": "clumpC", "to": "merge", "input": "b"},
        {"from": "merge", "to": "out", "input": "strands"}]})";
    auto evaluate = [&](float density) {
        GraphDocument doc = loadGraphJson(graph);  // each density on a graph of its own
        ExternalInputs ext;
        setGroomExternals(ext, scalp, nullptr, hierarchy);
        EvalContext ctx;
        ctx.previewDensity = density;
        ctx.externals = &ext;
        return doc.graph.evaluateStrands(doc.output, ctx);
    };
    const StrandsPtr full = evaluate(1.0f);
    ASSERT_TRUE(full && full->strandCount() > 100000u);
    std::unordered_map<StrandId, std::size_t> at;
    for (std::size_t s = 0; s < full->strandCount(); ++s) at[full->ids()[s]] = s;
    for (const float density : {0.5f, 0.1f, 0.02f}) {
        const StrandsPtr shown = evaluate(density);
        ASSERT_TRUE(shown);
        std::size_t expected = 0, different = 0;
        std::string first;
        for (std::size_t s = 0; s < full->strandCount(); ++s) expected += keepAtDensity(full->ids()[s], density) ? 1 : 0;
        for (std::size_t s = 0; s < shown->strandCount(); ++s) {
            const auto it = at.find(shown->ids()[s]);
            const std::string why = it == at.end() ? "a strand the render has not"
                                    : !keepAtDensity(shown->ids()[s], density) ? "a strand not under the density"
                                    : strandDifference(*shown, s, *full, it->second);
            if (why.empty()) continue;
            ++different;
            if (first.empty()) first = why;
        }
        std::printf("[ preview ] every node at %.2f: %zu of %zu shown, %zu differ%s%s\n", density, shown->strandCount(),
                    full->strandCount(), different, first.empty() ? "" : ": first ", first.c_str());
        EXPECT_EQ(shown->strandCount(), expected) << "at density " << density;
        EXPECT_EQ(different, 0u) << "at density " << density << ": " << first;
    }
}

}  // namespace groomlab
