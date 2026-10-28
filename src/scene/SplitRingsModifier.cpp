#include "scene/SplitRingsModifier.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "geo/LoopTools.h"
#include "geo/Mesh.h"
#include "geo/Ops.h"
#include "ops/Command.h"
#include "scene/ExtrudeModifier.h"
#include "scene/Graph.h"
#include "scene/Modifiers.h"

#include <algorithm>

namespace lila::scene
{
// Named, not anonymous: unity builds share this file's anonymous namespace with the other modifiers'.
namespace splitRingsImpl
{
using geo::Index;

enum Attr
{
    kInMesh,
    kEnabled,
    kExportEnabled,
    kCount,
    kDirection,
    kEdgeFlow,
    kComponents,
    kEditing,
    kEditBackup,
    kOutMesh,
    kLost,
};

/** The selection to put back when Edit Selection ends. */
struct EditBackup final : Data
{
    json value;
    const char* kind() const override { return "splitRingsEditBackup"; }
    json toJson() const override { return {{"kind", kind()}, {"value", value}}; }
};

class SplitRingsModifierNode final : public ModifierNode
{
public:
    SplitRingsModifierNode()
    {
        inAttr = kInMesh;
        enabledAttr = kEnabled;
        outAttr = kOutMesh;
    }

    bool editingInput() const override { return as<bool>(kEditing); }

    void compute(const EvalContext&) override
    {
        LILA_ZONE();

        mError.clear();
        const MeshPtr& in = as<MeshPtr>(kInMesh);
        setOutput(kOutMesh, in);
        setOutput(kLost, int64_t(0));
        if (!in || modifiers::bypassed(in))
            return;
        if (as<bool>(kEditing))
        {
            auto shown = std::make_shared<geo::Mesh>(*in);
            shown->pointAttrs().add(modifiers::kBypassLayer, geo::AttrType::Bool);
            setOutput(kOutMesh, MeshPtr(std::move(shown)));
            return;
        }
        if (!as<bool>(kEnabled))
            return;
        try
        {
            const auto* stored = dynamic_cast<const ExtrudeComponents*>(as<DataPtr>(kComponents).get());
            extrudeModifier::Resolved r = extrudeModifier::resolveComponents(*in, stored);
            setOutput(kLost, int64_t(r.lost));
            if (r.ids.empty())
                return;
            geo::Mesh m = *in;
            const std::vector<Index> ringEdges = r.kind == ComponentMode::Face ? geo::regionRingEdges(m, r.ids, as<int64_t>(kDirection) == 1) : r.ids;
            const std::vector<Index> made = geo::splitRings(m, ringEdges, int(std::clamp<int64_t>(as<int64_t>(kCount), 1, 32)));
            if (as<bool>(kEdgeFlow) && !made.empty())
                for (const geo::PointMove& mv : geo::editEdgeFlow(m, made, {}))
                    m.setPosition(mv.point, mv.position);
            m.compact();
            setOutput(kOutMesh, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
        }
        catch (const std::exception& e)
        {
            log::warn("{}: {}", name(), e.what());
            mError = e.what();
            setOutput(kOutMesh, in);
        }
    }
};

/** addModifier's hook: the new modifier takes the selected edges or faces; with none it splits the whole mesh. */
void takeSelection(Graph& g, Node& mod, NodeId mesh)
{
    if (DataPtr c = extrudeModifier::selectedComponents(g, mesh))
        g.setValue({mod.id(), kComponents, -1}, c);
}

Graph& graph() { return Graph::current(); }

/** A named splitRingsModifier, or the one editing (else the topmost) in a named or selected mesh's stack. */
Node& modifierArg(const Args& a, const char* cmd)
{
    Graph& g = graph();
    for (const std::string& name : a.objects())
    {
        Node* n = g.find(name);
        if (!n)
            continue;
        if (splitRingsModifier::isSplitRings(*n))
            return *n;
        if (n->type().isA("mesh"))
        {
            std::vector<NodeId> s = modifiers::stack(g, n->id());
            for (NodeId id : s)
                if (splitRingsModifier::editing(*g.node(id)))
                    return *g.node(id);
            for (auto it = s.rbegin(); it != s.rend(); ++it)
                if (splitRingsModifier::isSplitRings(*g.node(*it)))
                    return *g.node(*it);
        }
    }
    throw CommandError(std::string(cmd) + ": give a splitRings modifier or a mesh that has one");
}

NodeId meshArg(Graph& g, Node& mod, const char* cmd)
{
    NodeId mesh = modifiers::meshOf(g, mod.id());
    if (mesh == kNoNode)
        throw CommandError(std::string(cmd) + ": " + mod.path() + " feeds no mesh");
    return mesh;
}

void endEdit(Graph& g, Node& mod, NodeId mesh)
{
    json backup;
    if (auto* d = dynamic_cast<const EditBackup*>(mod.as<DataPtr>(kEditBackup).get()))
        backup = d->value;
    g.setValue({mod.id(), kEditing, -1}, false);
    g.setValue({mod.id(), kEditBackup, -1}, DataPtr());
    modifiers::restoreSelection(g, mesh, backup);
}

AttrDef param(std::string name, PlugType type, PlugValue def, std::string doc, std::optional<double> lo = {}, std::optional<double> hi = {})
{
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = AttrInput | AttrStorable | AttrKeyable,
            .doc = std::move(doc), .minValue = lo, .maxValue = hi};
}
} // namespace splitRingsImpl

namespace splitRingsModifier
{
bool isSplitRings(const Node& n) { return dynamic_cast<const splitRingsImpl::SplitRingsModifierNode*>(&n) != nullptr; }

bool editing(const Node& modifier) { return isSplitRings(modifier) && modifier.as<bool>(splitRingsImpl::kEditing); }

Selection selection(Graph& g, NodeId modifier)
{
    Selection out;
    Node* mod = g.node(modifier);
    if (!mod || !isSplitRings(*mod))
        return out;
    g.evaluate();
    const MeshPtr& in = mod->as<MeshPtr>(splitRingsImpl::kInMesh);
    const auto* stored = dynamic_cast<const ExtrudeComponents*>(mod->as<DataPtr>(splitRingsImpl::kComponents).get());
    out.whole = !stored;
    if (!in)
        return out;
    extrudeModifier::Resolved r = extrudeModifier::resolveComponents(*in, stored);
    out.kind = r.kind;
    out.ids.assign(r.ids.begin(), r.ids.end());
    out.lost = r.lost;
    return out;
}

void baseRemapped(Graph& g, NodeId mesh, const geo::MeshRemap& remap, const geo::Mesh& base)
{
    std::vector<NodeId> s = modifiers::stack(g, mesh);
    if (s.empty() || !isSplitRings(*g.node(s.front())))
        return;
    Node& mod = *g.node(s.front());
    if (DataPtr next = extrudeModifier::remapComponents(dynamic_cast<const ExtrudeComponents*>(mod.as<DataPtr>(splitRingsImpl::kComponents).get()), remap, base))
        g.setValue({mod.id(), splitRingsImpl::kComponents, -1}, next);
}

void registerTypes()
{
    if (types::find("splitRingsModifier"))
        return;
    data::registerKind("splitRingsEditBackup", [](const json& j) -> DataPtr {
        auto d = std::make_shared<splitRingsImpl::EditBackup>();
        d->value = j.value("value", json());
        return d;
    });
    NodeType t;
    t.name = "splitRingsModifier";
    t.icon = "stack-2";
    t.doc = "Subdivides one way, kept live: loops across the rings of the edges or faces selected when it was added, curved to "
            "the surface. Without a selection it splits the whole mesh.";
    AttrDef direction = splitRingsImpl::param("direction", PlugType::Enum, int64_t(0), "Faces: along adds loops down the selection's long way, across its short way.");
    direction.enumNames = {"along", "across"};
    t.ownAttrs = {
        {.name = "inMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden},
        splitRingsImpl::param("enabled", PlugType::Bool, true, "Turn the modifier off without removing it."),
        {.name = "exportEnabled", .type = PlugType::Bool, .defaultValue = true, .flags = AttrInput | AttrStorable,
         .doc = "Apply the modifier when exporting."},
        splitRingsImpl::param("count", PlugType::Int, int64_t(1), "Loops per ring, evenly spaced, 1 to 32.", 1.0, 32.0),
        direction,
        splitRingsImpl::param("edgeFlow", PlugType::Bool, true, "Curve the new loops to follow the surrounding surface rather than lie flat."),
        {.name = "components", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "The edges or faces whose rings are split."},
        {.name = "editing", .type = PlugType::Bool, .defaultValue = false, .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "Edit Selection is in progress."},
        {.name = "editBackup", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "The selection to restore when Edit Selection ends."},
        {.name = "outMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden},
        {.name = "lostComponents", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = AttrOutput,
         .doc = "Edges or faces no longer found on the input mesh."},
    };
    t.create = [] { return std::make_unique<splitRingsImpl::SplitRingsModifierNode>(); };
    types::registerType(std::move(t));
    modifiers::setCreateHook("splitRingsModifier", splitRingsImpl::takeSelection);
}

void registerCommands()
{
    ops::registerCommand({
        .name = "splitRingsEditSelection",
        .doc = "Show a splitRings modifier's input mesh (it and later modifiers pass through) with the edges or faces it splits "
               "selected, so they can be changed with the normal selection tools. Finish with splitRingsApplySelection or "
               "splitRingsCancelSelection.",
        .positionalDoc = "modifier or mesh",
        .run = [](const Args& a) -> json {
            Graph& g = splitRingsImpl::graph();
            Node& mod = splitRingsImpl::modifierArg(a, "splitRingsEditSelection");
            NodeId mesh = splitRingsImpl::meshArg(g, mod, "splitRingsEditSelection");
            for (NodeId id : modifiers::stack(g, mesh))
                if (id != mod.id() && static_cast<ModifierNode*>(g.node(id))->editingInput())
                    throw CommandError("splitRingsEditSelection: " + g.node(id)->path() + " is already editing its edges");
            if (!editing(mod))
            {
                g.evaluate();
                auto backup = std::make_shared<splitRingsImpl::EditBackup>();
                backup->value = modifiers::selectionSnapshot(g, mesh);
                g.setValue({mod.id(), splitRingsImpl::kEditBackup, -1}, DataPtr(backup));
                g.setValue({mod.id(), splitRingsImpl::kEditing, -1}, true);
            }
            const Selection sel = selection(g, mod.id());
            g.select({mesh});
            components::select(mesh, sel.kind, sel.ids);
            components::setMode(sel.kind);
            return mod.path();
        },
    });

    ops::registerCommand({
        .name = "splitRingsApplySelection",
        .doc = "Store the selected edges or faces (with their mirror images under symmetry) as what a splitRings modifier splits, "
               "and end Edit Selection. Returns how many it stored.",
        .positionalDoc = "modifier or mesh",
        .run = [](const Args& a) -> json {
            Graph& g = splitRingsImpl::graph();
            Node& mod = splitRingsImpl::modifierArg(a, "splitRingsApplySelection");
            if (!editing(mod))
                throw CommandError("splitRingsApplySelection: " + mod.path() + " is not editing its selection");
            NodeId mesh = splitRingsImpl::meshArg(g, mod, "splitRingsApplySelection");
            DataPtr picked = extrudeModifier::selectedComponents(g, mesh);
            const auto* c = dynamic_cast<const ExtrudeComponents*>(picked.get());
            if (!c)
                throw CommandError("splitRingsApplySelection: select the edges or faces to split, or cancel");
            const size_t count = c->edges ? c->edgeRefs.size() : c->faces.size();
            g.setValue({mod.id(), splitRingsImpl::kComponents, -1}, picked);
            splitRingsImpl::endEdit(g, mod, mesh);
            return count;
        },
    });

    ops::registerCommand({
        .name = "splitRingsCancelSelection",
        .doc = "End a splitRings modifier's Edit Selection without changing what it splits.",
        .positionalDoc = "modifier or mesh",
        .run = [](const Args& a) -> json {
            Graph& g = splitRingsImpl::graph();
            Node& mod = splitRingsImpl::modifierArg(a, "splitRingsCancelSelection");
            if (!editing(mod))
                throw CommandError("splitRingsCancelSelection: " + mod.path() + " is not editing its selection");
            splitRingsImpl::endEdit(g, mod, splitRingsImpl::meshArg(g, mod, "splitRingsCancelSelection"));
            return nullptr;
        },
    });
}
} // namespace splitRingsModifier
} // namespace lila::scene
