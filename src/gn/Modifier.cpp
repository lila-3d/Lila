#include "gn/Exec.h"
#include "gn/Gn.h"
#include "gn/InstanceView.h"
#include "gn/NodesCommon.h"
#include "gn/Stateful.h"
#include "gn/Viewer.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/Simulation.h"
#include "scene/Transform.h"

#include <atomic>
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
        GInput input;
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

std::mutex gHeightfieldMutex;
std::map<NodeId, std::shared_ptr<const geo::Heightfield>> gHeightfields;

void setHeightfield(NodeId modifier, std::shared_ptr<const geo::Heightfield> h)
{
    std::lock_guard lock(gHeightfieldMutex);
    if (h)
        gHeightfields[modifier] = std::move(h);
    else
        gHeightfields.erase(modifier);
}

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
            noteSimulationModifier(id(), false, false);
            clearInstanceView(id());
            setHeightfield(id(), nullptr);
            setOutput(outAttr, in);
            return;
        }
        const std::string& def = as<std::string>(mLayout->defAttr);
        if (!mPlan || def != mCachedDef)
        {
            mCachedDef = def;
            mPlan.reset();
            mCache = StepCache{};
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
        noteSimulationModifier(id(), mPlan && mPlan->statefulStep >= 0, mPlan && mPlan->statefulInputsUseTime);
        if (!mPlan)
        {
            clearInstanceView(id());
            setHeightfield(id(), nullptr);
            setOutput(outAttr, in);
            return;
        }
        try
        {
            ExecEnv env;
            env.time = ctx.time;
            env.owner = id();
            env.sampleOnly = mPlan->statefulStep >= 0 && scene::sim::sampling();
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
                env.params[p.input.name] = paramValue(p.input, j);
            }
            for (const TypeLayout::Object& o : mLayout->objects)
                env.objects[o.node] = {as<scene::MeshPtr>(o.meshAttr), as<glm::dmat4>(o.matrixAttr)};
            prepareCache(env.input);
            env.cache = &mCache;
            if (env.sampleOnly)
            {
                execute(*mPlan, env);
                setOutput(outAttr, in);
                return;
            }
            ViewerSink sink;
            if (mPlan->viewerStep >= 0)
                env.viewer = &sink;
            GeometryPtr result = execute(*mPlan, env);
            setHeightfield(id(), result->heightfield);
            scene::MeshPtr out = std::make_shared<geo::Mesh>(toOutputMesh(*result));
            if (InstanceViewPtr view = makeInstanceView(*result, out))
                setInstanceView(id(), std::move(view));
            else
                clearInstanceView(id());
            setOutput(outAttr, out);
            if (sink.captured)
                publishViewer(sink);
            else
                clearViewerData(id());
        }
        catch (const std::exception& e)
        {
            log::warn("{}: {}", name(), e.what());
            setOutput(outAttr, in);
            clearViewerData(id());
            clearInstanceView(id());
            setHeightfield(id(), nullptr);
        }
    }

    ~GnModifierNode() override
    {
        clearViewerData(id());
        forgetSimulationModifier(id());
        clearInstanceView(id());
        setHeightfield(id(), nullptr);
    }

private:
    /**
     * Keeps the cached step results only while nothing a time-independent step reads has changed. The input mesh counts
     * by content: upstream nodes can hand over a new copy of the same mesh on every evaluation.
     */
    void prepareCache(const GeometryPtr& in)
    {
        uint64_t key = scene::sim::hashBytes(&mPlan, sizeof mPlan);
        key = in ? hashGeometry(*in, key) : key;
        for (const TypeLayout::Param& p : mLayout->params)
        {
            std::string j = scene::toJson(value(p.attr)).dump();
            key = scene::sim::hashBytes(j.data(), j.size(), key);
        }
        for (const TypeLayout::Object& o : mLayout->objects)
        {
            Geometry object;
            object.mesh = as<scene::MeshPtr>(o.meshAttr);
            key = hashGeometry(object, key);
            key = scene::sim::hashBytes(&as<glm::dmat4>(o.matrixAttr), sizeof(glm::dmat4), key);
        }
        if (mCache.plan != mPlan.get() || mCache.key != key)
        {
            mCache = StepCache{};
            mCache.plan = mPlan.get();
            mCache.key = key;
        }
    }

    void publishViewer(const ViewerSink& sink)
    {
        LILA_ZONE();

        auto d = std::make_shared<ViewerData>();
        d->group = mPlan->group.name;
        d->node = sink.node;
        d->type = sink.type;
        d->domain = sink.domain;
        Geometry r = realize(sink.geometry ? *sink.geometry : Geometry{});
        geo::Mesh m = r.mesh ? *r.mesh : geo::Mesh();
        if (r.points)
            m.append(*r.points);
        if (r.curves)
            for (const glm::dvec3& p : r.curves->positions)
                m.addPoint(p);
        stripAnonymous(m);
        m.compact();
        if (sink.field)
        {
            FieldContext ctx = nodes::contextFor(m, sink.domain);
            d->domain = ctx.domain;
            bool vector = sink.type == SocketType::Vector || sink.type == SocketType::Rotation;
            std::vector<glm::dvec4> v(ctx.size);
            if (sink.type == SocketType::Color)
                v = expand<glm::dvec4>(evaluate(sink.field, ctx, SocketType::Color), ctx.size);
            else if (vector)
            {
                std::vector<glm::dvec3> raw = expand<glm::dvec3>(evaluate(sink.field, ctx, SocketType::Vector), ctx.size);
                for (size_t k = 0; k < ctx.size; ++k)
                    v[k] = glm::dvec4(raw[k], 0.0);
            }
            else
            {
                std::vector<double> raw = expand<double>(evaluate(sink.field, ctx, SocketType::Float), ctx.size);
                for (size_t k = 0; k < ctx.size; ++k)
                    v[k] = glm::dvec4(raw[k], 0.0, 0.0, 0.0);
            }
            d->values = std::move(v);
            d->hasValues = true;
            if (!d->values.empty())
            {
                d->minValue = d->maxValue = d->values[0].x;
                for (const glm::dvec4& x : d->values)
                {
                    d->minValue = std::min(d->minValue, x.x);
                    d->maxValue = std::max(d->maxValue, x.x);
                }
            }
        }
        d->mesh = std::make_shared<const geo::Mesh>(std::move(m));
        static std::atomic<uint64_t> serial{0};
        d->serial = ++serial;
        setViewerData(id(), std::move(d));
    }

    std::shared_ptr<const TypeLayout> mLayout;
    std::string mCachedDef;
    PlanPtr mPlan;
    StepCache mCache;
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
            j.push_back(std::string(socketTypeName(in.type)) + ":" + in.name + (in.subtype == "angle" ? ":angle" : ""));
        json o = objects;
        return json{{"in", j}, {"obj", o}, {"time", usesTime}}.dump();
    }
};

Interface interfaceOf(const Group& g)
{
    Interface i;
    i.inputs = g.inputs;
    for (const GNode& n : g.nodes)
        if (n.type == "ObjectInfo")
            i.objects.push_back(n.name);
    i.usesTime = usesSceneTime(g) || hasStateful(g);
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

/** Label, tooltip, limits and panel: what editors show, none of which changes the attr's layout. */
void presentParam(AttrDef& a, const GInput& in)
{
    a.niceName = in.displayName();
    a.doc = !in.description.empty() ? in.description
            : in.type == SocketType::Rotation ? "Exposed geometry-nodes rotation, degrees."
                                               : "Group input of the geometry-nodes group, fed to its Group Input node.";
    a.minValue = in.min;
    a.maxValue = in.max;
    a.softMin = in.softMin;
    a.softMax = in.softMax;
    a.step = in.step;
    if (in.subtype == "factor" && !a.softMin && !a.softMax)
    {
        a.softMin = 0.0;
        a.softMax = 1.0;
    }
    else if (in.subtype == "percentage" && !a.softMin && !a.softMax)
    {
        a.softMin = 0.0;
        a.softMax = 100.0;
    }
    a.category = in.panel;
    if (in.type == SocketType::Color || (in.type == SocketType::Vector && in.subtype == "color"))
        a.flags |= scene::AttrColor;
    else
        a.flags &= ~scene::AttrColor;
}

AttrDef paramAttr(const GInput& in)
{
    AttrDef a;
    a.name = in.name;
    a.flags = scene::AttrInput | scene::AttrStorable | scene::AttrKeyable;
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
        break;
    }
    case SocketType::Color:
    {
        a.type = PlugType::Vec3;
        glm::dvec3 v(0.0);
        if (d.is_array() && d.size() >= 3)
            v = {d[0].get<double>(), d[1].get<double>(), d[2].get<double>()};
        else if (d.is_number())
            v = glm::dvec3(d.get<double>());
        a.defaultValue = v;
        for (int k = 0; k < 3; ++k)
            a.childNames[k] = in.name + "RGB"[k];
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
    presentParam(a, in);
    return a;
}

/** Refreshes the presentation of an already registered type's params in place, so a tooltip edit needs no new type. */
void representParams(const scene::NodeType& t, const Group& g)
{
    auto& type = const_cast<scene::NodeType&>(t);
    for (const GInput& in : g.inputs)
        for (std::vector<AttrDef>* list : {&type.ownAttrs, &type.attrs})
            for (AttrDef& a : *list)
                if (a.name == in.name)
                    presentParam(a, in);
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
        {
            representParams(*t, g);
            return *t;
        }
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
        layout->params.push_back({in, int(t.ownAttrs.size())});
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
        log::warn("{}: {}", n.path(), e.what());
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
            throw std::invalid_argument("Object Info " + n.name + ": " + object + " depends on " + mod.path() + " (that would be a cycle)");
        }
    }
}

using Renames = std::map<std::string, std::string>;

/**
 * Replaces a modifier with one of the group's current type, keeping its name, connections and parameter values.
 * renames maps old parameter names to new ones, so a renamed input keeps its value and animation.
 */
Node* migrate(Graph& g, Node& old, const scene::NodeType& type, const Renames& renames)
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
    auto newName = [&](const std::string& n) {
        auto it = renames.find(n);
        return it == renames.end() ? n : it->second;
    };
    std::vector<std::pair<std::string, scene::PlugValue>> values;
    const auto& defs = old.type().attrs;
    for (int i = 0; i < int(defs.size()); ++i)
        if ((defs[i].flags & scene::AttrStorable) && (defs[i].flags & scene::AttrInput))
            values.push_back({defs[i].name, old.value(i)});

    Node* fresh = g.createNode(type, {}, scene::kNoNode, false);
    for (auto& [attr, v] : values)
    {
        int idx = type.attrIndex(newName(attr));
        if (idx >= 0 && type.attrs[idx].type == defs[old.type().attrIndex(attr)].type)
            g.setValue({fresh->id(), idx, -1}, v);
    }
    g.deleteNode(oldId);
    g.rename(fresh->id(), name);
    for (auto& [src, dst] : in)
    {
        int idx = type.attrIndex(newName(defs[dst.attr].name));
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

void syncInstances(Graph& g, Node& groupNode, const Group& grp, const Renames& renames)
{
    const scene::NodeType& type = ensureType(grp);
    for (NodeId id : instancesOf(g, groupNode.id()))
    {
        Node* mod = g.node(id);
        if (!mod)
            continue;
        if (&mod->type().registered() != &type)
            mod = migrate(g, *mod, type, renames);
        connectObjects(g, *mod, grp);
    }
}

/**
 * Copies the current definition of every group its Group nodes use into grp.subgroups, dropping unused ones. Groups
 * missing from the scene keep the copy they had. Throws when a group would end up containing itself.
 */
void embedSubgroups(Graph& g, Group& grp)
{
    std::map<std::string, json> next;
    for (const std::string& name : referencedGroups(grp))
    {
        if (name == grp.name)
            throw std::invalid_argument("group " + grp.name + " can't contain itself");
        json def;
        if (Node* n = findGroupNode(g, name))
            def = readGroup(*n).toJson();
        else if (auto it = grp.subgroups.find(name); it != grp.subgroups.end())
            def = it->second;
        else
            continue;
        if (dependsOn(Group::fromJson(def), grp.name))
            throw std::invalid_argument("group " + name + " uses " + grp.name + ", so it can't go inside it (that would be a cycle)");
        next[name] = std::move(def);
    }
    grp.subgroups = std::move(next);
}

/** Re-embeds a changed group into every group that uses it, which updates their modifiers in turn. */
void propagateToUsers(Graph& g, const std::string& changed)
{
    thread_local std::set<std::string> active;
    if (!active.insert(changed).second)
        return;
    for (const std::string& name : groupNames(g))
    {
        if (name == changed || active.contains(name))
            continue;
        Node* n = findGroupNode(g, name);
        Group user;
        try
        {
            user = readGroup(*n);
        }
        catch (const std::exception&)
        {
            continue;
        }
        std::vector<std::string> refs = referencedGroups(user);
        if (std::find(refs.begin(), refs.end(), changed) == refs.end())
            continue;
        try
        {
            commitGroup(g, *n, std::move(user));
        }
        catch (const std::exception& e)
        {
            log::warn("{}: couldn't update its use of {}: {}", name, changed, e.what());
        }
    }
    active.erase(changed);
}
} // namespace

std::string typeNameFor(const Group& g)
{
    return "gn_" + g.name + (g.typeVersion > 1 ? "_v" + std::to_string(g.typeVersion) : std::string());
}

scene::Node* findGroupNode(Graph& g, std::string_view group)
{
    // A hierarchy node elsewhere may share the group's name, so look for the group node among every match.
    for (NodeId id : g.findAll(group))
        if (Node* n = g.node(id); n->type().name == kGroupNodeType)
            return n;
    return nullptr;
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
        throw std::invalid_argument(groupNode.path() + " has no definition");
    try
    {
        return Group::fromJson(json::parse(def));
    }
    catch (const json::exception& e)
    {
        throw std::invalid_argument(groupNode.path() + ": " + e.what());
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

std::shared_ptr<const geo::Heightfield> modifierHeightfield(NodeId modifier)
{
    std::lock_guard lock(gHeightfieldMutex);
    auto it = gHeightfields.find(modifier);
    return it == gHeightfields.end() ? nullptr : it->second;
}

Node* groupOfModifier(Graph& g, const Node& modifier)
{
    if (!isGnModifier(modifier))
        return nullptr;
    int def = modifier.type().attrIndex("groupDefinition");
    auto src = g.source({modifier.id(), def, -1});
    return src ? g.node(src->node) : nullptr;
}

void commitGroup(Graph& g, Node& groupNode, Group next) { commitGroup(g, groupNode, std::move(next), {}); }

void commitGroup(Graph& g, Node& groupNode, Group next, const std::map<std::string, std::string>& renames)
{
    LILA_ZONE();

    Group prev = readGroup(groupNode);
    next.name = prev.name;
    embedSubgroups(g, next);
    pruneLinks(next);
    if (interfaceOf(next).signature() != interfaceOf(prev).signature())
        next.typeVersion = std::max(registry().maxVersion[prev.name], prev.typeVersion) + 1;
    else
        next.typeVersion = prev.typeVersion;
    bool changed = next.toJson() != prev.toJson();
    setGroupDefinition(g, groupNode, next);
    syncInstances(g, groupNode, next, renames);
    if (changed)
        propagateToUsers(g, next.name);
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

void createGroupNode(Graph& g, const Group& given)
{
    Group grp = given;
    embedSubgroups(g, grp);
    Node* n = g.createNode(*scene::types::find(kGroupNodeType), grp.name, scene::kNoNode, false);
    if (n->name() != grp.name)
    {
        g.deleteNode(n->id());
        throw std::invalid_argument("a node named " + grp.name + " already exists");
    }
    ensureType(grp);
    setGroupDefinition(g, *n, grp);
    propagateToUsers(g, grp.name);
}
} // namespace detail

void registerTypes()
{
    registerNodeLibrary();
    registerSimulationSupport();
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
