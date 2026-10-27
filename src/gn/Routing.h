#pragma once

#include "core/Export.h"

#include "gn/Group.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
 * Nodes that only carry a link along, shared by geometry nodes and texture graphs: reroutes (a dot on a wire) and
 * portals (a Send names a value, Receives with that name read it without a wire). Evaluation splices them out first,
 * so the node libraries never run them, and bypasses muted nodes the same way.
 */
namespace lila::gn
{
inline constexpr const char* kRerouteType = "Reroute";
inline constexpr const char* kSendType = "PortalSend";
inline constexpr const char* kReceiveType = "PortalReceive";

/** Reroute: "input" in, "output" out. Send: "value" in. Receive: "value" out. */
inline constexpr const char* kRerouteIn = "input";
inline constexpr const char* kRerouteOut = "output";
inline constexpr const char* kPortalSocket = "value";

LILA_SCENE_API bool isRoutingType(std::string_view type);
/** A portal's name: its "name" value, empty when unset. */
LILA_SCENE_API std::string portalName(const GNode& n);

/** A node's sockets with their type names, which is all routing needs to know about a node. */
struct PortTypes
{
    std::vector<std::pair<std::string, std::string>> inputs, outputs; // (id, type)
};

struct RoutingHooks
{
    std::function<PortTypes(const GNode&)> ports;
    /** Whether an output of one type may feed an input of the other. */
    std::function<bool(const std::string& from, const std::string& to)> linkable;
};

/**
 * The output feeding a routing node, following reroutes and portals upstream to the first other node: (node, socket).
 * Null when nothing feeds it, its portal has no Send, or the chain loops.
 */
LILA_SCENE_API std::optional<std::pair<std::string, std::string>> routedSource(const std::vector<GNode>& nodes, const std::vector<GLink>& links,
                                                                               const GNode& routing);

/**
 * The graph as evaluation sees it: every link out of a reroute or Receive comes from the output that feeds it instead,
 * links that nothing feeds are dropped, and routing nodes leave nodes. Muted nodes then pass their first input of a
 * matching type through to each output; an output with no such input (or an unlinked one) is left unlinked.
 */
LILA_SCENE_API void resolveRouting(std::vector<GNode>& nodes, std::vector<GLink>& links, const RoutingHooks& hooks);

/** Portal problems by node name: a Receive naming no Send, Sends sharing a name, a portal that feeds itself. */
LILA_SCENE_API std::map<std::string, std::string> portalProblems(const std::vector<GNode>& nodes, const std::vector<GLink>& links);
/** links, plus one from each Send to every Receive of its name, so cycle checks see through portals. */
LILA_SCENE_API std::vector<GLink> withPortalLinks(const std::vector<GNode>& nodes, const std::vector<GLink>& links);
LILA_SCENE_API bool hasCycle(const std::vector<GLink>& links);
} // namespace lila::gn
