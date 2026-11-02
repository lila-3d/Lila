#include "scene/AttrComponents.h"

#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "core/Paths.h"

#include <map>

namespace lila::scene
{
namespace attrCommandsDetail
{
using lila::Args;
using lila::CommandError;
using T = lila::ArgType;

Graph& graph() { return Graph::current(); }

template <class F> auto guarded(F&& f)
{
    try
    {
        return f();
    }
    catch (const std::invalid_argument& e)
    {
        throw CommandError(e.what());
    }
    catch (const std::runtime_error& e)
    {
        throw CommandError(e.what());
    }
}

Node& nodeArg(const std::string& name)
{
    Node* n = guarded([&] { return graph().find(name); });
    if (!n)
        throw CommandError("no node named '" + name + "'");
    return *n;
}

PlugRef plugArg(const std::string& path)
{
    return guarded([&] { return graph().plug(path); });
}

/** A whole attr: plugArg without its component or element. */
PlugRef attrArg(const std::string& path)
{
    PlugRef p = plugArg(path);
    return {p.node, p.attr};
}

/** The type names editAttr and the Add popover take, with whether each is an array of the type. */
struct TypeChoice
{
    PlugType type;
    uint32_t flags;
};
const std::map<std::string, TypeChoice>& typeChoices()
{
    static const std::map<std::string, TypeChoice> m = {
        {"bool", {PlugType::Bool, 0}},     {"int", {PlugType::Int, 0}},       {"float", {PlugType::Float, 0}},
        {"enum", {PlugType::Enum, 0}},     {"string", {PlugType::String, 0}}, {"vector", {PlugType::Vec3, 0}},
        {"vec3", {PlugType::Vec3, 0}},     {"color", {PlugType::Vec3, AttrColor}}, {"matrix", {PlugType::Matrix, 0}},
        {"file", {PlugType::String, AttrFile}}, {"text", {PlugType::String, AttrMultiline}},
    };
    return m;
}

/** The editAttr type name a definition reads as. */
std::string typeChoiceOf(const AttrDef& d)
{
    if (d.type == PlugType::Vec3)
        return d.flags & AttrColor ? "color" : "vector";
    if (d.type == PlugType::String)
        return d.flags & AttrFile ? "file" : d.flags & AttrMultiline ? "text" : "string";
    return typeName(d.type);
}

std::vector<std::string> splitEnum(const std::string& names)
{
    std::vector<std::string> out;
    for (size_t start = 0; start <= names.size();)
    {
        size_t end = names.find(':', start);
        std::string n = names.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!n.empty())
            out.push_back(n);
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return out;
}

/** Applies editAttr's flags to a copy of def. */
AttrDef edited(AttrDef def, const Args& a)
{
    if (a.has("longName"))
        def.name = a.get<std::string>("longName");
    if (a.has("shortName"))
        def.shortName = a.get<std::string>("shortName");
    if (a.has("niceName"))
        def.niceName = a.get<std::string>("niceName");
    if (a.has("doc"))
        def.doc = a.get<std::string>("doc");
    if (a.has("category"))
        def.category = a.get<std::string>("category");
    if (a.has("type"))
    {
        auto it = typeChoices().find(a.get<std::string>("type"));
        if (it == typeChoices().end())
            throw CommandError("editAttr: unknown type '" + a.get<std::string>("type") + "': bool, int, float, enum, string, vector, color, matrix, file or text");
        def.flags &= ~uint32_t(AttrColor | AttrFile | AttrMultiline);
        def.flags |= it->second.flags;
        if (def.type != it->second.type)
        {
            def.type = it->second.type;
            def.defaultValue = defaultFor(def.type);
        }
    }
    if (a.has("array"))
        def.flags = a.get<bool>("array") ? def.flags | AttrArray : def.flags & ~uint32_t(AttrArray);
    if (a.has("enumName"))
        def.enumNames = splitEnum(a.get<std::string>("enumName"));
    if (def.type == PlugType::Enum && def.enumNames.empty())
        def.enumNames = {"off", "on"};
    if (a.has("defaultValue"))
        def.defaultValue = guarded([&] { return fromJson(def, a.get<json>("defaultValue")); });
    for (auto [flag, slot] : {std::pair{"minValue", &def.minValue}, {"maxValue", &def.maxValue}, {"softMinValue", &def.softMin},
                              {"softMaxValue", &def.softMax}})
        if (a.has(flag))
        {
            const json v = a.get<json>(flag);
            if (v.is_boolean() && !v.get<bool>())
                slot->reset();
            else if (v.is_number())
                *slot = v.get<double>();
            else
                throw CommandError(std::string("editAttr: ") + flag + " takes a number, or False to remove it");
        }
    for (auto [flag, bit] : {std::pair{"keyable", AttrKeyable}, {"channelBox", AttrChannelBox}, {"hidden", AttrHidden}})
        if (a.has(flag))
            def.flags = a.get<bool>(flag) ? def.flags | bit : def.flags & ~uint32_t(bit);
    return def;
}

json connectionsOf(const PlugRef& p, bool sources, bool destinations, bool plugs, bool pairs, const std::string& type)
{
    Graph& g = graph();
    json out = json::array();
    auto keep = [&](const PlugRef& other) {
        const Node* n = g.node(other.node);
        return n && (type.empty() || n->type().isA(type));
    };
    auto push = [&](const PlugRef& mine, const PlugRef& other) {
        if (!keep(other))
            return;
        if (pairs)
            out.push_back(g.plugName(mine));
        out.push_back(plugs ? g.plugName(other) : g.node(other.node)->path());
    };
    for (const auto& [dst, src] : g.connections())
    {
        const bool mineDst = dst.node == p.node && (p.attr < 0 || dst.attr == p.attr);
        const bool mineSrc = src.node == p.node && (p.attr < 0 || src.attr == p.attr);
        if (sources && mineDst)
            push(dst, src);
        if (destinations && mineSrc)
            push(src, dst);
    }
    return out;
}
} // namespace attrCommandsDetail

namespace attrCommands
{
using namespace attrCommandsDetail;

void registerCommands()
{
    ops::registerCommand({
        .name = "renameAttr",
        .doc = "Rename an added attribute. Connections, keys and drivers stay on it. Returns the renamed plug.",
        .positionalDoc = "plug newName",
        .run = [](const Args& a) -> json {
            if (a.positional.size() != 2)
                throw CommandError("renameAttr: expected (plug, newName)");
            PlugRef p = attrArg(a.positional[0].get<std::string>());
            AttrDef def = graph().attr(p);
            def.name = a.positional[1].get<std::string>();
            guarded([&] { graph().editAttr(p, def); return 0; });
            return graph().plugName(p);
        },
    });

    ops::registerCommand({
        .name = "editAttr",
        .doc = "Change an added attribute: name, label, type (values convert), default, ranges, enum names, keyable, Channel Box, "
               "hidden, doc, heading. Changing the type drops its connections. With query=True, returns its settings instead.",
        .positionalDoc = "plugs",
        .flags = {
            {"longName", "ln", T::String, nullptr, "A new name; connections, keys and drivers stay on it."},
            {"shortName", "sn", T::String, nullptr, "A short name."},
            {"niceName", "nn", T::String, nullptr, "The label editors show; empty makes one from the name."},
            {"type", "typ", T::String, nullptr, "bool, int, float, enum, string, vector, color, matrix, file or text.",
             {"bool", "int", "float", "enum", "string", "vector", "color", "matrix", "file", "text"}},
            {"array", "a", T::Bool, nullptr, "Any number of values, attr[0], attr[1] ..."},
            {"defaultValue", "dv", T::Any, nullptr, "The value it resets to."},
            {"minValue", "min", T::Any, nullptr, "Lowest value it takes; False removes it."},
            {"maxValue", "max", T::Any, nullptr, "Highest value it takes; False removes it."},
            {"softMinValue", "smn", T::Any, nullptr, "Slider minimum; False removes it."},
            {"softMaxValue", "smx", T::Any, nullptr, "Slider maximum; False removes it."},
            {"enumName", "en", T::String, nullptr, "Enum labels in order, 'low:medium:high'."},
            {"keyable", "k", T::Bool, nullptr, "Shown in the Channel Box and keyable."},
            {"channelBox", "cb", T::Bool, nullptr, "Shown in the Channel Box although not keyable."},
            {"hidden", "h", T::Bool, nullptr, "Hidden from editors."},
            {"doc", "", T::String, nullptr, "What it is for: editors show it as a tooltip."},
            {"category", "", T::String, nullptr, "The heading it shows under (its component); empty for none."},
            {"query", "q", T::Bool, false, "Return the settings of the first plug."},
        },
        .run = [](const Args& a) -> json {
            if (a.positional.empty())
                throw CommandError("editAttr: expected a plug");
            if (a.get<bool>("query"))
            {
                const AttrDef& d = graph().attr(attrArg(a.positional[0].get<std::string>()));
                json j = attrDefToJson(d);
                j["type"] = typeChoiceOf(d);
                j["user"] = (d.flags & AttrUser) != 0;
                j["channelBox"] = (d.flags & AttrChannelBox) != 0;
                j["default"] = toJson(d.defaultValue);
                return j;
            }
            json out = json::array();
            for (const json& v : a.positional)
            {
                PlugRef p = attrArg(v.get<std::string>());
                AttrDef def = edited(graph().attr(p), a);
                guarded([&] { graph().editAttr(p, def); return 0; });
                out.push_back(graph().plugName(p));
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "reorderAttr",
        .doc = "Move an added attribute among the object's added attributes: before or after another, or to an index (0 first).",
        .positionalDoc = "plug",
        .flags = {{"before", "b", T::String, nullptr, "The attribute it moves in front of."},
                  {"after", "af", T::String, nullptr, "The attribute it moves behind."},
                  {"index", "i", T::Int, nullptr, "Its place among the added attributes, 0 first; -1 last."}},
        .run = [](const Args& a) -> json {
            if (a.positional.size() != 1)
                throw CommandError("reorderAttr: expected a plug");
            PlugRef p = attrArg(a.positional[0].get<std::string>());
            const Node& n = *graph().node(p.node);
            std::vector<int> user;
            for (size_t i = 0; i < n.type().attrs.size(); ++i)
                if (n.type().attrs[i].flags & AttrUser)
                    user.push_back(int(i));
            if (!(graph().attr(p).flags & AttrUser))
                throw CommandError("reorderAttr: " + graph().plugName(p) + " belongs to the node's type");
            int to = -1;
            auto other = [&](const char* flag) {
                const int i = n.type().attrIndex(a.get<std::string>(flag));
                if (i < 0 || !(n.type().attrs[size_t(i)].flags & AttrUser))
                    throw CommandError("reorderAttr: '" + a.get<std::string>(flag) + "' isn't an added attribute of " + n.name());
                return i;
            };
            if (a.has("before"))
            {
                to = other("before");
                if (to > p.attr)
                    --to;
            }
            else if (a.has("after"))
            {
                to = other("after");
                if (to < p.attr)
                    ++to;
            }
            else if (a.has("index"))
            {
                const int i = a.get<int>("index");
                to = i < 0 || size_t(i) >= user.size() ? user.back() : user[size_t(i)];
            }
            else
                throw CommandError("reorderAttr: give before, after or index");
            guarded([&] { graph().moveAttr(p.node, p.attr, to); return 0; });
            return graph().plugName({p.node, to});
        },
    });

    ops::registerCommand({
        .name = "copyAttr",
        .doc = "Copy added attributes from the first object to the others: their definitions, values and, with connections=True, "
               "their incoming connections. Ones a target already has only take the value. Returns the plugs added.",
        .positionalDoc = "source targets",
        .flags = {{"attribute", "at", T::List, nullptr, "Only these attributes; all added ones by default."},
                  {"category", "", T::String, nullptr, "Only the attributes under this heading (a component)."},
                  {"values", "v", T::Bool, true, "Copy the values too."},
                  {"connections", "ic", T::Bool, false, "Connect each copy to whatever drives the original."}},
        .run = [](const Args& a) -> json {
            std::vector<std::string> objs = a.objects();
            if (objs.size() < 2)
                throw CommandError("copyAttr: expected a source and at least one target");
            const Node& src = nodeArg(objs[0]);
            std::vector<std::string> only = a.has("attribute") ? a.get<std::vector<std::string>>("attribute") : std::vector<std::string>{};
            std::vector<int> attrs;
            for (size_t i = 0; i < src.type().attrs.size(); ++i)
            {
                const AttrDef& d = src.type().attrs[i];
                if (!(d.flags & AttrUser) || (a.has("category") && d.category != a.get<std::string>("category")))
                    continue;
                if (!only.empty() && std::find(only.begin(), only.end(), d.name) == only.end())
                    continue;
                attrs.push_back(int(i));
            }
            for (const std::string& name : only)
                if (src.type().attrIndex(name) < 0)
                    throw CommandError("copyAttr: " + src.name() + " has no attribute '" + name + "'");
            json out = json::array();
            Graph& g = graph();
            for (size_t t = 1; t < objs.size(); ++t)
            {
                const NodeId dst = nodeArg(objs[t]).id();
                if (dst == src.id())
                    continue;
                for (int i : attrs)
                {
                    const AttrDef& d = src.type().attrs[size_t(i)];
                    int at = g.node(dst)->type().attrIndex(d.name);
                    if (at < 0)
                    {
                        at = guarded([&] { return g.addAttr(dst, d); });
                        out.push_back(g.plugName({dst, at}));
                    }
                    else if (g.node(dst)->type().attrs[size_t(at)].type != d.type)
                        throw CommandError("copyAttr: " + g.plugName({dst, at}) + " is a different type");
                    if (a.get<bool>("values") && !g.source({dst, at}))
                    {
                        if (d.flags & AttrArray)
                        {
                            std::vector<PlugValue> values;
                            for (const auto& [index, v] : src.elements(i))
                                values.push_back(v);
                            guarded([&] { g.setElements({dst, at}, values); return 0; });
                        }
                        else if (!g.source({src.id(), i}))
                            guarded([&] { g.setValue({dst, at}, src.value(i)); return 0; });
                    }
                    if (a.get<bool>("connections"))
                        if (auto from = g.source({src.id(), i}))
                            guarded([&] { return g.connect(*from, {dst, at}, true); });
                }
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "listConnections",
        .doc = "What objects or plugs are connected to: sources feeding them and destinations they feed. plugs=True names the "
               "other plugs; connections=True pairs each with the plug of yours it connects to.",
        .positionalDoc = "objects or plugs",
        .flags = {{"source", "s", T::Bool, true, "Include what feeds them."},
                  {"destination", "d", T::Bool, true, "Include what they feed."},
                  {"plugs", "p", T::Bool, false, "Name plugs rather than nodes."},
                  {"connections", "c", T::Bool, false, "Pairs: your plug, then the other end."},
                  {"type", "t", T::String, nullptr, "Only other ends of this node type."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            json out = json::array();
            for (const std::string& o : a.objects())
            {
                PlugRef p = o.find('.') == std::string::npos ? PlugRef{nodeArg(o).id(), -1} : attrArg(o);
                for (json& v : connectionsOf(p, a.get<bool>("source"), a.get<bool>("destination"), a.get<bool>("plugs"),
                                             a.get<bool>("connections"), a.get<std::string>("type", "")))
                    out.push_back(std::move(v));
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "addComponent",
        .doc = "Add a component from the library (listComponents) to objects: its attributes under its heading, and its USD "
               "schema when it names one. Returns the plugs added.",
        .positionalDoc = "objects",
        .flags = {{"name", "n", T::String, nullptr, "The component."}},
        .run = [](const Args& a) -> json {
            if (!a.has("name"))
                throw CommandError("addComponent: give name");
            std::optional<attrComponents::Component> c = attrComponents::find(a.get<std::string>("name"));
            if (!c)
                throw CommandError("addComponent: no component named '" + a.get<std::string>("name") + "'");
            json out = json::array();
            for (const std::string& o : a.objects())
                for (std::string& p : guarded([&] { return attrComponents::apply(graph(), nodeArg(o).id(), *c); }))
                    out.push_back(std::move(p));
            return out;
        },
    });

    ops::registerCommand({
        .name = "listComponents",
        .doc = "Components Add offers: USD API schemas, extensions' components and saved ones. Each as {name, source, doc, attrs, "
               "usdSchema, unreal}.",
        .flags = {{"source", "src", T::String, nullptr, "Only this source: usd, user, studio or an extension."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            json out = json::array();
            for (const attrComponents::Component& c : attrComponents::all())
            {
                if (a.has("source") && c.source != a.get<std::string>("source"))
                    continue;
                json j = c.toJson();
                j.erase("format");
                j.erase("version");
                j["source"] = c.source;
                out.push_back(std::move(j));
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "saveComponent",
        .doc = "Save the attributes under one heading of an object as a reusable component (names, types, defaults, ranges, docs), "
               "in your library or the studio's. It then appears in Add. Returns the file.",
        .positionalDoc = "object",
        .flags = {{"category", "", T::String, nullptr, "The heading to save."},
                  {"name", "n", T::String, nullptr, "The component's name; the heading by default."},
                  {"studio", "st", T::Bool, false, "Save to the studio library (the first studio folder)."},
                  {"doc", "", T::String, nullptr, "What it is for."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::vector<std::string> objs = a.objects();
            if (objs.empty() || !a.has("category"))
                throw CommandError("saveComponent: give an object and category");
            attrComponents::Component c = attrComponents::fromNode(nodeArg(objs[0]), a.get<std::string>("category"));
            if (c.attrs.empty())
                throw CommandError("saveComponent: " + objs[0] + " has no added attributes under '" + c.name + "'");
            if (a.has("name"))
                c.name = a.get<std::string>("name");
            c.doc = a.get<std::string>("doc", "");
            return paths::toUtf8(guarded([&] { return attrComponents::save(c, a.get<bool>("studio")); }));
        },
    });

    ops::registerCommand({
        .name = "deleteComponent",
        .doc = "Delete a saved component from your library or the studio's. Objects carrying it keep their attributes.",
        .flags = {{"name", "n", T::String, nullptr, "The component."}, {"studio", "st", T::Bool, false, "From the studio library."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            return attrComponents::removeSaved(a.get<std::string>("name", ""), a.get<bool>("studio"));
        },
    });

    ops::registerCommand({
        .name = "componentExport",
        .doc = "How a component on objects reaches Unreal: actorTags, assetUserData (the default), component (an actor component "
               "of unrealClass with matching properties), dataAsset (of unrealClass), custom (a registered exporter named by "
               "unrealClass) or none. With query=True, returns the setting.",
        .positionalDoc = "objects",
        .flags = {{"component", "c", T::String, nullptr, "The component (heading)."},
                  {"mode", "m", T::String, nullptr, "none, actorTags, assetUserData, component, dataAsset or custom.",
                   {"none", "actorTags", "assetUserData", "component", "dataAsset", "custom"}},
                  {"unrealClass", "uc", T::String, nullptr, "The UE class (component, dataAsset) or exporter (custom)."},
                  {"query", "q", T::Bool, false, "Return {mode, class}."}},
        .run = [](const Args& a) -> json {
            if (!a.has("component"))
                throw CommandError("componentExport: give component");
            const std::string comp = a.get<std::string>("component");
            std::vector<std::string> objs = a.objects();
            if (objs.empty())
                throw CommandError("componentExport: no object");
            if (a.get<bool>("query"))
            {
                json s = attrComponents::exportSettings(nodeArg(objs[0]));
                return s.contains(comp) ? s[comp] : json{{"mode", "assetUserData"}};
            }
            const attrComponents::UnrealMode mode = guarded([&] { return attrComponents::unrealModeNamed(a.get<std::string>("mode", "assetUserData")); });
            for (const std::string& o : objs)
                guarded([&] { attrComponents::setExportSettings(graph(), nodeArg(o).id(), comp, mode, a.get<std::string>("unrealClass", "")); return 0; });
            return nullptr;
        },
    });
}
} // namespace attrCommands
} // namespace lila::scene
