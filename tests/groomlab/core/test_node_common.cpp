// SPDX-License-Identifier: Apache-2.0
// Modified for Lila (2026).
// What every node has besides its own params: "affect" (a per-strand mask restricting it, Yeti's
// group restriction), the Group node that names the groups, and the per-node evaluation mode
// (viewport only, render only).

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <string>

#include <nlohmann/json.hpp>

#include "groomlab/graph_io.h"
#include "groomlab/groom.h"
#include "groom/GroomEvaluator.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/operators.h"
#include "test_helpers.h"

using lila::groom::GroomEvaluator;

namespace groomlab {
namespace {

using json = nlohmann::json;

json defaultGraph() { return json::parse(defaultGraphJson()); }

json& nodeNamed(json& graph, const std::string& name) {
    for (json& n : graph["nodes"]) {
        if (n["name"] == name) return n;
    }
    throw std::runtime_error("no node " + name);
}

void setParam(json& graph, const std::string& node, const std::string& param, json value) {
    nodeNamed(graph, node)["params"][param] = std::move(value);
}

// `node` (a Curves -> Curves node) put in before `before`'s strands input.
void insertBefore(json& graph, json node, const std::string& before) {
    const std::string name = node["name"];
    graph["nodes"].push_back(std::move(node));
    for (json& link : graph["connections"]) {
        if (link["to"] == before && link["input"] == "strands") {
            link["to"] = name;
            break;
        }
    }
    graph["connections"].push_back({{"from", name}, {"to", before}, {"input", "strands"}});
}

GroomData data(const json& graph) {
    GroomData d;
    d.scalp = test::makeSphere(16, 32, 1.0f);
    d.graphJson = graph.dump();
    return d;
}

// The output, for the viewport or a render.
StrandsPtr run(const GroomData& d, bool viewport = false) {
    GroomEvaluator groom(d);
    GroomEvaluator::Request request;
    request.viewport = viewport;
    request.wantBounds = false;
    const GroomEvaluator::Result& result = groom.evaluate(request);
    if (!result.errors.empty()) throw GraphError(result.errors.front());
    return result.strands;
}
StrandsPtr run(const json& graph, bool viewport = false) { return run(data(graph), viewport); }

bool sameStrand(const StrandSet& a, const StrandSet& b, std::size_t s) {
    const std::size_t n = a.strandPointCount(s);
    return n == b.strandPointCount(s) && std::memcmp(a.strandPoints(s), b.strandPoints(s), n * sizeof(Vec3f)) == 0;
}


TEST(Affect, OneIsTheNodeBitForBitAndZeroItsInput) {
    const json base = defaultGraph();
    const std::uint64_t full = run(base)->contentHash();
    json bypassed = base;
    nodeNamed(bypassed, "noise")["bypass"] = true;
    const std::uint64_t without = run(bypassed)->contentHash();
    ASSERT_NE(full, without);

    // 1 as a number, and as an expression (the mask path, every weight 1): the node's result exactly.
    for (const json& one : {json(1.0), json{{"expression", "1"}}, json{{"expression", "x > -100 ? 1 : 0"}}}) {
        json g = base;
        setParam(g, "noise", kAffectParam, one);
        EXPECT_EQ(run(g)->contentHash(), full) << one.dump();
    }
    // 0 as a number, an expression or a field plugged in: its input exactly, as bypassed.
    for (const json& zero : {json(0.0), json{{"expression", "0"}}, json{{"expression", "y - 1000"}}}) {
        json g = base;
        setParam(g, "noise", kAffectParam, zero);
        EXPECT_EQ(run(g)->contentHash(), without) << zero.dump();
    }
    json plugged = base;
    plugged["nodes"].push_back({{"name", "none"}, {"type", "Expression"}, {"params", {{"expression", "0"}}}});
    nodeNamed(plugged, "noise")["paramInputs"] = {kAffectParam};
    plugged["connections"].push_back({{"from", "none"}, {"to", "noise"}, {"input", kAffectParam}});
    EXPECT_EQ(run(plugged)->contentHash(), without);
}

TEST(Affect, MasksStrandByStrandAndPointByPoint) {
    // Noise's own result where the mask is 1, its input where it is 0 (the strands' roots: Noise
    // leaves them where they are), measured right after the node.
    json base = defaultGraph();
    base["output"] = "noise";
    const StrandsPtr full = run(base);
    json bypassed = base;
    nodeNamed(bypassed, "noise")["bypass"] = true;
    const StrandsPtr input = run(bypassed);
    json half = base;
    setParam(half, "noise", kAffectParam, {{"expression", "x > 0"}});
    const StrandsPtr masked = run(half);
    ASSERT_EQ(masked->strandCount(), full->strandCount());
    std::size_t in = 0, out = 0;
    for (std::size_t s = 0; s < masked->strandCount(); ++s) {
        const bool right = masked->strandPoints(s)[0].x > 0.0f;
        if (std::abs(masked->strandPoints(s)[0].x) < 1e-4f) continue;  // on the seam: the scalp's x may round either way
        EXPECT_TRUE(sameStrand(*masked, right ? *full : *input, s)) << s;
        (right ? in : out) += 1;
    }
    EXPECT_GT(in, 10u);
    EXPECT_GT(out, 10u);

    // Per point: t, the root as it came, the tip the node's, between a mix.
    json along = base;
    setParam(along, "noise", kAffectParam, {{"expression", "t"}});
    const StrandsPtr mixed = run(along);
    for (std::size_t s = 0; s < mixed->strandCount(); s += 7) {
        const std::size_t n = mixed->strandPointCount(s);
        const Vec3f tip = mixed->strandPoints(s)[n - 1];
        EXPECT_EQ(std::memcmp(&tip, &full->strandPoints(s)[n - 1], sizeof tip), 0) << s;
        const Vec3f mid = mixed->strandPoints(s)[n / 2], a = input->strandPoints(s)[n / 2], b = full->strandPoints(s)[n / 2];
        EXPECT_LE((mid - a).length(), (b - a).length() + 1e-6f) << s;
    }
}

TEST(Affect, MixesChannelsAndSwitchesIdsAtHalf) {
    json base = defaultGraph();
    base["output"] = "tag";
    insertBefore(base, {{"name", "tag"}, {"type", "Attribute"}, {"params", {{"name", "w"}, {"mode", "random"}, {"min", 0.2}, {"max", 0.8}}}},
                 "noise");
    base["output"] = "tag";
    const StrandsPtr full = run(base);
    json g = base;
    setParam(g, "tag", kAffectParam, 0.25);
    const StrandsPtr quarter = run(g);
    const float fallback = full->strandChannelDefault("w");
    for (std::size_t s = 0; s < full->strandCount(); s += 5) {
        const float want = fallback + ((*full->strandChannel("w"))[s] - fallback) * 0.25f;
        EXPECT_FLOAT_EQ((*quarter->strandChannel("w"))[s], want) << s;
    }
    // An ID channel switches: the input's below 0.5, the node's from it on.
    json ids = base;
    setParam(ids, "tag", "name", "patchId");
    const StrandsPtr idsFull = run(ids);
    setParam(ids, "tag", kAffectParam, 0.3);
    const StrandsPtr low = run(ids);
    setParam(ids, "tag", kAffectParam, 0.7);
    const StrandsPtr high = run(ids);
    EXPECT_EQ(*high->strandChannel("patchId"), *idsFull->strandChannel("patchId"));
    for (const float v : *low->strandChannel("patchId")) EXPECT_EQ(v, idsFull->strandChannelDefault("patchId"));
}

TEST(GroupNode, SphereBoxMapExpressionAndCombine) {
    json base = defaultGraph();
    base["output"] = "grp";
    insertBefore(base, {{"name", "grp"}, {"type", "Group"}, {"params", {{"name", "crown"}, {"center", {0, 1, 0}}, {"radius", 0.5}}}},
                 "noise");
    base["output"] = "grp";
    auto check = [](const StrandsPtr& s, const std::string& name, auto want) {
        const std::vector<float>* g = s->strandChannel(name);
        ASSERT_NE(g, nullptr) << name;
        std::size_t inside = 0;
        for (std::size_t i = 0; i < s->strandCount(); ++i) {
            const Vec3f root = s->strandPoints(i)[0];
            const float expected = want(root);
            if (expected >= 0.0f) EXPECT_FLOAT_EQ((*g)[i], expected) << name << " strand " << i;
            EXPECT_GE((*g)[i], 0.0f);
            EXPECT_LE((*g)[i], 1.0f);
            inside += (*g)[i] > 0.5f ? 1 : 0;
        }
        EXPECT_GT(inside, 0u) << name;
        EXPECT_LT(inside, s->strandCount()) << name;
    };
    const Vec3f top{0, 1, 0};
    check(run(base), "crown", [&](const Vec3f& p) { return (p - top).length() <= 0.5f ? 1.0f : 0.0f; });
    // A soft edge: 1 inside, 0 past the falloff, between in it.
    json soft = base;
    setParam(soft, "grp", "falloff", 0.3);
    check(run(soft), "crown", [&](const Vec3f& p) {
        const float d = (p - top).length();
        return d <= 0.5f ? 1.0f : (d >= 0.8f ? 0.0f : -1.0f);
    });
    json box = base;
    setParam(box, "grp", "by", "box");
    setParam(box, "grp", "center", {0.5, 0, 0});
    setParam(box, "grp", "size", {1.0, 4.0, 4.0});
    check(run(box), "crown", [](const Vec3f& p) { return p.x >= 0.0f && p.x <= 1.0f ? 1.0f : 0.0f; });
    setParam(box, "grp", "invert", true);
    check(run(box), "crown", [](const Vec3f& p) { return p.x >= 0.0f && p.x <= 1.0f ? 0.0f : 1.0f; });
    // A map (here an expression bound to the value), against the threshold; an expression of a
    // test (1 or 0) the same way.
    json map = base;
    setParam(map, "grp", "by", "map");
    setParam(map, "grp", "value", {{"expression", "y * 0.5 + 0.5"}});
    setParam(map, "grp", "threshold", 0.75);
    check(run(map), "crown", [](const Vec3f& p) { return p.y * 0.5f + 0.5f >= 0.75f ? 1.0f : 0.0f; });
    json expression = base;
    setParam(expression, "grp", "by", "map");
    setParam(expression, "grp", "value", {{"expression", "$z < 0"}});
    check(run(expression), "crown", [](const Vec3f& p) { return p.z < 0.0f ? 1.0f : 0.0f; });
    // Two Group nodes of one name: the second adds to (or takes from, or keeps where both are) the first.
    for (const std::string combine : {"add", "subtract", "intersect"}) {
        json two = expression;
        insertBefore(two, {{"name", "more"}, {"type", "Group"},
                           {"params", {{"name", "crown"}, {"by", "map"}, {"value", {{"expression", "$x < 0"}}}, {"combine", combine}}}},
                     "noise");
        two["output"] = "more";
        check(run(two), "crown", [&](const Vec3f& p) {
            const bool a = p.z < 0.0f, b = p.x < 0.0f;
            const bool in = combine == "add" ? (a || b) : combine == "subtract" ? (a && !b) : (a && b);
            return in ? 1.0f : 0.0f;
        });
    }
    json bad = base;
    setParam(bad, "grp", "name", "not a name");
    EXPECT_THROW(run(bad), GraphError);
}

TEST(GroupNode, ByGuideGroupFromTheNearestGuides) {
    // Two hierarchy groups of guides on either side of the sphere: strands near the left ones are
    // in "left" (and in it through a child group), the others out.
    GuideHierarchy hierarchy;
    const GroupId left = hierarchy.addGroup("left");
    const GroupId right = hierarchy.addGroup("right");
    for (int i = 0; i < 12; ++i) {
        const float a = 6.2831853f * static_cast<float>(i) / 12.0f;
        const Vec3f p{std::cos(a), std::sin(a) * 0.5f, std::sin(a) * 0.8f};
        const Vec3f root = p * (1.0f / p.length());
        hierarchy.addGuide(root.x < 0.0f ? left : right, test::straightCurve(root, root, 4));
    }
    json graph = defaultGraph();
    insertBefore(graph, {{"name", "grp"}, {"type", "Group"}, {"params", {{"name", "side"}, {"by", "guideGroup"}, {"guideGroup", "left"}}}},
                 "noise");
    graph["output"] = "grp";
    GroomData d = data(graph);
    d.hierarchyJson = hierarchy.toJson();
    const StrandsPtr s = run(d);
    const std::vector<float>* side = s->strandChannel("side");
    ASSERT_NE(side, nullptr);
    std::size_t checked = 0;
    for (std::size_t i = 0; i < s->strandCount(); ++i) {
        const float x = s->strandPoints(i)[0].x;
        if (std::abs(x) < 0.4f) continue;  // near the seam the nearest guides blend
        EXPECT_FLOAT_EQ((*side)[i], x < 0.0f ? 1.0f : 0.0f) << i;
        ++checked;
    }
    EXPECT_GT(checked, 20u);
    // An unknown group is the node's error.
    nodeNamed(graph, "grp")["params"]["guideGroup"] = "nowhere";
    GroomData unknown = data(graph);
    unknown.hierarchyJson = hierarchy.toJson();
    EXPECT_THROW(run(unknown), GraphError);
}

TEST(EvaluationMode, ViewportOnlyAndRenderOnlyNodesAreBypassedOutOfTheirEvaluations) {
    const json base = defaultGraph();
    json bypassed = base;
    nodeNamed(bypassed, "noise")["bypass"] = true;
    const std::uint64_t full = run(base)->contentHash();
    const std::uint64_t without = run(bypassed)->contentHash();
    ASSERT_NE(full, without);
    EXPECT_EQ(run(base, true)->contentHash(), full);  // always: in both

    json viewportOnly = base;
    nodeNamed(viewportOnly, "noise")["evaluation"] = "viewportOnly";
    EXPECT_EQ(run(viewportOnly, true)->contentHash(), full);
    EXPECT_EQ(run(viewportOnly, false)->contentHash(), without);
    json renderOnly = base;
    nodeNamed(renderOnly, "noise")["evaluation"] = "renderOnly";
    EXPECT_EQ(run(renderOnly, true)->contentHash(), without);
    EXPECT_EQ(run(renderOnly, false)->contentHash(), full);

    // One session, the viewport then a render then the viewport again: each its own.
    GroomEvaluator groom(data(renderOnly));
    GroomEvaluator::Request request;
    request.wantBounds = false;
    request.viewport = true;
    EXPECT_EQ(groom.evaluate(request).strands->contentHash(), without);
    request.viewport = false;
    EXPECT_EQ(groom.evaluate(request).strands->contentHash(), full);
    request.viewport = true;
    EXPECT_EQ(groom.evaluate(request).strands->contentHash(), without);

    // Saved with the graph (left out when always); an unknown mode is an error.
    const GraphDocument doc = loadGraphJson(renderOnly.dump());
    EXPECT_EQ(doc.graph.evaluationMode(doc.node("noise")), EvaluationMode::RenderOnly);
    EXPECT_EQ(doc.graph.evaluationMode(doc.node("width")), EvaluationMode::Always);
    EXPECT_TRUE(doc.graph.bypassedFor(doc.node("noise"), true));
    EXPECT_FALSE(doc.graph.bypassedFor(doc.node("noise"), false));
    const json saved = json::parse(saveGraphJson(doc));
    for (const json& n : saved["nodes"]) {
        if (n["name"] == "noise") EXPECT_EQ(n["evaluation"], "renderOnly");
        if (n["name"] == "width") EXPECT_FALSE(n.contains("evaluation"));
    }
    const GraphDocument again = loadGraphJson(saveGraphJson(doc));
    EXPECT_EQ(again.graph.evaluationMode(again.node("noise")), EvaluationMode::RenderOnly);
    json bad = base;
    nodeNamed(bad, "noise")["evaluation"] = "sometimes";
    EXPECT_THROW(loadGraphJson(bad.dump()), GraphError);
    EXPECT_EQ(evaluationModeFromName("viewportOnly"), EvaluationMode::ViewportOnly);
}

}  // namespace
}  // namespace groomlab
