#include "scene/SceneCommands.h"

#include "ops/Command.h"
#include "core/Events.h"
#include "core/PrefStore.h"
#include "core/Workspace.h"
#include "scene/Anim.h"
#include "media/Media.h"
#include "media/SoundCues.h"
#include "scene/AnimLayer.h"
#include "scene/Take.h"
#include "scene/BatchRename.h"
#include "scene/Camera.h"
#include "scene/Light.h"
#include "scene/LightingChannels.h"
#include "scene/Components.h"
#include "scene/ComponentValues.h"
#include "scene/Deformers.h"
#include "scene/Diagnostics.h"
#include "scene/Drivers.h"
#include "scene/ArrayGroup.h"
#include "scene/AttrComponents.h"
#include "scene/Duplicate.h"
#include "scene/Instancing.h"
#include "scene/DisplayLayers.h"
#include "scene/Ghosts.h"
#include "scene/Graph.h"
#include "scene/LodGroup.h"
#include "scene/ProxySet.h"
#include "scene/ImagePlane.h"
#include "scene/Materials.h"
#include "scene/Measure.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/ObjectSets.h"
#include "scene/Turntable.h"
#include "scene/Picker.h"
#include "scene/Poses.h"
#include "scene/PostProcess.h"
#include "scene/Relocate.h"
#include "scene/RenderSettings.h"
#include "scene/Rig.h"
#include "scene/SceneData.h"
#include "scene/SceneGraph.h"
#include "scene/SceneScripts.h"
#include "scene/Simulation.h"
#include "scene/Spaces.h"
#include "scene/TextureFolders.h"
#include "scene/Transform.h"

#include <glm/gtc/matrix_transform.hpp>

#include <functional>

namespace lila::scene
{
namespace
{
Graph& graph() { return Graph::current(); }

Node& nodeNamed(const std::string& name)
{
    Node* n = graph().find(name);
    if (!n)
        throw CommandError("no node named '" + name + "'");
    return *n;
}

/** Every added attribute under the category, and the category's name out of usdApiSchemas (USD components). */
void deleteCategory(NodeId id, const std::string& category)
{
    const Node* n = graph().node(id);
    for (int i = int(n->type().attrs.size()) - 1; i >= 0; --i)
        if (const AttrDef& d = n->type().attrs[size_t(i)]; (d.flags & AttrUser) && d.category == category)
            graph().deleteAttr({id, i});
    const int list = n->type().attrIndex("usdApiSchemas");
    if (list < 0 || n->type().attrs[size_t(list)].type != PlugType::String || !(n->type().attrs[size_t(list)].flags & AttrArray))
        return;
    std::vector<PlugValue> kept;
    for (const auto& [index, v] : n->elements(list))
        if (std::get<std::string>(v) != category)
            kept.push_back(v);
    if (kept.size() != n->elements(list).size())
        graph().setElements({id, list, -1}, kept);
}

std::vector<NodeId> idsOf(const std::vector<std::string>& names)
{
    std::vector<NodeId> ids;
    for (const std::string& n : names)
        ids.push_back(nodeNamed(n).id());
    return ids;
}

json namesOf(const std::vector<NodeId>& ids)
{
    json out = json::array();
    for (NodeId id : ids)
        if (Node* n = graph().node(id))
            out.push_back(n->path());
    return out;
}

PlugRef plugArg(const std::string& path)
{
    try
    {
        return graph().plug(path);
    }
    catch (const std::invalid_argument& e)
    {
        throw CommandError(e.what());
    }
}

/** Re-throws graph errors as user-facing command errors. */
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

NodeId parentArg(const Args& a)
{
    if (!a.has("parent"))
        return kNoNode;
    return nodeNamed(a.get<std::string>("parent")).id();
}

/** A compound declared with addAttr(at='compound') that has no children yet; it exists once its first child is added. */
struct PendingCompound
{
    bool multi = false;
};

std::map<std::pair<NodeId, std::string>, PendingCompound>& pendingCompounds()
{
    static std::map<std::pair<NodeId, std::string>, PendingCompound> pending;
    return pending;
}

/** Whether a child of compound 'group' on n is an array: from its declaration, or from the children it already has. */
bool compoundIsMulti(const Node& n, const std::string& group)
{
    if (group.find('.') != std::string::npos || group.find('[') != std::string::npos)
        throw CommandError("addAttr: compounds nest one level; '" + group + "' is already a child");
    if (auto it = pendingCompounds().find({n.id(), group}); it != pendingCompounds().end())
        return it->second.multi;
    std::vector<int> children = n.type().groupChildren(group);
    if (children.empty())
        throw CommandError("addAttr: '" + n.name() + "' has no compound '" + group + "'; add it with attributeType='compound' first");
    return n.type().attrs[size_t(children[0])].flags & AttrArray;
}

json create(const std::string& type, const Args& a)
{
    const NodeType* t = types::find(type);
    if (!t)
        throw CommandError("unknown node type '" + type + "'");
    Node* n = guarded([&] { return graph().createNode(*t, a.get<std::string>("name", ""), parentArg(a)); });
    if (!a.get<bool>("skipSelect", false) && t->dag)
        graph().select({n->id()});
    return n->path();
}

/** The transforms among ids, the ones pickWalk steps between. */
std::vector<NodeId> walkable(const Graph& g, const std::vector<NodeId>& ids)
{
    std::vector<NodeId> out;
    for (NodeId id : ids)
        if (const Node* n = g.node(id); n && n->type().isA("transform"))
            out.push_back(id);
    return out;
}

/** A deleted object's place in the hierarchy, so a pickWalk right after the delete can walk on from it. */
struct WalkGhost
{
    NodeId id = kNoNode;
    NodeId parent = kNoNode;
    std::vector<NodeId> siblings; // walkable, itself included, in order
};

struct WalkGhosts
{
    const Graph* graph = nullptr;
    uint64_t selectionSerial = 0;
    std::vector<WalkGhost> ghosts;
};

WalkGhosts& walkGhosts()
{
    static WalkGhosts state;
    return state;
}

/** The remembered deleted objects while nothing has been selected since and they are still gone; empty otherwise. */
const std::vector<WalkGhost>* liveWalkGhosts(const Graph& g)
{
    const WalkGhosts& w = walkGhosts();
    if (w.ghosts.empty() || w.graph != &g || w.selectionSerial != g.selectionSerial() || !g.selection().empty())
        return nullptr;
    for (const WalkGhost& ghost : w.ghosts)
        if (g.node(ghost.id))
            return nullptr;
    return &w.ghosts;
}

/** Where pickWalk goes from a deleted object: its nearest surviving sibling that way, or its parent; kNoNode for nowhere. */
NodeId walkFromGhost(const Graph& g, const WalkGhost& ghost, const std::string& dir)
{
    if (dir == "up")
        return ghost.parent != kNoNode && g.node(ghost.parent) ? ghost.parent : kNoNode;
    if (dir != "left" && dir != "right")
        return kNoNode;
    auto at = std::find(ghost.siblings.begin(), ghost.siblings.end(), ghost.id);
    if (at == ghost.siblings.end())
        return kNoNode;
    const size_t n = ghost.siblings.size(), from = size_t(at - ghost.siblings.begin());
    for (size_t step = 1; step < n; ++step)
    {
        const NodeId sib = ghost.siblings[dir == "right" ? (from + step) % n : (from + n - step) % n];
        if (g.node(sib) && g.parent(sib) == ghost.parent)
            return sib;
    }
    return kNoNode;
}
} // namespace

void loadNamingPolicy()
{
    graph().setNamingPolicy(NamingPolicy::fromJson(PrefStore::get().merged("naming")));
}

void registerTransformCommands();
void registerMeshEditCommands();
void registerConformCommands();
void registerTransferCommands();
void registerBooleanCommands();
void registerSmartJoinCommands();
void registerObjectCommands();
void registerBoundingMeshCommands();
void registerComponentQueryCommands();

void registerCommands()
{
    using T = ArgType;

    registerTransformCommands();
    registerMeshCommands();
    registerMeshFileCommands();
    registerMeshEditCommands();
    registerConformCommands();
    registerComponentQueryCommands();
    registerTransferCommands();
    registerBooleanCommands();
    registerSmartJoinCommands();
    registerBatchRenameCommands();
    registerObjectCommands();
    duplicate::registerCommands();
    diagnostics::registerCommands();
    instancing::registerCommands();
    arrays::registerCommands();
    registerBoundingMeshCommands();
    modifiers::registerCommands();
    components::registerCommands();
    materials::registerCommands();
    registerRigCommands();
    deformers::registerCommands();
    registerMeasureCommands();
    registerImagePlaneCommands();
    lod::registerCommands();
    proxy::registerCommands();
    registerCameraCommands();
    registerLightCommands();
    postProcess::registerCommands();
    spaces::registerCommands();
    anim::registerCommands();
    drivers::registerCommands();
    lightingChannels::registerCommands();
    attrCommands::registerCommands();
    componentValues::registerCommands();
    sceneGraph::registerCommands();
    poses::registerCommands();
    picker::registerCommands();
    ghosts::registerCommands();
    anim::registerLayerCommands();
    anim::registerTakeCommands();
    media::registerMediaCommands();
    media::registerSoundCueCommands();
    sim::registerCommands();
    sim::registerCommands();
    registerRenderSettingsCommands();
    scripts::registerCommands();
    data::registerCommands();
    textureFolders::registerCommands();
    displayLayers::registerCommands();
    objectSets::registerCommands();
    turntable::registerCommands();
    relocate::registerCommands();

    loadNamingPolicy();
    PrefStore::get().onChanged([](const std::string& file) {
        if (file == "naming")
            loadNamingPolicy();
    });

    ops::registerCommand({
        .name = "namingConvention",
        .doc = "Naming rules for this workspace (.lila/naming.json). Query with no flags; check lists violations; fix "
               "renames them, whether or not enforcement is on. check and fix cover the given objects and everything under "
               "them, or the whole scene; referenced nodes keep their names.",
        .positionalDoc = "[objects]",
        .flags = {
            {"enable", "e", T::Bool, nullptr, "Turn enforcement on or off for the workspace (user prefs if none open)."},
            {"mode", "m", T::String, nullptr, "fix | warn | block"},
            {"uniqueNames", "un", T::Bool, nullptr,
             "Make names unique across the whole scene, not just among siblings (workspace, else user prefs)."},
            {"check", "c", T::Bool, false, "Return [[node, expectedName], ...] for names that break the rules."},
            {"fix", "f", T::Bool, false, "Rename every node that breaks the rules."},
        },
        .run = [](const Args& a) -> json {
            PrefScope scope = Workspace::get().isOpen() ? PrefScope::Workspace : PrefScope::User;
            if (a.has("enable"))
                PrefStore::get().set("naming", "/enabled", a.get<bool>("enable"), scope);
            if (a.has("mode"))
                PrefStore::get().set("naming", "/mode", a.get<std::string>("mode"), scope);
            if (a.has("uniqueNames"))
                PrefStore::get().set("naming", "/uniqueNames", a.get<bool>("uniqueNames"), scope);
            if (a.has("enable") || a.has("mode") || a.has("uniqueNames"))
                loadNamingPolicy();

            if (a.get<bool>("check") || a.get<bool>("fix"))
            {
                NamingPolicy policy = graph().namingPolicy();
                policy.enabled = true;
                std::vector<NodeId> ids;
                if (a.positional.empty())
                    ids = graph().nodes();
                else
                    for (const json& o : a.positional)
                    {
                        Node* n = graph().find(o.get<std::string>());
                        if (!n)
                            throw CommandError("namingConvention: no object '" + o.get<std::string>() + "'");
                        ids.push_back(n->id());
                        std::ranges::copy(graph().descendants(n->id()), std::back_inserter(ids));
                    }
                json out = json::array();
                for (NodeId id : ids)
                {
                    Node* n = graph().node(id);
                    if (n->referenceId())
                        continue;
                    std::string was = n->name();
                    std::string want = policy.conform(n->type(), was);
                    if (want == was)
                        continue;
                    std::string at = graph().path(id);
                    if (a.get<bool>("fix"))
                        want = guarded([&] { return materials::rename(graph(), id, want); });
                    out.push_back({at, want});
                }
                return out;
            }
            json q = NamingPolicy::defaults();
            q.merge_patch(PrefStore::get().merged("naming"));
            return q;
        },
    });

    ops::setSelectionProvider([] {
        std::vector<std::string> out;
        for (NodeId id : graph().selection())
            out.push_back(graph().path(id));
        return out;
    });

    ops::registerCommand({
        .name = "createNode",
        .doc = "Create a node of the given type. Returns its path (the name, unless another node shares it).",
        .positionalDoc = "type",
        .flags = {
            {"name", "n", T::String, nullptr, "Name; made unique among its siblings."},
            {"parent", "p", T::String, nullptr, "Parent transform."},
            {"skipSelect", "ss", T::Bool, false, "Don't select the new node."},
        },
        .run = [](const Args& a) -> json {
            if (a.positional.empty() || !a.positional[0].is_string())
                throw CommandError("createNode: node type required");
            return create(a.positional[0].get<std::string>(), a);
        },
    });

    ops::registerCommand({
        .name = "spaceLocator",
        .doc = "Create a locator. Returns its path.",
        .flags = {
            {"name", "n", T::String, nullptr, "Name; made unique among its siblings."},
            {"position", "p", T::Vec3, nullptr, "World position."},
            {"skipSelect", "ss", T::Bool, false, "Don't select the new locator."},
        },
        .run = [](const Args& a) -> json {
            Args c = a;
            c.flags.erase("position");
            json name = create("locator", c);
            if (a.has("position"))
                graph().setValue(graph().plug(name.get<std::string>() + ".translate"),
                                 glm::dvec3(a.flags["position"][0], a.flags["position"][1], a.flags["position"][2]));
            return name;
        },
    });

    ops::registerCommand({
        .name = "group",
        .doc = "Group objects under a new transform, or create an empty group with empty=True.",
        .positionalDoc = "objects",
        .flags = {
            {"name", "n", T::String, nullptr, "Group name."},
            {"empty", "em", T::Bool, false, "Create an empty group."},
            {"parent", "p", T::String, nullptr, "Parent for the new group."},
        },
        .run = [](const Args& a) -> json {
            std::vector<NodeId> members = a.get<bool>("empty") ? std::vector<NodeId>{} : idsOf(a.objects());
            Args c = a;
            if (!c.has("name"))
                c.flags["name"] = "Group";
            c.flags["skipSelect"] = true;
            std::string name = create("transform", c).get<std::string>();
            NodeId g = graph().find(name)->id();
            for (NodeId m : members)
                guarded([&] { graph().setParent(m, g); return 0; });
            graph().select({g});
            return graph().path(g);
        },
    });

    ops::registerCommand({
        .name = "delete",
        .doc = "Delete objects (and their children). Defaults to the selection.",
        .positionalDoc = "objects",
        .run = [](const Args& a) -> json {
            auto ids = idsOf(a.objects());
            for (NodeId id : ids)
            {
                std::vector<NodeId> doomed = graph().descendants(id);
                doomed.push_back(id);
                for (NodeId d : doomed)
                    if (graph().node(d)->referenceId())
                        throw CommandError("delete: '" + graph().path(d) +
                                           "' comes from a reference; remove or unload the reference instead");
            }
            Graph& g = graph();
            std::vector<WalkGhost> ghosts;
            for (NodeId id : ids)
                if (g.isSelected(id))
                {
                    const NodeId parent = g.parent(id);
                    ghosts.push_back({id, parent, walkable(g, parent == kNoNode ? g.roots() : g.children(parent))});
                }
            for (NodeId id : ids)
                g.deleteNode(id);
            if (g.selection().empty())
                walkGhosts() = {&g, g.selectionSerial(), std::move(ghosts)};
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "rename",
        .doc = "rename(old, new) or rename(new) for the single selected object. Returns the renamed node's path.",
        .positionalDoc = "[object] newName",
        .run = [](const Args& a) -> json {
            std::string target, newName;
            if (a.positional.size() == 2)
            {
                target = a.positional[0].get<std::string>();
                newName = a.positional[1].get<std::string>();
            }
            else if (a.positional.size() == 1 && graph().selection().size() == 1)
            {
                target = graph().path(graph().selection()[0]);
                newName = a.positional[0].get<std::string>();
            }
            else
                throw CommandError("rename: expected (object, newName)");
            if (nodeNamed(target).referenceId())
                throw CommandError("rename: '" + target + "' comes from a reference and keeps its name");
            NodeId id = nodeNamed(target).id();
            guarded([&] { return materials::rename(graph(), id, newName); });
            return graph().path(id);
        },
    });

    ops::registerCommand({
        .name = "parent",
        .doc = "parent(child..., parent), or parent(child..., world=True). Keeps world transforms unless relative=True. "
               "chain=True parents each object under the next one, in order: (a, b, c) puts a under b and b under c. "
               "index=N places the objects, in order, at position N among the parent's children (0 is first), which "
               "also reorders objects already under that parent. Leaves the moved objects selected.",
        .positionalDoc = "objects... parent",
        .flags = {
            {"world", "w", T::Bool, false, "Unparent to the world."},
            {"relative", "r", T::Bool, false, "Keep local transforms instead of world."},
            {"chain", "ch", T::Bool, false, "Parent in selection order, each under the next."},
            {"index", "i", T::Int, nullptr, "Position among the parent's children, counted without the moved objects."},
        },
        .run = [](const Args& a) -> json {
            std::vector<std::string> objs = a.objects();
            if (a.get<bool>("chain"))
            {
                if (objs.size() < 2)
                    throw CommandError("parent: chain needs at least two objects");
                std::vector<NodeId> ids = idsOf(objs);
                for (size_t i = 0; i + 1 < ids.size(); ++i)
                    guarded([&] { graph().setParent(ids[i], ids[i + 1], !a.get<bool>("relative")); return 0; });
                return objs;
            }
            NodeId p = kNoNode;
            if (!a.get<bool>("world"))
            {
                if (objs.size() < 2)
                    throw CommandError("parent: need children and a parent");
                p = nodeNamed(objs.back()).id();
                objs.pop_back();
            }
            std::vector<NodeId> ids = idsOf(objs);
            if (!a.has("index"))
            {
                for (NodeId c : ids)
                    guarded([&] { graph().setParent(c, p, !a.get<bool>("relative")); return 0; });
                graph().select(ids);
                return objs;
            }
            int index = a.get<int>("index");
            if (index < 0)
                throw CommandError("parent: index must be 0 or more");
            std::vector<NodeId> others;
            for (NodeId s : p == kNoNode ? graph().roots() : graph().children(p))
                if (std::find(ids.begin(), ids.end(), s) == ids.end())
                    others.push_back(s);
            // Each object goes in front of the sibling it should precede, so earlier moves don't shift later ones.
            NodeId before = size_t(index) < others.size() ? others[size_t(index)] : kNoNode;
            for (size_t i = ids.size(); i-- > 0;)
            {
                guarded([&] {
                    const auto& sib = p == kNoNode ? graph().roots() : graph().children(p);
                    size_t at = size_t(std::find(sib.begin(), sib.end(), before) - sib.begin());
                    auto self = std::find(sib.begin(), sib.end(), ids[i]);
                    if (self != sib.end() && size_t(self - sib.begin()) < at)
                        --at;
                    graph().setParent(ids[i], p, !a.get<bool>("relative"), at);
                    return 0;
                });
                before = ids[i];
            }
            graph().select(ids);
            return objs;
        },
    });

    ops::registerCommand({
        .name = "setAttr",
        .doc = "setAttr('node.attr', value). Vec3 takes a list or three numbers. lock=True/False locks or unlocks the "
               "plug, with or without a value: a locked plug can't be changed until it is unlocked.",
        .positionalDoc = "plug [value]",
        .flags = {{"lock", "l", T::Bool, nullptr, "Lock (True) or unlock (False) the plug."}},
        .run = [](const Args& a) -> json {
            bool lockOnly = a.has("lock") && a.positional.size() == 1;
            if ((a.positional.size() < 2 && !lockOnly) || !a.positional[0].is_string())
                throw CommandError("setAttr: expected (plug, value)");
            PlugRef p = plugArg(a.positional[0].get<std::string>());
            // Unlock before setting and lock after, so setAttr(plug, v, lock=...) works either way.
            if (a.has("lock") && !a.get<bool>("lock"))
                guarded([&] { graph().setLocked(p, false); return 0; });
            if (!lockOnly && (graph().attr(p).flags & AttrArray) && p.element < 0)
            {
                if (!a.positional[1].is_array())
                    throw CommandError("setAttr: " + graph().plugName(p) + " is an array; give a list of its values");
                std::vector<PlugValue> values;
                for (const json& v : a.positional[1])
                    values.push_back(guarded([&] { return fromJson(graph().attr(p), v); }));
                guarded([&] { graph().setElements(p, values); return 0; });
            }
            else if (!lockOnly)
            {
                json v = a.positional.size() == 4 ? json::array({a.positional[1], a.positional[2], a.positional[3]}) : a.positional[1];
                AttrDef def = graph().attr(p);
                if (p.component >= 0)
                    def.type = PlugType::Float;
                guarded([&] { graph().setValue(p, fromJson(def, v)); return 0; });
            }
            if (a.has("lock") && a.get<bool>("lock"))
                guarded([&] { graph().setLocked(p, true); return 0; });
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "getAttr",
        .doc = "Value of 'node.attr'; a whole array gives its elements' values in index order. lock=True returns whether it is locked "
               "instead; multiIndices=True an array's element indices.",
        .positionalDoc = "plug",
        .flags = {{"lock", "l", T::Bool, false, "Return the lock state instead of the value."},
                  {"multiIndices", "mi", T::Bool, false, "Return an array's element indices, ascending."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.positional.empty() || !a.positional[0].is_string())
                throw CommandError("getAttr: expected a plug");
            PlugRef p = plugArg(a.positional[0].get<std::string>());
            if (a.get<bool>("lock"))
                return graph().isLocked(p);
            if (a.get<bool>("multiIndices"))
            {
                json out = json::array();
                for (const PlugRef& e : graph().elements(p))
                    out.push_back(e.element);
                return out;
            }
            if ((graph().attr(p).flags & AttrArray) && p.element < 0)
            {
                json out = json::array();
                for (const PlugRef& e : graph().elements(p))
                    out.push_back(toJson(guarded([&] { return graph().value(e); })));
                return out;
            }
            return toJson(guarded([&] { return graph().value(p); }));
        },
    });

    ops::registerCommand({
        .name = "connectAttr",
        .doc = "connectAttr('src.attr', 'dst.attr'). A whole array input ('loft1.sections') gets a new last element. Returns the destination plug.",
        .positionalDoc = "src dst",
        .flags = {{"force", "f", T::Bool, false, "Replace an existing input connection."}},
        .run = [](const Args& a) -> json {
            if (a.positional.size() != 2)
                throw CommandError("connectAttr: expected (src, dst)");
            PlugRef s = plugArg(a.positional[0]), d = plugArg(a.positional[1]);
            return graph().plugName(guarded([&] { return graph().connect(s, d, a.get<bool>("force")); }));
        },
    });

    ops::registerCommand({
        .name = "disconnectAttr",
        .doc = "disconnectAttr('dst.attr'), or (src, dst). A whole array input loses every element, or with src only the ones src drives.",
        .positionalDoc = "[src] dst",
        .run = [](const Args& a) -> json {
            if (a.positional.empty())
                throw CommandError("disconnectAttr: expected a plug");
            PlugRef d = plugArg(a.positional.back());
            if (!(graph().attr(d).flags & AttrArray) || d.element >= 0)
            {
                graph().disconnect(d);
                return nullptr;
            }
            std::optional<PlugRef> from;
            if (a.positional.size() > 1)
                from = plugArg(a.positional.front());
            for (const PlugRef& e : graph().elements(d))
                if (!from || graph().source(e) == from)
                    graph().disconnect(e);
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "addAttr",
        .doc = "Add an attribute to objects (Maya's addAttr): attributeType for a single value or, with multi, an array of them; "
               "dataType for strings and arrays (stringArray, doubleArray, Int32Array, vectorArray). attributeType='compound' "
               "groups the attributes then added with parent=; a multi compound gives each element every child, "
               "node.targets[2].weight. Compounds nest one level. Saved with the scene. Returns the new plugs.",
        .positionalDoc = "objects",
        .flags = {
            {"longName", "ln", T::String, nullptr, "The attribute's name."},
            {"shortName", "sn", T::String, nullptr, "A short name."},
            {"niceName", "nn", T::String, nullptr, "The label editors show."},
            {"attributeType", "at", T::String, nullptr, "bool, long, float, double, double3, enum, matrix or compound (Lila's int, vec3 too)."},
            {"parent", "p", T::String, nullptr, "The compound this attribute is a child of."},
            {"numberOfChildren", "nc", T::Int, nullptr, "Accepted for Maya scripts: a compound takes any number of children."},
            {"dataType", "dt", T::String, nullptr, "string, stringArray, doubleArray, floatArray, Int32Array, vectorArray or matrix."},
            {"multi", "m", T::Bool, false, "An array of attributeType: any number of elements, attr[0], attr[1] ..."},
            {"defaultValue", "dv", T::Any, nullptr, "Default value (each element's, for an array)."},
            {"minValue", "min", T::Float, nullptr, "Lowest value it takes: lower values are clamped."},
            {"maxValue", "max", T::Float, nullptr, "Highest value it takes: higher values are clamped."},
            {"softMinValue", "smn", T::Float, nullptr, "Slider minimum."},
            {"softMaxValue", "smx", T::Float, nullptr, "Slider maximum."},
            {"enumName", "en", T::String, nullptr, "Enum labels, 'low:medium:high'."},
            {"keyable", "k", T::Bool, false, "Show it in the channel box, where it can be keyed."},
            {"hidden", "h", T::Bool, false, "Hide it from editors."},
            {"usedAsColor", "uac", T::Bool, false, "A double3 holding a colour: editors show a swatch."},
            {"usedAsFilename", "uaf", T::Bool, false, "A string holding a file path: editors offer a file browser."},
            {"category", "", T::String, nullptr, "A heading the Attribute Editor groups it under."},
            {"doc", "", T::String, nullptr, "What it is for: editors show it as a tooltip."},
        },
        .run = [](const Args& a) -> json {
            std::vector<std::string> objs = a.objects();
            if (objs.empty())
                throw CommandError("addAttr: no object");
            if (!a.has("longName"))
                throw CommandError("addAttr: give longName");
            if (a.get<std::string>("attributeType", "") == "compound")
            {
                const std::string name = a.get<std::string>("longName");
                if (a.has("parent"))
                    throw CommandError("addAttr: compounds nest one level; '" + name + "' can't have a parent");
                if (!Graph::validName(name))
                    throw CommandError("addAttr: invalid attribute name '" + name + "'");
                json out = json::array();
                for (const std::string& o : objs)
                {
                    Node& n = nodeNamed(o);
                    if (n.type().attrIndex(name) >= 0 || !n.type().groupChildren(name).empty() || pendingCompounds().contains({n.id(), name}))
                        throw CommandError("addAttr: '" + n.name() + "' already has an attribute '" + name + "'");
                    pendingCompounds()[{n.id(), name}] = {a.get<bool>("multi")};
                    out.push_back(n.path() + "." + name);
                }
                return out;
            }
            AttrDef def;
            def.name = a.get<std::string>("longName");
            def.shortName = a.get<std::string>("shortName", "");
            def.niceName = a.get<std::string>("niceName", "");
            def.doc = a.get<std::string>("doc", "");
            static const std::map<std::string, std::pair<PlugType, bool>> kinds = {
                {"bool", {PlugType::Bool, false}},       {"long", {PlugType::Int, false}},         {"short", {PlugType::Int, false}},
                {"byte", {PlugType::Int, false}},        {"int", {PlugType::Int, false}},          {"float", {PlugType::Float, false}},
                {"double", {PlugType::Float, false}},    {"doubleLinear", {PlugType::Float, false}}, {"doubleAngle", {PlugType::Float, false}},
                {"double3", {PlugType::Vec3, false}},    {"float3", {PlugType::Vec3, false}},      {"vec3", {PlugType::Vec3, false}},
                {"enum", {PlugType::Enum, false}},       {"matrix", {PlugType::Matrix, false}},    {"string", {PlugType::String, false}},
                {"stringArray", {PlugType::String, true}}, {"doubleArray", {PlugType::Float, true}}, {"floatArray", {PlugType::Float, true}},
                {"Int32Array", {PlugType::Int, true}},   {"intArray", {PlugType::Int, true}},      {"vectorArray", {PlugType::Vec3, true}},
                {"pointArray", {PlugType::Vec3, true}}};
            std::string kind = a.has("dataType") ? a.get<std::string>("dataType") : a.get<std::string>("attributeType", "float");
            auto k = kinds.find(kind);
            if (k == kinds.end())
                throw CommandError("addAttr: unknown type '" + kind + "'");
            def.type = k->second.first;
            if (k->second.second || a.get<bool>("multi"))
                def.flags |= AttrArray;
            def.defaultValue = defaultFor(def.type);
            if (def.type == PlugType::Enum)
            {
                std::string names = a.get<std::string>("enumName", "");
                for (size_t start = 0; start <= names.size();)
                {
                    size_t end = names.find(':', start);
                    std::string n = names.substr(start, end == std::string::npos ? std::string::npos : end - start);
                    if (!n.empty())
                        def.enumNames.push_back(n);
                    if (end == std::string::npos)
                        break;
                    start = end + 1;
                }
                if (def.enumNames.empty())
                    throw CommandError("addAttr: an enum needs enumName, 'a:b:c'");
            }
            if (a.has("defaultValue"))
                def.defaultValue = guarded([&] { return fromJson(def, a.get<json>("defaultValue")); });
            for (auto [flag, slot] : {std::pair{"minValue", &def.minValue}, {"maxValue", &def.maxValue}, {"softMinValue", &def.softMin},
                                      {"softMaxValue", &def.softMax}})
                if (a.has(flag))
                    *slot = a.get<double>(flag);
            if (a.get<bool>("keyable"))
                def.flags |= AttrKeyable;
            if (a.get<bool>("hidden"))
                def.flags |= AttrHidden;
            if (a.get<bool>("usedAsColor"))
                def.flags |= AttrColor;
            if (a.get<bool>("usedAsFilename"))
                def.flags |= AttrFile;
            def.category = a.get<std::string>("category", "");
            const std::string group = a.get<std::string>("parent", "");
            json out = json::array();
            for (const std::string& o : objs)
            {
                Node& n = nodeNamed(o);
                AttrDef child = def;
                if (!group.empty())
                {
                    const bool multi = compoundIsMulti(n, group);
                    if (multi && (child.flags & AttrArray))
                        throw CommandError("addAttr: '" + group + "' is a multi compound: its children hold one value per element, not arrays");
                    child.name = group + "." + def.name;
                    if (!def.shortName.empty())
                        child.shortName = group + "." + def.shortName;
                    if (multi)
                        child.flags |= AttrArray;
                }
                int index = guarded([&] { return graph().addAttr(n.id(), child); });
                pendingCompounds().erase({n.id(), group});
                out.push_back(graph().plugName({n.id(), index}));
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "deleteAttr",
        .doc = "Delete attributes added with addAttr: deleteAttr('node.attr'), deleteAttr(objects, attribute=name), or a whole "
               "component with deleteAttr(objects, category=name), which also takes the name out of the object's usdApiSchemas.",
        .positionalDoc = "plugs or objects",
        .flags = {{"attribute", "at", T::String, nullptr, "The attribute to delete from each object."},
                  {"category", "", T::String, nullptr, "Delete every added attribute under this Attribute Editor heading."}},
        .run = [](const Args& a) -> json {
            if (a.has("category"))
            {
                const std::string category = a.get<std::string>("category");
                for (const std::string& o : a.objects())
                    deleteCategory(nodeNamed(o).id(), category);
                return nullptr;
            }
            std::vector<std::string> paths;
            if (a.has("attribute"))
                for (const std::string& o : a.objects())
                    paths.push_back(nodeNamed(o).path() + "." + a.get<std::string>("attribute"));
            else
                for (const json& v : a.positional)
                    paths.push_back(v.get<std::string>());
            if (paths.empty())
                throw CommandError("deleteAttr: expected 'node.attr'");
            for (const std::string& path : paths)
            {
                // A compound goes with all its children.
                if (size_t dot = path.find('.'); dot != std::string::npos)
                {
                    Node& n = nodeNamed(path.substr(0, dot));
                    std::string group = path.substr(dot + 1);
                    std::vector<int> children = n.type().groupChildren(group);
                    if (pendingCompounds().erase({n.id(), group}) || (!children.empty() && n.type().attrIndex(group) < 0))
                    {
                        for (auto c = children.rbegin(); c != children.rend(); ++c)
                            guarded([&] { graph().deleteAttr({n.id(), *c}); return 0; });
                        continue;
                    }
                }
                PlugRef p = plugArg(path);
                guarded([&] { graph().deleteAttr({p.node, p.attr}); return 0; });
            }
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "removeMultiInstance",
        .doc = "Remove array elements with their connections and values: 'node.attr[3]', or 'node.group[3]' for every child of a "
               "grouped array.",
        .positionalDoc = "plugs",
        .run = [](const Args& a) -> json {
            for (const json& v : a.positional)
            {
                const std::string path = v.get<std::string>();
                // node.group[3]: no attr is named group, its children are group.<child>.
                size_t dot = path.find('.'), open = path.rfind('[');
                if (dot != std::string::npos && open != std::string::npos && open > dot && path.ends_with(']'))
                {
                    Node& n = nodeNamed(path.substr(0, dot));
                    std::string group = path.substr(dot + 1, open - dot - 1);
                    std::vector<int> children = n.type().groupChildren(group);
                    if (!children.empty() && n.type().attrIndex(group) < 0)
                    {
                        int index = std::stoi(path.substr(open + 1, path.size() - open - 2));
                        for (int c : children)
                            guarded([&] { graph().removeElement({n.id(), c, -1, index}); return 0; });
                        continue;
                    }
                }
                PlugRef p = plugArg(path);
                guarded([&] { graph().removeElement(p); return 0; });
            }
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "listAttr",
        .doc = "Attribute names of a node.",
        .positionalDoc = "object",
        .flags = {
            {"keyable", "k", T::Bool, false, "Only keyable attributes."},
            {"locked", "l", T::Bool, false, "Only locked plugs; a Vec3 lists each locked component (translateX)."},
            {"userDefined", "ud", T::Bool, false, "Only attributes added with addAttr."},
        },
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::vector<std::string> objs = a.objects();
            if (objs.empty())
                throw CommandError("listAttr: no object");
            json out = json::array();
            Node& n = nodeNamed(objs[0]);
            const auto& attrs = n.type().attrs;
            for (int i = 0; i < int(attrs.size()); ++i)
            {
                const AttrDef& d = attrs[i];
                if ((d.flags & AttrHidden) || (a.get<bool>("keyable") && !(d.flags & AttrKeyable)) ||
                    (a.get<bool>("userDefined") && !(d.flags & AttrUser)))
                    continue;
                if (!a.get<bool>("locked"))
                    out.push_back(d.name);
                else if (d.type != PlugType::Vec3 && n.lockMask(i))
                    out.push_back(d.name);
                else if (d.type == PlugType::Vec3)
                    for (int c = 0; c < 3; ++c)
                        if (n.lockMask(i) & (1u << c))
                            out.push_back(d.childNames[c]);
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "ls",
        .doc = "List nodes, optionally filtered by type or the selection.",
        .positionalDoc = "names",
        .flags = {
            {"type", "typ", T::String, nullptr, "Only nodes of this type (or derived)."},
            {"selection", "sl", T::Bool, false, "Only selected nodes, in selection order."},
            {"dag", "", T::Bool, false, "Only transforms in the hierarchy."},
        },
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::vector<NodeId> ids = a.get<bool>("selection") ? graph().selection() : graph().nodes();
            // A short name matching several nodes lists them all, like a pattern would.
            std::vector<NodeId> wanted;
            bool filtered = false;
            for (const json& v : a.positional)
                if (v.is_string())
                {
                    filtered = true;
                    for (NodeId id : graph().findAll(v.get<std::string>()))
                        wanted.push_back(id);
                }
            json out = json::array();
            for (NodeId id : ids)
            {
                Node* n = graph().node(id);
                if (a.has("type") && !n->type().isA(a.get<std::string>("type")))
                    continue;
                if (a.get<bool>("dag") && !n->type().dag)
                    continue;
                if (filtered && std::find(wanted.begin(), wanted.end(), id) == wanted.end())
                    continue;
                out.push_back(graph().path(id));
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "select",
        .doc = "Select objects. Replaces the selection unless add/deselect/toggle.",
        .positionalDoc = "objects",
        .flags = {
            {"add", "add", T::Bool, false, ""},
            {"deselect", "d", T::Bool, false, ""},
            {"toggle", "tgl", T::Bool, false, ""},
            {"clear", "cl", T::Bool, false, "Select nothing."},
        },
        .run = [](const Args& a) -> json {
            if (a.get<bool>("clear"))
            {
                graph().select({});
                return nullptr;
            }
            std::vector<std::string> names;
            for (const json& v : a.positional)
                if (v.is_string())
                    names.push_back(v.get<std::string>());
                else if (v.is_array())
                    for (const json& x : v)
                        names.push_back(x.get<std::string>());
            SelectMode mode = a.get<bool>("add") ? SelectMode::Add
                              : a.get<bool>("deselect") ? SelectMode::Remove
                              : a.get<bool>("toggle") ? SelectMode::Toggle
                                                     : SelectMode::Replace;
            graph().select(idsOf(names), mode);
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "pickWalk",
        .doc = "Walk the selection through the hierarchy like the arrow keys: up to the parent, down to the first child, "
               "left/right to the previous/next sibling (wrapping). Right after a delete, with nothing selected since, it walks from "
               "where the deleted objects were.",
        .flags = {{"direction", "", T::String, "down", "up, down, left or right."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::string dir = a.get<std::string>("direction");
            if (dir != "up" && dir != "down" && dir != "left" && dir != "right")
                throw CommandError("pickWalk: direction must be up, down, left or right");
            std::vector<NodeId> next;
            if (const std::vector<WalkGhost>* ghosts = liveWalkGhosts(g))
            {
                for (const WalkGhost& ghost : *ghosts)
                    if (NodeId to = walkFromGhost(g, ghost, dir); to != kNoNode && std::find(next.begin(), next.end(), to) == next.end())
                        next.push_back(to);
                if (!next.empty())
                    g.select(next);
                return nullptr;
            }
            for (NodeId id : g.selection())
            {
                NodeId to = id;
                if (dir == "up")
                    to = g.parent(id) != kNoNode ? g.parent(id) : id;
                else if (dir == "down")
                {
                    std::vector<NodeId> kids = walkable(g, g.children(id));
                    to = kids.empty() ? id : kids.front();
                }
                else
                {
                    NodeId parent = g.parent(id);
                    std::vector<NodeId> sibs = walkable(g, parent == kNoNode ? g.roots() : g.children(parent));
                    auto it = std::find(sibs.begin(), sibs.end(), id);
                    if (it != sibs.end() && sibs.size() > 1)
                    {
                        size_t i = size_t(it - sibs.begin());
                        to = sibs[dir == "right" ? (i + 1) % sibs.size() : (i + sibs.size() - 1) % sibs.size()];
                    }
                }
                if (std::find(next.begin(), next.end(), to) == next.end())
                    next.push_back(to);
            }
            if (next != g.selection())
                g.select(next);
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "selectHierarchy",
        .doc = "Add every transform below each selected object to the selection, parents before children.",
        .run = [](const Args&) -> json {
            Graph& g = graph();
            std::vector<NodeId> out;
            std::function<void(NodeId)> visit = [&](NodeId id) {
                if (std::find(out.begin(), out.end(), id) == out.end())
                    out.push_back(id);
                for (NodeId kid : g.children(id))
                    if (const Node* n = g.node(kid); n && n->type().isA("transform"))
                        visit(kid);
            };
            for (NodeId id : g.selection())
                visit(id);
            if (out != g.selection())
                g.select(out);
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "objExists",
        .doc = "True if a node with this name or path exists.",
        .positionalDoc = "name",
        .undoable = false,
        .run = [](const Args& a) -> json { return !a.positional.empty() && !graph().findAll(a.positional[0].get<std::string>()).empty(); },
    });

    ops::registerCommand({
        .name = "nodeType",
        .doc = "Type name of a node.",
        .positionalDoc = "object",
        .undoable = false,
        .run = [](const Args& a) -> json {
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("nodeType: no object");
            return nodeNamed(objs[0]).type().name;
        },
    });

    ops::registerCommand({
        .name = "listRelatives",
        .doc = "Children (default), parent or all descendants.",
        .positionalDoc = "object",
        .flags = {
            {"parent", "p", T::Bool, false, ""},
            {"allDescendents", "ad", T::Bool, false, ""},
        },
        .undoable = false,
        .run = [](const Args& a) -> json {
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("listRelatives: no object");
            NodeId id = nodeNamed(objs[0]).id();
            if (a.get<bool>("parent"))
                return namesOf({graph().parent(id)});
            if (a.get<bool>("allDescendents"))
                return namesOf(graph().descendants(id));
            return namesOf(graph().children(id));
        },
    });

    ops::registerCommand({
        .name = "xform",
        .doc = "Query or set the transform of objects, in local or world space.\n\n"
               "Sets every flag given on each object, or with query=True returns one value of the first object: pass the flag "
               "to read as True. Values are local to the parent unless worldSpace is on.",
        .positionalDoc = "Transforms to edit or query; the selection when none are given.",
        .flags = {
            {"query", "q", T::Bool, false, "Return a value of the first object instead of setting. Pick it by passing one of translation, rotation, scale or matrix as True.", {},
             "xform(\"cube1\", query=True, translation=True)"},
            {"worldSpace", "ws", T::Bool, false, "Read and write in world space instead of relative to the parent. Rotation and scale queries stay local.", {},
             "xform(\"cube1\", worldSpace=True, translation=(0, 2, 0))"},
            {"translation", "t", T::Any, nullptr, "Position as (x, y, z) to set, or True to read it with query.", {},
             "xform(\"cube1\", translation=(1, 0, 0))"},
            {"rotation", "ro", T::Any, nullptr, "Euler rotation in degrees as (x, y, z) to set, or True to read it with query.", {},
             "xform(\"cube1\", rotation=(0, 45, 0))"},
            {"scale", "s", T::Any, nullptr, "Scale as (x, y, z) to set, or True to read it with query.", {},
             "xform(\"cube1\", scale=(2, 2, 2))"},
            {"matrix", "m", T::Any, nullptr, "The whole transform as 16 floats, column-major, to set; or True to read it with query.", {},
             "xform(\"cube1\", query=True, worldSpace=True, matrix=True)"},
        },
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            auto objs = idsOf(a.objects());
            bool ws = a.get<bool>("worldSpace");
            if (a.get<bool>("query"))
            {
                if (objs.empty())
                    throw CommandError("xform: no object");
                g.evaluate();
                Node* n = g.node(objs[0]);
                glm::dmat4 m = n->as<glm::dmat4>(ws ? Transform::kWorldMatrix : Transform::kMatrix);
                if (a.has("matrix"))
                    return toJson(m);
                if (a.has("translation"))
                    return ws ? toJson(glm::dvec3(m[3])) : toJson(n->value(Transform::kTranslate));
                if (a.has("rotation"))
                    return toJson(n->value(Transform::kRotate));
                if (a.has("scale"))
                    return toJson(n->value(Transform::kScale));
                throw CommandError("xform: query what?");
            }
            for (NodeId id : objs)
            {
                Node* n = g.node(id);
                if (!n->type().dag)
                    continue;
                auto vec = [&](const char* f) {
                    const json& v = a.flags[f];
                    return glm::dvec3(v[0].get<double>(), v[1].get<double>(), v[2].get<double>());
                };
                if (a.has("matrix"))
                {
                    glm::dmat4 m = std::get<glm::dmat4>(fromJson({.type = PlugType::Matrix}, a.flags["matrix"]));
                    if (ws)
                    {
                        g.evaluate();
                        m = glm::inverse(n->as<glm::dmat4>(Transform::kParentMatrix)) * m;
                    }
                    Transform::setLocalMatrix(g, id, m);
                }
                if (a.has("rotation"))
                    g.setValue({id, Transform::kRotate}, vec("rotation"));
                if (a.has("scale"))
                    g.setValue({id, Transform::kScale}, vec("scale"));
                if (a.has("translation"))
                {
                    if (ws)
                    {
                        g.evaluate();
                        glm::dmat4 world = n->as<glm::dmat4>(Transform::kWorldMatrix);
                        world[3] = glm::dvec4(vec("translation"), 1.0);
                        Transform::setLocalMatrix(g, id, glm::inverse(n->as<glm::dmat4>(Transform::kParentMatrix)) * world);
                    }
                    else
                        g.setValue({id, Transform::kTranslate}, vec("translation"));
                }
            }
            return nullptr;
        },
        .examples = {
            "xform(\"cube1\", translation=(0, 1, 0), rotation=(0, 90, 0))",
            "pos = xform(\"cube1\", query=True, worldSpace=True, translation=True)",
        },
    });

    ops::registerCommand({
        .name = "newScene",
        .doc = "Clear the scene. Not undoable.",
        .undoable = false,
        .run = [](const Args&) -> json {
            graph().clear();
            events::send({.type = events::Event::SceneNew});
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "currentTime",
        .doc = "Query or set the current time in seconds.",
        .positionalDoc = "seconds",
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (!a.positional.empty())
                graph().setTime(a.positional[0].get<double>());
            return graph().time();
        },
    });
}
} // namespace lila::scene
