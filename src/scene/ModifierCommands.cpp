#include "scene/Modifiers.h"

#include "cmds/Command.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <algorithm>

namespace lila::scene::modifiers
{
namespace
{
constexpr std::string_view kSuffix = "Modifier";

Graph& graph() { return Graph::current(); }

ModifierNode& modifierNamed(const std::string& name, const char* cmd)
{
    auto* n = asModifier(graph().find(name));
    if (!n)
        throw CommandError(std::string(cmd) + ": " + name + " is not a modifier");
    return *n;
}

Node& meshNamed(const std::string& name, const char* cmd)
{
    Node* n = graph().find(name);
    if (!n || !n->type().isA("mesh"))
        throw CommandError(std::string(cmd) + ": " + name + " is not a mesh");
    return *n;
}

int outAttrOf(const Node& n)
{
    if (auto* m = asModifier(&n))
        return m->outAttr;
    if (auto* gen = dynamic_cast<const GeneratorNode*>(&n))
        return gen->outAttr;
    return n.type().attrIndex("outMesh");
}

/** Deletes a node nothing reads any more, unless it lives in the hierarchy. */
void dropIfOrphan(Graph& g, NodeId id)
{
    Node* n = g.node(id);
    if (!n || n->type().dag)
        return;
    int out = outAttrOf(*n);
    if (out < 0 || g.destinations({id, out, -1}).empty())
        g.deleteNode(id);
}

json describe(Graph& g, NodeId id)
{
    Node* n = g.node(id);
    json j = {{"name", n->name()}, {"type", n->type().name}};
    if (auto* m = asModifier(n))
    {
        j["enabled"] = n->as<bool>(m->enabledAttr);
        j["exportEnabled"] = exportEnabled(*n);
        j["error"] = m->error();
    }
    return j;
}
} // namespace

std::vector<std::string> kinds()
{
    std::vector<std::string> out;
    for (const NodeType* t : types::all())
    {
        const std::string& name = t->name;
        if (!t->create || name.size() <= kSuffix.size() || !name.ends_with(kSuffix))
            continue;
        // Geometry-node modifiers need their group wired in, so they are added from the node editor instead.
        if (t->attrIndex("groupDefinition") >= 0)
            continue;
        if (!asModifier(t->create().get()))
            continue;
        out.push_back(name.substr(0, name.size() - kSuffix.size()));
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string kindDoc(std::string_view kind)
{
    const NodeType* t = types::find(std::string(kind) + std::string(kSuffix));
    return t ? t->doc : std::string();
}

NodeId stackSource(Graph& g, NodeId mesh)
{
    std::vector<NodeId> mods = stack(g, mesh);
    PlugRef bottom{mesh, MeshNode::kInMesh, -1};
    if (!mods.empty())
        bottom = {mods.front(), static_cast<ModifierNode*>(g.node(mods.front()))->inAttr, -1};
    auto src = g.source(bottom);
    return src ? src->node : kNoNode;
}

NodeId meshOf(Graph& g, NodeId modifier)
{
    NodeId cur = modifier;
    // A chain can't loop, but a malformed graph shouldn't hang the UI either.
    for (int guard = 0; guard < 4096; ++guard)
    {
        Node* n = g.node(cur);
        if (!n)
            return kNoNode;
        if (n->type().isA("mesh"))
            return cur;
        int out = outAttrOf(*n);
        if (out < 0)
            return kNoNode;
        auto dsts = g.destinations({cur, out, -1});
        if (dsts.empty())
            return kNoNode;
        cur = dsts.front().node;
    }
    return kNoNode;
}

bool exportEnabled(const Node& modifier)
{
    int a = modifier.type().attrIndex("exportEnabled");
    return a < 0 || modifier.as<bool>(a);
}

MeshPtr exportMesh(Graph& g, NodeId mesh)
{
    LILA_ZONE();

    g.evaluate();
    Node* m = g.node(mesh);
    if (!m)
        return nullptr;
    std::vector<NodeId> mods = stack(g, mesh);
    auto ranInViewport = [](const ModifierNode& mod) { return mod.as<bool>(mod.enabledAttr) && !bypassed(mod.as<MeshPtr>(mod.inAttr)); };
    // Evaluated results stay valid up to the first modifier whose export state differs from what the viewport ran.
    auto split = std::find_if(mods.begin(), mods.end(), [&](NodeId id) {
        auto* mod = static_cast<ModifierNode*>(g.node(id));
        return ranInViewport(*mod) != exportEnabled(*mod);
    });
    if (split == mods.end())
        return m->as<MeshPtr>(MeshNode::kOutMesh);

    auto* first = static_cast<ModifierNode*>(g.node(*split));
    MeshPtr cur = first->as<MeshPtr>(first->inAttr);
    for (auto it = split; it != mods.end(); ++it)
    {
        auto* mod = static_cast<ModifierNode*>(g.node(*it));
        bool exported = exportEnabled(*mod);
        if (!exported || !cur)
            continue;
        if (!mod->apply)
        {
            log::warn("{}: can't re-run for export after an earlier modifier changed; exported without it", mod->name());
            continue;
        }
        try
        {
            cur = std::make_shared<geo::Mesh>(mod->apply(*cur, *mod));
        }
        catch (const std::exception& e)
        {
            log::warn("{}: {}", mod->name(), e.what());
        }
    }
    return cur;
}

void registerStackCommands()
{
    cmds::registerCommand({
        .name = "modifierStack",
        .doc = "A mesh's stack as data: its source (generator, meshSource or null) and each modifier, applied first first, with "
               "name, type, enabled, exportEnabled and its last evaluation error.",
        .positionalDoc = "mesh",
        .undoable = false,
        .run = [](const Args& a) -> json {
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("modifierStack: no mesh");
            Graph& g = graph();
            NodeId mesh = meshNamed(objs.back(), "modifierStack").id();
            g.evaluate();
            json mods = json::array();
            for (NodeId id : stack(g, mesh))
                mods.push_back(describe(g, id));
            NodeId src = stackSource(g, mesh);
            return {{"mesh", g.node(mesh)->name()}, {"source", src ? describe(g, src) : json(nullptr)}, {"modifiers", mods}};
        },
    });

    cmds::registerCommand({
        .name = "modifierKinds",
        .doc = "Every modifier kind addModifier accepts, including ones extensions registered. docs=True returns {kind: description} "
               "instead.",
        .flags = {{"docs", "", ArgType::Bool, false, "Return each kind's description too."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (!a.get<bool>("docs"))
                return kinds();
            json out = json::object();
            for (const std::string& kind : kinds())
                out[kind] = kindDoc(kind);
            return out;
        },
    });

    cmds::registerCommand({
        .name = "duplicateModifier",
        .doc = "Copy a modifier with its settings and insert the copy right after it in the stack. Returns the copy's name.",
        .positionalDoc = "modifier",
        .run = [](const Args& a) -> json {
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("duplicateModifier: no modifier");
            Graph& g = graph();
            ModifierNode& orig = modifierNamed(objs.back(), "duplicateModifier");
            const NodeType& type = orig.type();
            auto* copy = static_cast<ModifierNode*>(g.createNode(type, {}, kNoNode, true));
            for (int i = 0; i < int(type.attrs.size()); ++i)
            {
                const AttrDef& def = type.attrs[i];
                if (i == orig.inAttr || !(def.flags & AttrInput))
                    continue;
                // Driven inputs (keys, geometry-node groups, object info) share the same driver.
                if (auto src = g.source({orig.id(), i, -1}))
                    g.connect(*src, {copy->id(), i, -1});
                else if (def.type != PlugType::Mesh)
                    g.setValue({copy->id(), i, -1}, orig.value(i));
            }
            PlugRef out{orig.id(), orig.outAttr, -1};
            std::vector<PlugRef> dsts = g.destinations(out);
            for (const PlugRef& d : dsts)
                g.disconnect(d);
            g.connect(out, {copy->id(), copy->inAttr, -1});
            for (const PlugRef& d : dsts)
                g.connect({copy->id(), copy->outAttr, -1}, d);
            return copy->name();
        },
    });

    cmds::registerCommand({
        .name = "applyAllModifiers",
        .doc = "Bake every modifier of a mesh into its base mesh, leaving an empty stack over an editable source.",
        .positionalDoc = "meshes",
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            for (const std::string& name : a.objects())
            {
                NodeId mesh = meshNamed(name, "applyAllModifiers").id();
                std::vector<NodeId> mods = stack(g, mesh);
                if (mods.empty())
                    continue;
                NodeId src = stackSource(g, mesh);
                cmds::execute("applyModifier", {{"args", {g.node(mods.back())->name()}}});
                if (src && !dynamic_cast<MeshSourceNode*>(g.node(src)))
                    dropIfOrphan(g, src);
            }
            return nullptr;
        },
    });

    cmds::registerCommand({
        .name = "collapseStack",
        .doc = "Bake a mesh's whole history (generator and modifiers) into the mesh itself and delete it, like Maya's Delete History.",
        .positionalDoc = "meshes",
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            for (const std::string& name : a.objects())
            {
                Node& mesh = meshNamed(name, "collapseStack");
                std::vector<NodeId> mods = stack(g, mesh.id());
                for (NodeId id : mods)
                    if (static_cast<ModifierNode*>(g.node(id))->editingInput())
                        throw CommandError("collapseStack: " + g.node(id)->name() + " is editing its edges: apply or cancel that first");
                PlugRef in{mesh.id(), MeshNode::kInMesh, -1};
                if (!g.source(in))
                    continue;
                g.evaluate();
                MeshPtr baked = mesh.as<MeshPtr>(MeshNode::kOutMesh);
                NodeId src = stackSource(g, mesh.id());
                g.disconnect(in);
                g.setValue(in, baked);
                for (auto it = mods.rbegin(); it != mods.rend(); ++it)
                    g.deleteNode(*it);
                if (src)
                    dropIfOrphan(g, src);
            }
            return nullptr;
        },
    });
}
} // namespace lila::scene::modifiers
