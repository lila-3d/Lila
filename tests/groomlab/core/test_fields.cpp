// SPDX-License-Identifier: Apache-2.0
// Modified for Lila (2026).
// Field nodes and exposed params (PG-7): a node's mappable param driven by
// another node's Field output.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <functional>

#include "groomlab/curve.h"
#include "groomlab/field.h"
#include "groomlab/graph_io.h"
#include "groomlab/groom.h"
#include "groomlab/operators.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

// Scalp -> Scatter -> Length -> Output, with `extraNodes` and `extraLinks`.
std::string graph(const std::string& scatterExtra, const std::string& lengthExtra, const std::string& extraNodes,
                  const std::string& extraLinks) {
    return R"j({"version": 1, "output": "out", "nodes": [
        {"name": "scalp", "type": "Scalp"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 400.0})j" + scatterExtra + R"j(},
        {"name": "length", "type": "Length")j" + lengthExtra + R"j(},
        {"name": "out", "type": "Output"}, {"name": "resolution", "type": "Resample", "params": {"segments": 4}})j" + extraNodes + R"j(],
      "connections": [
        {"from": "scalp", "to": "scatter", "input": "scalp"}, {"from": "scatter", "to": "length", "input": "strands"},
        {"from": "length", "to": "out", "input": "strands"})j" + extraLinks + "]}";
}

float strandLength(const StrandSet& s, std::size_t i) { return arcLength(s.strandPoints(i), s.strandPointCount(i)); }

TEST(Fields, ExposingIsChecked) {
    GraphDocument doc = loadGraphJson(graph("", "", "", ""));
    const NodeId length = doc.node("length");
    EXPECT_THROW(doc.graph.setParamInputs(length, {"nope"}), GraphError);
    EXPECT_THROW(doc.graph.setParamInputs(length, {"keep", "keep"}), GraphError);
    doc.graph.setParamInputs(length, {"keep"});
    EXPECT_EQ(doc.graph.inputSpecs(length).back().type, PortType::Field);
    // A strand output does not fit a field port.
    EXPECT_THROW(doc.graph.connect(doc.node("scatter"), length, 1), GraphError);
    // Nothing connected: the param keeps its own value.
    EXPECT_GT(doc.graph.evaluateStrands(doc.output, [] {
        static ExternalInputs ext;
        ext.set("scalp", test::makePlane(4), 1);
        EvalContext ctx;
        ctx.externals = &ext;
        return ctx;
    }())->strandCount(), 0u);
}

TEST(Fields, EditingAFieldReevaluatesItsConsumers) {
    GraphDocument doc = loadGraphJson(graph(
        "", R"j(, "paramInputs": ["keep"])j",
        R"j(, {"name": "noise", "type": "FieldNoise", "params": {"min": 0.2, "max": 0.4}})j",
        R"j(, {"from": "noise", "to": "length", "input": "keep"})j"));
    ExternalInputs ext;
    ext.set("scalp", test::makePlane(4), 1);
    EvalContext ctx;
    ctx.externals = &ext;
    const StrandsPtr a = doc.graph.evaluateStrands(doc.output, ctx);
    doc.graph.node(doc.node("noise")).params().set("max", 2.0);
    const StrandsPtr b = doc.graph.evaluateStrands(doc.output, ctx);
    float la = 0.0f, lb = 0.0f;
    for (std::size_t i = 0; i < a->strandCount(); ++i) la = std::max(la, strandLength(*a, i));
    for (std::size_t i = 0; i < b->strandCount(); ++i) lb = std::max(lb, strandLength(*b, i));
    EXPECT_GT(lb, la + 0.3f);
}

TEST(GraphUx, BypassNamesInErrorsAndTimes) {
    ExternalInputs ext;
    ext.set("scalp", test::makePlane(4), 1);
    EvalContext ctx;
    ctx.externals = &ext;
    // Length 0.1: bypassed, strands keep Scatter's length 1.
    GraphDocument doc = loadGraphJson(graph("", R"j(, "params": {"keep": 0.1}, "bypass": true)j", "", ""));
    EXPECT_TRUE(doc.graph.bypassed(doc.node("length")));
    StrandsPtr s = doc.graph.evaluateStrands(doc.output, ctx);
    EXPECT_NEAR(strandLength(*s, 0), 1.0f, 1e-3f);
    EXPECT_NE(saveGraphJson(doc).find("\"bypass\": true"), std::string::npos);
    doc.graph.setBypass(doc.node("length"), false);
    s = doc.graph.evaluateStrands(doc.output, ctx);
    EXPECT_NEAR(strandLength(*s, 0), 0.1f, 1e-3f);
    // Timings for the nodes that ran.
    EXPECT_EQ(doc.graph.lastEvaluationTimes().count(doc.node("length")), 1u);

    // Errors name the node.
    GraphDocument bad = loadGraphJson(graph(R"j(, "params": {"density": {"colorSet": "missing"}})j", "", "", ""));
    try {
        bad.graph.evaluateStrands(bad.output, ctx);
        FAIL();
    } catch (const GraphError& e) {
        EXPECT_EQ(std::string(e.what()).rfind("node 'scatter': ", 0), 0u) << e.what();
    }
}

}  // namespace
}  // namespace groomlab

namespace groomlab {
namespace {

std::string clumpGraph(const std::string& clumpExtra, const std::string& extraNodes, const std::string& extraLinks) {
    return R"j({"version": 1, "output": "out", "nodes": [
        {"name": "scalp", "type": "Scalp"},
        {"name": "scatter", "type": "Scatter", "params": {"density": 400.0}},
        {"name": "clump", "type": "Clump", "params": {"count": 10, "strength": 1.0})j" + clumpExtra + R"j(},
        {"name": "out", "type": "Output"}, {"name": "resolution", "type": "Resample", "params": {"segments": 8}})j" + extraNodes + R"j(],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
        {"from": "scatter", "to": "clump", "input": "strands"},
        {"from": "clump", "to": "out", "input": "strands"})j" +
           extraLinks + "]}";
}

TEST(Curves, ACurveNodeDrivesARampParam) {
    GraphDocument doc = loadGraphJson(clumpGraph("", "", ""));
    ExternalInputs ext;
    ext.set("scalp", test::makePlane(8), 1);
    EvalContext ctx;
    ctx.externals = &ext;
    const StrandsPtr scattered = doc.graph.evaluateStrands(doc.node("scatter"), ctx);
    const StrandsPtr clumped = doc.graph.evaluateStrands(doc.output, ctx);
    ASSERT_NE(scattered->positions(), clumped->positions());

    // A flat-zero curve on the profile: no pull anywhere.
    GraphDocument zero = loadGraphJson(clumpGraph(R"j(, "paramInputs": ["profile"])j",
                                                  R"j(, {"name": "shape", "type": "Curve", "params": {"curve": [[0, 0], [1, 0]]}})j",
                                                  R"j(, {"from": "shape", "to": "clump", "input": "profile"})j"));
    EXPECT_EQ(zero.graph.inputSpecs(zero.node("clump")).back().type, PortType::Curve);
    const StrandsPtr none = zero.graph.evaluateStrands(zero.output, ctx);
    for (std::size_t i = 0; i < none->pointCount(); ++i) {
        EXPECT_LT((none->positions()[i] - scattered->positions()[i]).length(), 1e-4f);
    }
    // The same curve from min 1 to max 2: the full pull again, at every point (clumped fully).
    zero.graph.node(zero.node("shape")).params().set("min", 1.0);
    zero.graph.node(zero.node("shape")).params().set("max", 2.0);
    const StrandsPtr full = zero.graph.evaluateStrands(zero.output, ctx);
    EXPECT_NE(full->positions(), none->positions());

    // Smooth and flip resample the curve.
    GraphDocument smooth = loadGraphJson(clumpGraph("", R"j(, {"name": "c", "type": "Curve", "params": {"interpolation": "ease", "flip": true}})j", ""));
    const CurvePtr curve = std::get<CurvePtr>(smooth.graph.evaluate(smooth.node("c"), ctx));
    EXPECT_NEAR((*curve)(0.0f), 1.0f, 1e-4f);  // flipped: 1 at the root
    EXPECT_NEAR((*curve)(0.25f), 1.0f - 0.25f * 0.25f * (3 - 2 * 0.25f), 1e-3f);  // eased
}

// A field node on the given inputs, its params set by `set`.
FieldPtr fieldOf(const char* type, const std::vector<Value>& inputs, const std::function<void(ParamSet&)>& set) {
    auto node = OperatorRegistry::instance().create(type);
    set(node->params());
    return valueAs<FieldPtr>(node->evaluate(EvalContext{}, inputs));
}

TEST(FieldNodes, MaximumFollowsTheOutputRangeAndTheSign) {
    // A curve reaching only 0.5 of the way to outMax: the largest output is half of it.
    const FieldPtr low = fieldOf(op::kFieldRemap, {constantField(0.5f)}, [](ParamSet& p) {
        p.set("curve", Ramp::linear(0.0f, 0.5f));
        p.set("outMax", 4.0);
    });
    EXPECT_NEAR(low->maximum(), 2.0f, 1e-6f);
    // outMin above outMax, a curve reaching only 0.5: the largest is outMin.
    const FieldPtr reversed = fieldOf(op::kFieldRemap, {constantField(0.5f)}, [](ParamSet& p) {
        p.set("curve", Ramp::linear(0.0f, 0.5f));
        p.set("outMin", 3.0);
        p.set("outMax", 1.0);
    });
    EXPECT_NEAR(reversed->maximum(), 3.0f, 1e-6f);
    // Subtracting a negative number adds.
    const FieldPtr minus = fieldOf(op::kFieldCombine, {constantField(1.0f), Value{}}, [](ParamSet& p) {
        p.set("mode", std::string("subtract"));
        p.set("b", -2.0);
    });
    EXPECT_NEAR(minus->maximum(), 3.0f, 1e-6f);
    EXPECT_NEAR(minus->atRoot({}), 3.0f, 1e-6f);
}

TEST(FieldNodes, LayersCompositeByModeAndOpacity) {
    // Layer 1 black, layer 2 white by default: a mask on layer 2's opacity gives the mask.
    const FieldPtr masked = fieldOf(op::kLayers, {}, [](ParamSet& p) { p.setField("layer2Opacity", constantField(0.25f)); });
    EXPECT_NEAR(masked->atRoot({}), 0.25f, 1e-6f);
    EXPECT_FALSE(masked->isColor());

    // 0.5 on 0.6 by each mode.
    const std::vector<std::pair<std::string, float>> modes = {
        {"normal", 0.5f}, {"multiply", 0.3f}, {"add", 1.1f},     {"subtract", 0.1f},   {"screen", 0.8f},
        {"overlay", 0.6f}, {"darken", 0.5f},  {"lighten", 0.6f}, {"difference", 0.1f}};
    for (const auto& [mode, expected] : modes) {
        const FieldPtr f = fieldOf(op::kLayers, {}, [&, m = mode](ParamSet& p) {
            p.set("layer1", 0.6);
            p.set("layer2", 0.5);
            p.set("layer2Mode", m);
        });
        EXPECT_NEAR(f->atRoot({}), expected, 1e-5f) << mode;
        EXPECT_GE(f->maximum(), expected - 1e-5f) << mode;
    }
    // Half opacity: halfway; a layer off, or past the count, is left out.
    auto withLayer2 = [](const std::function<void(ParamSet&)>& more) {
        return fieldOf(op::kLayers, {}, [&](ParamSet& p) {
            p.set("layer1", 0.6);
            p.set("layer2", 0.4);
            more(p);
        });
    };
    EXPECT_NEAR(withLayer2([](ParamSet& p) { p.set("layer2Opacity", 0.5); })->atRoot({}), 0.5f, 1e-6f);
    EXPECT_NEAR(withLayer2([](ParamSet& p) { p.set("layer2On", false); })->atRoot({}), 0.6f, 1e-6f);
    EXPECT_NEAR(withLayer2([](ParamSet& p) { p.set("layers", std::int64_t{1}); })->atRoot({}), 0.6f, 1e-6f);
    // A field plugged into a layer: its value times the layer's own.
    EXPECT_NEAR(withLayer2([](ParamSet& p) { p.setField("layer2", constantField(0.5f)); })->atRoot({}), 0.2f, 1e-6f);
    // Three layers, the top adding 0.3 over half the stack.
    const FieldPtr three = withLayer2([](ParamSet& p) {
        p.set("layers", std::int64_t{3});
        p.set("layer3", 0.3);
        p.set("layer3Mode", std::string("add"));
        p.set("layer3Opacity", 0.5);
    });
    EXPECT_NEAR(three->atRoot({}), 0.55f, 1e-6f);
    // Raising the count adds clear layers: the stack gives what it did until one is given an opacity.
    for (std::int64_t n = 3; n <= 8; ++n) {
        EXPECT_NEAR(withLayer2([n](ParamSet& p) { p.set("layers", n); })->atRoot({}), 0.4f, 1e-6f) << n;
    }
    EXPECT_THROW(withLayer2([](ParamSet& p) { p.set("layer2Mode", std::string("dodge")); }), GraphError);
}

TEST(FieldNodes, ColorLayersCompositeEachChannel) {
    // A color stack plugged into another's layer: its color, times the layer's own (white).
    const FieldPtr tint = fieldOf(op::kColorLayers, {}, [](ParamSet& p) {
        p.set("layers", std::int64_t{1});
        p.set("layer1", Vec3f(0.5f, 1.0f, 0.25f));
    });
    EXPECT_TRUE(tint->isColor());
    const FieldPtr f = fieldOf(op::kColorLayers, {}, [&](ParamSet& p) {
        p.set("layer1", Vec3f(0.2f, 0.4f, 0.8f));
        p.set("layer2Mode", std::string("multiply"));
        p.setField("layer2", tint);
    });
    const std::array<float, 3> c = f->colorAtRoot({});
    EXPECT_NEAR(c[0], 0.1f, 1e-6f);
    EXPECT_NEAR(c[1], 0.4f, 1e-6f);
    EXPECT_NEAR(c[2], 0.2f, 1e-6f);
    // A number param reads its luminance.
    EXPECT_NEAR(f->atRoot({}), 0.2126f * 0.1f + 0.7152f * 0.4f + 0.0722f * 0.2f, 1e-6f);
    // Screen at half opacity over the same base.
    const FieldPtr screen = fieldOf(op::kColorLayers, {}, [](ParamSet& p) {
        p.set("layer1", Vec3f(0.2f, 0.4f, 0.8f));
        p.set("layer2", Vec3f(0.5f, 0.5f, 0.5f));
        p.set("layer2Mode", std::string("screen"));
        p.set("layer2Opacity", 0.5);
    });
    const std::array<float, 3> s = screen->colorAtRoot({});
    EXPECT_NEAR(s[0], 0.2f + (0.6f - 0.2f) * 0.5f, 1e-6f);
    EXPECT_NEAR(s[2], 0.8f + (0.9f - 0.8f) * 0.5f, 1e-6f);
}

TEST(FieldNodes, CurveFlipKeepsLinear) {
    auto node = OperatorRegistry::instance().create(op::kCurve);
    node->params().set("curve", Ramp{{0.0f, 0.0f}, {0.25f, 1.0f}, {1.0f, 1.0f}});
    node->params().set("flip", true);
    node->params().set("interpolation", std::string("linear"));
    const CurvePtr curve = valueAs<CurvePtr>(node->evaluate(EvalContext{}, {}));
    EXPECT_TRUE(curve->straight);
    EXPECT_NEAR((*curve)(0.0f), 1.0f, 1e-6f);
    EXPECT_NEAR((*curve)(0.875f), 0.5f, 1e-6f);  // a straight line from (0.75, 1) to (1, 0)
}

}  // namespace
}  // namespace groomlab
