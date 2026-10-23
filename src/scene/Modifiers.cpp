#include "scene/Modifiers.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "geo/Normals.h"
#include "geo/Ops.h"
#include "geo/Reduce.h"
#include "geo/Topology.h"
#include "scene/AutoBevel.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <set>

namespace lila::scene
{
/** TransferAttributes.cpp. */
void registerTransferModifier();

namespace
{
AttrDef param(std::string name, PlugType type, PlugValue def, std::string doc)
{
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = AttrInput | AttrStorable | AttrKeyable,
            .doc = std::move(doc)};
}

double f(const Node& n, const char* a) { return n.as<double>(n.type().attrIndex(a)); }
int64_t i(const Node& n, const char* a) { return n.as<int64_t>(n.type().attrIndex(a)); }
bool b(const Node& n, const char* a) { return n.as<bool>(n.type().attrIndex(a)); }
glm::dvec3 v(const Node& n, const char* a) { return n.as<glm::dvec3>(n.type().attrIndex(a)); }

std::vector<geo::Index> allFaces(const geo::Mesh& m)
{
    std::vector<geo::Index> out;
    for (geo::Index fi = 0; fi < m.faceCapacity(); ++fi)
        if (m.faceValid(fi))
            out.push_back(fi);
    return out;
}

/** Edges whose faces meet sharper than the angle, or every hard edge when the angle is negative. */
std::vector<geo::Index> sharpEdges(const geo::Mesh& m, double degrees)
{
    std::vector<geo::Index> out;
    auto hard = m.edgeAttrs().get<bool>(geo::attr::kHard);
    double cosLimit = std::cos(glm::radians(degrees));
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
    {
        if (!m.edgeValid(e) || m.edgeFaceCount(e) != 2)
            continue;
        if (degrees < 0.0)
        {
            if (e < hard.size() && hard[e])
                out.push_back(e);
            continue;
        }
        geo::Index c = m.edgeCorner(e);
        geo::Index f0 = m.cornerFace(c), f1 = m.cornerFace(m.cornerRadial(c));
        if (glm::dot(m.faceNormal(f0), m.faceNormal(f1)) < cosLimit)
            out.push_back(e);
    }
    return out;
}

geo::Mesh solidify(const geo::Mesh& in, double thickness)
{
    geo::Mesh m = in;
    m.compact();
    auto normals = geo::pointNormals(m, geo::NormalWeight::Angle);
    geo::Index n = m.pointCapacity();
    geo::Mesh inner = m;
    for (geo::Index p = 0; p < n; ++p)
        inner.setPosition(p, m.position(p) - glm::dvec3(normals[p]) * thickness);
    geo::flipNormals(inner, allFaces(inner));
    std::vector<geo::EdgePath> loops = geo::boundaryLoops(m);
    geo::Index offset = m.append(inner);
    // Rim quads join each boundary edge to its copy on the inner shell.
    for (const geo::EdgePath& loop : loops)
        for (geo::Index e : loop.edges)
        {
            geo::Index c = m.edgeCorner(e);
            geo::Index a = m.cornerPoint(c), bb = m.cornerNextPoint(c);
            m.addFace({bb, a, a + offset, bb + offset});
        }
    return m;
}

geo::Mesh decimate(const geo::Mesh& in, const Node& n)
{
    geo::ReduceOptions o;
    o.ratio = std::clamp(f(n, "ratio"), 0.0, 1.0);
    o.targetTriangles = geo::Index(std::max<int64_t>(0, i(n, "targetTriangles")));
    o.keepBorders = b(n, "keepBorders");
    o.keepHardEdges = b(n, "keepHardEdges");
    o.symmetryAxis = int(std::clamp<int64_t>(i(n, "symmetry"), 0, 3)) - 1;
    o.quads = b(n, "quads");
    return geo::reduceMesh(in, o).mesh;
}

struct ModifierType
{
    const char* name;
    const char* doc;
    std::vector<AttrDef> params;
    ModifierNode::Apply apply;
};

std::vector<ModifierType> modifierTypes()
{
    return {
        {"mirrorModifier",
         "Adds a mirrored copy across a plane through the object's origin and welds the seam. Model one half of a symmetrical "
         "object and let this build the other.",
         {param("axis", PlugType::Enum, int64_t(0), "Object-space axis the mirror plane faces: x mirrors left to right."),
          param("direction", PlugType::Enum, int64_t(0),
                "Which side builds the other: + to - keeps the positive side and replaces the negative one with its mirror, cutting "
                "faces that cross the plane; - to + the reverse; both mirrors the whole mesh."),
          param("weld", PlugType::Bool, true, "Merge points on the plane with their mirror images so the halves join."),
          param("tolerance", PlugType::Float, 0.01, "Points this close to the plane are welded, cm.")},
         [](const geo::Mesh& in, const Node& n) {
             geo::Mesh m = in;
             geo::MirrorOptions o;
             o.plane.normal = glm::dvec3(0.0);
             o.plane.normal[int(std::clamp<int64_t>(i(n, "axis"), 0, 2))] = 1.0;
             o.direction = geo::MirrorDirection(std::clamp<int64_t>(i(n, "direction"), 0, 2));
             o.weld = b(n, "weld");
             o.tolerance = f(n, "tolerance");
             geo::mirrorMesh(m, o);
             return m;
         }},
        {"subdivisionModifier",
         "Smooths the mesh with Catmull-Clark subdivision. Use it to turn a low-poly cage into a smooth high-poly surface.",
         {param("levels", PlugType::Int, int64_t(1), "Subdivision levels, 0 to 4. Each level about quadruples the face count; 0 passes the mesh through.")},
         [](const geo::Mesh& in, const Node& n) {
             geo::SubdivideOptions o;
             o.levels = int(std::clamp<int64_t>(i(n, "levels"), 0, 4));
             return o.levels == 0 ? in : geo::subdivide(in, o);
         }},
        {"bevelModifier",
         "Bevels every edge sharper than an angle so hard corners catch the light. A quick live bevel; Auto Bevel has finer "
         "edge rules and hand-picked edges.",
         {param("offset", PlugType::Float, 1.0, "Bevel width, cm."),
          param("segments", PlugType::Int, int64_t(2), "Edges across each bevel, 1 to 32. More is rounder."),
          param("profile", PlugType::Float, 0.5, "Profile shape: 0.5 is a round arc, 0.25 a straight chamfer."),
          param("angle", PlugType::Float, 30.0, "Bevel edges whose faces meet sharper than this, degrees. Negative bevels hard edges only.")},
         [](const geo::Mesh& in, const Node& n) {
             geo::Mesh m = in;
             geo::BevelOptions o;
             o.offset = f(n, "offset");
             o.segments = int(std::clamp<int64_t>(i(n, "segments"), 1, 32));
             o.profile = f(n, "profile");
             geo::bevelEdges(m, sharpEdges(m, f(n, "angle")), o);
             return m;
         }},
        {"arrayModifier",
         "Repeats the mesh in a row, each copy shifted by a fixed step. Use it for railings, stairs, tiles and other evenly "
         "spaced repeats.",
         {param("count", PlugType::Int, int64_t(3), "Copies including the original, 1 to 1000."),
          param("offset", PlugType::Vec3, glm::dvec3(110.0, 0.0, 0.0), "Step from one copy to the next, object space, cm.")},
         [](const geo::Mesh& in, const Node& n) {
             geo::Mesh m = in;
             m.compact();
             geo::Mesh copy = m;
             glm::dvec3 step = v(n, "offset");
             for (int64_t k = 1; k < std::clamp<int64_t>(i(n, "count"), 1, 1000); ++k)
             {
                 geo::Mesh shifted = copy;
                 for (geo::Index p = 0; p < shifted.pointCapacity(); ++p)
                     shifted.setPosition(p, copy.position(p) + step * double(k));
                 m.append(shifted);
             }
             return m;
         }},
        {"triangulateModifier",
         "Splits every face into triangles. Use it to see, or lock in, the triangulation an engine will get.",
         {},
         [](const geo::Mesh& in, const Node&) {
             geo::Mesh m = in;
             geo::triangulateFaces(m, allFaces(m));
             return m;
         }},
        {"weightedNormalsModifier",
         "Weights vertex normals by face area so big flat faces shade flat and the bevels take the curvature. Use it on "
         "bevelled hard-surface game meshes.",
         {param("keepHardEdges", PlugType::Bool, true, "Keep normals split along hard edges instead of smoothing across them.")},
         [](const geo::Mesh& in, const Node& n) {
             // Face-area weighting keeps big flat faces flat, the usual game-art bevel look.
             geo::Mesh m = in;
             geo::updateCornerNormals(m, {geo::NormalWeight::Area, b(n, "keepHardEdges")});
             return m;
         }},
        {"solidifyModifier",
         "Gives a surface thickness: an inner shell offset along the normals, joined to the outer one at open borders. Use it "
         "to turn planes and open shells into solid panels.",
         {param("thickness", PlugType::Float, 2.0, "Shell thickness, cm, offset inward along the normals.")},
         [](const geo::Mesh& in, const Node& n) { return solidify(in, f(n, "thickness")); }},
        {"decimateModifier",
         "Reduces the triangle count while keeping the shape, silhouette, UVs and normals. Use it for LODs or to lighten "
         "scans and sculpts.",
         {param("ratio", PlugType::Float, 0.5, "Fraction of triangles to keep, 0 to 1."),
          param("targetTriangles", PlugType::Int, int64_t(0), "Triangles to keep; above 0 it overrides ratio."),
          param("keepBorders", PlugType::Bool, true, "Open borders stay exactly where they are."),
          param("keepHardEdges", PlugType::Bool, true, "Hard edges stay intact, as UV seams and material borders always do; off lets them collapse for a lower count."),
          param("symmetry", PlugType::Enum, int64_t(0), "off, or reduce one half and mirror it so the result stays exactly symmetric across x, y or z."),
          param("quads", PlugType::Bool, false, "Join triangle pairs back into quads where they make good ones.")},
         [](const geo::Mesh& in, const Node& n) { return decimate(in, n); }},
    };
}

Graph& graph() { return Graph::current(); }

Node& meshNamed(const std::string& name, const char* cmd)
{
    Node* n = graph().find(name);
    if (!n || !n->type().isA("mesh"))
        throw CommandError(std::string(cmd) + ": " + name + " is not a mesh");
    return *n;
}

int outAttrOf(const Node& n)
{
    if (auto* m = modifiers::asModifier(&n))
        return m->outAttr;
    if (auto* g = dynamic_cast<const GeneratorNode*>(&n))
        return g->outAttr;
    return n.type().attrIndex("outMesh");
}
} // namespace

void ModifierNode::compute(const EvalContext&)
{
    LILA_ZONE();

    mError.clear();
    const MeshPtr& in = as<MeshPtr>(inAttr);
    if (!in || !as<bool>(enabledAttr) || modifiers::bypassed(in))
    {
        setOutput(outAttr, in);
        return;
    }
    try
    {
        setOutput(outAttr, MeshPtr(std::make_shared<geo::Mesh>(apply(*in, *this))));
    }
    catch (const std::exception& e)
    {
        log::warn("{}: {}", name(), e.what());
        mError = e.what();
        setOutput(outAttr, in);
    }
}

void MeshSourceNode::compute(const EvalContext&)
{
    setOutput(kOutMesh, as<MeshPtr>(kMesh));
}

namespace modifiers
{
bool bypassed(const MeshPtr& mesh) { return mesh && mesh->pointAttrs().has(kBypassLayer); }

bool isModifier(const Node& n) { return (n.traits() & Node::kModifier) != 0; }

std::vector<NodeId> stack(Graph& g, NodeId mesh)
{
    std::vector<NodeId> out;
    PlugRef plug{mesh, MeshNode::kInMesh, -1};
    while (auto src = g.source(plug))
    {
        Node* up = g.node(src->node);
        auto* mod = asModifier(up);
        if (!mod)
            break;
        out.insert(out.begin(), up->id());
        plug = {up->id(), mod->inAttr, -1};
    }
    return out;
}

namespace
{
/** The plug at the bottom of the modifier chain, and what feeds it. */
std::pair<PlugRef, std::optional<PlugRef>> chainBase(Graph& g, NodeId mesh)
{
    PlugRef plug{mesh, MeshNode::kInMesh, -1};
    while (auto src = g.source(plug))
    {
        auto* mod = asModifier(g.node(src->node));
        if (!mod)
            return {plug, src};
        plug = {mod->id(), mod->inAttr, -1};
    }
    return {plug, std::nullopt};
}
} // namespace

PlugRef editPlug(Graph& g, NodeId mesh)
{
    for (NodeId id : stack(g, mesh))
        if (static_cast<ModifierNode*>(g.node(id))->editingInput())
            throw CommandError(g.node(id)->name() + " is editing its edges: apply or cancel that first");
    auto [base, src] = chainBase(g, mesh);
    if (!src)
        return base;
    Node* up = g.node(src->node);
    if (dynamic_cast<MeshSourceNode*>(up))
        return {up->id(), MeshSourceNode::kMesh, -1};

    // A generator (or anything else) feeds the chain: freeze its current result into a stored source.
    g.evaluate();
    MeshPtr frozen = std::get<MeshPtr>(g.value(*src));
    g.disconnect(base);
    if (base.node == mesh)
    {
        g.setValue(base, frozen);
        if (g.destinations(*src).empty() && !up->type().dag)
            g.deleteNode(up->id());
        return base;
    }
    Node* source = g.createNode(*types::find("meshSource"), {}, kNoNode, true);
    g.setValue({source->id(), MeshSourceNode::kMesh, -1}, frozen);
    g.connect({source->id(), MeshSourceNode::kOutMesh, -1}, base);
    if (g.destinations(*src).empty() && !up->type().dag)
        g.deleteNode(up->id());
    return {source->id(), MeshSourceNode::kMesh, -1};
}

MeshPtr cage(Graph& g, NodeId mesh)
{
    g.evaluate();
    for (NodeId id : stack(g, mesh))
        if (auto* m = static_cast<ModifierNode*>(g.node(id)); m->editingInput())
            return m->as<MeshPtr>(m->inAttr);
    auto [base, src] = chainBase(g, mesh);
    if (!src)
        return std::get<MeshPtr>(g.value(base));
    return std::get<MeshPtr>(g.value(*src));
}

void registerTypes()
{
    if (types::find("meshSource"))
        return;
    NodeType source;
    source.name = "meshSource";
    source.icon = "database";
    source.doc = "Holds the base mesh under a modifier stack, where direct component edits land.";
    source.ownAttrs = {
        {.name = "mesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "Base mesh under a modifier stack."},
        {.name = "outMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden},
    };
    source.create = [] { return std::make_unique<MeshSourceNode>(); };
    types::registerType(std::move(source));

    for (ModifierType& m : modifierTypes())
    {
        NodeType t;
        t.name = m.name;
        t.doc = m.doc;
        t.icon = "stack-2";
        t.ownAttrs.push_back({.name = "inMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden});
        t.ownAttrs.push_back(param("enabled", PlugType::Bool, true, "Turn the modifier off without removing it."));
        t.ownAttrs.push_back({.name = "exportEnabled", .type = PlugType::Bool, .defaultValue = true, .flags = AttrInput | AttrStorable,
                              .doc = "Apply the modifier when exporting."});
        for (AttrDef& p : m.params)
        {
            if (std::string(m.name) == "mirrorModifier" && p.name == "axis")
                p.enumNames = {"x", "y", "z"};
            if (std::string(m.name) == "mirrorModifier" && p.name == "direction")
                p.enumNames = {"+ to -", "- to +", "both"};
            if (std::string(m.name) == "decimateModifier" && p.name == "symmetry")
                p.enumNames = {"off", "x", "y", "z"};
            if (p.type == PlugType::Vec3)
                for (int k = 0; k < 3; ++k)
                    p.childNames[k] = p.name + char('X' + k);
            t.ownAttrs.push_back(p);
        }
        t.ownAttrs.push_back({.name = "outMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden});
        int out = int(t.ownAttrs.size()) - 1;
        ModifierNode::Apply apply = m.apply;
        t.create = [apply, out] {
            auto n = std::make_unique<ModifierNode>();
            n->apply = apply;
            n->outAttr = out;
            return n;
        };
        types::registerType(std::move(t));
    }
    autoBevel::registerTypes();
    registerTransferModifier();
}

void registerCommands()
{
    autoBevel::registerCommands();
    registerStackCommands();
    using T = ArgType;
    std::string kindList;
    for (const std::string& kind : kinds())
        kindList += "\n- " + kind + ": " + kindDoc(kind);

    ops::registerCommand({
        .name = "addModifier",
        .doc = "Add a modifier to the top of a mesh's stack. params sets its parameters. Returns its name. modifierKinds(docs=True) "
               "also lists kinds extensions add later.\n\nKinds:" + kindList,
        .positionalDoc = "mesh",
        .flags = {{"kind", "", T::String, nullptr, "Modifier kind, such as bevel or mirror (see Kinds)."},
                  {"params", "", T::Any, nullptr, "{param: value} to set on the new modifier."}},
        .run = [](const Args& a) -> json {
            auto objs = a.objects();
            if (objs.empty() || !a.has("kind"))
                throw CommandError("addModifier: give a mesh and kind=");
            Graph& g = graph();
            Node& mesh = meshNamed(objs.back(), "addModifier");
            const NodeType* t = types::find(a.get<std::string>("kind") + "Modifier");
            if (!t)
                throw CommandError("addModifier: unknown kind '" + a.get<std::string>("kind") + "'");

            PlugRef top{mesh.id(), MeshNode::kInMesh, -1};
            auto src = g.source(top);
            Node* mod = g.createNode(*t, {}, kNoNode, true);
            auto* mn = static_cast<ModifierNode*>(mod);
            if (src)
            {
                g.disconnect(top);
                g.connect(*src, {mod->id(), mn->inAttr, -1});
            }
            else
            {
                // The mesh's stored base moves into a source node so it stays editable under the stack.
                Node* source = g.createNode(*types::find("meshSource"), {}, kNoNode, true);
                g.setValue({source->id(), MeshSourceNode::kMesh, -1}, g.storedValue(top));
                g.setValue(top, MeshPtr());
                g.connect({source->id(), MeshSourceNode::kOutMesh, -1}, {mod->id(), mn->inAttr, -1});
            }
            g.connect({mod->id(), mn->outAttr, -1}, top);
            if (a.has("params"))
                for (auto& [k, val] : a.flags["params"].items())
                {
                    PlugRef p = g.plug(mod->name() + "." + k);
                    AttrDef def = g.attr(p);
                    if (p.component >= 0)
                        def.type = PlugType::Float;
                    g.setValue(p, fromJson(def, val));
                }
            return mod->name();
        },
    });

    ops::registerCommand({
        .name = "modifiers",
        .doc = "A mesh's modifier stack, bottom (applied first) to top.",
        .positionalDoc = "mesh",
        .undoable = false,
        .run = [](const Args& a) -> json {
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("modifiers: no mesh");
            json out = json::array();
            for (NodeId id : stack(graph(), meshNamed(objs.back(), "modifiers").id()))
                out.push_back(graph().node(id)->name());
            return out;
        },
    });

    auto unlink = [](Graph& g, Node& mod) {
        auto* mn = static_cast<ModifierNode*>(&mod);
        auto src = g.source({mod.id(), mn->inAttr, -1});
        std::vector<PlugRef> dsts = g.destinations({mod.id(), mn->outAttr, -1});
        for (const PlugRef& d : dsts)
            g.disconnect(d);
        if (src)
            for (const PlugRef& d : dsts)
                g.connect(*src, d);
        g.deleteNode(mod.id());
    };

    ops::registerCommand({
        .name = "removeModifier",
        .doc = "Remove modifiers, reconnecting the stack around them.",
        .positionalDoc = "modifiers",
        .run = [unlink](const Args& a) -> json {
            Graph& g = graph();
            for (const std::string& name : a.objects())
            {
                Node* n = g.find(name);
                if (!n || !isModifier(*n))
                    throw CommandError("removeModifier: " + name + " is not a modifier");
                unlink(g, *n);
            }
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "applyModifier",
        .doc = "Bake a modifier (and everything below it in the stack) into the base mesh.",
        .positionalDoc = "modifier",
        .run = [unlink](const Args& a) -> json {
            Graph& g = graph();
            for (const std::string& name : a.objects())
            {
                Node* n = g.find(name);
                if (!n || !isModifier(*n))
                    throw CommandError("applyModifier: " + name + " is not a modifier");
                auto* mn = static_cast<ModifierNode*>(n);
                g.evaluate();
                MeshPtr baked = n->as<MeshPtr>(mn->outAttr);
                // Walk down to the source and drop every modifier below this one along the way.
                PlugRef plug{n->id(), mn->inAttr, -1};
                std::vector<NodeId> below;
                std::optional<PlugRef> src;
                while ((src = g.source(plug)))
                {
                    auto* down = asModifier(g.node(src->node));
                    if (!down)
                        break;
                    below.push_back(down->id());
                    plug = {down->id(), down->inAttr, -1};
                }
                std::vector<PlugRef> dsts = g.destinations({n->id(), mn->outAttr, -1});
                for (const PlugRef& d : dsts)
                    g.disconnect(d);
                Node* source = src ? g.node(src->node) : nullptr;
                if (!dynamic_cast<MeshSourceNode*>(source))
                {
                    if (src)
                        g.disconnect(plug);
                    source = g.createNode(*types::find("meshSource"), {}, kNoNode, true);
                }
                g.setValue({source->id(), MeshSourceNode::kMesh, -1}, baked);
                for (const PlugRef& d : dsts)
                    g.connect({source->id(), MeshSourceNode::kOutMesh, -1}, d);
                g.deleteNode(n->id());
                for (NodeId id : below)
                    g.deleteNode(id);
            }
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "moveModifier",
        .doc = "Move a modifier up or down its stack by a number of steps (positive is up, applied later).",
        .positionalDoc = "modifier",
        .flags = {{"by", "", T::Int, 1, "Steps."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("moveModifier: no modifier");
            Node* n = g.find(objs.back());
            if (!n || !isModifier(*n))
                throw CommandError("moveModifier: " + objs.back() + " is not a modifier");
            // Find the mesh at the top of this chain.
            NodeId cur = n->id();
            PlugRef top;
            while (true)
            {
                auto dsts = g.destinations({cur, outAttrOf(*g.node(cur)), -1});
                if (dsts.empty())
                    throw CommandError("moveModifier: modifier is not connected to a mesh");
                Node* d = g.node(dsts[0].node);
                if (d->type().isA("mesh"))
                {
                    top = dsts[0];
                    break;
                }
                cur = d->id();
            }
            std::vector<NodeId> order = stack(g, top.node);
            auto it = std::find(order.begin(), order.end(), n->id());
            int from = int(it - order.begin());
            int to = std::clamp(from + a.get<int>("by"), 0, int(order.size()) - 1);
            if (from == to)
                return nullptr;
            order.erase(order.begin() + from);
            order.insert(order.begin() + to, n->id());
            // Rewire the chain in the new order.
            auto* first = static_cast<ModifierNode*>(g.node(stack(g, top.node).front()));
            auto base = g.source({first->id(), first->inAttr, -1});
            for (NodeId id : stack(g, top.node))
                g.disconnect({id, static_cast<ModifierNode*>(g.node(id))->inAttr, -1});
            g.disconnect(top);
            std::optional<PlugRef> prev = base;
            for (NodeId id : order)
            {
                auto* m = static_cast<ModifierNode*>(g.node(id));
                if (prev)
                    g.connect(*prev, {id, m->inAttr, -1});
                prev = PlugRef{id, m->outAttr, -1};
            }
            g.connect(*prev, top);
            return nullptr;
        },
    });
}
} // namespace modifiers
} // namespace lila::scene
