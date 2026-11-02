#include "scene/LightingChannels.h"

#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "scene/SceneData.h"

#include <algorithm>
#include <format>
#include <map>
#include <set>

namespace lila::scene::lightingChannels
{
namespace
{
constexpr const char* kDataKey = "lightingChannels";

int attrOf(const Node& n)
{
    const int i = n.type().attrIndex(kAttr);
    if (i < 0)
        return -1;
    const AttrDef& d = n.type().attrs[size_t(i)];
    return d.type == PlugType::String && (d.flags & AttrArray) ? i : -1;
}

void setList(const std::vector<std::string>& list)
{
    json j = json::array();
    for (const std::string& n : list)
        if (n != kDefault)
            j.push_back(n);
    data::set(kDataKey, j.empty() ? json() : j);
}
} // namespace

std::vector<std::string> names()
{
    std::vector<std::string> out{kDefault};
    const json j = data::get(kDataKey);
    if (j.is_array())
        for (const json& n : j)
            if (n.is_string() && std::find(out.begin(), out.end(), n.get<std::string>()) == out.end())
                out.push_back(n.get<std::string>());
    return out;
}

void create(const std::string& name)
{
    if (name.empty())
        throw std::invalid_argument("a lighting channel needs a name");
    std::vector<std::string> list = names();
    if (std::find(list.begin(), list.end(), name) != list.end())
        throw std::invalid_argument("there is already a lighting channel '" + name + "'");
    list.push_back(name);
    setList(list);
}

std::vector<std::string> of(const Node& n)
{
    const int i = attrOf(n);
    std::vector<std::string> out;
    if (i >= 0)
        for (const auto& [index, v] : n.elements(i))
            out.push_back(std::get<std::string>(v));
    if (out.empty())
        out.push_back(kDefault);
    return out;
}

void set(Graph& g, NodeId node, std::vector<std::string> channels)
{
    Node* n = g.node(node);
    if (!n)
        throw std::invalid_argument("no such node");
    std::vector<std::string> unique;
    for (const std::string& c : channels)
        if (!c.empty() && std::find(unique.begin(), unique.end(), c) == unique.end())
            unique.push_back(c);
    const std::vector<std::string> known = names();
    for (const std::string& c : unique)
        if (std::find(known.begin(), known.end(), c) == known.end())
            create(c);
    int i = attrOf(*n);
    const bool plain = unique.empty() || (unique.size() == 1 && unique[0] == kDefault);
    if (plain)
    {
        if (i >= 0)
            g.deleteAttr({node, i});
        return;
    }
    if (i < 0)
        i = g.addAttr(node, AttrDef{.name = kAttr, .type = PlugType::String, .defaultValue = std::string(), .flags = AttrArray | AttrHidden,
                                    .doc = "The lighting channels this light or object is in."});
    std::vector<PlugValue> values(unique.begin(), unique.end());
    g.setElements({node, i, -1}, values);
}

void rename(Graph& g, const std::string& from, const std::string& to)
{
    if (from == kDefault)
        throw std::invalid_argument("the default channel keeps its name");
    std::vector<std::string> list = names();
    auto it = std::find(list.begin(), list.end(), from);
    if (it == list.end())
        throw std::invalid_argument("no lighting channel '" + from + "'");
    if (to.empty() || std::find(list.begin(), list.end(), to) != list.end())
        throw std::invalid_argument("'" + to + "' is empty or already a channel");
    *it = to;
    setList(list);
    for (NodeId id : g.nodes())
        if (const Node* n = g.node(id); n && attrOf(*n) >= 0)
        {
            std::vector<std::string> c = of(*n);
            if (std::replace(c.begin(), c.end(), from, to); true)
                set(g, id, c);
        }
}

void remove(Graph& g, const std::string& name)
{
    if (name == kDefault)
        throw std::invalid_argument("the default channel can't be removed");
    std::vector<std::string> list = names();
    if (std::find(list.begin(), list.end(), name) == list.end())
        throw std::invalid_argument("no lighting channel '" + name + "'");
    std::erase(list, name);
    for (NodeId id : g.nodes())
        if (const Node* n = g.node(id); n && attrOf(*n) >= 0)
        {
            std::vector<std::string> c = of(*n);
            if (std::erase(c, name))
                set(g, id, c);
        }
    setList(list);
}

uint32_t mask(const Node& n)
{
    const std::vector<std::string> list = names();
    uint32_t bits = 0;
    for (const std::string& c : of(n))
    {
        const auto it = std::find(list.begin(), list.end(), c);
        const int index = it == list.end() ? 0 : int(it - list.begin());
        if (index >= kViewportMax)
            return (1u << kViewportMax) - 1;
        bits |= 1u << index;
    }
    return bits ? bits : 1u;
}

bool lights(const Node& light, const Node& object) { return (mask(light) & mask(object)) != 0; }

std::vector<NodeId> nodesWithChannels(Graph& g)
{
    std::vector<NodeId> out;
    for (NodeId id : g.nodes())
        if (const Node* n = g.node(id); n && attrOf(*n) >= 0)
            out.push_back(id);
    return out;
}

std::vector<std::string> bakeLinks(Graph& g, const std::vector<std::pair<NodeId, std::vector<NodeId>>>& links)
{
    std::map<std::set<NodeId>, std::string> channelFor;
    std::map<NodeId, std::vector<std::string>> objectChannels;
    std::vector<std::string> made;
    for (const auto& [light, objects] : links)
    {
        const Node* l = g.node(light);
        if (!l)
            continue;
        std::set<NodeId> key(objects.begin(), objects.end());
        auto it = channelFor.find(key);
        if (it == channelFor.end())
        {
            std::string name = l->name();
            const std::vector<std::string> taken = names();
            for (int n = 2; std::find(taken.begin(), taken.end(), name) != taken.end(); ++n)
                name = l->name() + std::to_string(n);
            create(name);
            made.push_back(name);
            it = channelFor.emplace(key, name).first;
        }
        set(g, light, {it->second});
        for (NodeId o : key)
            objectChannels[o].push_back(it->second);
    }
    for (auto& [object, channels] : objectChannels)
        if (g.node(object))
        {
            // Objects keep default too, so lights nothing links still reach them as before.
            channels.insert(channels.begin(), kDefault);
            set(g, object, channels);
        }
    return made;
}

Usage usage(Graph& g, const std::vector<NodeId>& nodes, bool children)
{
    std::set<NodeId> all;
    for (NodeId id : nodes)
    {
        all.insert(id);
        if (children && g.node(id))
            for (NodeId d : g.descendants(id))
                all.insert(d);
    }
    Usage u;
    std::set<std::string> used;
    for (NodeId id : all)
    {
        const Node* n = g.node(id);
        if (!n || attrOf(*n) < 0)
            continue;
        (n->type().isA("light") ? u.lights : u.objects).push_back(id);
        for (const std::string& c : of(*n))
            used.insert(c);
    }
    for (const std::string& c : names())
        if (c != kDefault && used.contains(c))
            u.channels.push_back(c);
    return u;
}

UnrealChannels unrealChannels(const Usage& u)
{
    UnrealChannels out;
    out.slots[0] = kDefault;
    for (size_t i = 0; i < u.channels.size(); ++i)
        if (i < 2)
            out.slots[i + 1] = u.channels[i];
        else
            out.dropped.push_back(u.channels[i]);
    return out;
}

std::array<bool, 3> unrealBits(const Node& n, const UnrealChannels& c)
{
    std::array<bool, 3> bits{};
    for (const std::string& name : of(n))
        for (size_t s = 0; s < 3; ++s)
            if (!c.slots[s].empty() && c.slots[s] == name)
                bits[s] = true;
    if (!bits[0] && !bits[1] && !bits[2])
        bits[0] = true;
    return bits;
}

json report(const Graph& g, const Usage& u, bool written, const std::string& note, const UnrealChannels* unreal)
{
    auto paths = [&](const std::vector<NodeId>& ids) {
        json out = json::array();
        for (NodeId id : ids)
            if (const Node* n = g.node(id))
                out.push_back(n->path());
        return out;
    };
    json j = {{"channels", u.channels}, {"lights", paths(u.lights)}, {"objects", paths(u.objects)}, {"written", written}};
    if (!note.empty())
        j["note"] = note;
    if (unreal)
    {
        j["unreal"] = unreal->slots;
        j["dropped"] = unreal->dropped;
    }
    return j;
}

std::string summary(const Usage& u)
{
    std::string list;
    for (const std::string& c : u.channels)
        list += (list.empty() ? "" : ", ") + c;
    return std::format("{} on {} light(s) and {} object(s)", list.empty() ? std::string(kDefault) : list, u.lights.size(), u.objects.size());
}

std::string describeSelection(Graph& g, bool unreal)
{
    const Usage u = usage(g, g.selection(), true);
    if (u.empty())
        return "The selection has no lighting channels.";
    std::string out = summary(u) + " in the selection.";
    if (unreal)
    {
        const UnrealChannels c = unrealChannels(u);
        out += std::format(" Unreal channel 0 is default, 1 {}, 2 {}.", c.slots[1].empty() ? "unused" : c.slots[1],
                           c.slots[2].empty() ? "unused" : c.slots[2]);
        if (!c.dropped.empty())
        {
            std::string dropped;
            for (const std::string& d : c.dropped)
                dropped += (dropped.empty() ? "" : ", ") + d;
            out += " Dropped (Unreal has three): " + dropped + ".";
        }
    }
    return out;
}

void registerCommands()
{
    using T = ArgType;
    auto guarded = [](auto&& f) {
        try
        {
            return f();
        }
        catch (const std::invalid_argument& e)
        {
            throw CommandError(e.what());
        }
    };
    ops::registerCommand({
        .name = "lightingChannels",
        .doc = "The lighting channels of lights and objects: a light lights the objects that share a channel with it (Unreal's "
               "Lighting Channels, named and as many as you need). add, remove or set them; with none of those, returns "
               "{object: [channels]}. Unknown names are created.",
        .positionalDoc = "objects",
        .flags = {{"add", "a", T::List, nullptr, "Channels to put the objects in."},
                  {"remove", "rm", T::List, nullptr, "Channels to take them out of."},
                  {"set", "s", T::List, nullptr, "Exactly these channels."}},
        .run = [guarded](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<NodeId> ids;
            for (const std::string& o : a.objects())
            {
                Node* n = g.find(o);
                if (!n)
                    throw CommandError("lightingChannels: no node named '" + o + "'");
                ids.push_back(n->id());
            }
            if (!a.has("add") && !a.has("remove") && !a.has("set"))
            {
                json out = json::object();
                for (NodeId id : ids)
                    out[g.node(id)->path()] = of(*g.node(id));
                return out;
            }
            for (NodeId id : ids)
            {
                std::vector<std::string> c = a.has("set") ? a.get<std::vector<std::string>>("set") : of(*g.node(id));
                for (const std::string& add : a.get<std::vector<std::string>>("add", {}))
                    if (std::find(c.begin(), c.end(), add) == c.end())
                        c.push_back(add);
                for (const std::string& rm : a.get<std::vector<std::string>>("remove", {}))
                    std::erase(c, rm);
                guarded([&] { set(g, id, c); return 0; });
            }
            return nullptr;
        },
    });
    ops::registerCommand({
        .name = "lightingChannel",
        .doc = "Create, rename or delete a lighting channel; with no flag, list the channels.",
        .positionalDoc = "[name]",
        .flags = {{"create", "c", T::Bool, false, "Add the channel."},
                  {"rename", "rn", T::String, nullptr, "A new name: every light and object in it follows."},
                  {"delete", "d", T::Bool, false, "Remove it; lights and objects left in no channel go back to default."}},
        .run = [guarded](const Args& a) -> json {
            Graph& g = Graph::current();
            if (a.positional.empty())
                return names();
            const std::string name = a.positional[0].get<std::string>();
            if (a.get<bool>("create"))
                guarded([&] { create(name); return 0; });
            else if (a.has("rename"))
                guarded([&] { rename(g, name, a.get<std::string>("rename")); return 0; });
            else if (a.get<bool>("delete"))
                guarded([&] { remove(g, name); return 0; });
            else
                throw CommandError("lightingChannel: give create, rename or delete");
            return names();
        },
    });

    // The export toggles' option boxes say what the selection would carry; the commands may come from extensions.
    for (const auto& [command, forUnreal] : {std::pair{"exportUsd", false}, std::pair{"unrealPlaceInstances", true}})
    {
        const ops::options::Override* existing = ops::options::findOverride(command);
        ops::options::Override o = existing ? *existing : ops::options::Override{};
        o.notes["lightingChannels"] = [unreal = forUnreal] { return describeSelection(Graph::current(), unreal); };
        ops::options::setOverride(command, std::move(o));
    }
}
} // namespace lila::scene::lightingChannels
