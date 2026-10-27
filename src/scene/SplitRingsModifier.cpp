#include "scene/SplitRingsModifier.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "geo/LoopTools.h"
#include "geo/Mesh.h"
#include "geo/Ops.h"
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
    kOutMesh,
    kLost,
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

    void compute(const EvalContext&) override
    {
        LILA_ZONE();

        mError.clear();
        const MeshPtr& in = as<MeshPtr>(kInMesh);
        setOutput(kOutMesh, in);
        setOutput(kLost, int64_t(0));
        if (!in || !as<bool>(kEnabled) || modifiers::bypassed(in))
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

bool isSplitRings(const Node& n) { return dynamic_cast<const SplitRingsModifierNode*>(&n) != nullptr; }

/** addModifier's hook: the new modifier takes the selected edges or faces; with none it splits the whole mesh. */
void takeSelection(Graph& g, Node& mod, NodeId mesh)
{
    if (DataPtr c = extrudeModifier::selectedComponents(g, mesh))
        g.setValue({mod.id(), kComponents, -1}, c);
}

AttrDef param(std::string name, PlugType type, PlugValue def, std::string doc)
{
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = AttrInput | AttrStorable | AttrKeyable,
            .doc = std::move(doc)};
}
} // namespace splitRingsImpl

namespace splitRingsModifier
{

void baseRemapped(Graph& g, NodeId mesh, const geo::MeshRemap& remap, const geo::Mesh& base)
{
    std::vector<NodeId> s = modifiers::stack(g, mesh);
    if (s.empty() || !splitRingsImpl::isSplitRings(*g.node(s.front())))
        return;
    Node& mod = *g.node(s.front());
    if (DataPtr next = extrudeModifier::remapComponents(dynamic_cast<const ExtrudeComponents*>(mod.as<DataPtr>(splitRingsImpl::kComponents).get()), remap, base))
        g.setValue({mod.id(), splitRingsImpl::kComponents, -1}, next);
}

void registerTypes()
{
    if (types::find("splitRingsModifier"))
        return;
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
        splitRingsImpl::param("count", PlugType::Int, int64_t(1), "Loops per ring, evenly spaced, 1 to 32."),
        direction,
        splitRingsImpl::param("edgeFlow", PlugType::Bool, true, "Curve the new loops to follow the surrounding surface rather than lie flat."),
        {.name = "components", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "The edges or faces whose rings are split."},
        {.name = "outMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden},
        {.name = "lostComponents", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = AttrOutput,
         .doc = "Edges or faces no longer found on the input mesh."},
    };
    t.create = [] { return std::make_unique<splitRingsImpl::SplitRingsModifierNode>(); };
    types::registerType(std::move(t));
    modifiers::setCreateHook("splitRingsModifier", splitRingsImpl::takeSelection);
}
} // namespace splitRingsModifier
} // namespace lila::scene
