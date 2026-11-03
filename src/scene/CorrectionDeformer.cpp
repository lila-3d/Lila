#include "core/Log.h"
#include "core/Profile.h"
#include "deform/Deform.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "scene/DeformerUtil.h"
#include "scene/Deformers.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/RigUtil.h"

#include <tbb/parallel_for.h>

/**
 * The correction deformer (Cinema 4D's Correction): sculpted point offsets stored in each point's tangent frame of the
 * incoming shape, so the fix rides on whatever deforms below it.
 */
namespace lila::scene
{
using namespace rigutil;
using namespace deformutil;

namespace
{
/** Sparse offsets, each in its point's frame (x toward the first neighbour, z the normal). */
struct CorrectionDeltas final : Data
{
    std::vector<uint32_t> indices;
    std::vector<glm::dvec3> local;
    const char* kind() const override { return "correctionDeltas"; }
    size_t bytes() const override { return indices.size() * (4 + sizeof(glm::dvec3)); }
    json toJson() const override
    {
        std::vector<double> f;
        for (const glm::dvec3& d : local)
            f.insert(f.end(), {d.x, d.y, d.z});
        return {{"kind", kind()}, {"indices", indices}, {"local", f}};
    }
    static DataPtr fromJson(const json& j)
    {
        auto out = std::make_shared<CorrectionDeltas>();
        std::vector<uint32_t> idx = j.value("indices", std::vector<uint32_t>{});
        std::vector<double> f = j.value("local", std::vector<double>{});
        for (size_t i = 0; i < idx.size() && i * 3 + 2 < f.size(); ++i)
        {
            out->indices.push_back(idx[i]);
            out->local.push_back({f[i * 3], f[i * 3 + 1], f[i * 3 + 2]});
        }
        return out;
    }
};

/** Point frames of a shape, with the adjacency kept while the topology stays. */
class Frames
{
public:
    std::vector<glm::dmat3> of(const geo::Mesh& topology, std::span<const glm::dvec3> points) const
    {
        if (mEdges != topology.numEdges() || mPoints != topology.pointCapacity())
        {
            mAdj = deform::pointAdjacency(topology);
            mEdges = topology.numEdges();
            mPoints = topology.pointCapacity();
        }
        return deform::pointFrames(topology, points, mAdj);
    }

private:
    mutable deform::Adjacency mAdj;
    mutable geo::Index mEdges = geo::kInvalid, mPoints = 0;
};

class CorrectionNode : public DeformerNode
{
public:
    void deform(const geo::Mesh& in, std::vector<glm::dvec3>& points) const override
    {
        LILA_ZONE();

        if (const MeshPtr& edit = as<MeshPtr>(attr("editMesh")); edit && edit->pointCapacity() == points.size())
        {
            std::copy(edit->positions().begin(), edit->positions().end(), points.begin());
            return;
        }
        const auto* d = dynamic_cast<const CorrectionDeltas*>(as<DataPtr>(attr("deltas")).get());
        if (!d || d->indices.empty())
            return;
        std::vector<glm::dmat3> frames = mFrames.of(in, points);
        const std::vector<glm::dvec3> before = points;
        tbb::parallel_for(size_t(0), d->indices.size(), [&](size_t k) {
            uint32_t p = d->indices[k];
            if (p < points.size())
                points[p] = before[p] + frames[p] * d->local[k];
        });
    }

    Frames mFrames;
};

AttrDef attrDef(std::string name, PlugType type, PlugValue def, uint32_t flags, std::string doc)
{
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = flags, .doc = std::move(doc)};
}

DeformerNode& correctionNamed(const std::string& name, const char* cmd)
{
    Node& n = nodeNamed(name, cmd);
    if (n.type().name != "correction")
        throw CommandError(std::string(cmd) + ": " + name + " is not a correction deformer");
    return static_cast<DeformerNode&>(n);
}

MeshPtr incoming(Graph& graph, NodeId d)
{
    graph.evaluate();
    MeshPtr in = std::get<MeshPtr>(graph.value({d, DeformerNode::kInMesh, -1}));
    if (!in)
        throw CommandError(graph.node(d)->name() + ": the mesh has no geometry");
    return in;
}

/** Offsets that take the incoming shape to target (both object space), in the incoming shape's frames. */
std::shared_ptr<CorrectionDeltas> deltasTo(const geo::Mesh& in, std::span<const glm::dvec3> target)
{
    Frames f;
    std::vector<glm::dmat3> frames = f.of(in, in.positions());
    auto out = std::make_shared<CorrectionDeltas>();
    for (geo::Index p = 0; p < in.pointCapacity() && p < target.size(); ++p)
    {
        glm::dvec3 d = target[p] - in.position(p);
        if (in.pointValid(p) && glm::length(d) > 1e-9)
        {
            out->indices.push_back(p);
            out->local.push_back(glm::transpose(frames[p]) * d);
        }
    }
    return out;
}
} // namespace

namespace correction
{
void registerType()
{
    if (types::find("correction"))
        return;
    data::registerKind("correctionDeltas", &CorrectionDeltas::fromJson);
    deformers::registerDeformer({"correction", "brush",
                                 "Sculpted fixes that ride on the deformation below: point offsets kept in the tangent space of the "
                                 "incoming shape, so they follow it. Put it anywhere in the stack and sculpt it with editCorrection.",
                                 {attrDef("deltas", PlugType::Data, DataPtr(), AttrInput | AttrStorable | AttrHidden,
                                       "The sculpted offsets, each in its point's tangent frame."),
                                  attrDef("editMesh", PlugType::Mesh, MeshPtr(), AttrInput | AttrHidden | AttrResetOnDisconnect,
                                       "The mesh being sculpted while editing; the deformer shows it as is.")},
                                 [] { return std::make_unique<CorrectionNode>(); }});
}

void registerCommands()
{
    using T = ArgType;

    ops::registerCommand({
        .name = "correction",
        .doc = "Add a correction deformer to the top of each mesh's stack (move it anywhere with the modifier stack), then "
               "sculpt it with editCorrection. Returns the deformers.",
        .positionalDoc = "meshes",
        .flags = {{"belowSkin", "", T::Bool, false, "Put it just under the skinCluster instead of on top."},
                  {"name", "", T::String, nullptr, "Name for the deformer."}},
        .run = [](const Args& a) -> json {
            Graph& graph = Graph::current();
            std::vector<NodeId> meshes;
            for (const std::string& o : a.objects())
            {
                Node& n = nodeNamed(o, "correction");
                if (!n.type().isA("mesh") || n.type().isA("lattice"))
                    throw CommandError("correction: " + o + " is not a mesh");
                meshes.push_back(n.id());
            }
            if (meshes.empty())
                throw CommandError("correction: give or select a mesh");
            json out = json::array();
            for (NodeId d : guarded([&] {
                     return deformers::addDeformer(graph, "correction", meshes, a.has("name") ? a.get<std::string>("name") : "Correction",
                                                   a.get<bool>("belowSkin"));
                 }))
                out.push_back(graph.node(d)->name());
            return out;
        },
    });

    ops::registerCommand({
        .name = "editCorrection",
        .doc = "Sculpt a correction: on copies the mesh as the correction outputs it into <deformer>_sculpt, in place, hides the "
               "mesh and selects the copy (the correction shows it live). off stores the sculpt as offsets in the incoming "
               "shape's tangent space, deletes the copy and shows the mesh. Returns the sculpt mesh while on.",
        .positionalDoc = "correction",
        .flags = {{"on", "", T::Bool, nullptr, "Default: toggle."}},
        .run = [](const Args& a) -> json {
            Graph& graph = Graph::current();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("editCorrection: give a correction deformer");
            DeformerNode& d = correctionNamed(objs.front(), "editCorrection");
            const int editAttr = d.attr("editMesh");
            NodeId mesh = modifiers::meshOf(graph, d.id());
            if (mesh == kNoNode)
                throw CommandError("editCorrection: " + d.name() + " deforms no mesh");
            auto editing = graph.source({d.id(), editAttr, -1});
            const bool on = a.has("on") ? a.get<bool>("on") : !editing;
            if (on)
            {
                if (editing)
                    return graph.node(editing->node)->name();
                graph.evaluate();
                MeshPtr shape = std::get<MeshPtr>(graph.value({d.id(), d.outAttr, -1}));
                if (!shape)
                    throw CommandError("editCorrection: " + d.name() + " has no geometry");
                auto copy = std::make_shared<geo::Mesh>(*shape);
                copy->pointAttrs().remove(modifiers::kBypassLayer);
                Node* edit = guarded([&] { return graph.createNode(*types::find("mesh"), d.name() + "_sculpt", kNoNode, true); });
                graph.setValue({edit->id(), MeshNode::kInMesh, -1}, MeshPtr(std::move(copy)));
                Transform::setLocalMatrix(graph, edit->id(), worldOf(*graph.node(mesh)));
                graph.connect({edit->id(), MeshNode::kOutMesh, -1}, {d.id(), editAttr, -1});
                setVisible(graph, mesh, false);
                graph.select({edit->id()});
                return edit->name();
            }
            if (!editing)
                return nullptr;
            const NodeId editNode = editing->node;
            graph.evaluate();
            MeshPtr edited = graph.node(editNode)->as<MeshPtr>(MeshNode::kOutMesh);
            graph.disconnect({d.id(), editAttr, -1});
            MeshPtr in = incoming(graph, d.id());
            if (edited && edited->pointCapacity() == in->pointCapacity())
                graph.setValue({d.id(), d.attr("deltas"), -1}, DataPtr(deltasTo(*in, edited->positions())));
            else
                log::warn("editCorrection: the sculpt's points no longer match the mesh; the correction is unchanged");
            graph.deleteNode(editNode);
            setVisible(graph, mesh, true);
            graph.select({mesh});
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "setCorrection",
        .doc = "Set a correction from a sculpted mesh with the same points: the offsets that take the correction's incoming "
               "shape to the target's (matched in world space), stored in tangent space.",
        .positionalDoc = "correction, target",
        .run = [](const Args& a) -> json {
            Graph& graph = Graph::current();
            auto objs = a.objects();
            if (objs.size() < 2)
                throw CommandError("setCorrection: give a correction and a target mesh");
            DeformerNode& d = correctionNamed(objs[0], "setCorrection");
            Node& target = nodeNamed(objs[1], "setCorrection");
            NodeId mesh = modifiers::meshOf(graph, d.id());
            MeshPtr in = incoming(graph, d.id());
            MeshPtr t = target.type().isA("mesh") ? target.as<MeshPtr>(MeshNode::kOutMesh) : nullptr;
            if (!t || t->pointCapacity() != in->pointCapacity() || mesh == kNoNode)
                throw CommandError("setCorrection: " + target.name() + " must be a mesh with the same points");
            const glm::dmat4 toMesh = glm::inverse(worldOf(*graph.node(mesh))) * worldOf(target);
            std::vector<glm::dvec3> pts(t->pointCapacity());
            for (geo::Index p = 0; p < t->pointCapacity(); ++p)
                pts[p] = glm::dvec3(toMesh * glm::dvec4(t->position(p), 1.0));
            auto deltas = deltasTo(*in, pts);
            size_t moved = deltas->indices.size();
            graph.setValue({d.id(), d.attr("deltas"), -1}, DataPtr(std::move(deltas)));
            return moved;
        },
    });

    ops::registerCommand({
        .name = "resetCorrection",
        .doc = "Clear a correction's offsets: all of them, or just points=.",
        .positionalDoc = "correction",
        .flags = {{"points", "", T::List, nullptr, "Point ids to clear."}},
        .run = [](const Args& a) -> json {
            Graph& graph = Graph::current();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("resetCorrection: give a correction deformer");
            DeformerNode& d = correctionNamed(objs.front(), "resetCorrection");
            PlugRef plug{d.id(), d.attr("deltas"), -1};
            if (!a.has("points"))
            {
                graph.setValue(plug, DataPtr());
                return nullptr;
            }
            const auto* cur = dynamic_cast<const CorrectionDeltas*>(std::get<DataPtr>(graph.storedValue(plug)).get());
            if (!cur)
                return nullptr;
            std::vector<uint32_t> clear = a.flags["points"].get<std::vector<uint32_t>>();
            std::ranges::sort(clear);
            auto next = std::make_shared<CorrectionDeltas>();
            for (size_t k = 0; k < cur->indices.size(); ++k)
                if (!std::ranges::binary_search(clear, cur->indices[k]))
                {
                    next->indices.push_back(cur->indices[k]);
                    next->local.push_back(cur->local[k]);
                }
            graph.setValue(plug, DataPtr(std::move(next)));
            return nullptr;
        },
    });
}
} // namespace correction
} // namespace lila::scene
