#pragma once

#include "core/Export.h"

#include "scene/Modifiers.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace lila::scene
{
class Graph;

/** A per-point weight map on a deformer: missing entries (past the end) weigh 1. */
struct LILA_SCENE_API PointWeights final : Data
{
    std::vector<float> values;
    const char* kind() const override { return "pointWeights"; }
    size_t bytes() const override { return values.size() * sizeof(float); }
    json toJson() const override;
    static DataPtr fromJson(const json& j);
};

/** One morph target's sparse offsets from the blendShape's input, object space. */
struct LILA_SCENE_API BlendDeltas final : Data
{
    std::vector<uint32_t> indices;
    std::vector<glm::dvec3> deltas;
    const char* kind() const override { return "blendDeltas"; }
    size_t bytes() const override { return indices.size() * sizeof(uint32_t) + deltas.size() * sizeof(glm::dvec3); }
    json toJson() const override;
    static DataPtr fromJson(const json& j);
};

/**
 * A deformer: a modifier that only moves points. Every one has envelope and a per-point weight map, and blends each
 * point between its input and deformed position by envelope times weight. Normals and tangents the input carried
 * are rebuilt.
 */
class LILA_SCENE_API DeformerNode : public ModifierNode
{
public:
    enum Attr
    {
        kInMesh,
        kEnabled,
        kExportEnabled,
        kEnvelope,
        kWeights,
        kFirstParam,
    };

    void compute(const EvalContext& ctx) override;
    uint32_t traits() const override { return kModifier | kDeformer; }

    /** Moves points (input positions in, deformed out). Throws to report an error and pass the input through. */
    virtual void deform(const geo::Mesh& in, std::vector<glm::dvec3>& points) const = 0;

    /** The deformed mesh: deform, then the envelope and weight map blend, then normals. Throws like deform. */
    geo::Mesh run(const geo::Mesh& in) const;

    int attr(const char* name) const { return type().attrIndex(name); }
    double num(const char* name) const;
    int64_t integer(const char* name) const;
    glm::dmat4 matrix(const char* name) const;
};

namespace deformers
{
LILA_SCENE_API bool isDeformer(const Node& n);

/**
 * A deformer node type: the common attributes (inMesh, enabled, exportEnabled, envelope, weights) come first, then
 * params (Vec3 params get X/Y/Z children), then outMesh. Read params in deform() by name (num("angle")) or by index
 * from DeformerNode::kFirstParam.
 */
struct DeformerSpec
{
    std::string name; // the node type, "bulgeDeformer"
    std::string icon; // Tabler icon name
    std::string doc;
    std::vector<AttrDef> params;
    std::function<std::unique_ptr<DeformerNode>()> make;
};

/** Throws std::invalid_argument when the name is taken. */
LILA_SCENE_API const NodeType& registerDeformer(DeformerSpec spec);
/** For extensions: owned by ctx and removed when it unloads. Null, with a log message, if the name is taken. */
LILA_SCENE_API const NodeType* registerDeformer(ext::Context& ctx, DeformerSpec spec);

/**
 * Adds a deformer of a registered type to the top of each mesh's stack (under any skinCluster, like blendShape) and
 * returns them. Undoable when run inside a command.
 */
LILA_SCENE_API std::vector<NodeId> addDeformer(Graph& g, const std::string& type, const std::vector<NodeId>& meshes, const std::string& name = {});

/** The deformers in a mesh's stack, applied first first. */
LILA_SCENE_API std::vector<NodeId> list(Graph& g, NodeId mesh);

/** The blendShape in a mesh's stack, or kNoNode. */
LILA_SCENE_API NodeId blendShapeOf(Graph& g, NodeId mesh);

struct MorphTarget
{
    NodeId node = kNoNode;
    std::string name;   // what exporters call it
    double weight = 0.0; // target weight times the blendShape's envelope
    std::vector<uint32_t> indices;
    std::vector<glm::dvec3> deltas;
};

/** A blendShape's targets in order, live targets resolved against its current input. Evaluates the graph. */
LILA_SCENE_API std::vector<MorphTarget> morphTargets(Graph& g, NodeId blendShape);

/** Just the weights of morphTargets (target weight times envelope), cheap enough to sample every frame. */
LILA_SCENE_API std::vector<double> morphWeights(Graph& g, NodeId blendShape);

/** Puts a new blendShape in the mesh's stack, under any skinCluster. Undoable. */
LILA_SCENE_API NodeId addBlendShape(Graph& g, NodeId mesh, const std::string& name);

/** Appends a morph target given as offsets (importers). Returns the blendTarget node. Undoable. */
LILA_SCENE_API NodeId addBlendTarget(Graph& g, NodeId blendShape, const std::string& morphName, std::shared_ptr<const BlendDeltas> deltas, double weight);

/** The deformer's per-point weight map, expanded to n points. */
LILA_SCENE_API std::vector<float> pointWeights(Graph& g, NodeId deformer, size_t n);

LILA_SCENE_API void registerTypes();
LILA_SCENE_API void registerCommands();
} // namespace deformers
} // namespace lila::scene
