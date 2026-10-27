#include "gn/Routing.h"

#include <algorithm>
#include <functional>
#include <set>

namespace lila::gn
{
namespace
{
const GNode* find(const std::vector<GNode>& nodes, std::string_view name)
{
    for (const GNode& n : nodes)
        if (n.name == name)
            return &n;
    return nullptr;
}

const GLink* linkInto(const std::vector<GLink>& links, std::string_view node, std::string_view socket)
{
    for (const GLink& l : links)
        if (l.toNode == node && l.toSocket == socket)
            return &l;
    return nullptr;
}

/** The Sends by name; a name used twice maps to null. */
std::map<std::string, const GNode*> sendsByName(const std::vector<GNode>& nodes)
{
    std::map<std::string, const GNode*> out;
    for (const GNode& n : nodes)
        if (n.type == kSendType)
        {
            std::string name = portalName(n);
            if (name.empty())
                continue;
            auto [it, fresh] = out.emplace(name, &n);
            if (!fresh)
                it->second = nullptr;
        }
    return out;
}
} // namespace

/**
 * Rewires every link out of a muted node to come from the source of the node's first input whose type matches the
 * output (the same type, else one that links to it); with none, or an unlinked one, the link goes. Repeats while a new
 * source is itself muted, then drops the links into muted nodes.
 */
static void bypassMuted(std::vector<GNode>& nodes, std::vector<GLink>& links, const RoutingHooks& hooks)
{
    std::map<std::string, PortTypes> muted;
    for (const GNode& n : nodes)
        if (n.muted && hooks.ports)
            muted[n.name] = hooks.ports(n);
    if (muted.empty())
        return;
    auto typeOf = [](const std::vector<std::pair<std::string, std::string>>& list, const std::string& id) {
        for (auto& [k, t] : list)
            if (k == id)
                return t;
        return std::string();
    };
    // Each output of a muted node: the input it passes through, or empty.
    auto passed = [&](const std::string& node, const std::string& output) -> std::string {
        const PortTypes& p = muted[node];
        std::string type = typeOf(p.outputs, output);
        for (auto& [id, t] : p.inputs)
            if (t == type)
                return id;
        for (auto& [id, t] : p.inputs)
            if (hooks.linkable && hooks.linkable(t, type))
                return id;
        return {};
    };
    for (size_t pass = 0; pass <= nodes.size(); ++pass)
    {
        bool changed = false;
        std::vector<GLink> out;
        for (const GLink& l : links)
        {
            if (!muted.contains(l.fromNode))
            {
                out.push_back(l);
                continue;
            }
            changed = true;
            std::string in = passed(l.fromNode, l.fromSocket);
            if (in.empty())
                continue;
            if (const GLink* src = linkInto(links, l.fromNode, in))
                out.push_back({src->fromNode, src->fromSocket, l.toNode, l.toSocket});
        }
        links = std::move(out);
        if (!changed)
            break;
    }
    std::erase_if(links, [&](const GLink& l) { return muted.contains(l.toNode) || muted.contains(l.fromNode); });
}

bool isRoutingType(std::string_view type) { return type == kRerouteType || type == kSendType || type == kReceiveType; }

std::string portalName(const GNode& n)
{
    auto it = n.values.find("name");
    return it != n.values.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::optional<std::pair<std::string, std::string>> routedSource(const std::vector<GNode>& nodes, const std::vector<GLink>& links,
                                                                const GNode& routing)
{
    std::map<std::string, const GNode*> sends = sendsByName(nodes);
    std::set<std::string> seen;
    const GNode* at = &routing;
    while (at && isRoutingType(at->type))
    {
        if (!seen.insert(at->name).second)
            return std::nullopt;
        if (at->type == kReceiveType)
        {
            auto it = sends.find(portalName(*at));
            at = it == sends.end() ? nullptr : it->second;
            continue;
        }
        const GLink* in = linkInto(links, at->name, at->type == kRerouteType ? kRerouteIn : kPortalSocket);
        if (!in)
            return std::nullopt;
        const GNode* from = find(nodes, in->fromNode);
        if (!from)
            return std::nullopt;
        if (!isRoutingType(from->type))
            return std::make_pair(in->fromNode, in->fromSocket);
        at = from;
    }
    return std::nullopt;
}

void resolveRouting(std::vector<GNode>& nodes, std::vector<GLink>& links, const RoutingHooks& hooks)
{
    bool any = std::any_of(nodes.begin(), nodes.end(), [](const GNode& n) { return isRoutingType(n.type); });
    if (any)
    {
        std::vector<GLink> out;
        for (const GLink& l : links)
        {
            const GNode* to = find(nodes, l.toNode);
            if (!to || isRoutingType(to->type))
                continue;
            const GNode* from = find(nodes, l.fromNode);
            if (from && isRoutingType(from->type))
            {
                if (auto src = routedSource(nodes, links, *from))
                    out.push_back({src->first, src->second, l.toNode, l.toSocket});
                continue;
            }
            out.push_back(l);
        }
        links = std::move(out);
        std::erase_if(nodes, [](const GNode& n) { return isRoutingType(n.type); });
    }
    bypassMuted(nodes, links, hooks);
}

std::map<std::string, std::string> portalProblems(const std::vector<GNode>& nodes, const std::vector<GLink>& links)
{
    std::map<std::string, std::string> out;
    std::map<std::string, int> sendCount;
    for (const GNode& n : nodes)
        if (n.type == kSendType && !portalName(n).empty())
            ++sendCount[portalName(n)];
    for (const GNode& n : nodes)
    {
        std::string name = portalName(n);
        if (n.type == kSendType)
        {
            if (name.empty())
                out[n.name] = "Name this Send so Receives can read it";
            else if (sendCount[name] > 1)
                out[n.name] = "Another Send is also named " + name;
        }
        else if (n.type == kReceiveType)
        {
            if (name.empty())
                out[n.name] = "Pick the Send to read";
            else if (!sendCount.contains(name))
                out[n.name] = "No Send named " + name;
            else if (sendCount[name] > 1)
                out[n.name] = "Several Sends are named " + name;
        }
    }
    // A Receive feeding its own Send would loop; flag every portal node on such a loop.
    std::vector<GLink> all = withPortalLinks(nodes, links);
    std::map<std::string, std::vector<std::string>> next;
    for (const GLink& l : all)
        next[l.fromNode].push_back(l.toNode);
    for (const GNode& n : nodes)
    {
        if (n.type != kReceiveType || out.contains(n.name))
            continue;
        std::vector<std::string> stack{n.name};
        std::set<std::string> seen;
        bool loops = false;
        while (!stack.empty() && !loops)
        {
            std::string cur = stack.back();
            stack.pop_back();
            for (const std::string& m : next[cur])
            {
                if (m == n.name)
                    loops = true;
                else if (seen.insert(m).second)
                    stack.push_back(m);
            }
        }
        if (loops)
            out[n.name] = "This Receive feeds its own Send " + portalName(n) + ", which would loop";
    }
    return out;
}

std::vector<GLink> withPortalLinks(const std::vector<GNode>& nodes, const std::vector<GLink>& links)
{
    std::vector<GLink> out = links;
    for (const GNode& s : nodes)
        if (s.type == kSendType && !portalName(s).empty())
            for (const GNode& r : nodes)
                if (r.type == kReceiveType && portalName(r) == portalName(s))
                    out.push_back({s.name, kPortalSocket, r.name, kPortalSocket});
    return out;
}

bool hasCycle(const std::vector<GLink>& links)
{
    std::map<std::string, std::vector<std::string>> next;
    for (const GLink& l : links)
        next[l.fromNode].push_back(l.toNode);
    std::map<std::string, int> state; // 1 visiting, 2 done
    std::function<bool(const std::string&)> visit = [&](const std::string& n) {
        int& s = state[n];
        if (s == 1)
            return true;
        if (s == 2)
            return false;
        s = 1;
        for (const std::string& m : next[n])
            if (visit(m))
                return true;
        state[n] = 2;
        return false;
    };
    for (auto& [n, list] : next)
        if (visit(n))
            return true;
    return false;
}
} // namespace lila::gn
