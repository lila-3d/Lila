#include "seq/RigidPieces.h"

#include "ops/Command.h"
#include "core/Profile.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Skin.h"
#include "scene/Transform.h"
#include "seq/MeshSequence.h"

#include <glm/gtc/quaternion.hpp>

#include <format>

namespace lila::seq
{
namespace
{
using scene::Graph;
using scene::NodeId;

std::string makeJoint(const std::string& name, const glm::dvec3& at, const std::string& parent)
{
    return ops::execute("joint", {{"position", json::array({at.x, at.y, at.z})}, {"name", name}, {"parent", parent}, {"skipSelect", true}})
        .get<std::string>();
}
} // namespace

PieceJointsResult bakePiecesToJoints(Graph& g, NodeId mesh, const SequenceOptions& sampling, const std::string& name, bool hideSource)
{
    LILA_ZONE();

    SequenceOptions so = sampling;
    so.worldSpace = true;
    so.applyModifiers = true;
    so.scale = 1.0;
    so.recenter = Recenter::None;
    std::vector<geo::Mesh> frames;
    std::vector<double> times;
    sample(g, {mesh}, so, [&](FrameMeshes& f) {
        geo::Mesh m;
        for (geo::Mesh& part : f.meshes)
        {
            part.compact();
            m.append(part);
        }
        m.compact();
        frames.push_back(std::move(m));
        times.push_back(f.frame);
    }, "Baking pieces to joints");
    if (frames.empty())
        throw std::invalid_argument("no frames to sample");
    for (const geo::Mesh& m : frames)
        if (m.numPoints() != frames.front().numPoints())
            throw std::invalid_argument("the point count changes between frames; pieces must stay whole");

    const RigidPieces pieces = findRigidPieces({frames.front()});
    std::vector<std::span<const glm::dvec3>> spans;
    for (const geo::Mesh& m : frames)
        spans.push_back(m.positions());
    const RigidFit fit = fitRigidPieces(spans, times, frames.front().positions(), pieces, glm::dmat3(1.0));

    PieceJointsResult r;
    r.residual = fit.residual;
    scene::Node* source = g.node(mesh);
    const std::string base = name.empty() ? source->name() + "_Rigid" : name;
    scene::Node* rigid = g.createNode(*scene::types::find("mesh"), base);
    g.setValue({rigid->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(frames.front())));
    r.mesh = rigid->path();

    r.root = makeJoint(base + "_root", glm::dvec3(0.0), "");
    r.joints.reserve(pieces.count);
    for (geo::Index k = 0; k < pieces.count; ++k)
        r.joints.push_back(makeJoint(std::format("{}_piece{:03}", base, k), fit.pivots[k], r.root));

    json bind = json::array();
    for (const std::string& j : r.joints)
        bind.push_back(j);
    bind.push_back(r.mesh);
    ops::execute("skinCluster", {{"args", bind}, {"maxInfluences", 1}, {"bindTo", "selected"}});
    const NodeId skin = scene::skin::find(g, rigid->id());
    if (skin == scene::kNoNode)
        throw std::runtime_error("binding the pieces' joints failed");
    std::vector<scene::skin::Influence> infs = scene::skin::influences(g, skin);
    std::vector<uint32_t> influenceOf(pieces.count, 0);
    for (geo::Index k = 0; k < pieces.count; ++k)
        influenceOf[k] = uint32_t(std::max<int64_t>(0, scene::skin::indexOf(infs, g.find(r.joints[k])->name())));
    scene::WeightTable t;
    t.points.resize(pieces.of.size());
    for (size_t p = 0; p < pieces.of.size(); ++p)
        t.points[p] = {{influenceOf[pieces.of[p]], 1.0f}};
    scene::skin::setWeights(g, skin, t);

    // Root at the origin, so a joint's local translate is where its pivot went and its rotate the piece's rotation.
    for (geo::Index k = 0; k < pieces.count; ++k)
    {
        const NodeId j = g.find(r.joints[k])->id();
        const auto order = scene::RotateOrder(g.node(j)->as<int64_t>(scene::Transform::kRotateOrder));
        for (size_t f = 0; f < frames.size(); ++f)
        {
            const double time = scene::anim::secondsOf(times[f]);
            const glm::dvec3& c = fit.centres[f * pieces.count + k];
            const glm::dvec4& q = fit.rotations[f * pieces.count + k];
            for (int axis = 0; axis < 3; ++axis)
                scene::anim::setKey(g, {j, scene::Transform::kTranslate, axis}, time, c[axis]);
            const glm::dmat3 rot = glm::mat3_cast(glm::dquat(q.w, q.x, q.y, q.z));
            scene::anim::setRotationKey(g, {j, scene::Transform::kRotate, -1}, time, scene::matrixToEuler(rot, order));
        }
    }
    if (hideSource)
        g.setValue({mesh, scene::Transform::kVisibility, -1}, false);
    return r;
}
} // namespace lila::seq
