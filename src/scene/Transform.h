#pragma once

#include "scene/Node.h"

namespace lila::scene
{
class Graph;

enum class RotateOrder : int64_t
{
    XYZ,
    YZX,
    ZXY,
    XZY,
    YXZ,
    ZYX,
};

/** Rotation matrix from Euler degrees; XYZ means X is applied first. */
glm::dmat4 eulerToMatrix(const glm::dvec3& degrees, RotateOrder order);
glm::dvec3 matrixToEuler(const glm::dmat3& rotation, RotateOrder order);

/** Pref general /display/inheritVisibility (default on): hiding a node hides everything under it, as in Maya. */
inline constexpr const char* kInheritVisibilityPref = "/display/inheritVisibility";
bool inheritVisibility();

/**
 * Whether the viewport shows a node: its own visibility, and every DAG ancestor's too when inherit is on. Picking,
 * framing, snapping and counts all go by this rather than the node's own flag.
 */
bool effectivelyVisible(const Graph& g, NodeId id, bool inherit);
inline bool effectivelyVisible(const Graph& g, NodeId id) { return effectivelyVisible(g, id, inheritVisibility()); }

class Transform : public Node
{
public:
    enum Attr
    {
        kTranslate,
        kRotate,
        kScale,
        kRotateOrder,
        kRotatePivot,
        kScalePivot,
        kRotatePivotTranslate,
        kScalePivotTranslate,
        kVisibility,
        kParentMatrix,
        kMatrix,
        kWorldMatrix,
        kPivotOrient,
        kDisplayLayer,
        kCount
    };

    void compute(const EvalContext& ctx) override;

    /** Rotation applied between rotate and the parent: a joint's jointOrient, identity otherwise. */
    virtual glm::dmat4 preRotation() const { return glm::dmat4(1.0); }

    /**
     * Maya's pivot model, column vectors:
     * T * Rpt * Rp * Pre * R * Rp^-1 * Spt * Sp * S * Sp^-1  (Pre = preRotation())
     * The pivot-translate terms let a pivot move without moving the object.
     */
    static glm::dmat4 localMatrix(const Node& n);

    /** Sets translate/rotate/scale (undoably) so the local matrix becomes m, keeping pivots. Shear is dropped. */
    static void setLocalMatrix(Graph& g, NodeId id, const glm::dmat4& m);

    static NodeType makeType();
};

class Locator : public Transform
{
public:
    enum Attr
    {
        kLocalScale = Transform::kCount,
    };
    static NodeType makeType(const NodeType& base);
};
} // namespace lila::scene
