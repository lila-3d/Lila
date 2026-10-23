#pragma once

#include "core/Export.h"

#include "scene/Transform.h"

#include <vector>

namespace lila::scene
{
class Graph;

/**
 * An IK handle: a transform whose world position is the chain's target. It solves the chain from startJoint to the
 * end effector with a registered ik::Solver and drives each solved joint's solvedRotate and ikBlend, so the joints'
 * keyable rotate stays FK and ikBlend crossfades between the two.
 *
 * The chain's rest layout (each joint's FK local matrix, jointOrient, rotateAxis, rotate order) is copied into fixed
 * slots when the handle is made, because reading the joints live would make the handle depend on what it drives. The
 * start joint's parent world matrix is connected, so the chain follows its parent. editSkeleton and
 * `ikHandle -refresh` copy the layout again.
 */
class LILA_SCENE_API IkHandle : public Transform
{
public:
    static constexpr int kMaxJoints = 16;

    enum Attr
    {
        kSolver = Transform::kCount,
        kPoleVector,
        kPoleAngle,
        kIkBlend,
        kJointCount,
        kChainParentMatrix,
        kEndEffector,
        kSlots, // kMaxJoints * {local, orient, axis, rotateOrder, restRotate, solvedRotate}
        kCount = kSlots + kMaxJoints * 6,
    };

    static int slotLocal(int i) { return kSlots + i * 6; }
    static int slotOrient(int i) { return kSlots + i * 6 + 1; }
    static int slotAxis(int i) { return kSlots + i * 6 + 2; }
    static int slotOrder(int i) { return kSlots + i * 6 + 3; }
    static int slotRest(int i) { return kSlots + i * 6 + 4; }
    static int slotSolved(int i) { return kSlots + i * 6 + 5; }

    void compute(const EvalContext& ctx) override;
};

namespace ik
{
/** The chain's joints, start first, read back from the handle's solvedRotate connections plus its end effector. */
LILA_SCENE_API std::vector<NodeId> chainOf(Graph& g, NodeId handle);
/** Every ikHandle whose chain includes joint. */
LILA_SCENE_API std::vector<NodeId> handlesOf(Graph& g, NodeId joint);
/** Copies the chain's current FK layout into the handle's slots (undoable). The joints' rotate is the rest pose. */
LILA_SCENE_API void refresh(Graph& g, NodeId handle);
} // namespace ik

/** ikHandle type and the built-in solvers (ik2BoneSolver, ikFabrikSolver). Idempotent. */
LILA_SCENE_API void registerIkTypes();
/** ikHandle. Idempotent. */
LILA_SCENE_API void registerIkCommands();
} // namespace lila::scene
