// SPDX-License-Identifier: Apache-2.0
#include "groomlab/node_catalog.h"

#include <cctype>
#include <map>

#include "groomlab/graph.h"

namespace groomlab {

const std::vector<NodeCategory>& nodeCategories() {
    // What it reads, the strands grown from it, their shape, deformers, collisions, the look
    // (clumps and detail), motion, feathers, color, conversions, fields, plumbing, games.
    static const std::vector<NodeCategory> categories = {
        {"source", "Sources"}, {"grow", "Grow"},         {"shape", "Shape"},           {"deform", "Deform"},
        {"collide", "Collide"}, {"look", "Look"},        {"motion", "Motion"},         {"feathers", "Feathers"},
        {"color", "Color"},    {"convert", "Convert"},   {"field", "Fields & Curves"}, {"utility", "Utility"},
        {"games", "Games"},
    };
    return categories;
}

int nodeCategoryOrder(const std::string& id) {
    const std::vector<NodeCategory>& categories = nodeCategories();
    for (std::size_t i = 0; i < categories.size(); ++i) {
        if (categories[i].id == id) return static_cast<int>(i);
    }
    return static_cast<int>(categories.size());
}

const NodeInfo* nodeInfo(const std::string& type) { return OperatorRegistry::instance().info(type); }

const std::vector<std::string>& builtinNodeTypes() {
    static const std::vector<std::string> types = {
#define GROOMLAB_OPERATOR(Type) #Type,
#define GROOMLAB_OPERATOR_ELSEWHERE(Type) #Type,
#include "groomlab/builtin_operators.inc"
#undef GROOMLAB_OPERATOR
#undef GROOMLAB_OPERATOR_ELSEWHERE
    };
    return types;
}

int nodeMenuOrder(const std::string& type) {
    static const std::map<std::string, int> order = [] {
        std::map<std::string, int> out;
        for (const std::string& t : builtinNodeTypes()) out.emplace(t, static_cast<int>(out.size()));
        return out;
    }();
    const auto it = order.find(type);
    return it == order.end() ? static_cast<int>(order.size()) : it->second;
}

std::string nodeLabel(const std::string& type) {
    const NodeInfo* info = nodeInfo(type);
    if (info && !info->label.empty()) return info->label;
    std::string out;
    for (std::size_t i = 0; i < type.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(type[i]);
        const bool lowerBefore = i > 0 && std::islower(static_cast<unsigned char>(type[i - 1]));
        if (std::isupper(c) && lowerBefore) out += ' ';
        out += static_cast<char>(c);
    }
    return out;
}

}  // namespace groomlab
