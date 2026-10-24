#include "scene/SceneCommands.h"

#include "ops/Command.h"
#include "core/Events.h"
#include "core/PrefStore.h"
#include "core/Workspace.h"
#include "scene/Anim.h"
#include "scene/AnimLayer.h"
#include "scene/Take.h"
#include "scene/BatchRename.h"
#include "scene/Camera.h"
#include "scene/Components.h"
#include "scene/Deformers.h"
#include "scene/Drivers.h"
#include "scene/DisplayLayers.h"
#include "scene/Ghosts.h"
#include "scene/Graph.h"
#include "scene/ImagePlane.h"
#include "scene/Materials.h"
#include "scene/Measure.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/ObjectSets.h"
#include "scene/Picker.h"
#include "scene/Poses.h"
#include "scene/RenderSettings.h"
#include "scene/Rig.h"
#include "scene/SceneData.h"
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
} // namespace

void loadNamingPolicy()
{
    graph().setNamingPolicy(NamingPolicy::fromJson(PrefStore::get().merged("naming")));
}

void registerTransformCommands();
void registerMeshEditCommands();
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
    registerMeshEditCommands();
    registerComponentQueryCommands();
    registerTransferCommands();
    registerBooleanCommands();
    registerSmartJoinCommands();
    registerBatchRenameCommands();
    registerObjectCommands();
    registerBoundingMeshCommands();
    modifiers::registerCommands();
    components::registerCommands();
    materials::registerCommands();
    registerRigCommands();
    deformers::registerCommands();
    registerMeasureCommands();
    registerImagePlaneCommands();
    registerCameraCommands();
    spaces::registerCommands();
    anim::registerCommands();
    drivers::registerCommands();
    poses::registerCommands();
    picker::registerCommands();
    ghosts::registerCommands();
    anim::registerLayerCommands();
    anim::registerTakeCommands();
    sim::registerCommands();
    sim::registerCommands();
    registerRenderSettingsCommands();
    scripts::registerCommands();
    data::registerCommands();
    textureFolders::registerCommands();
    displayLayers::registerCommands();
    objectSets::registerCommands();

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
                        want = guarded([&] { return graph().rename(id, want); });
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
            for (NodeId id : ids)
                graph().deleteNode(id);
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
            guarded([&] { return graph().rename(id, newName); });
            return graph().path(id);
        },
    });

    ops::registerCommand({
        .name = "parent",
        .doc = "parent(child..., parent), or parent(child..., world=True). Keeps world transforms unless relative=True. "
               "chain=True parents each object under the next one, in order: (a, b, c) puts a under b and b under c. "
               "index=N places the objects, in order, at position N among the parent's children (0 is first), which "
               "also reorders objects already under that parent.",
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
            if (!lockOnly)
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
        .doc = "Value of 'node.attr'. lock=True returns whether it is locked instead.",
        .positionalDoc = "plug",
        .flags = {{"lock", "l", T::Bool, false, "Return the lock state instead of the value."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.positional.empty() || !a.positional[0].is_string())
                throw CommandError("getAttr: expected a plug");
            if (a.get<bool>("lock"))
                return graph().isLocked(plugArg(a.positional[0].get<std::string>()));
            return toJson(guarded([&] { return graph().value(plugArg(a.positional[0].get<std::string>())); }));
        },
    });

    ops::registerCommand({
        .name = "connectAttr",
        .doc = "connectAttr('src.attr', 'dst.attr').",
        .positionalDoc = "src dst",
        .flags = {{"force", "f", T::Bool, false, "Replace an existing input connection."}},
        .run = [](const Args& a) -> json {
            if (a.positional.size() != 2)
                throw CommandError("connectAttr: expected (src, dst)");
            PlugRef s = plugArg(a.positional[0]), d = plugArg(a.positional[1]);
            guarded([&] { graph().connect(s, d, a.get<bool>("force")); return 0; });
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "disconnectAttr",
        .doc = "disconnectAttr('dst.attr'), or (src, dst).",
        .positionalDoc = "[src] dst",
        .run = [](const Args& a) -> json {
            if (a.positional.empty())
                throw CommandError("disconnectAttr: expected a plug");
            graph().disconnect(plugArg(a.positional.back()));
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
                if ((d.flags & AttrHidden) || (a.get<bool>("keyable") && !(d.flags & AttrKeyable)))
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
               "left/right to the previous/next sibling (wrapping).",
        .flags = {{"direction", "", T::String, "down", "up, down, left or right."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::string dir = a.get<std::string>("direction");
            auto walkable = [&](const std::vector<NodeId>& ids) {
                std::vector<NodeId> out;
                for (NodeId id : ids)
                    if (const Node* n = g.node(id); n && n->type().isA("transform"))
                        out.push_back(id);
                return out;
            };
            std::vector<NodeId> next;
            for (NodeId id : g.selection())
            {
                NodeId to = id;
                if (dir == "up")
                    to = g.parent(id) != kNoNode ? g.parent(id) : id;
                else if (dir == "down")
                {
                    std::vector<NodeId> kids = walkable(g.children(id));
                    to = kids.empty() ? id : kids.front();
                }
                else if (dir == "left" || dir == "right")
                {
                    NodeId parent = g.parent(id);
                    std::vector<NodeId> sibs = walkable(parent == kNoNode ? g.roots() : g.children(parent));
                    auto it = std::find(sibs.begin(), sibs.end(), id);
                    if (it != sibs.end() && sibs.size() > 1)
                    {
                        size_t i = size_t(it - sibs.begin());
                        to = sibs[dir == "right" ? (i + 1) % sibs.size() : (i + sibs.size() - 1) % sibs.size()];
                    }
                }
                else
                    throw CommandError("pickWalk: direction must be up, down, left or right");
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
        .doc = "Query or set translation/rotation/scale, local or world space.",
        .positionalDoc = "objects",
        .flags = {
            {"query", "q", T::Bool, false, "Return values instead of setting."},
            {"worldSpace", "ws", T::Bool, false, "World space."},
            {"translation", "t", T::Any, nullptr, "Vec3 to set, or True with query."},
            {"rotation", "ro", T::Any, nullptr, "Vec3 degrees to set, or True with query."},
            {"scale", "s", T::Any, nullptr, "Vec3 to set, or True with query."},
            {"matrix", "m", T::Any, nullptr, "16 floats to set, or True with query."},
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
