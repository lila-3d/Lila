#include "scene/ObjectSets.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "scene/Graph.h"

#include <algorithm>
#include <charconv>
#include <map>
#include <set>

namespace lila::scene::objectSets
{
namespace
{
using T = ArgType;

/**
 * The stored member list. Entries remember the node's id and name: the id is authoritative while the node lives, the
 * name binds entries read from a file and rebinds referenced members after their reference reloads.
 */
struct SetMembers final : Data
{
    struct Entry
    {
        // Binding cache, refreshed on the main thread by bind(); the entry's meaning never changes.
        mutable NodeId id = kNoNode;
        mutable std::string name;
        mutable bool referenced = false;
        ComponentMode kind = ComponentMode::Object;
        components::Ids ids;
    };
    std::vector<Entry> entries;

    const char* kind() const override { return kMembersKind; }
    json toJson() const override;
    size_t bytes() const override
    {
        size_t n = 64;
        for (const Entry& e : entries)
            n += sizeof(Entry) + e.name.size() + e.ids.size() * sizeof(uint32_t);
        return n;
    }
    static DataPtr fromJson(const json& j);
};

const char* kindKey(ComponentMode k)
{
    switch (k)
    {
    case ComponentMode::Vertex: return "vertices";
    case ComponentMode::Edge: return "edges";
    case ComponentMode::Face: return "faces";
    default: return "";
    }
}

const char* kindSuffix(ComponentMode k)
{
    switch (k)
    {
    case ComponentMode::Vertex: return "vtx";
    case ComponentMode::Edge: return "e";
    case ComponentMode::Face: return "f";
    default: return "";
    }
}

/** Looks the entry's node up by name: relative to the set's namespace first, as files from a reference store them. */
const Node* findByName(const Graph& g, const std::string& setNamespace, const std::string& name)
{
    if (!setNamespace.empty())
        if (const Node* n = g.find(setNamespace + ":" + name))
            return n;
    return g.find(name);
}

/** The live node an entry means, or null. Read only, so it is safe from the parallel scene serializer. */
const Node* lookup(const Graph& g, const std::string& setNamespace, const SetMembers::Entry& e)
{
    if (e.id != kNoNode)
        if (const Node* n = g.node(e.id))
            return n;
    // A deleted scene node stays deleted; only file entries and reloaded references bind by name.
    if (e.id != kNoNode && !e.referenced)
        return nullptr;
    const Node* n = findByName(g, setNamespace, e.name);
    if (n && e.id != kNoNode && n->referenceId() == 0)
        return nullptr;
    return n;
}

/** lookup() that also refreshes the entry's cache. Main thread. */
const Node* bind(const Graph& g, const std::string& setNamespace, const SetMembers::Entry& e)
{
    const Node* n = lookup(g, setNamespace, e);
    if (n)
    {
        e.id = n->id();
        e.name = n->name();
        e.referenced = n->referenceId() != 0;
    }
    return n;
}

json SetMembers::toJson() const
{
    const Graph& g = Graph::current();
    json list = json::array();
    for (const Entry& e : entries)
    {
        const Node* n = e.id != kNoNode ? g.node(e.id) : nullptr;
        std::string name;
        if (n)
            name = n->name();
        else if (e.id == kNoNode || e.referenced)
            name = e.name; // unbound or its reference is unloaded: keep it for when it comes back
        else
            continue;
        json m = {{"node", name}};
        if (e.kind != ComponentMode::Object)
            m[kindKey(e.kind)] = e.ids;
        list.push_back(std::move(m));
    }
    return {{"kind", kMembersKind}, {"members", std::move(list)}};
}

DataPtr SetMembers::fromJson(const json& j)
{
    auto out = std::make_shared<SetMembers>();
    for (const json& m : j.value("members", json::array()))
    {
        if (!m.is_object() || !m.contains("node") || !m["node"].is_string())
            continue;
        Entry e;
        e.name = m["node"].get<std::string>();
        for (ComponentMode k : {ComponentMode::Vertex, ComponentMode::Edge, ComponentMode::Face})
            if (m.contains(kindKey(k)) && m[kindKey(k)].is_array())
            {
                e.kind = k;
                for (const json& id : m[kindKey(k)])
                    if (id.is_number_unsigned() || id.is_number_integer())
                        e.ids.push_back(id.get<uint32_t>());
            }
        out->entries.push_back(std::move(e));
    }
    return out;
}

const SetMembers* stored(const Graph& g, NodeId set)
{
    const Node* n = g.node(set);
    if (!n || !isSet(*n))
        return nullptr;
    const DataPtr& d = n->as<DataPtr>(ObjectSet::kMembers);
    return d ? dynamic_cast<const SetMembers*>(d.get()) : nullptr;
}

/** Live members as (node, kind) -> ids, in joining order. */
std::vector<Member> resolve(const Graph& g, NodeId set)
{
    std::vector<Member> out;
    const SetMembers* s = stored(g, set);
    if (!s)
        return out;
    const std::string& ns = g.node(set)->nameSpace();
    for (const SetMembers::Entry& e : s->entries)
        if (const Node* n = bind(g, ns, e))
            out.push_back({n->id(), e.kind, e.ids});
    return out;
}

void normalize(components::Ids& ids)
{
    std::ranges::sort(ids);
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
}

/** Writes a member list as the set's value, as one undoable set. Live members only. */
void store(Graph& g, NodeId set, const std::vector<Member>& list)
{
    auto d = std::make_shared<SetMembers>();
    // Members of an unloaded reference aren't live, but still belong to the set.
    if (const SetMembers* old = stored(g, set))
        for (const SetMembers::Entry& e : old->entries)
            if ((e.id == kNoNode || e.referenced) && !lookup(g, g.node(set)->nameSpace(), e))
                d->entries.push_back(e);
    for (const Member& m : list)
    {
        const Node* n = g.node(m.node);
        if (!n)
            continue;
        SetMembers::Entry e;
        e.id = n->id();
        e.name = n->name();
        e.referenced = n->referenceId() != 0;
        e.kind = m.kind;
        e.ids = m.ids;
        d->entries.push_back(std::move(e));
    }
    g.setValue({set, ObjectSet::kMembers, -1}, DataPtr(std::move(d)));
}

std::vector<Member> merged(std::vector<Member> list, const std::vector<Member>& extra)
{
    for (Member m : extra)
    {
        normalize(m.ids);
        if (m.kind != ComponentMode::Object && m.ids.empty())
            continue;
        auto it = std::ranges::find_if(list, [&](const Member& x) { return x.node == m.node && x.kind == m.kind; });
        if (it == list.end())
            list.push_back(std::move(m));
        else if (m.kind != ComponentMode::Object)
        {
            it->ids.insert(it->ids.end(), m.ids.begin(), m.ids.end());
            normalize(it->ids);
        }
    }
    return list;
}

Node* setArg(const Graph& g, const std::string& name, const char* cmd)
{
    Node* n = g.find(name);
    if (!n || !isSet(*n))
        throw CommandError(std::string(cmd) + ": " + name + " is not a set");
    return n;
}

std::vector<Member> membersArg(const Graph& g, const std::vector<std::string>& items, const char* cmd)
{
    try
    {
        return parse(g, items);
    }
    catch (const std::invalid_argument& e)
    {
        throw CommandError(std::string(cmd) + ": " + e.what());
    }
}

/** The first positional names the set; the rest are members, the selection when there are none. */
std::pair<Node*, std::vector<Member>> setAndMembers(const Args& a, const char* cmd)
{
    Graph& g = Graph::current();
    if (a.positional.empty() || !a.positional[0].is_string())
        throw CommandError(std::string(cmd) + ": name the set first");
    Node* set = setArg(g, a.positional[0].get<std::string>(), cmd);
    Args rest = a;
    rest.positional.erase(rest.positional.begin());
    std::vector<std::string> items;
    for (const json& v : rest.positional)
        if (v.is_string())
            items.push_back(v.get<std::string>());
        else if (v.is_array())
            for (const json& x : v)
                if (x.is_string())
                    items.push_back(x.get<std::string>());
    return {set, items.empty() ? fromSelection(g) : membersArg(g, items, cmd)};
}

json namesOf(const Graph& g, const std::vector<NodeId>& ids)
{
    json out = json::array();
    for (NodeId id : ids)
        out.push_back(g.node(id)->name());
    return out;
}

std::vector<NodeId> setsArg(const Args& a, const char* cmd)
{
    Graph& g = Graph::current();
    std::vector<NodeId> out;
    for (const json& v : a.positional)
        if (v.is_string())
            out.push_back(setArg(g, v.get<std::string>(), cmd)->id());
        else if (v.is_array())
            for (const json& x : v)
                if (x.is_string())
                    out.push_back(setArg(g, x.get<std::string>(), cmd)->id());
    if (out.empty())
        for (NodeId id : g.selection())
            if (isSet(*g.node(id)))
                out.push_back(id);
    if (out.empty())
        throw CommandError(std::string(cmd) + ": name a set");
    return out;
}

bool parseIndex(std::string_view s, uint32_t& out)
{
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && p == s.data() + s.size();
}
} // namespace

bool isSet(const Node& n) { return n.type().isA(kTypeName); }

std::vector<Node*> all(const Graph& g, std::optional<bool> quickSelect)
{
    std::vector<NodeId> ids = g.nodes();
    std::ranges::sort(ids);
    std::vector<Node*> out;
    for (NodeId id : ids)
        if (Node* n = g.node(id); isSet(*n) && (!quickSelect || n->as<bool>(ObjectSet::kQuickSelect) == *quickSelect))
            out.push_back(n);
    return out;
}

std::vector<Member> members(const Graph& g, NodeId set) { return resolve(g, set); }

std::vector<NodeId> nodesOf(const Graph& g, NodeId set)
{
    std::vector<NodeId> out;
    for (const Member& m : resolve(g, set))
        if (std::ranges::find(out, m.node) == out.end())
            out.push_back(m.node);
    return out;
}

std::vector<NodeId> setsOf(const Graph& g, NodeId node)
{
    std::vector<NodeId> out;
    for (const Node* s : all(g))
        if (std::ranges::any_of(resolve(g, s->id()), [&](const Member& m) { return m.node == node; }))
            out.push_back(s->id());
    return out;
}

std::vector<Member> fromSelection(const Graph& g)
{
    std::vector<Member> out;
    const ComponentMode mode = components::mode();
    if (mode != ComponentMode::Object)
        for (const auto& [node, ids] : components::selected(mode))
            if (!ids.empty() && g.node(node))
                out.push_back({node, mode, ids});
    if (!out.empty())
        return out;
    for (NodeId id : g.selection())
        if (const Node* n = g.node(id); n && !isSet(*n))
            out.push_back({id});
    return out;
}

std::vector<Member> parse(const Graph& g, const std::vector<std::string>& items)
{
    std::vector<Member> out;
    for (const std::string& item : items)
    {
        if (const Node* n = g.find(item))
        {
            if (isSet(*n))
                throw std::invalid_argument(item + " is a set; sets can't hold sets");
            out.push_back({n->id()});
            continue;
        }
        const size_t dot = item.rfind('.');
        const size_t open = item.find('[', dot == std::string::npos ? 0 : dot);
        if (dot == std::string::npos || open == std::string::npos || item.back() != ']')
            throw std::invalid_argument("no object named " + item);
        const std::string nodeName = item.substr(0, dot);
        const std::string suffix = item.substr(dot + 1, open - dot - 1);
        const Node* n = g.find(nodeName);
        if (!n)
            throw std::invalid_argument("no object named " + nodeName);
        if (!n->type().isA("mesh"))
            throw std::invalid_argument(nodeName + " is not a mesh, so it has no components");
        ComponentMode kind = suffix == "vtx" ? ComponentMode::Vertex
                             : suffix == "e" ? ComponentMode::Edge
                             : suffix == "f" ? ComponentMode::Face
                                             : ComponentMode::Object;
        if (kind == ComponentMode::Object)
            throw std::invalid_argument("unknown component '" + suffix + "' in " + item + " (vtx, e or f)");
        const std::string_view range = std::string_view(item).substr(open + 1, item.size() - open - 2);
        const size_t colon = range.find(':');
        uint32_t first = 0, last = 0;
        if (!parseIndex(range.substr(0, colon), first) ||
            !parseIndex(colon == std::string_view::npos ? range : range.substr(colon + 1), last) || last < first)
            throw std::invalid_argument("bad component range in " + item);
        Member m{n->id(), kind, {}};
        for (uint32_t i = first; i <= last; ++i)
            m.ids.push_back(i);
        out = merged(std::move(out), {m});
    }
    return out;
}

std::vector<std::string> format(const Graph& g, const std::vector<Member>& list)
{
    std::vector<std::string> out;
    for (const Member& m : list)
    {
        const Node* n = g.node(m.node);
        if (!n)
            continue;
        if (m.kind == ComponentMode::Object)
        {
            out.push_back(n->name());
            continue;
        }
        const std::string prefix = n->name() + "." + kindSuffix(m.kind) + "[";
        for (size_t i = 0; i < m.ids.size();)
        {
            size_t j = i;
            while (j + 1 < m.ids.size() && m.ids[j + 1] == m.ids[j] + 1)
                ++j;
            out.push_back(prefix + std::to_string(m.ids[i]) + (j > i ? ":" + std::to_string(m.ids[j]) : "") + "]");
            i = j + 1;
        }
    }
    return out;
}

Node* create(Graph& g, std::string_view name, const std::vector<Member>& list, bool quickSelect)
{
    Node* set = g.createNode(*types::find(kTypeName), name.empty() ? "set1" : name);
    if (quickSelect)
        g.setValue({set->id(), ObjectSet::kQuickSelect, -1}, true);
    if (!list.empty())
        store(g, set->id(), merged({}, list));
    return set;
}

void add(Graph& g, NodeId set, const std::vector<Member>& list)
{
    LILA_ZONE();

    std::vector<Member> before = resolve(g, set);
    std::vector<Member> after = merged(before, list);
    if (after != before)
        store(g, set, after);
}

void remove(Graph& g, NodeId set, const std::vector<Member>& list)
{
    LILA_ZONE();

    std::vector<Member> before = resolve(g, set);
    std::vector<Member> after = before;
    for (const Member& r : list)
    {
        if (r.kind == ComponentMode::Object)
        {
            std::erase_if(after, [&](const Member& m) { return m.node == r.node; });
            continue;
        }
        for (Member& m : after)
            if (m.node == r.node && m.kind == r.kind)
                std::erase_if(m.ids, [&](uint32_t id) { return std::ranges::binary_search(r.ids, id); });
    }
    std::erase_if(after, [](const Member& m) { return m.kind != ComponentMode::Object && m.ids.empty(); });
    if (after != before)
        store(g, set, after);
}

void select(Graph& g, const std::vector<NodeId>& sets, bool add)
{
    LILA_ZONE();

    std::vector<NodeId> objects = add ? g.selection() : std::vector<NodeId>{};
    std::map<NodeId, components::Ids> picked[3];
    if (add)
        for (int k = 0; k < 3; ++k)
            picked[k] = components::selected(ComponentMode(k + 1));
    ComponentMode firstKind = ComponentMode::Object;
    for (NodeId set : sets)
        for (const Member& m : resolve(g, set))
        {
            if (std::ranges::find(objects, m.node) == objects.end())
                objects.push_back(m.node);
            if (m.kind == ComponentMode::Object)
                continue;
            if (firstKind == ComponentMode::Object)
                firstKind = m.kind;
            components::Ids& ids = picked[int(m.kind) - 1][m.node];
            ids.insert(ids.end(), m.ids.begin(), m.ids.end());
            normalize(ids);
        }
    g.select(objects, SelectMode::Replace);
    components::setAll(std::move(picked[0]), std::move(picked[1]), std::move(picked[2]));
    if (firstKind != ComponentMode::Object)
    {
        // Stay in the current component mode when the set has components of that kind.
        const ComponentMode now = components::mode();
        bool hasNow = false;
        for (NodeId set : sets)
            for (const Member& m : resolve(g, set))
                hasNow |= m.kind == now;
        components::setMode(hasNow ? now : firstKind);
    }
    else if (!add)
        components::setMode(ComponentMode::Object);
}

void meshRemapped(Graph& g, NodeId mesh, const geo::MeshRemap& remap)
{
    for (const Node* s : all(g))
    {
        std::vector<Member> list = resolve(g, s->id());
        bool changed = false;
        for (Member& m : list)
        {
            if (m.node != mesh || m.kind == ComponentMode::Object)
                continue;
            const std::vector<geo::Index>& map = m.kind == ComponentMode::Vertex ? remap.points
                                                 : m.kind == ComponentMode::Edge ? remap.edges
                                                                                 : remap.faces;
            components::Ids ids;
            for (uint32_t i : m.ids)
                if (i < map.size() && map[i] != geo::kInvalid)
                    ids.push_back(map[i]);
            normalize(ids);
            if (ids != m.ids)
            {
                m.ids = std::move(ids);
                changed = true;
            }
        }
        if (changed)
        {
            std::erase_if(list, [](const Member& m) { return m.kind != ComponentMode::Object && m.ids.empty(); });
            store(g, s->id(), list);
        }
    }
}

json info(const Graph& g, NodeId set)
{
    const Node* n = g.node(set);
    return {
        {"name", n->name()},
        {"quickSelect", n->as<bool>(ObjectSet::kQuickSelect)},
        {"members", format(g, resolve(g, set))},
    };
}

void registerTypes()
{
    if (types::find(kTypeName))
        return;
    data::registerKind(kMembersKind, &SetMembers::fromJson);
    NodeType t;
    t.name = kTypeName;
    t.icon = "list-details";
    t.doc = "A Maya object set: a named list of objects and mesh components. Quick select sets select theirs from Edit > "
            "Quick Select Sets; the game exporter and the Unreal link export a set as one unit.";
    t.ownAttrs = {
        {.name = "quickSelect", .shortName = "qs", .type = PlugType::Bool, .defaultValue = false,
         .flags = AttrInput | AttrStorable, .doc = "List it under Edit > Quick Select Sets."},
        {.name = "members", .shortName = "dsm", .type = PlugType::Data, .defaultValue = DataPtr(),
         .flags = AttrInput | AttrStorable | AttrHidden, .doc = "The objects and components in the set."},
    };
    t.create = [] { return std::make_unique<ObjectSet>(); };
    types::registerType(std::move(t));
}

void registerCommands()
{
    if (ops::find("createSet"))
        return;

    ops::registerCommand({
        .name = "createSet",
        .doc = "Create an object set holding the members (the selected components in component mode, else the selected "
               "objects). Members are object names or components like pCube1.vtx[0:3], pCube1.e[5], pCube1.f[2]. Returns "
               "its name.",
        .positionalDoc = "members",
        .flags = {{"name", "n", T::String, "", "Name; set1 when empty, made unique."},
                  {"quickSelect", "qs", T::Bool, false, "Make it a quick select set (Edit > Quick Select Sets)."},
                  {"empty", "e", T::Bool, false, "Make it with no members, whatever is selected."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<Member> list;
            if (!a.get<bool>("empty", false))
            {
                std::vector<std::string> items;
                for (const json& v : a.positional)
                    if (v.is_string())
                        items.push_back(v.get<std::string>());
                list = items.empty() ? fromSelection(g) : membersArg(g, items, "createSet");
            }
            try
            {
                return create(g, a.get<std::string>("name", ""), list, a.get<bool>("quickSelect", false))->name();
            }
            catch (const std::invalid_argument& e)
            {
                throw CommandError(std::string("createSet: ") + e.what());
            }
        },
    });

    ops::registerCommand({
        .name = "addToSet",
        .doc = "Add members (the selection by default) to a set. Returns the set's members.",
        .positionalDoc = "set, members",
        .run = [](const Args& a) -> json {
            auto [set, list] = setAndMembers(a, "addToSet");
            Graph& g = Graph::current();
            add(g, set->id(), list);
            return format(g, members(g, set->id()));
        },
    });

    ops::registerCommand({
        .name = "removeFromSet",
        .doc = "Take members (the selection by default) out of a set. An object takes its components with it. Returns "
               "the set's members.",
        .positionalDoc = "set, members",
        .run = [](const Args& a) -> json {
            auto [set, list] = setAndMembers(a, "removeFromSet");
            Graph& g = Graph::current();
            remove(g, set->id(), list);
            return format(g, members(g, set->id()));
        },
    });

    ops::registerCommand({
        .name = "selectSetMembers",
        .doc = "Select what sets hold: their objects, and their components in component mode. Returns the members.",
        .positionalDoc = "sets",
        .flags = {{"add", "", T::Bool, false, "Add them to the selection instead of replacing it."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<NodeId> sets = setsArg(a, "selectSetMembers");
            select(g, sets, a.get<bool>("add", false));
            json out = json::array();
            for (NodeId s : sets)
                for (const std::string& m : format(g, members(g, s)))
                    out.push_back(m);
            return out;
        },
    });

    ops::registerCommand({
        .name = "deleteSet",
        .doc = "Delete sets; their members stay.",
        .positionalDoc = "sets",
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            for (NodeId id : setsArg(a, "deleteSet"))
                g.deleteNode(id);
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "listSets",
        .doc = "Every object set's name in creation order. quickSelect=True lists only quick select sets, False only plain "
               "ones; object= lists the sets that hold that object. info=True returns {name, quickSelect, members} each.",
        .flags = {{"quickSelect", "qs", T::Bool, nullptr, "Only quick select sets (True) or only plain sets (False)."},
                  {"object", "o", T::String, nullptr, "Only sets holding this object, whole or by its components."},
                  {"info", "i", T::Bool, false, "Return each set as a dict instead of its name."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::optional<bool> quick;
            if (a.has("quickSelect"))
                quick = a.get<bool>("quickSelect");
            std::vector<NodeId> holding;
            if (a.has("object"))
            {
                const Node* n = g.find(a.get<std::string>("object"));
                if (!n)
                    throw CommandError("listSets: no object named " + a.get<std::string>("object"));
                holding = setsOf(g, n->id());
            }
            json out = json::array();
            for (const Node* s : all(g, quick))
            {
                if (a.has("object") && std::ranges::find(holding, s->id()) == holding.end())
                    continue;
                out.push_back(a.get<bool>("info", false) ? info(g, s->id()) : json(s->name()));
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "setMembers",
        .doc = "A set's members as names and components (pCube1, pCube1.vtx[0:3]). nodes=True gives the distinct objects "
               "instead, component owners included.",
        .positionalDoc = "set",
        .flags = {{"nodes", "", T::Bool, false, "Return the objects the set touches instead of its members."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (a.positional.empty() || !a.positional[0].is_string())
                throw CommandError("setMembers: name the set");
            Node* set = setArg(g, a.positional[0].get<std::string>(), "setMembers");
            if (a.get<bool>("nodes", false))
                return namesOf(g, nodesOf(g, set->id()));
            return format(g, members(g, set->id()));
        },
    });
}
} // namespace lila::scene::objectSets
