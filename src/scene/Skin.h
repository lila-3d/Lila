#pragma once

#include "core/Export.h"

#include "scene/Modifiers.h"

#include <string>
#include <utility>
#include <vector>

namespace lila::scene
{
class Graph;

/**
 * Sparse per-point skin weights, CSR: point p's entries are [offsets[p], offsets[p + 1]). Influence ids are the
 * skinInfluence nodes' index attrs, not chain positions, so rewiring the chain never scrambles weights.
 */
struct LILA_SCENE_API SkinWeights final : Data
{
    std::vector<uint32_t> offsets{0};
    std::vector<uint32_t> influences;
    std::vector<float> weights;

    uint32_t pointCount() const { return uint32_t(offsets.size() - 1); }
    const char* kind() const override { return "skinWeights"; }
    size_t bytes() const override { return (offsets.size() + influences.size() + weights.size()) * 4; }
    json toJson() const override;
    static DataPtr fromJson(const json& j);
};

/** Editable copy of SkinWeights: one small (influence, weight) list per point. */
struct LILA_SCENE_API WeightTable
{
    using Entry = std::pair<uint32_t, float>;
    std::vector<std::vector<Entry>> points;

    static WeightTable from(const SkinWeights* w, size_t pointCount);
    std::shared_ptr<const SkinWeights> pack() const;

    float get(size_t p, uint32_t influence) const;
    void set(size_t p, uint32_t influence, float w);
    /** Scales the unlocked entries so the point sums to 1; locked ones keep their value. */
    void normalize(size_t p, const std::vector<char>& locked);
    /** Drops entries below threshold and all but the heaviest maxInfluences, then normalizes. */
    void prune(size_t p, int maxInfluences, float threshold, const std::vector<char>& locked);
};

/** Payload one skinInfluence passes down the chain: everything upstream plus its own skinning matrix. */
struct InfluenceLink final : Data
{
    DataPtr prev;
    uint32_t index = 0;
    glm::dmat4 matrix{1.0}; // joint world * bind pre-matrix
    const char* kind() const override { return "influenceLink"; }
    json toJson() const override { return {{"kind", kind()}}; }
};

/**
 * One influence of a skinCluster. Graph plugs take a single connection each, so a skinCluster can't take N joint
 * matrices directly; instead each influence is a tiny node that takes one joint's worldMatrix and the previous link,
 * and the last link feeds skinCluster.influences:
 *
 *   joint_a.worldMatrix -> SkinInfluence1.matrix,  SkinInfluence1.link -> SkinInfluence2.previous
 *   joint_b.worldMatrix -> SkinInfluence2.matrix,  SkinInfluence2.link -> SkinCluster1.influences
 *
 * Moving any joint dirties exactly the links after it and the skin, undo and save/load are plain connections and
 * attrs, and the joint's name is always read from the connection, so renames are free.
 */
class LILA_SCENE_API SkinInfluenceNode : public Node
{
public:
    enum Attr
    {
        kMatrix,
        kBindPreMatrix,
        kIndex,
        kLockWeights,
        kPrevious,
        kLink,
    };
    void compute(const EvalContext& ctx) override;
};

/**
 * Linear blend (or dual quaternion) skinning as a modifier, so it sits in a mesh's stack like any other:
 *   meshSource -> SkinCluster1 -> Body.inMesh
 * Points are skinned in bind space: out = G^-1 * sum(w * jointWorld * bindPre) * G * p, with G the mesh's world
 * matrix at bind time (geomMatrix), so moving the mesh's own transform double-transforms it, as in Maya.
 */
class LILA_SCENE_API SkinClusterNode : public ModifierNode
{
public:
    enum Attr
    {
        kInMesh,
        kEnabled,
        kEnvelope,
        kSkinningMethod,
        kNormalizeWeights,
        kMaxInfluences,
        kGeomMatrix,
        kWeights,
        kInfluences,
        kOutMesh,
    };
    void compute(const EvalContext& ctx) override;
};

namespace skin
{
struct Influence
{
    NodeId link = kNoNode;
    NodeId joint = kNoNode; // kNoNode if the matrix plug lost its connection
    uint32_t index = 0;
    std::string name;
    bool locked = false;
};

/** The skinCluster in a mesh's stack, or kNoNode. */
LILA_SCENE_API NodeId find(Graph& g, NodeId mesh);
/** Mesh or skinCluster name to the skinCluster; throws CommandError. */
LILA_SCENE_API NodeId resolve(Graph& g, const std::string& name, const char* cmd);
/** The mesh node a skinCluster feeds, or kNoNode. */
LILA_SCENE_API NodeId meshOf(Graph& g, NodeId skin);

LILA_SCENE_API std::vector<Influence> influences(Graph& g, NodeId skin);
/** Influence id for a joint name, or -1. */
LILA_SCENE_API int64_t indexOf(const std::vector<Influence>& infs, const std::string& joint);
/** Lock flags indexed by influence id. */
LILA_SCENE_API std::vector<char> lockMask(const std::vector<Influence>& infs);

/** The undeformed points the weights refer to. */
LILA_SCENE_API MeshPtr inputMesh(Graph& g, NodeId skin);
LILA_SCENE_API WeightTable table(Graph& g, NodeId skin);
/** Undoable. */
LILA_SCENE_API void setWeights(Graph& g, NodeId skin, const WeightTable& t);

/** Adds a joint as an influence with zero weights; returns its id (existing id if it already is one). Undoable. */
LILA_SCENE_API uint32_t addInfluence(Graph& g, NodeId skin, NodeId joint);

enum class PaintMode
{
    Add,
    Replace,
    Smooth,
    Scale,
};

/**
 * One brush sample of `amount` (0..1, falloff times strength) on point p for one influence. Add/Replace/Scale use
 * `value`; invert turns Add into subtract. The other unlocked influences rescale so the point still sums to 1, and
 * the point is pruned to maxInfluences. Smooth is a no-op here: use smoothPoints.
 */
LILA_SCENE_API void paintPoint(WeightTable& t, size_t p, uint32_t influence, PaintMode mode, bool invert, float value, float amount,
                const std::vector<char>& locked, int maxInfluences);

/** Blends each (point, amount) toward its edge neighbours' average. Locked influences keep their values. */
LILA_SCENE_API void smoothPoints(const geo::Mesh& m, WeightTable& t, const std::vector<std::pair<uint32_t, float>>& points,
                  const std::vector<char>& locked, int maxInfluences);
} // namespace skin
} // namespace lila::scene
