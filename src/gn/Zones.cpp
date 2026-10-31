#include "gn/Zones.h"

#include "gn/NodeLib.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>

namespace lila::gn
{
const std::vector<ZoneKind>& zoneKinds()
{
    static const std::vector<ZoneKind> kinds = {
        {"RepeatInput", "RepeatOutput", "RepeatGroup", "Repeat", "items", "items", true, {{"iteration", SocketType::Int}}},
        {"ForEachInput", "ForEachOutput", "ForEachElement", "For Each Element", "items", "items", true,
         {{"index", SocketType::Int}, {"count", SocketType::Int}}},
        {"SimulationInput", "SimulationOutput", "SimulationZone", "Simulation", "items", "items", true, {{"deltaTime", SocketType::Float}}},
        {"ClosureInput", "ClosureOutput", "MakeClosure", "Closure", "inputs", "outputs", false, {}},
    };
    return kinds;
}

const ZoneKind* zoneOfInput(std::string_view type)
{
    for (const ZoneKind& k : zoneKinds())
        if (type == k.input)
            return &k;
    return nullptr;
}

const ZoneKind* zoneOfOutput(std::string_view type)
{
    for (const ZoneKind& k : zoneKinds())
        if (type == k.output)
            return &k;
    return nullptr;
}

std::vector<ZoneItem> itemsOf(const GNode& n, std::string_view key)
{
    std::vector<ZoneItem> out;
    auto it = n.values.find(key);
    if (it == n.values.end() || !it->is_array())
        return out;
    for (const json& e : *it)
    {
        if (!e.is_object() || !e.contains("name") || !e["name"].is_string())
            continue;
        ZoneItem item;
        item.name = e["name"].get<std::string>();
        if (!e.contains("type") || !e["type"].is_string() || !parseSocketType(e["type"].get<std::string>(), &item.type))
            continue;
        out.push_back(std::move(item));
    }
    return out;
}

json itemsToJson(const std::vector<ZoneItem>& items)
{
    json out = json::array();
    for (const ZoneItem& i : items)
        out.push_back({{"name", i.name}, {"type", socketTypeName(i.type)}});
    return out;
}

std::string itemsProblem(const std::vector<ZoneItem>& items, const std::vector<std::string>& reserved)
{
    std::set<std::string> seen;
    for (const ZoneItem& i : items)
    {
        if (!validIdentifier(i.name) || i.name.starts_with("__"))
            return "'" + i.name + "' isn't a valid item name: use letters, digits and underscores, not a digit first";
        if (std::find(reserved.begin(), reserved.end(), i.name) != reserved.end())
            return "'" + i.name + "' is taken by the node's own socket";
        if (!seen.insert(i.name).second)
            return "two items are named '" + i.name + "'";
    }
    return {};
}

std::vector<std::string> reservedItemNames(std::string_view type)
{
    const ZoneKind* k = zoneOfOutput(type);
    if (!k)
        k = zoneOfInput(type);
    if (k)
    {
        std::vector<std::string> out = {"geometry", "iterations", "startFrame", "substeps", "closure", "output", "items"};
        for (const auto& [name, t] : k->driven)
            out.push_back(name);
        return out;
    }
    if (type == "EvaluateClosure")
        return {"closure"};
    if (type == "Bake")
        return {"geometry", "directory", "baked", "mode"};
    return {"bundle"};
}

std::vector<std::string> itemListsOf(std::string_view type)
{
    if (const ZoneKind* k = zoneOfOutput(type))
        return std::string_view(k->inputItems) == k->outputItems ? std::vector<std::string>{k->inputItems}
                                                                 : std::vector<std::string>{k->inputItems, k->outputItems};
    if (type == "EvaluateClosure")
        return {"inputs", "outputs"};
    if (type == "CombineBundle" || type == "SeparateBundle" || type == "Bake")
        return {"items"};
    return {};
}

const GNode* zoneOutputOf(const Group& g, const GNode& input)
{
    const ZoneKind* k = zoneOfInput(input.type);
    auto it = input.values.find("output");
    if (!k || it == input.values.end() || !it->is_string())
        return nullptr;
    const GNode* out = g.node(it->get<std::string>());
    return out && out->type == k->output ? out : nullptr;
}

const GNode* zoneInputOf(const Group& g, const GNode& output)
{
    for (const GNode& n : g.nodes)
        if (zoneOfInput(n.type) && zoneOutputOf(g, n) == &output)
            return &n;
    return nullptr;
}

std::vector<std::string> withZonePartners(const Group& g, std::vector<std::string> names)
{
    for (size_t i = 0, n = names.size(); i < n; ++i)
    {
        const GNode* node = g.node(names[i]);
        const GNode* partner = !node ? nullptr : zoneOfInput(node->type) ? zoneOutputOf(g, *node) : zoneOfOutput(node->type) ? zoneInputOf(g, *node) : nullptr;
        if (partner && std::find(names.begin(), names.end(), partner->name) == names.end())
            names.push_back(partner->name);
    }
    return names;
}

void retargetZoneCopy(GNode& copy, const std::map<std::string, std::string>& renamed)
{
    if (!zoneOfInput(copy.type))
        return;
    auto it = copy.values.find("output");
    if (it == copy.values.end() || !it->is_string())
        return;
    auto to = renamed.find(it->get<std::string>());
    if (to != renamed.end())
        *it = to->second;
    else
        copy.values.erase("output");
}

bool isStructuralValue(std::string_view type, std::string_view key)
{
    if (key == "output" && zoneOfInput(type))
        return true;
    for (const std::string& list : itemListsOf(type))
        if (key == list)
            return true;
    return false;
}

namespace
{
/** Names reachable from start along links, forward or backward, start excluded. */
std::set<std::string> reach(const std::vector<GLink>& links, const std::string& start, bool forward)
{
    std::set<std::string> seen;
    std::vector<std::string> stack{start};
    while (!stack.empty())
    {
        std::string cur = std::move(stack.back());
        stack.pop_back();
        for (const GLink& l : links)
        {
            const std::string& from = forward ? l.fromNode : l.toNode;
            const std::string& to = forward ? l.toNode : l.fromNode;
            if (from == cur && to != start && seen.insert(to).second)
                stack.push_back(to);
        }
    }
    return seen;
}

std::vector<std::string> bodyOf(const Group& g, const GNode& in, const GNode& out)
{
    std::set<std::string> down = reach(g.links, in.name, true), up = reach(g.links, out.name, false);
    std::vector<std::string> body;
    for (const GNode& n : g.nodes)
        if (n.name != out.name && down.contains(n.name) && up.contains(n.name))
            body.push_back(n.name);
    return body;
}

SocketType outputType(const Group& g, const GNode& n, const std::string& socket)
{
    if (const NodeDecl* d = findDecl(n.type))
    {
        Sockets s = d->socketsFor(g, n);
        if (int k = s.outputIndex(socket); k >= 0)
            return s.outputs[size_t(k)].type;
    }
    return SocketType::Float;
}

/** Lowers one zone whose body holds no other zone. */
void lowerOne(Group& g, const ZoneKind& kind, const GNode& inNode, const GNode& outNode, const std::vector<std::string>& bodyNames)
{
    const GNode in = inNode, out = outNode;
    std::set<std::string> body(bodyNames.begin(), bodyNames.end());
    const std::vector<ZoneItem> inItems = itemsOf(out, kind.inputItems), outItems = itemsOf(out, kind.outputItems);

    Group sub = Group::makeDefault("__zone_" + out.name);
    sub.links.clear();
    const std::string subIn = "GroupInput", subOut = "GroupOutput";
    for (const auto& [name, type] : kind.driven)
        sub.inputs.push_back({.name = name, .type = type});
    for (const ZoneItem& i : inItems)
        sub.inputs.push_back({.name = i.name, .type = i.type});
    for (const ZoneItem& i : outItems)
        sub.outputs.push_back({.name = i.name, .type = i.type});
    for (const GNode& n : g.nodes)
        if (body.contains(n.name))
            sub.nodes.push_back(n);

    GNode runner{out.name, kind.runner, out.position, json::object()};
    runner.values["group"] = sub.name;
    for (auto it = in.values.begin(); it != in.values.end(); ++it)
        if (it.key() != "output")
            runner.values[it.key()] = it.value();

    std::vector<GLink> outer;
    std::map<std::pair<std::string, std::string>, std::string> captured;
    auto capture = [&](const GLink& l) {
        auto key = std::make_pair(l.fromNode, l.fromSocket);
        auto it = captured.find(key);
        if (it != captured.end())
            return it->second;
        std::string name = "__capture" + std::to_string(captured.size());
        const GNode* src = g.node(l.fromNode);
        sub.inputs.push_back({.name = name, .type = src ? outputType(g, *src, l.fromSocket) : SocketType::Float});
        outer.push_back({l.fromNode, l.fromSocket, runner.name, name});
        captured[key] = name;
        return name;
    };
    for (const GLink& l : g.links)
    {
        const bool fromIn = l.fromNode == in.name, toOut = l.toNode == out.name;
        const bool fromBody = body.contains(l.fromNode), toBody = body.contains(l.toNode);
        if (l.toNode == in.name)
            outer.push_back({l.fromNode, l.fromSocket, runner.name, l.toSocket});
        else if (l.fromNode == out.name)
            outer.push_back(l);
        else if (toBody || toOut)
        {
            const std::string to = toOut ? subOut : l.toNode;
            if (fromIn)
                sub.links.push_back({subIn, l.fromSocket, to, l.toSocket});
            else if (fromBody)
                sub.links.push_back({l.fromNode, l.fromSocket, to, l.toSocket});
            else
                sub.links.push_back({subIn, capture(l), to, l.toSocket});
        }
        else if (!fromIn && !fromBody)
            outer.push_back(l);
        // Links leaving the zone anywhere but its output are dropped: the zone's values only exist inside it.
    }

    for (const std::string& r : referencedGroups(sub))
        if (auto it = g.subgroups.find(r); it != g.subgroups.end())
            sub.subgroups[r] = it->second;
    json definition = sub.toJson();
    if (std::string_view(kind.runner) == "SimulationZone")
        runner.values["definition"] = definition;
    g.subgroups[sub.name] = std::move(definition);

    std::erase_if(g.nodes, [&](const GNode& n) { return n.name == in.name || n.name == out.name || body.contains(n.name); });
    g.nodes.push_back(std::move(runner));
    g.links = std::move(outer);
}
} // namespace

std::vector<ZoneRegion> zoneRegions(const Group& g)
{
    std::vector<ZoneRegion> out;
    for (const GNode& n : g.nodes)
        if (const ZoneKind* k = zoneOfInput(n.type))
            if (const GNode* o = zoneOutputOf(g, n))
                out.push_back({n.name, o->name, k, bodyOf(g, n, *o)});
    return out;
}

std::string lowerZones(Group& g)
{
    for (;;)
    {
        std::vector<ZoneRegion> zones = zoneRegions(g);
        for (const GNode& n : g.nodes)
        {
            if (zoneOfInput(n.type) && !zoneOutputOf(g, n))
                return n.name + " has no zone output to pair with";
            if (zoneOfOutput(n.type) && !zoneInputOf(g, n))
                return n.name + " has no zone input to pair with";
        }
        if (zones.empty())
            return {};
        const ZoneRegion* inner = nullptr;
        for (const ZoneRegion& z : zones)
        {
            bool nested = std::any_of(z.body.begin(), z.body.end(), [&](const std::string& b) {
                const GNode* n = g.node(b);
                return n && zoneOfInput(n->type);
            });
            if (!nested)
            {
                inner = &z;
                break;
            }
        }
        if (!inner)
            return "zones " + zones[0].input + " and the zones inside it overlap";
        const GNode* in = g.node(inner->input);
        const GNode* out = g.node(inner->output);
        if (std::string problem = itemsProblem(itemsOf(*out, inner->kind->inputItems)); !problem.empty())
            return out->name + ": " + problem;
        lowerOne(g, *inner->kind, *in, *out, inner->body);
    }
}
} // namespace lila::gn
