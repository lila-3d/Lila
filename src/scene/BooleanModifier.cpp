#include "ops/Command.h"
#include "core/Profile.h"
#include "scene/AttrBuilders.h"
#include "scene/Graph.h"
#include "scene/MeshBoolean.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"

namespace lila::scene
{
using namespace attrdefs;

namespace
{
const std::vector<std::string> kOperations = {"difference", "union", "intersection"};

geo::Mesh run(const geo::Mesh& in, const Node& self)
{
    LILA_ZONE();

    auto at = [&](const char* name) { return self.type().attrIndex(name); };
    MeshPtr cutter = self.as<MeshPtr>(at("cutterMesh"));
    if (!cutter)
        throw std::runtime_error("no cutter: connect one with polyBoolean(live=True)");
    const glm::dmat4 world = self.as<glm::dmat4>(at("targetMatrix"));
    BooleanOperand operands[] = {{&in, world}, {cutter.get(), self.as<glm::dmat4>(at("cutterMatrix"))}};
    const int64_t op = std::clamp<int64_t>(self.as<int64_t>(at("operation")), 0, 2);
    BooleanOptions o{.op = op == 0 ? BooleanOp::Difference : op == 1 ? BooleanOp::Union : BooleanOp::Intersection,
                     .cleanup = self.as<bool>(at("cleanup")),
                     .splitNgons = self.as<bool>(at("splitNgons")),
                     .quadBand = self.as<bool>(at("quadBand")),
                     .bandRings = int(std::clamp<int64_t>(self.as<int64_t>(at("bandRings")), 0, 8)),
                     .bandTimeout = std::max(0.0, self.as<double>(at("timeout"))),
                     .filletWidth = std::max(0.0, self.as<double>(at("filletWidth"))),
                     .filletSegments = int(std::clamp<int64_t>(self.as<int64_t>(at("filletSegments")), 1, 64)),
                     .hardAngle = self.as<double>(at("hardAngle"))};
    BooleanResult r = meshBoolean(operands, glm::inverse(world), o);
    if (!r.ok())
        throw std::runtime_error((r.badOperand == 1 ? "the cutter " : r.badOperand == 0 ? "this mesh " : "") + r.error);
    return std::move(r.mesh);
}
} // namespace

void registerBooleanModifier()
{
    if (types::find("booleanModifier"))
        return;
    NodeType t;
    t.name = "booleanModifier";
    t.icon = "boolean";
    t.doc = "A boolean kept live: cuts, joins or intersects this mesh with a cutter mesh, so moving or editing the cutter "
            "updates the result. Stack one per cutter. Apply bakes it.";
    t.ownAttrs = {
        {.name = "inMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden},
        param("enabled", PlugType::Bool, true, "Turn the modifier off without removing it."),
        {.name = "exportEnabled", .type = PlugType::Bool, .defaultValue = true, .flags = AttrInput | AttrStorable,
         .doc = "Apply the modifier when exporting."},
        {.name = "cutterMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput,
         .doc = "The cutter's outMesh, connected by polyBoolean(live=True)."},
        {.name = "cutterMatrix", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrInput | AttrHidden,
         .doc = "The cutter's worldMatrix."},
        {.name = "targetMatrix", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "This mesh's world matrix when the cutter was connected."},
        param("operation", PlugType::Enum, int64_t(0), "difference, union or intersection.", kOperations),
        param("cleanup", PlugType::Bool, true, "Weld slivers and stray points along the seam."),
        param("splitNgons", PlugType::Bool, true, "Cut the n-gons along the seam into quads that carry on the edge loops."),
        param("quadBand", PlugType::Bool, false, "Remesh a band around the seam into quads that follow it. Slower: it reruns as the cutter moves."),
        param("bandRings", PlugType::Int, int64_t(1), "Rings of faces past the seam the quad band takes in.", 0.0, 8.0),
        param("timeout", PlugType::Float, 2.0, "Seconds the quad band may take before the n-gon split runs instead.", 0.0),
        param("filletWidth", PlugType::Float, 0.0, "Width of a rounded strip along the seam, cm; 0 keeps it sharp.", 0.0),
        param("filletSegments", PlugType::Int, int64_t(2), "Rows of faces across the fillet.", 1.0, 64.0),
        param("hardAngle", PlugType::Float, 30.0, "New edges sharper than this many degrees are marked hard.", 0.0, 180.0),
        {.name = "outMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden},
    };
    const int out = int(t.ownAttrs.size()) - 1;
    t.create = [out] {
        auto n = std::make_unique<ModifierNode>();
        n->outAttr = out;
        n->apply = run;
        return n;
    };
    types::registerType(std::move(t));
}
} // namespace lila::scene
