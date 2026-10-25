#pragma once

#include "scene/AnimCurve.h"
#include "scene/Plug.h"
#include "scene/RotationCurve.h"
#include "scene/Transform.h"

#include <glm/glm.hpp>

#include <string>
#include <string_view>

namespace lila::geo
{
class Mesh;
}

namespace lila::scene
{
class Graph;
}

/** Scene building shared by the FBX and glTF importers. */
namespace lila::fbx
{
/** A valid Lila node name from a file's one; collision and socket prefixes survive as they are. */
std::string safeName(std::string_view raw, const char* fallback);

/** Hard where the corner normals of an edge's two faces disagree. */
void hardEdgesFromNormals(geo::Mesh& m);

struct Trs
{
    glm::dvec3 t{0.0}, r{0.0}, s{1.0};
    bool sheared = false;
};

/** TRS of a local matrix, XYZ Euler degrees. */
Trs decompose(const glm::dmat4& m);
/** TRS of a Lila-space local matrix with the pre-rotation (jointOrient) taken out, Euler angles near ref. */
Trs decomposeWith(const glm::dmat4& m, const glm::dmat3& pre, scene::RotateOrder order, const glm::dvec3& ref);

/** A new curve node of `type` ("animCurveTL", "animCurveTA", "animCurveTU") holding c, connected to plug. */
scene::NodeId connectCurve(scene::Graph& g, const scene::PlugRef& plug, const char* type, const scene::anim::Curve& c, const std::string& name);
/** A new animCurveRQ holding c, connected to the node's whole rotate plug. */
scene::NodeId connectRotationCurve(scene::Graph& g, scene::NodeId node, const scene::anim::RotationCurve& c);

/** Sets the scene's fps and playback range, in seconds. */
void setPlayback(double fps, double start, double end);
} // namespace lila::fbx
