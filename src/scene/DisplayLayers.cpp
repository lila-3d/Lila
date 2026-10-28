#include "scene/DisplayLayers.h"

#include "ops/Command.h"
#include "core/Profile.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/Transform.h"

#include <algorithm>

namespace lila::scene::displayLayers
{
namespace
{
using T = ArgType;

const std::vector<std::string> kDisplayTypeNames = {"normal", "template", "reference"};

bool hasLayerPlug(const Node& n) { return n.type().dag && n.type().isA("transform"); }

Node* layerArg(const Graph& g, const std::string& name, const char* cmd)
{
    Node* n = g.find(name);
    if (!n || !isLayer(*n))
        throw CommandError(std::string(cmd) + ": " + name + " is not a display layer");
    return n;
}

std::vector<NodeId> objectIds(const Graph& g, const std::vector<std::string>& names, const char* cmd)
{
    std::vector<NodeId> out;
    for (const std::string& name : names)
    {
        Node* n = g.find(name);
        if (!n)
            throw CommandError(std::string(cmd) + ": no object named " + name);
        if (!hasLayerPlug(*n))
            throw CommandError(std::string(cmd) + ": " + name + " is not a DAG object");
        out.push_back(n->id());
    }
    return out;
}

json namesOf(const Graph& g, const std::vector<NodeId>& ids)
{
    json out = json::array();
    for (NodeId id : ids)
        out.push_back(g.node(id)->path());
    return out;
}

int64_t displayTypeArg(const std::string& v, const char* cmd)
{
    auto it = std::ranges::find(kDisplayTypeNames, v);
    if (it == kDisplayTypeNames.end())
        throw CommandError(std::string(cmd) + ": displayType must be normal, template or reference");
    return int64_t(it - kDisplayTypeNames.begin());
}

std::vector<AttrDef> layerAttrs()
{
    constexpr uint32_t kStored = AttrInput | AttrStorable;
    return {
        {.name = "visibility", .shortName = "v", .type = PlugType::Bool, .defaultValue = true, .flags = kStored | AttrKeyable,
         .doc = "Off hides the layer's objects (V)."},
        {.name = "hideOnPlayback", .shortName = "hpb", .type = PlugType::Bool, .defaultValue = false, .flags = kStored,
         .doc = "Hide the layer's objects while the animation plays (P)."},
        {.name = "displayType", .shortName = "dt", .type = PlugType::Enum, .defaultValue = int64_t(0), .flags = kStored,
         .enumNames = kDisplayTypeNames,
         .doc = "normal; template: grey wireframe, not pickable in the viewport; reference: drawn as usual, not pickable."},
        {.name = "useColor", .shortName = "uc", .type = PlugType::Bool, .defaultValue = false, .flags = kStored,
         .doc = "Tint the objects' wireframe with color."},
        {.name = "color", .shortName = "c", .type = PlugType::Vec3, .defaultValue = glm::dvec3(0.5), .flags = kStored,
         .childNames = {"colorR", "colorG", "colorB"}, .childShort = {"cr", "cg", "cb"}, .doc = "Wireframe tint, linear RGB.", .minValue = 0.0, .maxValue = 1.0},
        {.name = "displayOrder", .shortName = "do", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = kStored,
         .doc = "Row in the Display Layers panel, lowest first.", .minValue = 0.0},
        {.name = "drawInfo", .shortName = "di", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = AttrOutput | AttrHidden,
         .doc = "Connected to each member's displayLayer."},
    };
}
} // namespace

bool isLayer(const Node& n) { return n.type().isA(kTypeName); }

std::vector<Node*> all(const Graph& g)
{
    std::vector<Node*> out;
    for (NodeId id : g.nodes())
        if (Node* n = g.node(id); isLayer(*n))
            out.push_back(n);
    std::ranges::sort(out, [](const Node* a, const Node* b) {
        int64_t oa = a->as<int64_t>(DisplayLayer::kDisplayOrder), ob = b->as<int64_t>(DisplayLayer::kDisplayOrder);
        return oa != ob ? oa < ob : a->name() < b->name();
    });
    return out;
}

Node* layerOf(const Graph& g, NodeId id)
{
    const Node* n = g.node(id);
    if (!n || !hasLayerPlug(*n))
        return nullptr;
    auto src = g.source({id, Transform::kDisplayLayer, -1});
    if (!src)
        return nullptr;
    Node* layer = g.node(src->node);
    return layer && isLayer(*layer) ? layer : nullptr;
}

std::vector<NodeId> members(const Graph& g, NodeId layer)
{
    std::vector<NodeId> out;
    for (const PlugRef& dst : g.destinations({layer, DisplayLayer::kDrawInfo, -1}))
        if (dst.attr == Transform::kDisplayLayer)
            out.push_back(dst.node);
    std::ranges::sort(out);
    return out;
}

bool hiddenByOwnLayer(const Graph& g, NodeId id)
{
    const Node* layer = layerOf(g, id);
    if (!layer)
        return false;
    if (!layer->as<bool>(DisplayLayer::kVisibility))
        return true;
    return layer->as<bool>(DisplayLayer::kHideOnPlayback) && anim::playDirection() != 0;
}

Display display(const Graph& g, NodeId id)
{
    for (NodeId n = id; n != kNoNode; n = g.parent(n))
        if (const Node* layer = layerOf(g, n))
        {
            Display d;
            d.type = DisplayType(layer->as<int64_t>(DisplayLayer::kDisplayType));
            if (layer->as<bool>(DisplayLayer::kUseColor))
                d.color = layer->as<glm::dvec3>(DisplayLayer::kColor);
            return d;
        }
    return {};
}

bool pickable(const Graph& g, NodeId id) { return display(g, id).type == DisplayType::Normal; }

Node* create(Graph& g, std::string_view name, const std::vector<NodeId>& nodes)
{
    int64_t order = 0;
    for (const Node* l : all(g))
        order = std::max(order, l->as<int64_t>(DisplayLayer::kDisplayOrder) + 1);
    Node* layer = g.createNode(*types::find(kTypeName), name.empty() ? "layer1" : name);
    if (order != 0)
        g.setValue({layer->id(), DisplayLayer::kDisplayOrder, -1}, order);
    addMembers(g, layer->id(), nodes);
    return layer;
}

void addMembers(Graph& g, NodeId layer, const std::vector<NodeId>& nodes)
{
    LILA_ZONE();

    for (NodeId id : nodes)
    {
        const Node* n = g.node(id);
        if (!n || !hasLayerPlug(*n))
            continue;
        const PlugRef dst{id, Transform::kDisplayLayer, -1};
        if (auto src = g.source(dst); src && src->node == layer)
            continue;
        g.connect({layer, DisplayLayer::kDrawInfo, -1}, dst, true);
    }
}

void removeMembers(Graph& g, const std::vector<NodeId>& nodes)
{
    for (NodeId id : nodes)
        if (layerOf(g, id))
            g.disconnect({id, Transform::kDisplayLayer, -1});
}

void remove(Graph& g, NodeId layer)
{
    removeMembers(g, members(g, layer));
    g.deleteNode(layer);
}

void reorder(Graph& g, NodeId layer, size_t index)
{
    std::vector<Node*> layers = all(g);
    auto it = std::ranges::find_if(layers, [&](const Node* n) { return n->id() == layer; });
    if (it == layers.end())
        return;
    Node* moved = *it;
    layers.erase(it);
    layers.insert(layers.begin() + std::ptrdiff_t(std::min(index, layers.size())), moved);
    for (size_t i = 0; i < layers.size(); ++i)
        if (layers[i]->as<int64_t>(DisplayLayer::kDisplayOrder) != int64_t(i))
            g.setValue({layers[i]->id(), DisplayLayer::kDisplayOrder, -1}, int64_t(i));
}

json info(const Graph& g, NodeId layer)
{
    const Node* n = g.node(layer);
    json color = nullptr;
    if (n->as<bool>(DisplayLayer::kUseColor))
    {
        const glm::dvec3 c = n->as<glm::dvec3>(DisplayLayer::kColor);
        color = {c.x, c.y, c.z};
    }
    return {
        {"name", n->path()},
        {"visibility", n->as<bool>(DisplayLayer::kVisibility)},
        {"hideOnPlayback", n->as<bool>(DisplayLayer::kHideOnPlayback)},
        {"displayType", kDisplayTypeNames[size_t(std::clamp<int64_t>(n->as<int64_t>(DisplayLayer::kDisplayType), 0, 2))]},
        {"color", color},
        {"order", n->as<int64_t>(DisplayLayer::kDisplayOrder)},
        {"members", namesOf(g, members(g, layer))},
    };
}

void registerTypes()
{
    if (types::find(kTypeName))
        return;
    NodeType t;
    t.name = kTypeName;
    t.icon = "stack-2";
    t.doc = "A display layer: shows, hides, templates or references its objects together. Edit it in Windows > Display Layers.";
    t.ownAttrs = layerAttrs();
    t.create = [] { return std::make_unique<DisplayLayer>(); };
    types::registerType(std::move(t));
}

void registerCommands()
{
    if (ops::find("createDisplayLayer"))
        return;

    ops::registerCommand({
        .name = "createDisplayLayer",
        .doc = "Create a display layer holding the objects (the selection by default), taking them out of any layer they "
               "were in. Returns its name.",
        .positionalDoc = "objects",
        .flags = {{"name", "n", T::String, "", "Name; layer1 when empty, made unique."},
                  {"empty", "e", T::Bool, false, "Make it with no members, whatever is selected."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<NodeId> ids;
            if (!a.get<bool>("empty", false))
                ids = objectIds(g, a.objects(), "createDisplayLayer");
            try
            {
                return create(g, a.get<std::string>("name", ""), ids)->path();
            }
            catch (const std::invalid_argument& e)
            {
                throw CommandError(std::string("createDisplayLayer: ") + e.what());
            }
        },
    });

    ops::registerCommand({
        .name = "editDisplayLayerMembers",
        .doc = "Put objects (the selection by default) in a display layer, moving them out of their old one. Naming "
               "defaultLayer takes them out of any layer. Returns the layer's members.",
        .positionalDoc = "layer, objects",
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (a.positional.empty() || !a.positional[0].is_string())
                throw CommandError("editDisplayLayerMembers: name the layer first");
            const std::string layerName = a.positional[0].get<std::string>();
            Args rest = a;
            rest.positional.erase(rest.positional.begin());
            std::vector<NodeId> ids = objectIds(g, rest.objects(), "editDisplayLayerMembers");
            if (layerName == kDefaultLayer)
            {
                removeMembers(g, ids);
                return json::array();
            }
            Node* layer = layerArg(g, layerName, "editDisplayLayerMembers");
            addMembers(g, layer->id(), ids);
            return namesOf(g, members(g, layer->id()));
        },
    });

    ops::registerCommand({
        .name = "removeFromDisplayLayer",
        .doc = "Take objects (the selection by default) out of their display layers.",
        .positionalDoc = "objects",
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            removeMembers(g, objectIds(g, a.objects(), "removeFromDisplayLayer"));
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "deleteDisplayLayer",
        .doc = "Delete display layers; their objects stay, in no layer.",
        .positionalDoc = "layers",
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<NodeId> layers;
            for (const json& v : a.positional)
                if (v.is_string())
                    layers.push_back(layerArg(g, v.get<std::string>(), "deleteDisplayLayer")->id());
            if (layers.empty())
                throw CommandError("deleteDisplayLayer: name a layer");
            for (NodeId id : layers)
                remove(g, id);
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "displayLayer",
        .doc = "Edit a display layer's switches (V, P, display type, colour, name); with no flags, query it. Returns its "
               "info: name, visibility, hideOnPlayback, displayType, color (null for none), order and members.",
        .positionalDoc = "layer",
        .flags = {{"visibility", "v", T::Bool, nullptr, "Show or hide its objects."},
                  {"hideOnPlayback", "hpb", T::Bool, nullptr, "Hide its objects while the animation plays."},
                  {"displayType", "dt", T::String, nullptr, "normal, template or reference.", kDisplayTypeNames},
                  {"color", "c", T::Vec3, nullptr, "Wireframe tint, linear RGB; turns the tint on."},
                  {"useColor", "uc", T::Bool, nullptr, "Turn the wireframe tint on or off."},
                  {"name", "n", T::String, nullptr, "Rename the layer; made unique."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (a.positional.empty() || !a.positional[0].is_string())
                throw CommandError("displayLayer: name the layer");
            Node* layer = layerArg(g, a.positional[0].get<std::string>(), "displayLayer");
            const NodeId id = layer->id();
            auto set = [&](int attr, PlugValue v) {
                if (layer->value(attr) != v)
                    g.setValue({id, attr, -1}, std::move(v));
            };
            if (a.has("visibility"))
                set(DisplayLayer::kVisibility, a.get<bool>("visibility"));
            if (a.has("hideOnPlayback"))
                set(DisplayLayer::kHideOnPlayback, a.get<bool>("hideOnPlayback"));
            if (a.has("displayType"))
                set(DisplayLayer::kDisplayType, displayTypeArg(a.get<std::string>("displayType"), "displayLayer"));
            if (a.has("color"))
            {
                const json& c = a.flags["color"];
                if (!c.is_array() || c.size() != 3)
                    throw CommandError("displayLayer: color takes [r, g, b]");
                set(DisplayLayer::kColor, glm::dvec3(c[0].get<double>(), c[1].get<double>(), c[2].get<double>()));
                set(DisplayLayer::kUseColor, true);
            }
            if (a.has("useColor"))
                set(DisplayLayer::kUseColor, a.get<bool>("useColor"));
            if (a.has("name"))
            {
                try
                {
                    g.rename(id, a.get<std::string>("name"));
                }
                catch (const std::invalid_argument& e)
                {
                    throw CommandError(std::string("displayLayer: ") + e.what());
                }
            }
            return info(g, id);
        },
    });

    ops::registerCommand({
        .name = "listDisplayLayers",
        .doc = "Every display layer in panel order, each as displayLayer returns it.",
        .undoable = false,
        .run = [](const Args&) -> json {
            Graph& g = Graph::current();
            json out = json::array();
            for (const Node* n : all(g))
                out.push_back(info(g, n->id()));
            return out;
        },
    });

    ops::registerCommand({
        .name = "reorderDisplayLayer",
        .doc = "Move a display layer to a row of the Display Layers panel (0 is the top).",
        .positionalDoc = "layer",
        .flags = {{"index", "i", T::Int, 0, "Row it moves to.", {}, {}, 0.0}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (a.positional.empty() || !a.positional[0].is_string())
                throw CommandError("reorderDisplayLayer: name the layer");
            Node* layer = layerArg(g, a.positional[0].get<std::string>(), "reorderDisplayLayer");
            reorder(g, layer->id(), size_t(std::max<int64_t>(0, a.get<int64_t>("index", 0))));
            json out = json::array();
            for (const Node* n : all(g))
                out.push_back(n->path());
            return out;
        },
    });

    ops::registerCommand({
        .name = "selectDisplayLayerMembers",
        .doc = "Select the objects in display layers.",
        .positionalDoc = "layers",
        .flags = {{"add", "", T::Bool, false, "Add them to the selection instead of replacing it."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<NodeId> ids;
            for (const json& v : a.positional)
                if (v.is_string())
                    for (NodeId m : members(g, layerArg(g, v.get<std::string>(), "selectDisplayLayerMembers")->id()))
                        ids.push_back(m);
            g.select(ids, a.get<bool>("add", false) ? SelectMode::Add : SelectMode::Replace);
            return namesOf(g, ids);
        },
    });
}
} // namespace lila::scene::displayLayers
