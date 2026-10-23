#pragma once

#include "core/Export.h"

#include "gn/Geometry.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace lila::gn
{
using json = nlohmann::json;

/** One node in a group. Values hold socket and property overrides by id; anything missing takes its default. */
struct GNode
{
    std::string name; // unique in the group
    std::string type;
    glm::dvec2 position{0.0};
    json values = json::object();
};

struct GLink
{
    std::string fromNode;
    std::string fromSocket;
    std::string toNode;
    std::string toSocket;

    bool operator==(const GLink&) const = default;
};

/**
 * An exposed parameter: an output of the Group Input node, a socket on Group nodes using the group, and a keyable attr
 * on the group's modifier type. Everything after defaultValue is presentation: editors show the label, tooltip and
 * limits, and group inputs by panel in interface order.
 */
struct GInput
{
    std::string name;
    SocketType type = SocketType::Float;
    json defaultValue;
    std::string label;       // nice name; empty derives one from name
    std::string description; // tooltip
    /** "", factor, percentage, angle (float in degrees, fed to the graph as radians), distance, color. */
    std::string subtype;
    std::optional<double> min, max, softMin, softMax, step;
    std::string panel;

    /** label, or name split into words. */
    LILA_SCENE_API std::string displayName() const;
};

/** A value a nested group hands back through its Group Output. Only geometry reaches the modifier stack. */
struct GOutput
{
    std::string name;
    SocketType type = SocketType::Float;
    std::string description;
};

/**
 * A node-group definition. Saved as JSON in the scene (on a gnGroup node) and in library folders:
 *   { "name", "typeVersion", "inputs": [{name, type, default, label, description, subtype, min, max, softMin, softMax,
 *     step, panel}], "outputs": [{name, type, description}], "nodes": [{name, type, position, values}],
 *     "links": [[fromNode, fromSocket, toNode, toSocket]], "subgroups": {name: definition} }
 */
struct LILA_SCENE_API Group
{
    std::string name;
    int typeVersion = 1;
    std::vector<GInput> inputs;
    std::vector<GOutput> outputs;
    std::vector<GNode> nodes;
    std::vector<GLink> links;
    std::string description;
    /** Definitions of the groups its Group nodes use, by name, each self-contained (with its own subgroups). */
    std::map<std::string, json> subgroups;

    json toJson() const;
    /** Throws std::invalid_argument on malformed input. */
    static Group fromJson(const json& j);

    GNode* node(std::string_view name);
    const GNode* node(std::string_view name) const;
    const GInput* input(std::string_view name) const;
    GInput* input(std::string_view name);
    const GOutput* output(std::string_view name) const;
    /** Panel names in first-use order; "" (no panel) is left out. */
    std::vector<std::string> panels() const;
    /** First node of a type, or null. */
    const GNode* firstOfType(std::string_view type) const;
    /** base, base1, base2... whichever is free. */
    std::string uniqueNodeName(std::string_view base) const;
    std::vector<GLink> linksInto(std::string_view node, std::string_view socket) const;

    /** A new group: Group Input feeding Group Output. */
    static Group makeDefault(std::string name);
};

/** Whether the group or a group nested in it reads the scene time. */
LILA_SCENE_API bool usesSceneTime(const Group& g);
/** Names of the groups its Group nodes use directly. */
LILA_SCENE_API std::vector<std::string> referencedGroups(const Group& g);
/** Whether the definition is, or uses at any depth, the named group. */
LILA_SCENE_API bool dependsOn(const Group& g, std::string_view group);

inline constexpr const char* kGroupNodeDeclType = "Group";

/** Identifier rules for group names, node names and exposed inputs: letters, digits and underscores, not a digit first. */
LILA_SCENE_API bool validIdentifier(std::string_view s);
/** An identifier that doesn't clash with the modifier's own attrs, so it can be an exposed input. */
LILA_SCENE_API bool validInputName(std::string_view s);
} // namespace lila::gn
