#pragma once

#include "core/Export.h"

#include "scene/Node.h"

namespace lila::scene
{
/**
 * parentConstraint, pointConstraint, orientConstraint and aimConstraint. Each is a DG node fed by up to kMaxTargets
 * target worldMatrix connections (fixed slots: target0Matrix/Weight/Offset ... target7*, targetCount of them in use)
 * and drives the constrained node's translate and/or rotate through its constraintTranslate/constraintRotate
 * outputs. Slots keep weights keyable in the channel box and need no extra nodes; eight covers rig use.
 *
 * What the constraint needs to know about the constrained node (rotate order, jointOrient, pivots, rest values for
 * skipped axes) is copied onto it when it is made: reading those live would make the node depend on what it drives.
 * The constrained node's parent worldMatrix is connected, so constraints work under moving parents.
 */
class LILA_SCENE_API ConstraintNode : public Node
{
public:
    static constexpr int kMaxTargets = 8;

    enum class Kind
    {
        Parent,
        Point,
        Orient,
        Aim,
    };

    enum Attr
    {
        kTargetCount,
        kParentMatrix,
        kRotateOrder,
        kJointOrient,
        kRotatePivot,
        kRotatePivotTranslate,
        kRestTranslate,
        kRestRotate,
        kSkipTranslateX,
        kSkipTranslateY,
        kSkipTranslateZ,
        kSkipRotateX,
        kSkipRotateY,
        kSkipRotateZ,
        kOffset,
        kTargets, // kMaxTargets * {matrix, weight, offsetMatrix}
        kConstraintTranslate = kTargets + kMaxTargets * 3,
        kConstraintRotate,
        // aimConstraint only
        kAimVector,
        kUpVector,
        kWorldUpType,
        kWorldUpVector,
        kWorldUpMatrix,
    };

    enum WorldUp : int64_t
    {
        kUpScene,
        kUpObject,
        kUpObjectRotation,
        kUpVectorOnly,
        kUpNone,
    };

    static int targetMatrix(int slot) { return kTargets + slot * 3; }
    static int targetWeight(int slot) { return kTargets + slot * 3 + 1; }
    static int targetOffset(int slot) { return kTargets + slot * 3 + 2; }

    void compute(const EvalContext& ctx) override;

    Kind kind = Kind::Parent;
};
} // namespace lila::scene
