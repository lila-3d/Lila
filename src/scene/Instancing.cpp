#include "scene/Instancing.h"

#include "ops/Command.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"

#include <algorithm>
#include <set>

namespace lila::scene::instancing
{
namespace
{
bool isMesh(const Graph& g, NodeId id)
{
    const Node* n = g.node(id);
    return n && n->type().isA("mesh");
}

int inAttrOf(const Node& n)
{
    if (n.type().isA("mesh"))
        return MeshNode::kInMesh;
    if (const ModifierNode* m = modifiers::asModifier(&n))
        return m->inAttr;
    return n.type().attrIndex("inMesh");
}

int outAttrOf(const Node& n)
{
    if (n.type().isA("mesh"))
        return MeshNode::kOutMesh;
    if (const ModifierNode* m = modifiers::asModifier(&n))
        return m->outAttr;
    if (auto* gen = dynamic_cast<const GeneratorNode*>(&n))
        return gen->outAttr;
    return n.type().attrIndex("outMesh");
}

/** The plug at the bottom of the modifier chain, and what feeds it. */
std::pair<PlugRef, std::optional<PlugRef>> chainBase(const Graph& g, NodeId mesh)
{
    PlugRef plug{mesh, MeshNode::kInMesh, -1};
    while (auto src = g.source(plug))
    {
        const ModifierNode* mod = modifiers::asModifier(g.node(src->node));
        if (!mod)
            return {plug, src};
        plug = {mod->id(), mod->inAttr, -1};
    }
    return {plug, std::nullopt};
}

/** Copies every storable input value that isn't driven by a connection; driven ones share their source. */
void copyNode(Graph& g, const Node& from, Node& to)
{
    const auto& defs = from.type().attrs;
    for (int i = 0; i < int(defs.size()); ++i)
    {
        if (!(defs[i].flags & AttrInput))
            continue;
        if (defs[i].flags & AttrArray)
            g.copyElements(from.id(), to.id(), i, true);
        else if (auto src = g.source({from.id(), i, -1}))
            g.connect(*src, {to.id(), i, -1});
        else
            g.setValue({to.id(), i, -1}, from.value(i));
    }
}

/** Deletes the non-DAG chain feeding a plug, node by node, as long as nothing else uses it. */
void dropHistory(Graph& g, const PlugRef& dst)
{
    auto src = g.source(dst);
    g.disconnect(dst);
    while (src)
    {
        Node* up = g.node(src->node);
        if (!up || up->type().dag || !g.destinations(*src).empty())
            return;
        int in = inAttrOf(*up);
        std::optional<PlugRef> next = in >= 0 ? g.source({up->id(), in, -1}) : std::nullopt;
        g.deleteNode(up->id());
        src = next;
    }
}

/** The base-sharing member a reference counts as made from: the lowest id among the meshes on its base. */
NodeId referenceSource(const Graph& g, NodeId mesh)
{
    auto base = basePlug(g, mesh);
    if (!base)
        return kNoNode;
    std::vector<NodeId> fed = meshesFedBy(g, *base);
    return fed.size() > 1 ? fed.front() : kNoNode;
}
} // namespace

const char* kindName(Kind k)
{
    switch (k)
    {
    case Kind::Source: return "source";
    case Kind::Instance: return "instance";
    case Kind::Reference: return "reference";
    case Kind::None: break;
    }
    return "none";
}

NodeId instanceParent(const Graph& g, NodeId mesh)
{
    if (!isMesh(g, mesh))
        return kNoNode;
    auto src = g.source({mesh, MeshNode::kInMesh, -1});
    if (!src || src->attr != MeshNode::kOutMesh || !isMesh(g, src->node))
        return kNoNode;
    return src->node;
}

NodeId root(const Graph& g, NodeId mesh)
{
    NodeId cur = mesh;
    std::set<NodeId> seen{cur};
    for (NodeId up = instanceParent(g, cur); up != kNoNode && seen.insert(up).second; up = instanceParent(g, cur))
        cur = up;
    return cur;
}

std::optional<PlugRef> basePlug(const Graph& g, NodeId mesh)
{
    if (!isMesh(g, mesh))
        return std::nullopt;
    auto [bottom, src] = chainBase(g, mesh);
    if (!src || g.node(src->node)->type().dag)
        return std::nullopt;
    return src;
}

std::vector<NodeId> meshesFedBy(const Graph& g, const PlugRef& plug)
{
    std::vector<NodeId> out;
    std::vector<PlugRef> todo = g.destinations(plug);
    while (!todo.empty())
    {
        PlugRef d = todo.back();
        todo.pop_back();
        const Node* n = g.node(d.node);
        if (!n)
            continue;
        if (n->type().isA("mesh"))
        {
            if (d.attr == MeshNode::kInMesh)
                out.push_back(n->id());
            continue;
        }
        if (const ModifierNode* mod = modifiers::asModifier(n); mod && d.attr == mod->inAttr)
            for (const PlugRef& next : g.destinations({mod->id(), mod->outAttr, -1}))
                todo.push_back(next);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<NodeId> directInstances(const Graph& g, NodeId mesh)
{
    std::vector<NodeId> out;
    if (!isMesh(g, mesh))
        return out;
    for (const PlugRef& d : g.destinations({mesh, MeshNode::kOutMesh, -1}))
        if (d.attr == MeshNode::kInMesh && isMesh(g, d.node))
            out.push_back(d.node);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<NodeId> members(const Graph& g, NodeId mesh)
{
    if (!isMesh(g, mesh))
        return {};
    std::set<NodeId> seen{mesh};
    std::vector<NodeId> todo{mesh};
    auto visit = [&](NodeId id) {
        if (id != kNoNode && seen.insert(id).second)
            todo.push_back(id);
    };
    while (!todo.empty())
    {
        NodeId m = todo.back();
        todo.pop_back();
        visit(instanceParent(g, m));
        for (NodeId c : directInstances(g, m))
            visit(c);
        if (auto base = basePlug(g, m))
            for (NodeId r : meshesFedBy(g, *base))
                visit(r);
    }
    return {seen.begin(), seen.end()};
}

Kind kind(const Graph& g, NodeId mesh)
{
    if (!isMesh(g, mesh))
        return Kind::None;
    if (instanceParent(g, mesh) != kNoNode)
        return Kind::Instance;
    NodeId refSource = referenceSource(g, mesh);
    if (refSource != kNoNode && refSource != mesh)
        return Kind::Reference;
    if (refSource == mesh || !directInstances(g, mesh).empty())
        return Kind::Source;
    return Kind::None;
}

NodeId source(const Graph& g, NodeId mesh)
{
    switch (kind(g, mesh))
    {
    case Kind::Instance: return root(g, mesh);
    case Kind::Reference: return referenceSource(g, mesh);
    case Kind::Source: return mesh;
    case Kind::None: break;
    }
    return kNoNode;
}

void connectInstance(Graph& g, NodeId src, NodeId target)
{
    NodeId from = root(g, src);
    PlugRef in{target, MeshNode::kInMesh, -1};
    dropHistory(g, in);
    g.setValue(in, MeshPtr());
    g.connect({from, MeshNode::kOutMesh, -1}, in);
}

void connectReference(Graph& g, NodeId src, NodeId target)
{
    NodeId from = root(g, src);
    auto [bottom, base] = chainBase(g, from);
    if (!base)
    {
        // The source stores its base itself; it moves into a meshSource both can share.
        Node* holder = g.createNode(*types::find("meshSource"), {}, kNoNode, true);
        g.setValue({holder->id(), MeshSourceNode::kMesh, -1}, g.storedValue(bottom));
        g.setValue(bottom, MeshPtr());
        base = PlugRef{holder->id(), MeshSourceNode::kOutMesh, -1};
        g.connect(*base, bottom);
    }
    PlugRef in{target, MeshNode::kInMesh, -1};
    dropHistory(g, in);
    g.setValue(in, MeshPtr());
    PlugRef top = in;
    std::vector<NodeId> mods = modifiers::stack(g, from);
    for (auto it = mods.rbegin(); it != mods.rend(); ++it)
    {
        Node* mod = g.node(*it);
        Node* copy = g.createNode(mod->type(), {}, kNoNode, true);
        const ModifierNode* mc = modifiers::asModifier(copy);
        const auto& defs = mod->type().attrs;
        for (int i = 0; i < int(defs.size()); ++i)
            if ((defs[i].flags & AttrInput) && i != mc->inAttr)
            {
                if (defs[i].flags & AttrArray)
                    g.copyElements(mod->id(), copy->id(), i, true);
                else if (auto s = g.source({mod->id(), i, -1}))
                    g.connect(*s, {copy->id(), i, -1});
                else
                    g.setValue({copy->id(), i, -1}, mod->value(i));
            }
        g.connect({copy->id(), mc->outAttr, -1}, top);
        top = {copy->id(), mc->inAttr, -1};
    }
    g.connect(*base, top);
}

void copyHistory(Graph& g, NodeId srcMesh, NodeId dstMesh, std::unordered_map<NodeId, NodeId>& copies)
{
    PlugRef srcPlug{srcMesh, MeshNode::kInMesh, -1};
    PlugRef dstPlug{dstMesh, MeshNode::kInMesh, -1};
    if (!g.source(srcPlug))
    {
        g.setValue(dstPlug, g.storedValue(srcPlug));
        return;
    }
    // Walk the chain from the mesh down, copying each history node and reconnecting the copies in the same shape.
    while (auto src = g.source(srcPlug))
    {
        Node* up = g.node(src->node);
        if (up->type().dag)
        {
            g.connect(*src, dstPlug);
            break;
        }
        NodeId copyId;
        if (auto it = copies.find(up->id()); it != copies.end())
            copyId = it->second;
        else
        {
            Node* copy = g.createNode(up->type(), {}, kNoNode, true);
            const auto& defs = up->type().attrs;
            for (int i = 0; i < int(defs.size()); ++i)
                if (!(defs[i].flags & AttrInput))
                    continue;
                else if (defs[i].flags & AttrArray)
                    g.copyElements(up->id(), copy->id(), i, false);
                else if (!g.source({up->id(), i, -1}))
                    g.setValue({copy->id(), i, -1}, up->value(i));
            copyId = copy->id();
            copies[up->id()] = copyId;
        }
        g.connect({copyId, src->attr, -1}, dstPlug);
        int inAttr = up->type().attrIndex("inMesh");
        if (inAttr < 0)
            break;
        srcPlug = {up->id(), inAttr, -1};
        dstPlug = {copyId, inAttr, -1};
    }
}

void promote(Graph& g, NodeId from, NodeId to)
{
    PlugRef fromIn{from, MeshNode::kInMesh, -1};
    PlugRef toIn{to, MeshNode::kInMesh, -1};
    std::vector<NodeId> others = directInstances(g, from);
    g.disconnect(toIn);
    if (auto src = g.source(fromIn))
    {
        g.disconnect(fromIn);
        g.connect(*src, toIn);
    }
    else
    {
        g.setValue(toIn, g.storedValue(fromIn));
        g.setValue(fromIn, MeshPtr());
    }
    for (NodeId other : others)
        if (other != to)
        {
            PlugRef in{other, MeshNode::kInMesh, -1};
            g.disconnect(in);
            g.connect({to, MeshNode::kOutMesh, -1}, in);
        }
}

bool makeUnique(Graph& g, NodeId mesh)
{
    LILA_ZONE();

    switch (kind(g, mesh))
    {
    case Kind::Instance:
    {
        NodeId from = root(g, mesh);
        PlugRef in{mesh, MeshNode::kInMesh, -1};
        g.disconnect(in);
        std::unordered_map<NodeId, NodeId> copies;
        copyHistory(g, from, mesh, copies);
        return true;
    }
    case Kind::Reference:
    {
        auto [bottom, base] = chainBase(g, mesh);
        Node* shared = g.node(base->node);
        Node* own = g.createNode(shared->type(), {}, kNoNode, true);
        copyNode(g, *shared, *own);
        g.disconnect(bottom);
        g.connect({own->id(), base->attr, -1}, bottom);
        return true;
    }
    case Kind::Source:
    {
        std::vector<NodeId> inst = directInstances(g, mesh);
        if (!inst.empty())
        {
            NodeId heir = inst.front();
            promote(g, mesh, heir);
            std::unordered_map<NodeId, NodeId> copies;
            copyHistory(g, heir, mesh, copies);
            return true;
        }
        // Only references share its base: it keeps the stack and takes a base of its own.
        auto [bottom, base] = chainBase(g, mesh);
        Node* shared = g.node(base->node);
        Node* own = g.createNode(shared->type(), {}, kNoNode, true);
        copyNode(g, *shared, *own);
        g.disconnect(bottom);
        g.connect({own->id(), base->attr, -1}, bottom);
        return true;
    }
    case Kind::None: break;
    }
    return false;
}

bool sameGeometry(const geo::Mesh& a, const geo::Mesh& b, double tolerance)
{
    if (a.numPoints() != b.numPoints() || a.numFaces() != b.numFaces() || a.numCorners() != b.numCorners())
        return false;
    // Compact numbering on both sides, so holes left by deleted elements don't matter.
    auto numbering = [](const geo::Mesh& m) {
        std::vector<geo::Index> out(m.pointCapacity(), geo::kInvalid);
        geo::Index next = 0;
        for (geo::Index p = 0; p < m.pointCapacity(); ++p)
            if (m.pointValid(p))
                out[p] = next++;
        return out;
    };
    std::vector<geo::Index> na = numbering(a), nb = numbering(b);
    std::vector<geo::Index> pa, pb;
    for (geo::Index p = 0; p < a.pointCapacity(); ++p)
        if (a.pointValid(p))
            pa.push_back(p);
    for (geo::Index p = 0; p < b.pointCapacity(); ++p)
        if (b.pointValid(p))
            pb.push_back(p);
    const double tol2 = tolerance * tolerance;
    for (size_t i = 0; i < pa.size(); ++i)
    {
        glm::dvec3 d = a.position(pa[i]) - b.position(pb[i]);
        if (glm::dot(d, d) > tol2)
            return false;
    }
    std::vector<geo::Index> fa, fb;
    for (geo::Index f = 0; f < a.faceCapacity(); ++f)
        if (a.faceValid(f))
            fa.push_back(f);
    for (geo::Index f = 0; f < b.faceCapacity(); ++f)
        if (b.faceValid(f))
            fb.push_back(f);
    for (size_t i = 0; i < fa.size(); ++i)
    {
        std::vector<geo::Index> ca = a.facePoints(fa[i]), cb = b.facePoints(fb[i]);
        if (ca.size() != cb.size())
            return false;
        for (size_t k = 0; k < ca.size(); ++k)
            if (na[ca[k]] != nb[cb[k]])
                return false;
    }
    return true;
}

void replaceWithInstance(Graph& g, NodeId src, NodeId target)
{
    for (NodeId id : modifiers::stack(g, target))
        if (modifiers::asModifier(g.node(id))->editingInput())
            throw CommandError(g.node(id)->path() + " is editing its edges: apply or cancel that first");
    connectInstance(g, src, target);
}

void beforeDelete(Graph& g, const std::vector<NodeId>& doomed)
{
    std::set<NodeId> gone(doomed.begin(), doomed.end());
    for (NodeId id : doomed)
    {
        const Node* n = g.node(id);
        // A file reference keeps its own connections; unloading one must not rewire the scene around it.
        if (!n || n->referenceId() || !n->type().isA("mesh"))
            continue;
        for (NodeId heir : directInstances(g, id))
            if (!gone.contains(heir) && !g.node(heir)->referenceId())
            {
                promote(g, id, heir);
                break;
            }
    }
}

void registerCommands()
{
    using T = ArgType;
    Graph::current().setDeleteHook(beforeDelete);

    auto meshes = [](const Args& a, const char* cmd) {
        Graph& g = Graph::current();
        std::vector<NodeId> out;
        for (const std::string& name : a.objects())
        {
            Node* n = g.find(name);
            if (!n)
                throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
            if (n->type().isA("mesh"))
                out.push_back(n->id());
        }
        return out;
    };
    auto names = [](const Graph& g, const std::vector<NodeId>& ids) {
        json out = json::array();
        for (NodeId id : ids)
            out.push_back(g.node(id)->path());
        return out;
    };

    ops::registerCommand({
        .name = "makeUnique",
        .doc = "Give instances and references data of their own, so edits to them stop reaching the other members. An "
               "instance gets a live copy of its source's stack, a reference its own base mesh; a source hands its data to "
               "its first instance and keeps a copy. Returns the meshes changed.",
        .positionalDoc = "meshes",
        .run = [meshes, names](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<NodeId> changed;
            for (NodeId id : meshes(a, "makeUnique"))
                if (makeUnique(g, id))
                    changed.push_back(id);
            return names(g, changed);
        },
        .examples = {"makeUnique(\"Bolt3\")"},
    });

    ops::registerCommand({
        .name = "selectInstances",
        .doc = "Select every mesh sharing data with the given ones: their source, instances and references. Returns the selection.",
        .positionalDoc = "meshes",
        .flags = {{"add", "", T::Bool, false, "Add to the selection instead of replacing it."}},
        .run = [meshes, names](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<NodeId> out;
            for (NodeId id : meshes(a, "selectInstances"))
                for (NodeId m : members(g, id))
                    if (std::find(out.begin(), out.end(), m) == out.end())
                        out.push_back(m);
            g.select(out, a.get<bool>("add", false) ? SelectMode::Add : SelectMode::Replace);
            return names(g, out);
        },
    });

    ops::registerCommand({
        .name = "selectInstanceSource",
        .doc = "Select the source of each given instance or reference (a source selects itself). Returns the selection.",
        .positionalDoc = "meshes",
        .run = [meshes, names](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<NodeId> out;
            for (NodeId id : meshes(a, "selectInstanceSource"))
                if (NodeId src = source(g, id); src != kNoNode && std::find(out.begin(), out.end(), src) == out.end())
                    out.push_back(src);
            if (out.empty())
                throw CommandError("selectInstanceSource: none of these meshes is an instance or reference");
            g.select(out);
            return names(g, out);
        },
    });

    ops::registerCommand({
        .name = "convertToInstance",
        .doc = "Replace meshes identical to a source with instances of it, dropping their own copies of the data: cleans up "
               "imported kits. The source is the last mesh given (the lead); with only the source, every other mesh in the "
               "scene is checked. Returns the meshes converted.",
        .positionalDoc = "meshes... source",
        .flags = {{"tolerance", "", T::Float, 0.001, "Points may differ by this much, object space, cm.", {}, {}, 0.0},
                  {"force", "", T::Bool, false, "Convert the given meshes even when their geometry differs from the source's."}},
        .run = [meshes, names](const Args& a) -> json {
            LILA_ZONE();
            Graph& g = Graph::current();
            std::vector<NodeId> given = meshes(a, "convertToInstance");
            if (given.empty())
                throw CommandError("convertToInstance: give the meshes to convert and the source last");
            const NodeId src = root(g, given.back());
            given.pop_back();
            const bool all = given.empty();
            if (all)
                for (NodeId id : g.nodes())
                    if (isMesh(g, id) && !g.node(id)->type().isA("lattice") && !g.node(id)->referenceId())
                        given.push_back(id);
            g.evaluate();
            const MeshPtr want = g.node(src)->as<MeshPtr>(MeshNode::kOutMesh);
            if (!want)
                throw CommandError("convertToInstance: " + g.node(src)->path() + " has no mesh");
            const double tol = a.get<double>("tolerance", 0.001);
            const bool force = a.get<bool>("force", false) && !all;
            std::vector<NodeId> converted;
            for (NodeId id : given)
            {
                if (id == src || root(g, id) == src)
                    continue;
                const MeshPtr have = g.node(id)->as<MeshPtr>(MeshNode::kOutMesh);
                if (!force && (!have || !sameGeometry(*want, *have, tol)))
                    continue;
                replaceWithInstance(g, src, id);
                converted.push_back(id);
            }
            return names(g, converted);
        },
        .examples = {"convertToInstance(\"Bolt\")  # every copy of Bolt in the scene", "convertToInstance(\"Bolt2\", \"Bolt3\", \"Bolt\")"},
    });

    ops::registerCommand({
        .name = "instanceInfo",
        .doc = "How a mesh shares data: {kind: none | source | instance | reference, source, members}.",
        .positionalDoc = "mesh",
        .undoable = false,
        .run = [names](const Args& a) -> json {
            Graph& g = Graph::current();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("instanceInfo: no mesh");
            Node* n = g.find(objs.back());
            if (!n || !n->type().isA("mesh"))
                throw CommandError("instanceInfo: " + objs.back() + " is not a mesh");
            const Kind k = kind(g, n->id());
            const NodeId src = source(g, n->id());
            return {{"kind", kindName(k)},
                    {"source", src == kNoNode ? json(nullptr) : json(g.node(src)->path())},
                    {"members", k == Kind::None ? json::array() : names(g, members(g, n->id()))}};
        },
    });
}
} // namespace lila::scene::instancing
