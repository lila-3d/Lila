#pragma once

#include "gn/Geometry.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

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

/** An exposed parameter: an output of the Group Input node, and a keyable attr on the group's modifier type. */
struct GInput
{
    std::string name;
    SocketType type = SocketType::Float;
    json defaultValue;
};

/**
 * A node-group definition. Saved as JSON in the scene (on a gnGroup node) and in library folders:
 *   { "name", "typeVersion", "inputs": [{name, type, default}], "nodes": [{name, type, position, values}],
 *     "links": [[fromNode, fromSocket, toNode, toSocket]] }
 */
struct Group
{
    std::string name;
    int typeVersion = 1;
    std::vector<GInput> inputs;
    std::vector<GNode> nodes;
    std::vector<GLink> links;

    json toJson() const;
    /** Throws std::invalid_argument on malformed input. */
    static Group fromJson(const json& j);

    GNode* node(std::string_view name);
    const GNode* node(std::string_view name) const;
    const GInput* input(std::string_view name) const;
    /** First node of a type, or null. */
    const GNode* firstOfType(std::string_view type) const;
    /** base, base1, base2... whichever is free. */
    std::string uniqueNodeName(std::string_view base) const;
    std::vector<GLink> linksInto(std::string_view node, std::string_view socket) const;

    /** A new group: Group Input feeding Group Output. */
    static Group makeDefault(std::string name);
};

/** Identifier rules for group names, node names and exposed inputs: letters, digits and underscores, not a digit first. */
bool validIdentifier(std::string_view s);
/** An identifier that doesn't clash with the modifier's own attrs, so it can be an exposed input. */
bool validInputName(std::string_view s);
} // namespace lila::gn
