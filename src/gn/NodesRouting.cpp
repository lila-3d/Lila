#include "gn/NodesCommon.h"
#include "gn/Routing.h"

namespace lila::gn::nodes
{
namespace
{
/** The socket feeding a routing node, as declared on the node it comes from; a plain float when nothing feeds it. */
SocketDecl routedSocket(const Group& g, const GNode& n, std::string id)
{
    SocketDecl s = in(std::move(id), T::Float);
    if (auto src = routedSource(g.nodes, g.links, n))
        if (const GNode* from = g.node(src->first))
            if (const NodeDecl* d = findDecl(from->type))
            {
                Sockets fs = d->socketsFor(g, *from);
                if (int o = fs.outputIndex(src->second); o >= 0)
                {
                    s.type = fs.outputs[size_t(o)].type;
                    s.field = fs.outputs[size_t(o)].field;
                }
            }
    return s;
}

[[noreturn]] void neverRuns(ExecContext& c) { throw ExecError(c.node.name + ": routing nodes are spliced out before evaluation"); }
} // namespace

void registerRoutingNodes()
{
    registerDecl({
        .type = kRerouteType,
        .label = "Reroute",
        .category = "Layout",
        .doc = "A point a wire passes through, to steer it. Takes the type of whatever feeds it and can feed several inputs.",
        .dynamicSockets =
            [](const Group& g, const GNode& n) {
                SocketDecl s = routedSocket(g, n, kRerouteIn);
                SocketDecl o = s;
                o.id = kRerouteOut;
                return Sockets{{s}, {o}};
            },
        .exec = neverRuns,
    });
    registerDecl({
        .type = kSendType,
        .label = "Send",
        .category = "Layout",
        .doc = "Names the value wired into it, so Receive nodes anywhere in the group can read it without a wire.",
        .props = {prop("name", {}, "", "What Receive nodes call this value.")},
        .dynamicSockets = [](const Group& g, const GNode& n) { return Sockets{{routedSocket(g, n, kPortalSocket)}, {}}; },
        .exec = neverRuns,
    });
    registerDecl({
        .type = kReceiveType,
        .label = "Receive",
        .category = "Layout",
        .doc = "Reads the value of the Send node with its name, without a wire. Takes that value's type.",
        .props = {prop("name", {}, "", "The Send node to read.")},
        .dynamicSockets =
            [](const Group& g, const GNode& n) {
                SocketDecl o = routedSocket(g, n, kPortalSocket);
                return Sockets{{}, {o}};
            },
        .exec = neverRuns,
    });
}
} // namespace lila::gn::nodes
