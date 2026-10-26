#include "fbx/ImportCommon.h"

#include "geo/Mesh.h"
#include "scene/Anim.h"
#include "scene/Graph.h"

#include <tbb/parallel_for.h>

#include <glm/gtx/matrix_decompose.hpp>

namespace lila::fbx
{
using geo::Index;

std::string safeName(std::string_view raw, const char* fallback)
{
    std::string s;
    for (char c : raw)
    {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        s.push_back(ok ? c : '_');
    }
    if (s.empty())
        return fallback;
    if (s[0] >= '0' && s[0] <= '9')
        s.insert(s.begin(), '_');
    return s;
}

void hardEdgesFromNormals(geo::Mesh& m)
{
    std::span<const glm::vec3> normals = m.cornerAttrs().get<glm::vec3>(geo::attr::kNormal);
    std::span<bool> hard = m.edgeAttrs().getOrAdd<bool>(geo::attr::kHard);
    tbb::parallel_for(Index(0), m.edgeCapacity(), [&](Index e) {
        if (!m.edgeValid(e) || !m.isManifoldEdge(e))
            return;
        Index c0 = m.edgeCorner(e), c1 = m.cornerRadial(c0);
        auto at = [&](Index c, Index p) { return m.cornerPoint(c) == p ? c : m.cornerNext(c); };
        for (int i = 0; i < 2 && !hard[e]; ++i)
        {
            Index p = m.edgePoint(e, i);
            hard[e] = glm::dot(normals[at(c0, p)], normals[at(c1, p)]) < 0.9999f;
        }
    });
}

Trs decompose(const glm::dmat4& m)
{
    Trs out;
    glm::dquat q;
    glm::dvec3 skew;
    glm::dvec4 persp;
    if (!glm::decompose(m, out.s, q, out.t, skew, persp))
        return out;
    out.sheared = glm::length(skew) > 1e-6;
    out.r = scene::matrixToEuler(glm::mat3_cast(q), scene::RotateOrder::XYZ);
    return out;
}

Trs decomposeWith(const glm::dmat4& m, const glm::dmat3& pre, scene::RotateOrder order, const glm::dvec3& ref)
{
    Trs out;
    glm::dquat q;
    glm::dvec3 skew;
    glm::dvec4 persp;
    if (!glm::decompose(m, out.s, q, out.t, skew, persp))
        return out;
    out.sheared = glm::length(skew) > 1e-6;
    out.r = scene::eulerNear(glm::transpose(pre) * glm::dmat3(glm::mat3_cast(q)), order, ref);
    return out;
}

scene::NodeId connectCurve(scene::Graph& g, const scene::PlugRef& plug, const char* type, const scene::anim::Curve& c, const std::string& name)
{
    using namespace scene;
    NodeId curve = g.createNode(*types::find(type), name, kNoNode, false)->id();
    anim::setCurve(g, curve, c);
    g.connect({curve, anim::CurveNode::kOutput, -1}, plug);
    return curve;
}

scene::NodeId connectRotationCurve(scene::Graph& g, scene::NodeId node, const scene::anim::RotationCurve& c)
{
    using namespace scene;
    NodeId curve = g.createNode(*types::find("animCurveRQ"), g.node(node)->name() + "_rotate", kNoNode, false)->id();
    anim::setRotationCurve(g, curve, c);
    g.connect({curve, anim::RotationCurveNode::kOutput, -1}, {node, Transform::kRotate, -1});
    return curve;
}

void setPlayback(double fps, double start, double end)
{
    scene::anim::Playback& pb = scene::anim::playback();
    pb.fps = fps;
    pb.minTime = pb.animStart = start;
    pb.maxTime = pb.animEnd = end;
    scene::anim::playbackChanged();
}
} // namespace lila::fbx
