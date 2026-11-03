#include "ops/Command.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "geo/Ops.h"
#include "geo/SmartJoin.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"

namespace lila::scene
{
namespace
{
geo::Mesh worldMeshOf(Node& n)
{
    geo::Mesh m = *n.as<MeshPtr>(MeshNode::kOutMesh);
    const glm::dmat4 w = n.as<glm::dmat4>(Transform::kWorldMatrix);
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            m.setPosition(p, glm::dvec3(w * glm::dvec4(m.position(p), 1.0)));
    // Mirrored transforms flip winding; keep faces pointing out.
    if (glm::determinant(glm::dmat3(w)) < 0.0)
    {
        std::vector<geo::Index> all;
        for (geo::Index f = 0; f < m.faceCapacity(); ++f)
            if (m.faceValid(f))
                all.push_back(f);
        geo::flipNormals(m, all);
    }
    return m;
}
} // namespace

void registerSmartJoinCommands()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "smartJoin",
        .doc = "Join a limb mesh to a body mesh with a quad bridge, changing only a local region of each. The smaller mesh is "
               "the limb unless flip=True. Returns {name, confidence (0-100), summary, reasons, counts...}.",
        .positionalDoc = "limb body",
        .flags = {{"segments", "", T::Int, 0, "Rows of quads in the bridge; 0 picks them from the gap.", {}, {}, 0.0},
                  {"tension", "", T::Float, 1.0, "Curvature of the bridge: how far it follows the limb and the body normal; 0 is straight."},
                  {"twist", "", T::Int, 0, "Rotates the loop correspondence by this many points."},
                  {"holeScale", "", T::Float, 1.0, "Scales the hole about its centre.", {}, {}, 0.01},
                  {"offsetU", "", T::Float, 0.0, "Slides the hole across the body (cm)."},
                  {"offsetV", "", T::Float, 0.0, "Slides the hole across the body (cm)."},
                  {"limbLoop", "", T::Int, -1, "Limb loop to bridge from: -1 auto, else 0 for the nearest loop facing the body, 1 the next...", {}, {}, -1.0},
                  {"deleteBuried", "", T::Bool, true, "Delete the limb beyond the loop; off keeps it as a separate shell."},
                  {"relax", "", T::Int, 0, "Smoothing passes over the bridge and hole ring.", {}, {}, 0.0, 100.0},
                  {"trimMargin", "", T::Float, 0.25, "How far outside the body a trim loop goes, as a fraction of the limb radius.", {}, {}, 0.0},
                  {"keepOriginals", "", T::Bool, false, "Hide the input meshes instead of deleting them."},
                  {"flip", "", T::Bool, false, "Swap which mesh is the limb and which the body."},
                  {"name", "", T::String, nullptr, "Name of the joined mesh."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            auto names = a.objects();
            if (names.size() != 2)
                throw CommandError("smartJoin: give or select exactly two meshes (limb and body)");
            Graph& g = Graph::current();
            g.evaluate();
            Node* nodes[2];
            for (int i = 0; i < 2; ++i)
            {
                nodes[i] = g.find(names[size_t(i)]);
                if (!nodes[i] || !nodes[i]->type().isA("mesh") || !nodes[i]->as<MeshPtr>(MeshNode::kOutMesh))
                    throw CommandError("smartJoin: " + names[size_t(i)] + " is not a mesh");
            }
            geo::Mesh meshes[2] = {worldMeshOf(*nodes[0]), worldMeshOf(*nodes[1])};
            // The smaller mesh is the limb.
            int limb = glm::length(meshes[0].bounds().size()) <= glm::length(meshes[1].bounds().size()) ? 0 : 1;
            if (a.get<bool>("flip"))
                limb = 1 - limb;
            const int body = 1 - limb;

            geo::SmartJoinOptions o;
            o.segments = std::max(0, a.get<int>("segments", 0));
            o.tension = a.get<double>("tension", 1.0);
            o.twist = a.get<int>("twist", 0);
            o.holeScale = a.get<double>("holeScale", 1.0);
            o.offsetU = a.get<double>("offsetU", 0.0);
            o.offsetV = a.get<double>("offsetV", 0.0);
            o.limbLoop = a.get<int>("limbLoop", -1);
            o.deleteBuried = a.get<bool>("deleteBuried", true);
            o.relax = std::clamp(a.get<int>("relax", 0), 0, 100);
            o.trimMargin = std::max(0.0, a.get<double>("trimMargin", 0.25));
            geo::SmartJoinResult r = geo::smartJoin(meshes[limb], meshes[body], o);
            if (!r.ok)
                throw CommandError("smartJoin: " + r.summary);

            Node* out = g.createNode(*types::find("mesh"), a.get<std::string>("name", nodes[body]->name()), kNoNode, true);
            g.setValue({out->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(std::move(r.mesh))));
            const std::string limbName = nodes[limb]->name(), bodyName = nodes[body]->name();
            for (Node* n : nodes)
            {
                if (a.get<bool>("keepOriginals"))
                    g.setValue({n->id(), Transform::kVisibility, -1}, false);
                else
                    g.deleteNode(n->id());
            }
            g.select({out->id()});
            return {{"name", out->name()},
                    {"limb", limbName},
                    {"body", bodyName},
                    {"confidence", r.confidence},
                    {"summary", r.summary},
                    {"reasons", r.reasons},
                    {"limbLoopSize", r.limbLoopSize},
                    {"holeSize", r.holeSize},
                    {"triangles", r.triangles},
                    {"ngons", r.ngons},
                    {"supportLoops", r.supportLoops},
                    {"segments", r.segments},
                    {"limbLoop", r.loopIndex},
                    {"loopCandidates", r.loopCandidates},
                    {"trimmed", r.trimmed},
                    {"flippedLimb", r.flippedLimb},
                    {"preservedPoints", r.preservedPoints},
                    {"originalPoints", r.originalPoints}};
        },
    });
    ops::setAdjustable("smartJoin");
}
} // namespace lila::scene
