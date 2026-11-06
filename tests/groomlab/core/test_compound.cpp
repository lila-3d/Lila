// SPDX-License-Identifier: Apache-2.0
// Modified for Lila (2026).
// Compound nodes (PG-6).

#include <gtest/gtest.h>

#include <atomic>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "groomlab/cards.h"
#include "groomlab/graph_edit.h"
#include "groomlab/graph_io.h"
#include "groomlab/groom.h"
#include "groomlab/operators.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

using json = nlohmann::json;

// scatter -> [noise -> curl] -> output, inline or as a compound.
json baseGraph() {
    return json::parse(R"j({"version": 1, "output": "out",
      "nodes": [{"name": "scalp", "type": "Scalp"},
                {"name": "scatter", "type": "Scatter", "params": {"density": 200.0}},
                {"name": "out", "type": "Output"}],
      "connections": [{"from": "scalp", "to": "scatter", "input": "scalp"}]})j");
}

json inlineGraph(double amplitude) {
    json g = baseGraph();
    g["nodes"].push_back({{"name", "noise"}, {"type", "Noise"}, {"params", {{"amplitude", amplitude}}}});
    g["nodes"].push_back({{"name", "curl"}, {"type", "Curl"}});
    g["connections"].push_back({{"from", "scatter"}, {"to", "noise"}, {"input", "strands"}});
    g["connections"].push_back({{"from", "noise"}, {"to", "curl"}, {"input", "strands"}});
    g["connections"].push_back({{"from", "curl"}, {"to", "out"}, {"input", "strands"}});
    return g;
}

json frizzyDefinition() {
    return json::parse(R"j({
      "graph": {"version": 1, "output": "curl",
        "nodes": [{"name": "in", "type": "Input", "params": {"name": "strands", "type": "Strands"}},
                  {"name": "noise", "type": "Noise", "params": {"amplitude": 0.05}},
                  {"name": "curl", "type": "Curl"}],
        "connections": [{"from": "in", "to": "noise", "input": "strands"},
                        {"from": "noise", "to": "curl", "input": "strands"}]},
      "inputs": [{"name": "strands", "type": "Strands"}],
      "expose": {"amount": "noise.amplitude"}})j");
}

json compoundGraph(const json& params) {
    json g = baseGraph();
    json p = params;
    p["definition"] = frizzyDefinition();
    g["nodes"].push_back({{"name", "frizzy"}, {"type", "Compound"}, {"params", p}});
    g["connections"].push_back({{"from", "scatter"}, {"to", "frizzy"}, {"input", "strands"}});
    g["connections"].push_back({{"from", "frizzy"}, {"to", "out"}, {"input", "strands"}});
    return g;
}

// A node that counts how often it runs, handing its strands on.
std::atomic<int> gCountingRuns{0};
class CountingOp : public Operator {
public:
    std::string typeName() const override { return "TestCounting"; }
    std::vector<PortSpec> inputs() const override { return {{"strands", PortType::Strands}}; }
    Value evaluate(const EvalContext&, const std::vector<Value>& inputs) const override {
        ++gCountingRuns;
        return inputs.empty() ? Value{} : inputs[0];
    }
};

// Displacement of every point of `a` from the same point of `b`, summed.
float totalMove(const StrandSet& a, const StrandSet& b) {
    float sum = 0.0f;
    for (std::size_t i = 0; i < a.pointCount() && i < b.pointCount(); ++i) sum += (a.positions()[i] - b.positions()[i]).length();
    return sum;
}

}  // namespace
}  // namespace groomlab
