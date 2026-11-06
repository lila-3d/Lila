// SPDX-License-Identifier: Apache-2.0
// Modified for Lila (2026).
// Value ports (graph.h: PortType::Value, UniformValue): every param can be exposed as an input. A
// mappable number or a color field takes a Field port and a ramp a Curve port, as before; every
// other param (a count, a seed, a switch, a choice, a vector, text) takes a Value port, fed one
// value for the whole node by a Value node or an Expression set to give a value.

#include <gtest/gtest.h>

#include <iostream>

#include <nlohmann/json.hpp>

#include "groomlab/field.h"
#include "groomlab/graph_edit.h"
#include "groomlab/graph_io.h"
#include "groomlab/operators.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

// A source whose one strand spells its params: x "amount", y "count", z "on", a second point at
// "dir", a third at x = 1 for mode "b" (else 0); "seed" moves the root's x (a fixed param).
class ParamsOp : public Operator {
public:
    std::string typeName() const override { return "TestValueParams"; }
    std::vector<PortSpec> inputs() const override { return {}; }
    std::vector<ParamSpec> paramSpecs() const override {
        return {
            {"amount", 1.0, "a number"},
            {"count", std::int64_t{0}, "an integer"},
            {"on", false, "a switch"},
            {"dir", Vec3f(0.0f, 1.0f, 0.0f), "a vector"},
            {"mode", std::string("a"), "a choice", "", {"a", "b"}},
            {"label", std::string("x"), "text"},
            notAnimatable({"seed", std::int64_t{0}, "Random seed"}),
        };
    }
    Value evaluate(const EvalContext&, const std::vector<Value>&) const override {
        auto out = std::make_shared<StrandSet>();
        const Vec3f p(static_cast<float>(params().getDouble("amount")), static_cast<float>(params().getInt("count")),
                      params().getBool("on") ? 1.0f : 0.0f);
        const Vec3f m(params().getString("mode") == "b" ? 1.0f : 0.0f, static_cast<float>(params().getInt("seed")),
                      params().getString("label") == "x" ? 0.0f : 1.0f);
        out->addStrand(1, {p, params().getVec3("dir"), m});
        return StrandsPtr(out);
    }
};

struct Registered {
    Registered() { OperatorRegistry::instance().registerType("TestValueParams", {}, [] { return std::make_unique<ParamsOp>(); }); }
    ~Registered() { OperatorRegistry::instance().unregisterType("TestValueParams"); }
};

NodeId valueNode(Graph& g, const std::string& kind, const std::string& param, ParamValue value) {
    const NodeId v = g.addNode(op::kValue);
    g.node(v).params().set("kind", kind);
    g.node(v).params().set(param, std::move(value));
    return v;
}

// Connects `src` into the exposed param `param` of `dst` (exposing it).
void drive(Graph& g, NodeId src, NodeId dst, const std::string& param) {
    std::vector<std::string> exposed = g.paramInputs(dst);
    const std::size_t own = g.node(dst).inputs().size();
    std::vector<NodeId> sources(exposed.size(), 0);  // what fed them (setParamInputs drops it)
    for (std::size_t k = 0; k < exposed.size(); ++k) g.inputSource(dst, own + k, &sources[k]);
    exposed.push_back(param);
    g.setParamInputs(dst, exposed);
    for (std::size_t k = 0; k < sources.size(); ++k) {
        if (sources[k]) g.connect(sources[k], dst, own + k);
    }
    const auto specs = g.inputSpecs(dst);
    for (std::size_t slot = 0; slot < specs.size(); ++slot) {
        if (specs[slot].name == param) return g.connect(src, dst, slot);
    }
    FAIL() << "no slot " << param;
}

TEST(ValueInputs, TheSchemaGivesEachParamItsPort) {
    const auto ops = nlohmann::json::parse(operatorSpecsJson());
    auto port = [&](const std::string& type, const std::string& param) {
        for (const auto& o : ops) {
            if (o.at("type") != type) continue;
            for (const auto& p : o.at("params")) {
                if (p.at("name") == param) return p.value("port", nlohmann::json());
            }
        }
        ADD_FAILURE() << type << "." << param;
        return nlohmann::json();
    };
    EXPECT_EQ(port("Clump", "size").value("type", ""), "Value");
    EXPECT_EQ(port("Clump", "size").value("valueKind", ""), "number");
    EXPECT_EQ(port("Interpolate", "neighbors").value("valueKind", ""), "integer");
    EXPECT_EQ(port("Scatter", "layout").value("valueKind", ""), "choice");
    EXPECT_EQ(port("Length", "keep").value("type", ""), "Field");
    EXPECT_EQ(port("Clump", "profile").value("type", ""), "Curve");
    EXPECT_TRUE(port("Value", "kind").is_null());
}

TEST(ValueInputs, AValueNodeDrivesEachKind) {
    Registered registered;
    Graph g;
    const NodeId n = g.addNode("TestValueParams");
    drive(g, valueNode(g, "number", "number", 0.25), n, "amount");
    drive(g, valueNode(g, "integer", "integer", std::int64_t{7}), n, "count");
    drive(g, valueNode(g, "bool", "on", true), n, "on");
    drive(g, valueNode(g, "vector", "vector", Vec3f(1.0f, 2.0f, 3.0f)), n, "dir");
    drive(g, valueNode(g, "text", "text", std::string("b")), n, "mode");
    drive(g, valueNode(g, "text", "text", std::string("y")), n, "label");
    const StrandsPtr s = g.evaluateStrands(n);
    const Vec3f* p = s->strandPoints(0);
    EXPECT_FLOAT_EQ(p[0].x, 0.25f);
    EXPECT_FLOAT_EQ(p[0].y, 7.0f);
    EXPECT_FLOAT_EQ(p[0].z, 1.0f);
    EXPECT_FLOAT_EQ(p[1].z, 3.0f);
    EXPECT_FLOAT_EQ(p[2].x, 1.0f);
    EXPECT_FLOAT_EQ(p[2].z, 1.0f);
    EXPECT_TRUE(g.paramDriven(n, "amount"));
    // Stored: each param its own value; the node's params as it evaluated (a GPU preview's).
    EXPECT_DOUBLE_EQ(g.storedParams(n).getDouble("amount"), 1.0);
    EXPECT_EQ(g.storedParams(n).getString("mode"), "a");
    EXPECT_DOUBLE_EQ(g.node(n).params().getDouble("amount"), 0.25);
}

TEST(ValueInputs, NumbersConvert) {
    Registered registered;
    Graph g;
    const NodeId n = g.addNode("TestValueParams");
    drive(g, valueNode(g, "number", "number", 2.6), n, "count");    // rounds
    drive(g, valueNode(g, "number", "number", 0.7), n, "on");       // on above 0.5
    drive(g, valueNode(g, "number", "number", 0.5), n, "dir");      // every component
    drive(g, valueNode(g, "integer", "integer", std::int64_t{3}), n, "amount");
    const Vec3f* p = g.evaluateStrands(n)->strandPoints(0);
    EXPECT_FLOAT_EQ(p[0].x, 3.0f);
    EXPECT_FLOAT_EQ(p[0].y, 3.0f);
    EXPECT_FLOAT_EQ(p[0].z, 1.0f);
    EXPECT_FLOAT_EQ(p[1].x, 0.5f);
    EXPECT_FLOAT_EQ(p[1].y, 0.5f);
}

TEST(ValueInputs, MismatchesAreRefused) {
    Registered registered;
    Graph g;
    const NodeId n = g.addNode("TestValueParams");
    g.setParamInputs(n, {"amount", "mode", "dir"});
    const NodeId field = g.addNode(op::kFieldNoise);
    const NodeId vec = valueNode(g, "vector", "vector", Vec3f(1.0f, 0.0f, 0.0f));
    const NodeId text = valueNode(g, "text", "text", std::string("b"));
    const NodeId number = valueNode(g, "number", "number", 1.0);
    try {
        g.connect(field, n, 0);  // a value per strand into one value
        FAIL() << "a Field fed a Value port";
    } catch (const GraphError& e) {
        EXPECT_NE(std::string(e.what()).find("one value for the whole node"), std::string::npos) << e.what();
    }
    EXPECT_THROW(g.connect(vec, n, 0), GraphError);     // a vector into a number
    EXPECT_THROW(g.connect(text, n, 0), GraphError);    // text into a number
    EXPECT_THROW(g.connect(number, n, 1), GraphError);  // a number into a choice
    EXPECT_NO_THROW(g.connect(number, n, 2));           // a number into a vector
    EXPECT_NO_THROW(g.connect(text, n, 1));
    // A choice that is not one of the param's: the node's error when evaluated.
    g.node(text).params().set("text", std::string("c"));
    try {
        g.evaluateStrands(n);
        FAIL() << "'c' is no choice of mode";
    } catch (const GraphError& e) {
        EXPECT_NE(std::string(e.what()).find("not one of its choices"), std::string::npos) << e.what();
    }
    // A param that sets the node's ports cannot be an input.
    EXPECT_THROW(g.setParamInputs(number, {"kind"}), GraphError);
    // In a graph's JSON too.
    const std::string bad = R"({"nodes": [
        {"name": "v", "type": "Value", "params": {"kind": "vector"}},
        {"name": "c", "type": "Clump", "paramInputs": ["size"]}],
        "connections": [{"from": "v", "to": "c", "input": "size"}], "output": "c"})";
    EXPECT_THROW(loadGraphJson(bad), GraphError);
    // The editor's model says so before it is wired.
    GraphEdit edit(R"({"nodes": [{"name": "n", "type": "FieldNoise"}, {"name": "v", "type": "Value", "params": {"kind": "text"}},
        {"name": "c", "type": "Clump", "paramInputs": ["size", "centersFrom"]}], "connections": [], "output": "c"})");
    EXPECT_THROW(edit.connect("n", "c", "size"), GraphError);
    EXPECT_THROW(edit.connect("v", "c", "size"), GraphError);
    EXPECT_NO_THROW(edit.connect("v", "c", "centersFrom"));
}

TEST(ValueInputs, AFixedParamRefusesAValueThatChangesOverTime) {
    Registered registered;
    Graph g;
    const NodeId n = g.addNode("TestValueParams");
    const NodeId constant = valueNode(g, "integer", "integer", std::int64_t{4});
    drive(g, constant, n, "seed");
    EXPECT_FLOAT_EQ(g.evaluateStrands(n)->strandPoints(0)[2].y, 4.0f);
    // An expression of the frame.
    const NodeId frame = g.addNode(op::kExpression);
    g.node(frame).params().set("output", std::string("value"));
    g.node(frame).params().set("expression", std::string("frame"));
    g.connect(frame, n, g.node(n).inputs().size());
    try {
        g.evaluateStrands(n);
        FAIL() << "the seed changes over time";
    } catch (const GraphError& e) {
        EXPECT_NE(std::string(e.what()).find("changes over time"), std::string::npos) << e.what();
    }
    // The same expression drives a param that may change.
    g.setParamInputs(n, {"amount"});
    g.connect(frame, n, g.node(n).inputs().size());
    EvalContext at2;
    at2.time = 2.0;
    EXPECT_FLOAT_EQ(g.evaluateStrands(n, at2)->strandPoints(0)[0].x, 48.0f);
    // A keyed Value node changes over time as well.
    Graph keyed;
    const NodeId m = keyed.addNode("TestValueParams");
    const NodeId v = valueNode(keyed, "integer", "integer", std::int64_t{1});
    keyed.setParamControls(v, {{"integer", {"s"}}});
    drive(keyed, v, m, "seed");
    EXPECT_THROW(keyed.evaluateStrands(m), GraphError);
}

TEST(ValueInputs, AnExpressionGivesOneValueOfTheFrameAndItsVariables) {
    Registered registered;
    Graph g;
    const NodeId n = g.addNode("TestValueParams");
    const NodeId e = g.addNode(op::kExpression);
    g.node(e).params().set("output", std::string("value"));
    g.node(e).params().set("expression", std::string("$amount * 2 + frame"));
    g.node(e).params().set("variables", std::string(R"({"amount": 1.5})"));
    EXPECT_EQ(g.outputSpecs(e)[0].type, PortType::Value);
    drive(g, e, n, "count");
    EvalContext at;
    at.time = 1.0;  // frame 24
    EXPECT_FLOAT_EQ(g.evaluateStrands(n, at)->strandPoints(0)[0].y, 27.0f);
    // A strand's variables have no strand to read.
    g.node(e).params().set("expression", std::string("$length"));
    EXPECT_THROW(g.evaluateStrands(n, at), GraphError);
    g.node(e).params().set("expression", std::string("rand()"));
    EXPECT_THROW(g.evaluateStrands(n, at), GraphError);
}

TEST(ValueInputs, AValueChangeReevaluatesLikeAnEdit) {
    Registered registered;
    Graph g;
    const NodeId n = g.addNode("TestValueParams");
    const NodeId v = valueNode(g, "number", "number", 0.5);
    drive(g, v, n, "amount");
    g.evaluateStrands(n);
    const std::size_t first = g.evaluationCount();
    g.evaluateStrands(n);
    EXPECT_EQ(g.evaluationCount(), first);  // nothing changed: cached
    g.node(v).params().set("number", 0.75);
    EXPECT_FLOAT_EQ(g.evaluateStrands(n)->strandPoints(0)[0].x, 0.75f);
    EXPECT_EQ(g.evaluationCount(), first + 2);  // the value and the node, once each
    g.evaluateStrands(n);
    EXPECT_EQ(g.evaluationCount(), first + 2);
    // A copy of the graph (reloaded after an edit elsewhere) reuses the results.
    Graph copy = g.clone();
    EXPECT_FLOAT_EQ(copy.evaluateStrands(n)->strandPoints(0)[0].x, 0.75f);
    EXPECT_EQ(copy.evaluationCount(), 0u);
    // Cut, the param takes its own value back.
    g.disconnect(n, g.node(n).inputs().size());
    EXPECT_DOUBLE_EQ(g.node(n).params().getDouble("amount"), 1.0);
    EXPECT_FLOAT_EQ(g.evaluateStrands(n)->strandPoints(0)[0].x, 1.0f);
}

TEST(ValueInputs, JsonRoundTrip) {
    const std::string text = R"({"version": 1, "nodes": [
        {"name": "count", "type": "Value", "params": {"kind": "integer", "integer": 9}},
        {"name": "scalp", "type": "Scalp"}, {"name": "guides", "type": "Guides"},
        {"name": "scatter", "type": "Scatter"},
        {"name": "interp", "type": "Interpolate", "params": {"neighbors": 4}, "paramInputs": ["neighbors"]},
        {"name": "out", "type": "Output"}],
        "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"},
                        {"from": "scatter", "to": "interp", "input": "strands"},
                        {"from": "guides", "to": "interp", "input": "guides"},
                        {"from": "count", "to": "interp", "input": "neighbors"},
                        {"from": "interp", "to": "out", "input": "strands"}], "output": "out"})";
    GraphDocument doc = loadGraphJson(text);
    const NodeId interp = doc.node("interp");
    EXPECT_EQ(doc.graph.inputSpecs(interp).back().type, PortType::Value);
    EXPECT_EQ(doc.graph.inputSpecs(interp).back().valueKind, ValueKind::Integer);
    EXPECT_TRUE(doc.graph.paramDriven(interp, "neighbors"));
    const auto saved = nlohmann::json::parse(saveGraphJson(doc));
    bool linked = false;
    for (const auto& c : saved.at("connections")) linked = linked || (c.at("from") == "count" && c.at("input") == "neighbors");
    EXPECT_TRUE(linked);
    GraphDocument again = loadGraphJson(saved.dump());
    EXPECT_EQ(again.graph.paramInputs(again.node("interp")), std::vector<std::string>{"neighbors"});
    EXPECT_TRUE(again.graph.paramDriven(again.node("interp"), "neighbors"));
}

TEST(ValueInputs, ANumberValueFeedsAField) {
    Graph g;
    const NodeId v = valueNode(g, "number", "number", 2.0);
    const NodeId combine = g.addNode(op::kFieldCombine);
    g.connect(v, combine, 0);  // a: the number on every strand
    const FieldPtr f = valueAs<FieldPtr>(g.evaluate(combine));
    ASSERT_TRUE(f);
    EXPECT_FLOAT_EQ(f->maximum(), 2.0f);
    // A vector does not.
    const NodeId vec = valueNode(g, "vector", "vector", Vec3f(1.0f, 1.0f, 1.0f));
    EXPECT_THROW(g.connect(vec, combine, 0), GraphError);
}

TEST(ValueInputs, AColorValueFeedsAColorField) {
    Graph g;
    const NodeId color = valueNode(g, "color", "color", Vec3f(0.2f, 0.4f, 0.8f));
    const NodeId hair = g.addNode("HairColor");
    g.setParamInputs(hair, {"tipColor"});
    EXPECT_NO_THROW(g.connect(color, hair, g.node(hair).inputs().size()));
    const UniformValue value{Vec3f(0.2f, 0.4f, 0.8f), false};
    const FieldPtr field = uniformField(value);
    ASSERT_TRUE(field);
    EXPECT_TRUE(field->isColor());
    EXPECT_FLOAT_EQ(field->maximum(), 0.8f);

    GraphEdit edit(R"({"nodes": [
        {"name": "color", "type": "Value", "params": {"kind": "color", "color": [0.2, 0.4, 0.8]}},
        {"name": "hair", "type": "HairColor", "paramInputs": ["tipColor"]}],
        "connections": [], "output": "hair"})");
    const auto spec = nlohmann::json::parse(edit.nodeSpecJson("hair"));
    const auto input = std::find_if(spec["inputs"].begin(), spec["inputs"].end(),
                                    [](const auto& p) { return p.value("name", "") == "tipColor"; });
    ASSERT_NE(input, spec["inputs"].end());
    EXPECT_EQ(input->value("valueKind", ""), "vec3");
    EXPECT_NO_THROW(edit.connect("color", "hair", "tipColor"));
}

}  // namespace
}  // namespace groomlab
