// SPDX-License-Identifier: Apache-2.0
// Modified for Lila (2026).
#pragma once

// Edits of a graph's JSON (graph_io.h's schema) as every host's node editor makes them: Maya's
// Graph Editor (the Python GraphModel, groomlab/graph.py) and the standalone app's node graph.
// The JSON is edited as it is written: every field a node has (its params, paramInputs,
// paramControls, bypass, "ui" position, anything a host added) is kept, and a graph that does
// not load yet (an input unconnected, no output) can still be edited.
//
// A node's ports come from its operator's spec (operatorSpecsJson), except that a Compound's
// come from its definition (its inputs, its inner output's type, its exposed params), an Input's
// output type from its "type" param, and every param in "paramInputs" adds a Field port (a
// Curve port for a ramp). A connection's types must fit (portTypesMatch on the output's type as
// connected: a Curves output gives what feeds it) and may not make a cycle.

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "groomlab/graph.h"

namespace groomlab {

class GraphEdit {
public:
    // Throws GraphError when the text is not a JSON object.
    explicit GraphEdit(const std::string& graphJson);
    ~GraphEdit();
    GraphEdit(const GraphEdit&);
    GraphEdit& operator=(const GraphEdit&);

    std::string toJson() const;  // compact

    bool hasNode(const std::string& name) const;
    std::string nodeType(const std::string& name) const;  // GraphError for no such node
    std::vector<std::string> nodeNames() const;           // in the graph's order

    // A new node's name: `base` with its first letter lowered when no node has it, else
    // base1, base2, ... (the first free).
    std::string uniqueName(const std::string& base) const;
    // Adds a node of `type` (GraphError for an unknown one) named `name` ("": uniqueName(type);
    // GraphError when a node has it), its params from `paramsJson` (an object; "" none). A node
    // whose operator has a "seed" param `paramsJson` leaves out takes a seed no other node of its
    // type has: the default when free, else the largest used + 1. `x`, `y` set its position
    // ("ui"). `groomSize`: the groom's scalp size (presetScalpSize): the spec defaults
    // `paramsJson` leaves out that measure something are fitted to it as a preset's params are,
    // the defaults being authored at kDefaultsScalpSize (fitNewNodeParamsJson, presets.h); nullopt:
    // as they are. Returns the name.
    std::string addNode(const std::string& type, const std::string& name = {}, const std::string& paramsJson = {},
                        std::optional<double> x = std::nullopt, std::optional<double> y = std::nullopt,
                        std::optional<double> groomSize = std::nullopt);
    // Removes the node with every connection to or from it; the graph's output is cleared when
    // it was the node. GraphError for no such node.
    void removeNode(const std::string& name);
    // Renames a node: its connections and the graph's output follow. GraphError for no such
    // node, an empty name, or a name another node has.
    void renameNode(const std::string& name, const std::string& newName);
    // The node whose result is the groom ("" none).
    std::string outputNode() const;
    // Makes `name` the graph's output. GraphError for no such node.
    void setOutputNode(const std::string& name);
    // Drops every node's position ("ui"), so an editor lays the graph out again (Tidy Layout).
    void clearPositions();
    // Connects src's output `output` ("": its first) to dst's input, replacing what fed it; the
    // connection records the output only when it is not the first. Throws GraphError for an
    // unknown node, input or output, types that do not fit, or a cycle.
    void connect(const std::string& src, const std::string& dst, const std::string& input, const std::string& output = {});
    // A link the user drew in an editor (every host's): connect, then a node whose dropdown says
    // whether it uses that input set to use it: a Clump of centersFrom pick fed centers takes them
    // (inputPoints; guides, a Guides node's, an ImportCurves', a GuideCache's or a Simulate's, through
    // any curve nodes after them: inputCurves, each clump along its guide), a Cards node of
    // cardsFrom strands fed guides lays its cards along them (guides).
    // Disconnecting changes nothing back; connect alone (pastes, compounds, scripts,
    // loading) never changes a param. True when a param changed.
    bool connectByHand(const std::string& src, const std::string& dst, const std::string& input,
                       const std::string& output = {});
    // Removes what feeds dst's input; false when nothing did.
    bool disconnect(const std::string& dst, const std::string& input);
    // Every connection checked and made again in order (connect's checks), as when a host loads a
    // graph to edit it. Throws GraphError at the first that does not fit.
    void checkConnections();

    // A node's ports and params (operatorSpecsJson's form), as the edits see them: a Compound's
    // and an Input's from their params, the exposed params' ports ({"param": true}) after the
    // operator's inputs, "outputs" always listed, and "nodeVersion" (the behaviour the node keeps)
    // with "outdated" (true: an Upgrade is available, upgradeNode). A Compound's exposed params
    // take their inner params' specs (range, unit, group, advanced...; the default the inner
    // node's value; a showWhen naming an inner param shown by its exposed name, else dropped), in
    // the inner graph's order: its nodes as listed, each one's params as its operator lists them.
    // GraphError for no such node.
    std::string nodeSpecJson(const std::string& name) const;
    // A node's output type as connected (`output` "": its first): Curves gives what feeds its
    // first Curves input.
    std::string outputType(const std::string& name, const std::string& output = {}) const;
    // Every node feeding `name`, directly or not.
    std::vector<std::string> upstream(const std::string& name) const;

    // Writes every param each node leaves out with its spec default, and each node's version
    // when it has none (its operator's newest), compounds' inner nodes too: what saved grooms hold,
    // so a later change of a default never alters them (graph_io.h). addNode does it for its node.
    void completeParams();
    // The behaviour version the node evaluates with ("version"; none: its operator's newest).
    int nodeVersion(const std::string& name) const;
    // Raises an outdated node (nodeSpecJson's "outdated") to its operator's newest version (what
    // a host's Upgrade button does: the node then evaluates as new nodes do). False when it is
    // already the newest. GraphError for no such node.
    bool upgradeNode(const std::string& name);

    // The strand channels (attributes) per node as the graph is now: graphChannelsJson
    // (attributes.h), `guideMaps` the maps painted on the guides.
    std::string channelsJson(const std::vector<std::string>& guideMaps = {}) const;

    // Nodes of one type sharing a seed where that is likely a mistake (two Scatters: their
    // strands coincide; two Instances or Flyaways: they pick the same strands), as warnings.
    std::vector<std::string> seedWarnings() const;


    // The node paths this edit's renames, groups and ungroups moved, in order (old -> new; a node
    // inside a compound by its path, "compound/inner"): a rename moves the node, Group moves each
    // grouped node in ("sculpt1" -> "compound1/sculpt1"), Ungroup moves them out (to their new
    // names when theirs were taken). A compound's move takes the paths inside it along. What a
    // host keeps by node path outside the graph follows them in the same step (a Sculpt node's
    // edits: moveSculptNodes, sculpt.h).
    const std::vector<std::pair<std::string, std::string>>& movedNodes() const;

private:
    void recordMove(const std::string& from, const std::string& to);  // one more movedNodes entry

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The graph with every param and node version written (GraphEdit::completeParams).
std::string completeGraphJson(const std::string& graphJson);

// A node's spec as the edits see it (GraphEdit::nodeSpecJson), from the node's own JSON alone
// (`nodeJson`: {"type", "params", "paramInputs"...}).
std::string nodeSpecJson(const std::string& nodeJson);

}  // namespace groomlab
