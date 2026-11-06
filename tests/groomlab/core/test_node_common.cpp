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
#include "groomlab/guide_hierarchy.h"
#include "groomlab/operators.h"
#include "test_helpers.h"

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

bool sameStrand(const StrandSet& a, const StrandSet& b, std::size_t s) {
    const std::size_t n = a.strandPointCount(s);
    return n == b.strandPointCount(s) && std::memcmp(a.strandPoints(s), b.strandPoints(s), n * sizeof(Vec3f)) == 0;
}

}  // namespace
}  // namespace groomlab
