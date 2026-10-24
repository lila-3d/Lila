#pragma once

#include "core/Export.h"

#include "scene/Transform.h"

#include <functional>

namespace lila::scene
{
/**
 * A polygon mesh in the hierarchy. `inMesh` holds the base mesh (or is fed by a generator such as polyCube);
 * `outMesh` is what gets drawn and exported once modifiers run.
 */
class LILA_SCENE_API MeshNode : public Transform
{
public:
    enum Attr
    {
        kInMesh = Transform::kCount,
        kOutMesh,
        kSymmetry,
        kSmoothPreview,
        kRetopoDrawReference,
        kRetopoDrawDots,
        kSymmetryPlane,
        kSymmetryOffset,
        kCount
    };

    void compute(const EvalContext& ctx) override;
    static NodeType makeType(const NodeType& base);
};

/** Procedural source: keyable parameters in, a mesh out. One class serves every primitive type. */
class LILA_SCENE_API GeneratorNode : public Node
{
public:
    using Build = std::function<geo::Mesh(const Node&)>;

    void compute(const EvalContext& ctx) override;

    Build build;
    int outAttr = -1;
};

/** The `symmetry` attribute across a set of nodes, for toggles: how many are meshes, how many have it on, and on which axis. */
struct SymmetryState
{
    int meshes = 0;
    int on = 0;
    int axis = -1; // 0-2 when every mesh that has it on agrees, else -1
};
LILA_SCENE_API SymmetryState symmetryState(const Graph& g, const std::vector<NodeId>& nodes);

/**
 * Where a mesh's symmetry plane for axis (0-2) lies along that axis, in object space: through the object's pivot, or
 * at symmetryOffset when symmetryPlane is custom (set from an edge selection).
 */
LILA_SCENE_API double symmetryOffset(const Node& mesh, int axis);

/**
 * Stands a mesh on a surface point: its up axis turned to normal (left alone when normal is zero) and its origin on
 * position, or, for a sphere primitive cut short by its fraction, its open base on position.
 */
LILA_SCENE_API void placePrimitive(Graph& g, NodeId mesh, const glm::dvec3& position, const glm::dvec3& normal);

LILA_SCENE_API void registerMeshTypes();
LILA_SCENE_API void registerMeshCommands();
} // namespace lila::scene
