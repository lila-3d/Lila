#pragma once

#include "core/Export.h"

#include "gn/Geometry.h"
#include "scene/Plug.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/**
 * What active Viewer nodes saw on their last evaluation, kept per modifier for the viewport to draw over the mesh.
 * The modifier's own output is untouched: a Viewer only previews.
 */
namespace lila::gn
{
struct ViewerData
{
    std::string group;
    std::string node;
    /** The previewed geometry, realized into one mesh (point clouds and curve points as loose points). */
    std::shared_ptr<const geo::Mesh> mesh;
    /** Whether a field was linked; without one the geometry is drawn plain. */
    bool hasValues = false;
    SocketType type = SocketType::Float;
    Domain domain = Domain::Point;
    /** One per element of the domain: floats in x, vectors in xyz, colours in xyzw. */
    std::vector<glm::dvec4> values;
    double minValue = 0.0;
    double maxValue = 0.0;
    uint64_t serial = 0;
};

using ViewerPtr = std::shared_ptr<const ViewerData>;

LILA_SCENE_API void setViewerData(scene::NodeId modifier, ViewerPtr data);
LILA_SCENE_API void clearViewerData(scene::NodeId modifier);
LILA_SCENE_API ViewerPtr viewerData(scene::NodeId modifier);
/** Every modifier with a preview, by node id. */
LILA_SCENE_API std::vector<std::pair<scene::NodeId, ViewerPtr>> viewerSnapshots();

/** Display colour (linear RGB) of a value: a heat ramp over min..max for numbers, the colour itself, or the vector's direction. */
LILA_SCENE_API glm::vec3 viewerColor(const ViewerData& d, const glm::dvec4& value);
/** A readable value, e.g. "0.42" or "(1, 0, 0)". */
LILA_SCENE_API std::string viewerText(const ViewerData& d, const glm::dvec4& value);
} // namespace lila::gn
