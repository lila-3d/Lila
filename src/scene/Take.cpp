#include "scene/Take.h"

#include "cmds/Command.h"
#include "cmds/Undo.h"
#include "core/Profile.h"
#include "scene/Anim.h"
#include "scene/AnimLayer.h"
#include "scene/Graph.h"
#include "scene/RotationCurve.h"
#include "scene/Transform.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace lila::scene::anim
{
namespace
{
bool isAnim(const Node& n)
{
    return isAnyCurve(n) || isBlend(n);
}

/** The take's character root, connected by its world matrix so renames keep it; kNoNode for the whole scene. */
NodeId rootOf(Graph& g, NodeId take)
{
    auto src = g.source({take, TakeNode::kRoot, -1});
    return src ? src->node : kNoNode;
}

bool conflicts(Graph& g, NodeId a, NodeId b)
{
    NodeId ra = rootOf(g, a), rb = rootOf(g, b);
    return !ra || !rb || ra == rb;
}

/** Nodes whose plugs the take animates: the root and everything under it, or every scene node. */
std::set<NodeId> scopeOf(Graph& g, NodeId take)
{
    std::set<NodeId> out;
    NodeId root = rootOf(g, take);
    if (!root)
    {
        for (NodeId id : g.nodes())
            if (Node* n = g.node(id); !isAnim(*n) && !isTake(*n))
                out.insert(id);
        return out;
    }
    out.insert(root);
    for (NodeId d : g.descendants(root))
        out.insert(d);
    return out;
}

/**
 * The animation connections of an active take: curves and layer chains into the scope's plugs. A constrained
 * channel keeps its constraint blend; only the keys under it (the blend's inputA) belong to the take.
 */
std::vector<std::pair<PlugRef, PlugRef>> liveConnections(Graph& g, NodeId take)
{
    std::set<NodeId> scope = scopeOf(g, take);
    std::vector<std::pair<PlugRef, PlugRef>> out;
    for (const auto& [dst, src] : g.connections())
    {
        Node* s = g.node(src.node);
        Node* d = g.node(dst.node);
        if (!s || !d || !isAnim(*s))
            continue;
        if (isBlend(*d))
        {
            if (dst.attr != BlendNode::kInputA || layerOf(g, dst.node))
                continue;
            PlugRef m = memberOf(g, dst.node);
            std::vector<NodeId> chain = m.valid() ? blendChain(g, m) : std::vector<NodeId>{};
            if (!chain.empty() && chain.back() == dst.node && scope.contains(m.node))
                out.emplace_back(dst, src);
            continue;
        }
        if (!scope.contains(dst.node) || isAnim(*d))
            continue;
        if (isBlend(*s))
        {
            std::vector<NodeId> chain = blendChain(g, {dst.node, dst.attr, -1});
            if (!chain.empty() && !layerOf(g, chain.back()))
                continue;
        }
        out.emplace_back(dst, src);
    }
    return out;
}

std::vector<std::pair<PlugRef, PlugRef>> parkedConnections(Graph& g, NodeId take)
{
    std::vector<std::pair<PlugRef, PlugRef>> out;
    json list = json::parse(g.node(take)->as<std::string>(TakeNode::kParked), nullptr, false);
    if (!list.is_array())
        return out;
    for (const json& pair : list)
    {
        if (!pair.is_array() || pair.size() != 2 || !pair[0].is_string() || !pair[1].is_string())
            continue;
        try
        {
            out.emplace_back(g.plug(pair[0].get<std::string>()), g.plug(pair[1].get<std::string>()));
        }
        catch (const std::invalid_argument&)
        {
            // A renamed or deleted node: that channel is dropped from the take.
        }
    }
    return out;
}

void setParked(Graph& g, NodeId take, const std::vector<std::pair<PlugRef, PlugRef>>& conns)
{
    json list = json::array();
    for (const auto& [dst, src] : conns)
        list.push_back({g.plugName(dst), g.plugName(src)});
    g.setValue({take, TakeNode::kParked}, list.dump());
}

void park(Graph& g, NodeId take)
{
    auto conns = liveConnections(g, take);
    setParked(g, take, conns);
    for (const auto& [dst, src] : conns)
        g.disconnect(dst);
    g.setValue({take, TakeNode::kActive}, false);
}

void unpark(Graph& g, NodeId take)
{
    for (const auto& [dst, src] : parkedConnections(g, take))
    {
        try
        {
            g.connect(src, dst, true);
        }
        catch (const std::invalid_argument&)
        {
        }
    }
    g.setValue({take, TakeNode::kParked}, std::string());
    g.setValue({take, TakeNode::kActive}, true);
}

/** Playback settings as an undo step, since playback lives outside the graph. */
struct PlaybackUndo final : UndoAction
{
    json before, after;
    PlaybackUndo(json b, json a) : before(std::move(b)), after(std::move(a)) {}
    void undo() override
    {
        playback().fromJson(before);
        playbackChanged();
    }
    void redo() override
    {
        playback().fromJson(after);
        playbackChanged();
    }
};

void applyPlayback(Graph& g, NodeId take, bool recordUndo)
{
    Node& t = *g.node(take);
    json before = playback().toJson();
    json j = before;
    double start = t.as<double>(TakeNode::kStart), end = std::max(start, t.as<double>(TakeNode::kEnd));
    j["minTime"] = start;
    j["maxTime"] = end;
    j["animationStartTime"] = std::min(j["animationStartTime"].get<double>(), start);
    j["animationEndTime"] = std::max(j["animationEndTime"].get<double>(), end);
    j["fps"] = t.as<double>(TakeNode::kFps);
    playback().fromJson(j);
    json after = playback().toJson();
    if (before == after)
        return;
    if (recordUndo)
        UndoStack::get().record(std::make_unique<PlaybackUndo>(before, after));
    playbackChanged();
}

/** Anim nodes upstream of id (itself included), stopping at anything that isn't a curve or blend. */
void collectUpstream(Graph& g, NodeId id, std::set<NodeId>& out)
{
    Node* n = g.node(id);
    if (!n || !isAnim(*n) || !out.insert(id).second)
        return;
    for (const auto& [dst, src] : g.connections())
        if (dst.node == id)
            collectUpstream(g, src.node, out);
}

/** A copy of an anim node and the anim nodes feeding it; everything else (layers) is shared. */
NodeId copyAnim(Graph& g, NodeId id, std::map<NodeId, NodeId>& memo, const std::string& suffix)
{
    if (auto it = memo.find(id); it != memo.end())
        return it->second;
    Node& n = *g.node(id);
    Node* c = g.createNode(n.type(), n.name() + "_" + suffix, kNoNode, false);
    NodeId cid = c->id();
    memo[id] = cid;
    const auto& attrs = n.type().attrs;
    for (int i = 0; i < int(attrs.size()); ++i)
        if ((attrs[i].flags & AttrInput) && (attrs[i].flags & AttrStorable) && !(g.node(cid)->value(i) == n.value(i)))
            g.setValue({cid, i}, n.value(i));
    std::vector<std::pair<PlugRef, PlugRef>> inputs;
    for (const auto& [dst, src] : g.connections())
        if (dst.node == id)
            inputs.emplace_back(dst, src);
    for (const auto& [dst, src] : inputs)
    {
        PlugRef s = src;
        if (Node* sn = g.node(src.node); sn && isAnim(*sn))
            s.node = copyAnim(g, src.node, memo, suffix);
        g.connect(s, {cid, dst.attr, dst.component});
    }
    return cid;
}

/** Sets a take's curves looping (or not) through the curves' loop key. */
void applyLoop(Graph& g, NodeId take)
{
    std::vector<NodeId> curves = takeCurves(g, take);
    if (curves.empty())
        return;
    bool on = g.node(take)->as<bool>(TakeNode::kLoop);
    std::optional<double> at;
    if (on)
        at = g.node(take)->as<double>(TakeNode::kEnd);
    setCurvesLoop(g, curves, on, at);
}
} // namespace

bool isTake(const Node& n)
{
    return n.type().isA("take");
}

std::vector<NodeId> takes(Graph& g)
{
    std::vector<NodeId> out;
    for (NodeId id : g.nodes())
        if (isTake(*g.node(id)))
            out.push_back(id);
    std::stable_sort(out.begin(), out.end(), [&](NodeId a, NodeId b) {
        int64_t oa = g.node(a)->as<int64_t>(TakeNode::kOrder), ob = g.node(b)->as<int64_t>(TakeNode::kOrder);
        return oa != ob ? oa < ob : a < b;
    });
    return out;
}

std::vector<NodeId> activeTakes(Graph& g)
{
    std::vector<NodeId> out;
    for (NodeId t : takes(g))
        if (g.node(t)->as<bool>(TakeNode::kActive))
            out.push_back(t);
    return out;
}

NodeId createTake(Graph& g, const std::string& name, const TakeSettings& s)
{
    LILA_ZONE();

    std::vector<NodeId> existing = takes(g);
    Node* t = g.createNode(*types::find("take"), name.empty() ? "Take1" : name, kNoNode, true);
    NodeId id = t->id();
    if (!s.root.empty())
    {
        Node* r = g.find(s.root);
        if (!r || !r->type().isA("transform"))
            throw std::invalid_argument("take root " + s.root + " is not a transform");
        g.connect({r->id(), Transform::kWorldMatrix, -1}, {id, TakeNode::kRoot, -1});
    }
    g.setValue({id, TakeNode::kStart}, s.start);
    g.setValue({id, TakeNode::kEnd}, std::max(s.start, s.end));
    g.setValue({id, TakeNode::kFps}, std::max(1.0, s.fps));
    g.setValue({id, TakeNode::kLoop}, s.loop);
    g.setValue({id, TakeNode::kOrder}, int64_t(existing.empty() ? 0 : g.node(existing.back())->as<int64_t>(TakeNode::kOrder) + 1));
    bool first = std::none_of(existing.begin(), existing.end(), [&](NodeId o) { return conflicts(g, o, id); });
    if (first)
    {
        g.setValue({id, TakeNode::kActive}, true);
        applyLoop(g, id);
    }
    else
        g.setValue({id, TakeNode::kParked}, std::string("[]"));
    return id;
}

void setActiveTake(Graph& g, NodeId take)
{
    LILA_ZONE();

    if (!g.node(take)->as<bool>(TakeNode::kActive))
    {
        for (NodeId a : activeTakes(g))
            if (conflicts(g, a, take))
                park(g, a);
        unpark(g, take);
    }
    if (g.node(take)->as<bool>(TakeNode::kLoop))
        applyLoop(g, take);
    applyPlayback(g, take, true);
    g.evaluate();
}

void deleteTake(Graph& g, NodeId take)
{
    LILA_ZONE();

    const bool active = g.node(take)->as<bool>(TakeNode::kActive);
    auto conns = active ? liveConnections(g, take) : parkedConnections(g, take);
    std::set<NodeId> doomed;
    for (const auto& [dst, src] : conns)
    {
        if (active)
            g.disconnect(dst);
        collectUpstream(g, src.node, doomed);
    }
    for (NodeId id : doomed)
        if (g.node(id))
            g.deleteNode(id);
    g.deleteNode(take);
}

NodeId duplicateTake(Graph& g, NodeId take, const std::string& name)
{
    LILA_ZONE();

    Node& src = *g.node(take);
    NodeId root = rootOf(g, take);
    TakeSettings s{root ? g.node(root)->name() : std::string(), src.as<double>(TakeNode::kStart), src.as<double>(TakeNode::kEnd), src.as<double>(TakeNode::kFps),
                   src.as<bool>(TakeNode::kLoop)};
    std::string base = src.name();
    auto conns = takeConnections(g, take);
    const std::string folder = src.as<std::string>(TakeNode::kFolder);
    NodeId copy = createTake(g, name.empty() ? base + "Copy" : name, s);
    g.setValue({copy, TakeNode::kFolder}, folder);
    std::map<NodeId, NodeId> memo;
    std::vector<std::pair<PlugRef, PlugRef>> parked;
    for (const auto& [dst, from] : conns)
    {
        PlugRef s2 = from;
        s2.node = copyAnim(g, from.node, memo, g.node(copy)->name());
        parked.emplace_back(dst, s2);
    }
    if (g.node(copy)->as<bool>(TakeNode::kActive))
        g.setValue({copy, TakeNode::kActive}, false);
    setParked(g, copy, parked);
    return copy;
}

std::vector<std::pair<PlugRef, PlugRef>> takeConnections(Graph& g, NodeId take)
{
    return g.node(take)->as<bool>(TakeNode::kActive) ? liveConnections(g, take) : parkedConnections(g, take);
}

std::vector<NodeId> takeCurves(Graph& g, NodeId take)
{
    std::set<NodeId> nodes;
    for (const auto& [dst, src] : takeConnections(g, take))
        collectUpstream(g, src.node, nodes);
    std::vector<NodeId> out;
    for (NodeId id : nodes)
        if (isAnyCurve(*g.node(id)))
            out.push_back(id);
    return out;
}

void withTake(Graph& g, NodeId take, const std::function<void()>& fn)
{
    if (g.node(take)->as<bool>(TakeNode::kActive))
    {
        fn();
        return;
    }
    UndoStack& undo = UndoStack::get();
    const bool wasEnabled = undo.enabled();
    const json savedPlayback = playback().toJson();
    std::vector<NodeId> parkedNow;
    for (NodeId a : activeTakes(g))
        if (conflicts(g, a, take))
            parkedNow.push_back(a);
    struct Restore
    {
        Graph& g;
        NodeId take;
        const std::vector<NodeId>& others;
        const json& playbackJson;
        UndoStack& undo;
        bool wasEnabled;
        ~Restore()
        {
            try
            {
                park(g, take);
                for (NodeId a : others)
                    unpark(g, a);
            }
            catch (const std::exception&)
            {
            }
            playback().fromJson(playbackJson);
            g.evaluate();
            undo.setEnabled(wasEnabled);
        }
    };
    undo.setEnabled(false);
    Restore restore{g, take, parkedNow, savedPlayback, undo, wasEnabled};
    for (NodeId a : parkedNow)
        park(g, a);
    unpark(g, take);
    playback().fromJson(json{{"minTime", g.node(take)->as<double>(TakeNode::kStart)},
                             {"maxTime", g.node(take)->as<double>(TakeNode::kEnd)},
                             {"fps", g.node(take)->as<double>(TakeNode::kFps)}});
    g.evaluate();
    fn();
}

std::vector<NodeId> selectTakes(Graph& g, const json& selector, NodeId root)
{
    auto related = [&](NodeId t) {
        NodeId r = rootOf(g, t);
        if (!r || root == kNoNode)
            return true;
        for (NodeId p = root; p != kNoNode; p = g.parent(p))
            if (p == r)
                return true;
        for (NodeId p = r; p != kNoNode; p = g.parent(p))
            if (p == root)
                return true;
        return false;
    };
    std::vector<NodeId> out;
    std::vector<std::string> names;
    if (selector.is_string() && selector.get<std::string>() != "all")
        names.push_back(selector.get<std::string>());
    else if (selector.is_array())
        for (const json& n : selector)
            if (n.is_string())
                names.push_back(n.get<std::string>());
    for (NodeId t : takes(g))
    {
        if (!related(t))
            continue;
        if (selector.is_string() && selector.get<std::string>() == "all")
            out.push_back(t);
        else if (std::find(names.begin(), names.end(), g.node(t)->name()) != names.end())
            out.push_back(t);
    }
    return out;
}

void registerTakeTypes()
{
    if (types::find("take"))
        return;
    const uint32_t stored = AttrInput | AttrStorable;
    NodeType t;
    t.name = "take";
    t.icon = "movie";
    t.doc = "A named clip: the animation of a character (root and below) or the whole scene, with its range and fps.";
    t.ownAttrs = {
        {.name = "root", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrInput | AttrHidden,
         .doc = "Connected from the character root's worldMatrix; unconnected for the whole scene."},
        {.name = "start", .type = PlugType::Float, .defaultValue = 0.0, .flags = stored, .doc = "Range start, seconds."},
        {.name = "end", .type = PlugType::Float, .defaultValue = 4.0, .flags = stored, .doc = "Range end, seconds."},
        {.name = "fps", .type = PlugType::Float, .defaultValue = 30.0, .flags = stored, .doc = "Frames per second."},
        {.name = "loop", .type = PlugType::Bool, .defaultValue = false, .flags = stored,
         .doc = "The take loops: its curves' last keys mirror their first."},
        {.name = "active", .type = PlugType::Bool, .defaultValue = false, .flags = stored | AttrHidden,
         .doc = "This take's animation is the one connected."},
        {.name = "order", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = stored | AttrHidden, .doc = "List position."},
        {.name = "parked", .type = PlugType::String, .defaultValue = std::string(), .flags = stored | AttrHidden,
         .doc = "While inactive: [[destination plug, source plug], ...] of its animation."},
        {.name = "folder", .type = PlugType::String, .defaultValue = std::string(), .flags = stored,
         .doc = "Folder path the take is filed under in take lists, '/' separated; empty for the top level."},
    };
    t.create = [] { return std::make_unique<TakeNode>(); };
    types::registerType(std::move(t));
}

namespace
{
Graph& graph() { return Graph::current(); }

NodeId takeNamed(Graph& g, const std::string& name, const char* cmd)
{
    Node* n = g.find(name);
    if (!n || !isTake(*n))
        throw CommandError(std::string(cmd) + ": no take named '" + name + "'");
    return n->id();
}

std::string onlyArg(const Args& a, const char* cmd)
{
    if (a.positional.size() != 1 || !a.positional[0].is_string())
        throw CommandError(std::string(cmd) + ": name one take");
    return a.positional[0].get<std::string>();
}

json takeJson(Graph& g, NodeId t)
{
    Node& n = *g.node(t);
    return {{"name", n.name()},
            {"root", rootOf(g, t) ? g.node(rootOf(g, t))->name() : std::string()},
            {"start", n.as<double>(TakeNode::kStart)},
            {"end", n.as<double>(TakeNode::kEnd)},
            {"fps", n.as<double>(TakeNode::kFps)},
            {"loop", n.as<bool>(TakeNode::kLoop)},
            {"active", n.as<bool>(TakeNode::kActive)},
            {"folder", n.as<std::string>(TakeNode::kFolder)},
            {"channels", takeConnections(g, t).size()}};
}

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
}
} // namespace

void registerTakeCommands()
{
    using T = ArgType;
    const std::vector<FlagDef> settings = {
        {"start", "s", T::Float, nullptr, "Range start, seconds. Default: the playback range."},
        {"end", "e", T::Float, nullptr, "Range end, seconds. Default: the playback range."},
        {"fps", "fps", T::Float, nullptr, "Frames per second. Default: the scene's."},
        {"loop", "l", T::Bool, nullptr, "Loop the take (loop keys on its curves)."},
        {"folder", "fo", T::String, nullptr, "Folder path the take is filed under, '/' separated; empty for the top level."},
    };

    std::vector<FlagDef> createFlags = {{"name", "n", T::String, nullptr, "Take name."},
                                        {"root", "r", T::String, nullptr, "Character root; default the whole scene."},
                                        {"activate", "a", T::Bool, false, "Switch to the new take."}};
    createFlags.insert(createFlags.end(), settings.begin(), settings.end());
    cmds::registerCommand({
        .name = "createTake",
        .doc = "Make a take for a character root or the whole scene. The first one adopts the animation already there "
               "and is active; later ones start empty. Returns its name.",
        .flags = createFlags,
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            TakeSettings s;
            s.root = a.get<std::string>("root");
            if (!s.root.empty() && !g.find(s.root))
                throw CommandError("createTake: no node named '" + s.root + "'");
            s.start = a.get<double>("start", playback().minTime);
            s.end = a.get<double>("end", playback().maxTime);
            s.fps = a.get<double>("fps", playback().fps);
            s.loop = a.get<bool>("loop", false);
            NodeId t = guarded([&] { return createTake(g, a.get<std::string>("name", "Take1"), s); });
            if (a.has("folder"))
                g.setValue({t, TakeNode::kFolder}, a.get<std::string>("folder"));
            if (a.get<bool>("activate"))
                setActiveTake(g, t);
            return g.node(t)->name();
        },
    });

    cmds::registerCommand({
        .name = "deleteTake",
        .doc = "Delete a take and its animation.",
        .positionalDoc = "take",
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            deleteTake(g, takeNamed(g, onlyArg(a, "deleteTake"), "deleteTake"));
            return nullptr;
        },
    });

    cmds::registerCommand({
        .name = "renameTake",
        .doc = "Rename a take. Returns the name it got.",
        .positionalDoc = "take, newName",
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            if (a.positional.size() != 2)
                throw CommandError("renameTake: give the take and its new name");
            NodeId t = takeNamed(g, a.positional[0].get<std::string>(), "renameTake");
            return guarded([&] { return g.rename(t, a.positional[1].get<std::string>()); });
        },
    });

    cmds::registerCommand({
        .name = "duplicateTake",
        .doc = "Copy a take and its animation as a new, inactive take. Returns its name.",
        .positionalDoc = "take",
        .flags = {{"name", "n", T::String, nullptr, "Name of the copy."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            NodeId t = takeNamed(g, onlyArg(a, "duplicateTake"), "duplicateTake");
            return g.node(guarded([&] { return duplicateTake(g, t, a.get<std::string>("name")); }))->name();
        },
    });

    cmds::registerCommand({
        .name = "setActiveTake",
        .doc = "Switch to a take: its animation is connected (the take it replaces is set aside) and its range and fps "
               "become the playback settings.",
        .positionalDoc = "take",
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            NodeId t = takeNamed(g, onlyArg(a, "setActiveTake"), "setActiveTake");
            setActiveTake(g, t);
            return g.node(t)->name();
        },
    });

    cmds::registerCommand({
        .name = "editTake",
        .doc = "Change a take's range, fps, loop or folder. Returns its settings.",
        .positionalDoc = "take",
        .flags = settings,
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            NodeId t = takeNamed(g, onlyArg(a, "editTake"), "editTake");
            if (a.has("start"))
                g.setValue({t, TakeNode::kStart}, a.get<double>("start"));
            if (a.has("end"))
                g.setValue({t, TakeNode::kEnd}, a.get<double>("end"));
            if (a.has("fps"))
            {
                if (a.get<double>("fps") <= 0.0)
                    throw CommandError("editTake: fps must be positive");
                g.setValue({t, TakeNode::kFps}, a.get<double>("fps"));
            }
            if (a.has("folder"))
                g.setValue({t, TakeNode::kFolder}, a.get<std::string>("folder"));
            if (a.has("loop"))
            {
                g.setValue({t, TakeNode::kLoop}, a.get<bool>("loop"));
                if (g.node(t)->as<bool>(TakeNode::kActive))
                    applyLoop(g, t);
            }
            if (g.node(t)->as<bool>(TakeNode::kActive))
                applyPlayback(g, t, true);
            return takeJson(g, t);
        },
    });

    cmds::registerCommand({
        .name = "listTakes",
        .doc = "Every take: name, root, start, end, fps, loop, active and how many channels it animates.",
        .undoable = false,
        .run = [](const Args&) -> json {
            Graph& g = graph();
            json out = json::array();
            for (NodeId t : takes(g))
                out.push_back(takeJson(g, t));
            return out;
        },
    });
}
} // namespace lila::scene::anim
