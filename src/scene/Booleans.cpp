#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Profile.h"
#include "core/Progress.h"
#include "geo/Mesh.h"
#include "scene/Graph.h"
#include "scene/MeshBoolean.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"

namespace lila::scene
{
namespace
{
/** polyBoolean(live=True): a boolean modifier per cutter, wired to it, and the cutters moved to a template layer. */
json liveBoolean(Graph& g, const std::vector<Node*>& nodes, const Args& a)
{
    const std::string base = nodes[0]->path();
    const glm::dmat4 world = nodes[0]->as<glm::dmat4>(Transform::kWorldMatrix);
    UndoChunk chunk("Live Boolean");
    json params = {{"operation", a.get<std::string>("operation", "difference") == "union"          ? 1
                                 : a.get<std::string>("operation", "difference") == "intersection" ? 2
                                                                                                   : 0},
                   {"cleanup", a.get<bool>("cleanup", true)},
                   {"splitNgons", a.get<bool>("splitNgons", true)},
                   {"quadBand", a.get<bool>("quadBand", false)},
                   {"bandRings", a.get<int64_t>("bandRings", 1)},
                   {"filletWidth", a.get<double>("filletWidth", 0.0)},
                   {"filletSegments", a.get<int64_t>("filletSegments", 2)},
                   {"hardAngle", a.get<double>("hardAngle", 30.0)}};
    json cutters = json::array();
    for (size_t i = 1; i < nodes.size(); ++i)
    {
        std::string mod = ops::execute("addModifier", {{"args", {base}}, {"kind", "boolean"}, {"params", params}});
        Node* m = g.find(mod);
        g.connect({nodes[i]->id(), MeshNode::kOutMesh, -1}, {m->id(), m->type().attrIndex("cutterMesh"), -1}, true);
        g.connect({nodes[i]->id(), Transform::kWorldMatrix, -1}, {m->id(), m->type().attrIndex("cutterMatrix"), -1}, true);
        g.setValue({m->id(), m->type().attrIndex("targetMatrix"), -1}, world);
        cutters.push_back(nodes[i]->path());
    }
    const char* layer = "BooleanCutters";
    if (!g.find(layer))
        ops::execute("createDisplayLayer", {{"name", layer}, {"empty", true}});
    json members = cutters;
    members.insert(members.begin(), layer);
    ops::execute("editDisplayLayerMembers", {{"args", members}});
    ops::execute("setAttr", {{"args", {std::string(layer) + ".displayType", 1}}});
    g.select({nodes[0]->id()});
    return base;
}
} // namespace

void registerBooleanCommands()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "polyBoolean",
        .doc = "Union, difference or intersection of meshes (Manifold: robust, watertight output), with the seam cleaned up: "
               "slivers welded, the n-gons along the cut split into quads that carry on the edge loops, an optional rounded "
               "fillet, and weighted normals. The first object keeps the result; the others are deleted unless keepCutters=True.",
        .positionalDoc = "base cutters...",
        .flags = {{"operation", "", T::String, "difference", "union, difference or intersection.", {"union", "difference", "intersection"}},
                  {"keepCutters", "", T::Bool, false, "Keep the cutter objects (hidden)."},
                  {"live", "", T::Bool, false,
                   "Keep it live instead of baking: a boolean modifier per cutter, so moving or editing a cutter updates the "
                   "cut. The cutters go in a template display layer (BooleanCutters): grey wire you select from the outliner."},
                  {"cleanup", "", T::Bool, true, "Weld slivers and stray points along the seam."},
                  {"snapDistance", "", T::Float, -1.0, "Weld distance for cleanup, cm; negative is a tenth of the median seam edge."},
                  {"splitNgons", "", T::Bool, true, "Cut the n-gons along the seam into quads (and triangles) that carry on the edge loops."},
                  {"quadBand", "", T::Bool, false,
                   "Remesh a band of faces around the seam into quads that follow it, welded back into the faces around it. "
                   "Falls back to splitting n-gons where the band's border can't be kept."},
                  {"bandRings", "", T::Int, 1, "Rings of faces past the seam the quad band takes in."},
                  {"timeout", "", T::Float, 5.0, "Seconds the quad band remesh may take before the n-gon split runs instead; 0 waits forever."},
                  {"filletWidth", "", T::Float, 0.0, "Width of a rounded strip along the seam, cm; 0 keeps it sharp."},
                  {"filletSegments", "", T::Int, 2, "Rows of faces across the fillet."},
                  {"hardAngle", "", T::Float, 30.0, "New edges sharper than this many degrees are marked hard."},
                  {"weightedNormals", "", T::Bool, true, "Add a weighted normals modifier, if the mesh has none, so the n-gons shade flat."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            auto objs = a.objects();
            if (objs.size() < 2)
                throw CommandError("polyBoolean: give a base and at least one cutter");
            auto op = booleanOpFromName(a.get<std::string>("operation", "difference"));
            if (!op)
                throw CommandError("polyBoolean: operation must be union, difference or intersection");
            Graph& g = Graph::current();
            g.evaluate();
            std::vector<Node*> nodes;
            std::vector<BooleanOperand> operands;
            for (const std::string& name : objs)
            {
                Node* n = g.find(name);
                if (!n || !n->type().isA("mesh") || !n->as<MeshPtr>(MeshNode::kOutMesh))
                    throw CommandError("polyBoolean: " + name + " is not a mesh");
                nodes.push_back(n);
                operands.push_back({n->as<MeshPtr>(MeshNode::kOutMesh).get(), n->as<glm::dmat4>(Transform::kWorldMatrix)});
            }

            BooleanOptions opts{.op = *op,
                                .cleanup = a.get<bool>("cleanup", true),
                                .snapDistance = a.get<double>("snapDistance", -1.0),
                                .splitNgons = a.get<bool>("splitNgons", true),
                                .quadBand = a.get<bool>("quadBand", false),
                                .bandRings = std::clamp(int(a.get<int64_t>("bandRings", 1)), 0, 8),
                                .bandTimeout = std::max(0.0, a.get<double>("timeout", 5.0)),
                                .filletWidth = std::max(0.0, a.get<double>("filletWidth", 0.0)),
                                .filletSegments = std::clamp(int(a.get<int64_t>("filletSegments", 2)), 1, 64),
                                .hardAngle = a.get<double>("hardAngle", 30.0)};
            if (a.get<bool>("live"))
                return liveBoolean(g, nodes, a);

            BooleanResult r;
            {
                progress::Scope scope("Boolean " + nodes[0]->name(), 1.0, progress::Cancellable);
                progress::runOnWorker([&] { r = meshBoolean(operands, glm::inverse(nodes[0]->as<glm::dmat4>(Transform::kWorldMatrix)), opts); });
            }
            if (!r.ok())
                throw CommandError("polyBoolean: " + (r.badOperand >= 0 ? nodes[r.badOperand]->path() + " " : std::string()) + r.error);

            UndoChunk chunk("Boolean");
            PlugRef base = modifiers::editPlug(g, nodes[0]->id());
            g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(r.mesh))));
            for (size_t i = 1; i < nodes.size(); ++i)
            {
                if (a.get<bool>("keepCutters"))
                    g.setValue({nodes[i]->id(), Transform::kVisibility, -1}, false);
                else
                    g.deleteNode(nodes[i]->id());
            }
            if (a.get<bool>("weightedNormals", true))
            {
                bool has = false;
                for (NodeId m : modifiers::stack(g, nodes[0]->id()))
                    has |= g.node(m) && g.node(m)->type().name == "weightedNormalsModifier";
                if (!has)
                    ops::execute("addModifier", {{"args", {nodes[0]->path()}}, {"kind", "weightedNormals"}});
            }
            g.select({nodes[0]->id()});
            return nodes[0]->path();
        },
    });
    ops::setAdjustable("polyBoolean");
}
} // namespace lila::scene
