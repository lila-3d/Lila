#pragma once

#include "core/Export.h"

#include "tex/Graph.h"
#include "tex/NodeDef.h"

#include <string>
#include <utility>
#include <vector>

namespace lila::tex
{
/** Extra Material Output channels (a texture set's user channels): name and type. */
using ChannelList = std::vector<std::pair<std::string, PortType>>;

/** A node's sockets and parameters as they are in its graph: Group Input/Output and graph nodes take theirs from a graph. */
struct LILA_SCENE_API NodePorts
{
    const NodeDef* def = nullptr;
    std::vector<PortDecl> inputs;
    std::vector<PortDecl> outputs;
    std::vector<ParamDecl> params;
    /** Graph and Group nodes: the graph they run. */
    json graph;
    /** Unknown type, missing subgroup, unreadable graph. */
    std::string error;

    const PortDecl* input(std::string_view id) const;
    const PortDecl* output(std::string_view id) const;
    const ParamDecl* param(std::string_view id) const;
    int inputIndex(std::string_view id) const;
    int outputIndex(std::string_view id) const;
};

/** extra adds Material Output inputs beyond the standard channels. */
LILA_SCENE_API NodePorts portsOf(const Graph& g, const GNode& n, const ChannelList* extra = nullptr);

/** The param value a node uses: its own value, else the declaration's default. */
LILA_SCENE_API json paramValue(const GNode& n, const ParamDecl& p);
} // namespace lila::tex
