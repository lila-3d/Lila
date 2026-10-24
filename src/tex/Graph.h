#pragma once

#include "core/Export.h"

#include "gn/Group.h"
#include "tex/Types.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace lila::tex
{
using gn::GLink;
using gn::GNode;

/**
 * Part of a graph's interface. A port input is an output of the Group Input node and an input socket on nodes using the
 * graph; a param input is a setting of those nodes that drives the inner parameters it targets ("Node.param").
 */
struct GraphInput
{
    std::string name;
    bool param = false;
    PortType port = PortType::Float;
    ParamType paramType = ParamType::Float;
    json defaultValue;
    std::string label;
    std::string description;
    std::string panel;
    std::optional<double> min, max, softMin, softMax, step;
    /** Enum options. */
    std::vector<std::string> options;
    /** Param inputs: the inner "node.param" values it sets. */
    std::vector<std::string> targets;
};

struct GraphOutput
{
    std::string name;
    PortType type = PortType::Color;
    std::string description;
};

/**
 * A texture graph. Same JSON shape as a geometry-nodes group, so the clipboard, the node canvas and agents treat both
 * alike:
 *   { "name", "typeVersion", "description", "inputs": [{name, kind: port|param, type, default, label, description, panel,
 *     min, max, softMin, softMax, step, options, targets}], "outputs": [{name, type, description}],
 *     "nodes": [{name, type, position, values}], "links": [[fromNode, fromSocket, toNode, toSocket]],
 *     "subgroups": {name: graph} }
 * Node values hold parameter values and unlinked input values by id; missing ones take the definition's default.
 */
struct LILA_SCENE_API Graph
{
    std::string name;
    int typeVersion = 1;
    std::string description;
    std::vector<GraphInput> inputs;
    std::vector<GraphOutput> outputs;
    std::vector<GNode> nodes;
    std::vector<GLink> links;
    /** Graphs its Group nodes use (values.group names one), by name, each self-contained. */
    std::map<std::string, json> subgroups;

    json toJson() const;
    /** Throws std::invalid_argument on malformed input. */
    static Graph fromJson(const json& j);

    GNode* node(std::string_view name);
    const GNode* node(std::string_view name) const;
    const GraphInput* input(std::string_view name) const;
    const GraphOutput* output(std::string_view name) const;
    const GNode* firstOfType(std::string_view type) const;
    std::string uniqueNodeName(std::string_view base) const;
    std::vector<GLink> linksInto(std::string_view node, std::string_view socket) const;

    /** A texture set's graph: one Material Output named Output. */
    static Graph makeMaterial(std::string name);
    /** A graph used as a node: Group Input and Group Output. */
    static Graph makeGroup(std::string name);
};

inline constexpr const char* kGroupInputType = "GroupInput";
inline constexpr const char* kGroupOutputType = "GroupOutput";
inline constexpr const char* kGroupType = "Group";
inline constexpr const char* kMaterialOutputType = "materialOutput";
} // namespace lila::tex
