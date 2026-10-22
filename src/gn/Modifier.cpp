#include "gn/Exec.h"
#include "gn/Gn.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/Transform.h"

#include <map>
#include <mutex>
#include <set>

namespace lila::gn
{
using scene::AttrDef;
using scene::Graph;
using scene::Node;
using scene::NodeId;
using scene::PlugRef;
using scene::PlugType;

namespace
{
constexpr int kDefinitionAttr = 0;

/** Where a gn modifier type keeps each exposed parameter and Object Info input. */
struct TypeLayout
{
    struct Param
    {
        std::string name;
        SocketType type;
        int attr;
    };
    struct Object
    {
        std::string node;
        int meshAttr;
        int matrixAttr;
    };
    int defAttr = 2;
    std::vector<Param> params;
    std::vector<Object> objects;
};

std::string objectMeshAttr(const std::string& node) { return "obj_" + node + "_mesh"; }
std::string objectMatrixAttr(const std::string& node) { return "obj_" + node + "_matrix"; }

/** A modifier running a node group. compute() reads only its own plugs: the definition arrives by connection. */
class GnModifierNode final : public scene::ModifierNode
{
public:
    explicit GnModifierNode(std::shared_ptr<const TypeLayout> layout) : mLayout(std::move(layout)) {}

    void compute(const scene::EvalContext& ctx) override
    {
        LILA_ZONE();

        const scene::MeshPtr& in = as<scene::MeshPtr>(inAttr);
        if (!as<bool>(enabledAttr) || scene::modifiers::bypassed(in))
        {
            setOutput(outAttr, in);
            return;
        }
        const std::string& def = as<std::string>(mLayout->defAttr);
        if (!mPlan || def != mCachedDef)
        {
            mCachedDef = def;
            mPlan.reset();
            try
            {
                if (!def.empty())
                    mPlan = compile(Group::fromJson(json::parse(def)));
            }
            catch (const std::exception& e)
            {
                log::warn("{}: {}", name(), e.what());
            }
        }
        if (!mPlan)
        {
            setOutput(outAttr, in);
            return;
        }
        try
        {
            ExecEnv env;
            env.time = ctx.time;
            if (in)
            {
                if (in->hasDeleted())
                    env.input = meshGeometry(*in);
                else
                {
                    Geometry g;
                    g.mesh = in;
                    env.input = makeGeometry(std::move(g));
                }
            }
            for (const TypeLayout::Param& p : mLayout->params)
            {
                const scene::PlugValue& v = value(p.attr);
                json j = scene::toJson(v);
                env.params[p.name] = valueFromJson(p.type, j);
            }
            for (const TypeLayout::Object& o : mLayout->objects)
                env.objects[o.node] = {as<scene::MeshPtr>(o.meshAttr), as<glm::dmat4>(o.matrixAttr)};
            GeometryPtr result = execute(*mPlan, env);
            setOutput(outAttr, scene::MeshPtr(std::make_shared<geo::Mesh>(toOutputMesh(*result))));
        }
        catch (const std::exception& e)
        {
            log::warn("{}: {}", name(), e.what());
            setOutput(outAttr, in);
        }
    }

private:
    std::shared_ptr<const TypeLayout> mLayout;
    std::string mCachedDef;
    PlanPtr mPlan;
};

/** Node holding a group definition. Nothing to compute: modifiers read the definition through connections. */
class GroupNode final : public Node
{
};

struct Interface
{
    std::vector<GInput> inputs;
    std::vector<std::string> objects;
    bool usesTime = false;

    std::string signature() const
    {
        json j = json::array();
        for (const GInput& in : inputs)
            j.push_back(std::string(socketTypeName(in.type)) + ":" + in.name);
        json o = objects;
        return json{{"in", j}, {"obj", o}, {"time", usesTime}}.dump();
    }
};

Interface interfaceOf(const Group& g)
{
    Interface i;
    i.inputs = g.inputs;
    for (const GNode& n : g.nodes)
    {
        if (n.type == "ObjectInfo")
            i.objects.push_back(n.name);
        if (n.type == "SceneTime")
            i.usesTime = true;
    }
    return i;
}

/** Registered gn types: name -> interface signature, and the highest version seen per group. */
struct Registry
{
    std::map<std::string, std::string> signatures;
    std::map<std::string, int> maxVersion;
};

Registry& registry()
{
    static Registry r;
    return r;
}

AttrDef paramAttr(const GInput& in)
{
    AttrDef a;
    a.name = in.name;
    a.flags = scene::AttrInput | scene::AttrStorable | scene::AttrKeyable;
    a.doc = "Group input of the geometry-nodes group, fed to its Group Input node.";
    const json& d = in.defaultValue;
    auto num = [&](double fallback) { return d.is_number() ? d.get<double>() : d.is_boolean() ? double(d.get<bool>()) : fallback; };
    switch (in.type)
    {
    case SocketType::Int:
        a.type = PlugType::Int;
        a.defaultValue = int64_t(std::llround(num(0.0)));
        break;
    case SocketType::Bool:
        a.type = PlugType::Bool;
        a.defaultValue = num(0.0) != 0.0;
        break;
    case SocketType::Vector:
    case SocketType::Rotation:
    {
        a.type = PlugType::Vec3;
        glm::dvec3 v(0.0);
        if (d.is_array() && d.size() == 3)
            v = {d[0].get<double>(), d[1].get<double>(), d[2].get<double>()};
        a.defaultValue = v;
        for (int k = 0; k < 3; ++k)
            a.childNames[k] = in.name + char('X' + k);
        if (in.type == SocketType::Rotation)
            a.doc = "Exposed geometry-nodes rotation, degrees.";
        break;
    }
    case SocketType::String:
        a.type = PlugType::String;
        a.defaultValue = d.is_string() ? d.get<std::string>() : std::string();
        a.flags &= ~scene::AttrKeyable;
        break;
    default:
        a.type = PlugType::Float;
        a.defaultValue = num(0.0);
        break;
    }
    return a;
}

const scene::NodeType& ensureType(const Group& g)
{
    LILA_ZONE();

    std::string name = typeNameFor(g);
    Interface iface = interfaceOf(g);
    std::string sig = iface.signature();
    Registry& r = registry();
    r.maxVersion[g.name] = std::max(r.maxVersion[g.name], g.typeVersion);
    if (const scene::NodeType* t = scene::types::find(name))
    {
        auto it = r.signatures.find(name);
        if (it != r.signatures.end() && it->second == sig)
            return *t;
        // Same name, other interface (another scene loaded): nodes still holding the old type keep it alive.
        scene::types::unregisterType(name);
    }

    scene::NodeType t;
    t.name = name;
    t.icon = "topology-complex";
    t.doc = "Runs the " + g.name + " geometry-nodes group on the mesh. Edit the group in the Node Editor; its group inputs "
            "show here as parameters.";
    t.timeDependent = iface.usesTime;
    auto layout = std::make_shared<TypeLayout>();
    t.ownAttrs.push_back({.name = "inMesh", .type = PlugType::Mesh, .defaultValue = scene::MeshPtr(), .flags = scene::AttrInput | scene::AttrHidden});
    t.ownAttrs.push_back({.name = "enabled", .type = PlugType::Bool, .defaultValue = true,
                          .flags = scene::AttrInput | scene::AttrStorable | scene::AttrKeyable, .doc = "Turn the modifier off without removing it."});
    t.ownAttrs.push_back({.name = "groupDefinition", .type = PlugType::String, .defaultValue = std::string(),
                          .flags = scene::AttrInput | scene::AttrHidden, .doc = "Connected from the group node."});
    layout->defAttr = 2;
    for (const GInput& in : iface.inputs)
    {
        layout->params.push_back({in.name, in.type, int(t.ownAttrs.size())});
        t.ownAttrs.push_back(paramAttr(in));
    }
    for (const std::string& obj : iface.objects)
    {
        int meshAttr = int(t.ownAttrs.size());
        t.ownAttrs.push_back({.name = objectMeshAttr(obj), .type = PlugType::Mesh, .defaultValue = scene::MeshPtr(),
                              .flags = scene::AttrInput | scene::AttrHidden, .doc = "Object Info mesh."});
        t.ownAttrs.push_back({.name = objectMatrixAttr(obj), .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0),
                              .flags = scene::AttrInput | scene::AttrHidden, .doc = "Object Info world matrix."});
        layout->objects.push_back({obj, meshAttr, meshAttr + 1});
    }
    t.ownAttrs.push_back({.name = "outMesh", .type = PlugType::Mesh, .defaultValue = scene::MeshPtr(), .flags = scene::AttrOutput | scene::AttrHidden});
    int out = int(t.ownAttrs.size()) - 1;
    std::shared_ptr<const TypeLayout> frozen = layout;
    t.create = [frozen, out] {
        auto n = std::make_unique<GnModifierNode>(frozen);
        n->outAttr = out;
        return n;
    };
    r.signatures[name] = sig;
    return scene::types::registerType(std::move(t));
}

/** Registers the type for a group node's current definition, if it has a readable one. */
void syncTypeFor(const Node& n)
{
    const std::string& def = n.as<std::string>(kDefinitionAttr);
    if (def.empty())
        return;
    try
    {
        ensureType(Group::fromJson(json::parse(def)));
    }
    catch (const std::exception& e)
    {
        log::warn("{}: {}", n.name(), e.what());
    }
}

void setGroupDefinition(Graph& g, Node& n, const Group& grp)
{
    g.setValue({n.id(), kDefinitionAttr, -1}, grp.toJson().dump());
}

/** Drops links whose sockets no longer exist (a property change removed them) or whose nodes are gone. */
void pruneLinks(Group& grp)
{
    registerNodeLibrary();
    std::erase_if(grp.links, [&](const GLink& l) {
        const GNode* from = grp.node(l.fromNode);
        const GNode* to = grp.node(l.toNode);
        if (!from || !to)
            return true;
        const NodeDecl* fd = findDecl(from->type);
        const NodeDecl* td = findDecl(to->type);
        if (!fd || !td)
            return false;
        return fd->socketsFor(grp, *from).outputIndex(l.fromSocket) < 0 || td->socketsFor(grp, *to).inputIndex(l.toSocket) < 0;
    });
}

/** Points each Object Info input of a modifier at the object its node names. Throws on cycles or non-meshes. */
void connectObjects(Graph& g, Node& mod, const Group& grp)
{
    for (const GNode& n : grp.nodes)
    {
        if (n.type != "ObjectInfo")
            continue;
        int meshAttr = mod.type().attrIndex(objectMeshAttr(n.name));
        int matAttr = mod.type().attrIndex(objectMatrixAttr(n.name));
        if (meshAttr < 0 || matAttr < 0)
            continue;
        std::string object = findDecl("ObjectInfo")->propValue(n, "object");
        PlugRef meshDst{mod.id(), meshAttr, -1}, matDst{mod.id(), matAttr, -1};
        Node* obj = object.empty() ? nullptr : g.find(object);
        if (obj && !obj->type().isA("mesh"))
            throw std::invalid_argument(object + " is not a mesh");
        PlugRef meshSrc = obj ? PlugRef{obj->id(), scene::MeshNode::kOutMesh, -1} : PlugRef{};
        PlugRef matSrc = obj ? PlugRef{obj->id(), scene::Transform::kWorldMatrix, -1} : PlugRef{};
        auto cur = g.source(meshDst);
        if (cur && obj && *cur == meshSrc && g.source(matDst) == matSrc)
            continue;
        g.disconnect(meshDst);
        g.disconnect(matDst);
        if (!obj)
            continue;
        try
        {
            g.connect(meshSrc, meshDst);
            g.connect(matSrc, matDst);
        }
        catch (const std::exception&)
        {
            throw std::invalid_argument("Object Info " + n.name + ": " + object + " depends on " + mod.name() + " (that would be a cycle)");
        }
    }
}

/** Replaces a modifier with one of the group's current type, keeping its name, connections and parameter values. */
Node* migrate(Graph& g, Node& old, const scene::NodeType& type)
{
    LILA_ZONE();

    std::string name = old.name();
    NodeId oldId = old.id();
    std::vector<std::pair<PlugRef, PlugRef>> in, out;
    for (auto& [dst, src] : g.connections())
    {
        if (dst.node == oldId)
            in.push_back({src, dst});
        else if (src.node == oldId)
            out.push_back({src, dst});
    }
    std::vector<std::pair<std::string, scene::PlugValue>> values;
    const auto& defs = old.type().attrs;
    for (int i = 0; i < int(defs.size()); ++i)
        if ((defs[i].flags & scene::AttrStorable) && (defs[i].flags & scene::AttrInput))
            values.push_back({defs[i].name, old.value(i)});

    Node* fresh = g.createNode(type, {}, scene::kNoNode, false);
    for (auto& [attr, v] : values)
    {
        int idx = type.attrIndex(attr);
        if (idx >= 0 && type.attrs[idx].type == defs[old.type().attrIndex(attr)].type)
            g.setValue({fresh->id(), idx, -1}, v);
    }
    g.deleteNode(oldId);
    g.rename(fresh->id(), name);
    for (auto& [src, dst] : in)
    {
        int idx = type.attrIndex(defs[dst.attr].name);
        if (idx < 0)
            continue;
        try
        {
            g.connect(src, {fresh->id(), idx, dst.component});
        }
        catch (const std::exception& e)
        {
            log::warn("{}: couldn't reconnect {}: {}", name, defs[dst.attr].name, e.what());
        }
    }
    for (auto& [src, dst] : out)
    {
        int idx = type.attrIndex(defs[src.attr].name);
        if (idx >= 0)
            g.connect({fresh->id(), idx, src.component}, dst);
    }
    return fresh;
}

void syncInstances(Graph& g, Node& groupNode, const Group& grp)
{
    const scene::NodeType& type = ensureType(grp);
    for (NodeId id : instancesOf(g, groupNode.id()))
    {
        Node* mod = g.node(id);
        if (!mod)
            continue;
        if (&mod->type() != &type)
            mod = migrate(g, *mod, type);
        connectObjects(g, *mod, grp);
    }
}
} // namespace

std::string typeNameFor(const Group& g)
{
    return "gn_" + g.name + (g.typeVersion > 1 ? "_v" + std::to_string(g.typeVersion) : std::string());
}

scene::Node* findGroupNode(Graph& g, std::string_view group)
{
    Node* n = g.find(group);
    return n && n->type().name == kGroupNodeType ? n : nullptr;
}

std::vector<std::string> groupNames(Graph& g)
{
    std::vector<std::string> out;
    for (NodeId id : g.nodes())
        if (g.node(id)->type().name == kGroupNodeType)
            out.push_back(g.node(id)->name());
    return out;
}

Group readGroup(const Node& groupNode)
{
    const std::string& def = groupNode.as<std::string>(kDefinitionAttr);
    if (def.empty())
        throw std::invalid_argument(groupNode.name() + " has no definition");
    try
    {
        return Group::fromJson(json::parse(def));
    }
    catch (const json::exception& e)
    {
        throw std::invalid_argument(groupNode.name() + ": " + e.what());
    }
}

std::vector<NodeId> instancesOf(Graph& g, NodeId groupNode)
{
    std::vector<NodeId> out;
    for (const PlugRef& d : g.destinations({groupNode, kDefinitionAttr, -1}))
        if (Node* n = g.node(d.node); n && isGnModifier(*n))
            out.push_back(d.node);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

bool isGnModifier(const Node& n) { return dynamic_cast<const GnModifierNode*>(&n) != nullptr; }

Node* groupOfModifier(Graph& g, const Node& modifier)
{
    if (!isGnModifier(modifier))
        return nullptr;
    int def = modifier.type().attrIndex("groupDefinition");
    auto src = g.source({modifier.id(), def, -1});
    return src ? g.node(src->node) : nullptr;
}

void commitGroup(Graph& g, Node& groupNode, Group next)
{
    LILA_ZONE();

    Group prev = readGroup(groupNode);
    next.name = prev.name;
    pruneLinks(next);
    if (interfaceOf(next).signature() != interfaceOf(prev).signature())
        next.typeVersion = std::max(registry().maxVersion[prev.name], prev.typeVersion) + 1;
    else
        next.typeVersion = prev.typeVersion;
    setGroupDefinition(g, groupNode, next);
    syncInstances(g, groupNode, next);
}

namespace detail
{
/** Used by the commands: a new modifier of the group's type, inserted at the top of a mesh's stack. */
Node* applyGroup(Graph& g, Node& groupNode, Node& mesh)
{
    Group grp = readGroup(groupNode);
    const scene::NodeType& type = ensureType(grp);
    PlugRef top{mesh.id(), scene::MeshNode::kInMesh, -1};
    auto src = g.source(top);
    Node* mod = g.createNode(type, grp.name + "Modifier", scene::kNoNode, false);
    auto* mn = static_cast<scene::ModifierNode*>(mod);
    if (src)
    {
        g.disconnect(top);
        g.connect(*src, {mod->id(), mn->inAttr, -1});
    }
    else
    {
        Node* source = g.createNode(*scene::types::find("meshSource"), {}, scene::kNoNode, true);
        g.setValue({source->id(), scene::MeshSourceNode::kMesh, -1}, g.storedValue(top));
        g.setValue(top, scene::MeshPtr());
        g.connect({source->id(), scene::MeshSourceNode::kOutMesh, -1}, {mod->id(), mn->inAttr, -1});
    }
    g.connect({mod->id(), mn->outAttr, -1}, top);
    g.connect({groupNode.id(), kDefinitionAttr, -1}, {mod->id(), mod->type().attrIndex("groupDefinition"), -1});
    connectObjects(g, *mod, grp);
    return mod;
}

void createGroupNode(Graph& g, const Group& grp)
{
    Node* n = g.createNode(*scene::types::find(kGroupNodeType), grp.name, scene::kNoNode, false);
    if (n->name() != grp.name)
    {
        g.deleteNode(n->id());
        throw std::invalid_argument("a node named " + grp.name + " already exists");
    }
    ensureType(grp);
    setGroupDefinition(g, *n, grp);
}
} // namespace detail

void registerTypes()
{
    registerNodeLibrary();
    if (scene::types::find(kGroupNodeType))
        return;
    scene::NodeType t;
    t.name = kGroupNodeType;
    t.icon = "topology-complex";
    t.doc = "Stores a geometry-nodes group in the scene. The modifiers that run it read their definition from here.";
    t.ownAttrs = {{.name = "definition", .type = PlugType::String, .defaultValue = std::string(),
                   .flags = scene::AttrInput | scene::AttrStorable | scene::AttrHidden, .doc = "Geometry-nodes group, JSON."}};
    t.create = [] { return std::make_unique<GroupNode>(); };
    scene::types::registerType(std::move(t));

    // Loading a scene or undoing an edit sets the definition first; the modifier type has to exist before
    // nodes of it are created, so register it right then.
    Graph::current().addListener([](scene::Change c, NodeId id) {
        if (c != scene::Change::Value && c != scene::Change::Created)
            return;
        Node* n = Graph::current().node(id);
        if (n && n->type().name == kGroupNodeType)
            syncTypeFor(*n);
    }, true);
}
} // namespace lila::gn
