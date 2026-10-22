#pragma once

#include "scene/Modifiers.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace lila::scene
{
class Graph;

/** A per-point weight map on a deformer: missing entries (past the end) weigh 1. */
struct PointWeights final : Data
{
    std::vector<float> values;
    const char* kind() const override { return "pointWeights"; }
    size_t bytes() const override { return values.size() * sizeof(float); }
    json toJson() const override;
    static DataPtr fromJson(const json& j);
};

/** One morph target's sparse offsets from the blendShape's input, object space. */
struct BlendDeltas final : Data
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
class DeformerNode : public ModifierNode
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
bool isDeformer(const Node& n);

/** The deformers in a mesh's stack, applied first first. */
std::vector<NodeId> list(Graph& g, NodeId mesh);

/** The blendShape in a mesh's stack, or kNoNode. */
NodeId blendShapeOf(Graph& g, NodeId mesh);

struct MorphTarget
{
    NodeId node = kNoNode;
    std::string name;   // what exporters call it
    double weight = 0.0; // target weight times the blendShape's envelope
    std::vector<uint32_t> indices;
    std::vector<glm::dvec3> deltas;
};

/** A blendShape's targets in order, live targets resolved against its current input. Evaluates the graph. */
std::vector<MorphTarget> morphTargets(Graph& g, NodeId blendShape);

/** Just the weights of morphTargets (target weight times envelope), cheap enough to sample every frame. */
std::vector<double> morphWeights(Graph& g, NodeId blendShape);

/** Puts a new blendShape in the mesh's stack, under any skinCluster. Undoable. */
NodeId addBlendShape(Graph& g, NodeId mesh, const std::string& name);

/** Appends a morph target given as offsets (importers). Returns the blendTarget node. Undoable. */
NodeId addBlendTarget(Graph& g, NodeId blendShape, const std::string& morphName, std::shared_ptr<const BlendDeltas> deltas, double weight);

/** The deformer's per-point weight map, expanded to n points. */
std::vector<float> pointWeights(Graph& g, NodeId deformer, size_t n);

void registerTypes();
void registerCommands();
} // namespace deformers
} // namespace lila::scene
