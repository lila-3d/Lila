#pragma once

#include "core/Export.h"

#include "gn/Geometry.h"
#include "scene/Plug.h"

#include <glm/glm.hpp>

#include <memory>
#include <vector>

namespace lila::scene
{
class Graph;
}

/**
 * The instances a geometry nodes modifier's result ended in, kept per modifier so the viewport can draw each referenced
 * mesh once with GPU instancing. The modifier's outMesh stays the realized mesh, so picking, snapping, export and
 * anything downstream see flat geometry as before; only drawing takes this path.
 */
namespace lila::gn
{
/** Every instance of one referenced mesh. */
struct InstancePart
{
    scene::MeshPtr mesh;
    /** Object-space placements, float for the renderer. */
    std::shared_ptr<const std::vector<glm::mat4>> transforms;
};

struct InstanceView
{
    /** The realized outMesh this view stands for; the view only applies while the mesh node still shows that mesh. */
    scene::MeshPtr output;
    /** Geometry beside the instances, realized; null when there is none. */
    scene::MeshPtr base;
    std::vector<InstancePart> parts;
    size_t instanceCount = 0;
};
using InstanceViewPtr = std::shared_ptr<const InstanceView>;

/**
 * The geometry's instances flattened (nested instances multiply through) and grouped by referenced mesh, with output as
 * the realized mesh they stand for. Null when the geometry has no instances of meshes.
 */
LILA_SCENE_API InstanceViewPtr makeInstanceView(const Geometry& g, scene::MeshPtr output);

LILA_SCENE_API void setInstanceView(scene::NodeId modifier, InstanceViewPtr view);
LILA_SCENE_API void clearInstanceView(scene::NodeId modifier);
LILA_SCENE_API InstanceViewPtr instanceView(scene::NodeId modifier);

/**
 * The instance view to draw a mesh node with: its inMesh comes straight from a geometry nodes modifier whose view still
 * matches the node's outMesh. Null otherwise, and the mesh draws as usual.
 */
LILA_SCENE_API InstanceViewPtr instanceViewFor(scene::Graph& g, scene::NodeId mesh);
} // namespace lila::gn
