#pragma once

#include "core/Export.h"

#include "scene/Transform.h"

#include <string>
#include <string_view>

namespace lila::scene
{
/**
 * A skeleton joint: a transform whose jointOrient sits between rotate and the parent, like Maya:
 *   world = parent * T * [inverseScale^-1] * jointOrient * R * S
 * The inverse-scale term only applies with segmentScaleCompensate on, which is off by default because Unreal ignores it.
 */
class LILA_SCENE_API Joint : public Transform
{
public:
    enum Attr
    {
        kJointOrient = Transform::kCount,
        kRadius,
        kSegmentScaleCompensate,
        kInverseScale,
        kSide,
        kJointType,
        kOtherType,
        kDrawStyle,
        kCenterInMesh,
        kCenterMeshes,
        kSolvedRotate,
        kIkBlend,
        kSelectAs,
        kDeform,
        kLimitType,
        kLimitAxis,
        kLimitMin,
        kLimitMax,
        kConeAngle,
        kStiffness,
        kPreferredAngle,
        kIkStretch,
        kCount
    };

    enum LimitType : int64_t
    {
        kNoLimit,
        kHinge,
        kCone,
    };

    enum Side : int64_t
    {
        kCenter,
        kLeft,
        kRight,
        kNoSide,
    };

    enum DrawStyle : int64_t
    {
        kBone,
        kBox,
        kHidden,
    };

    glm::dmat4 preRotation() const override;
    /** FK rotate, slerped toward solvedRotate by ikBlend; rotate itself is never written by IK. */
    glm::dmat4 rotation() const override;
    void compute(const EvalContext& ctx) override;

    static NodeType makeType(const NodeType& base);
};

namespace rig
{
LILA_SCENE_API bool isJoint(const Node& n);

/** "x", "-y", "z" ... to a unit vector. Throws std::invalid_argument. */
LILA_SCENE_API glm::dvec3 axisVector(std::string_view axis);

/**
 * Rotation whose `primary` local axis points along `aim` and whose `secondary` local axis leans toward `up`.
 * Axes are signed unit axis vectors such as (0, -1, 0); they must differ.
 */
LILA_SCENE_API glm::dmat3 aimFrame(const glm::dvec3& primary, const glm::dvec3& secondary, const glm::dvec3& aim, const glm::dvec3& up);

/**
 * Left/right name swap for mirroring: a suffix match first (arm_l), then a prefix (l_arm), then whole tokens
 * between underscores. Returns the name unchanged when nothing matches.
 */
LILA_SCENE_API std::string mirrorName(const std::string& name, const std::string& search, const std::string& replace);

/** True while editSkeleton is on: joints move without deforming their skins. */
LILA_SCENE_API bool editingSkeleton(Graph& g);

/** Sets a joint's jointOrient (degrees, XYZ), leaving its children where they are. */
LILA_SCENE_API void setJointOrient(Graph& g, NodeId joint, const glm::dvec3& degrees);
/** The jointOrient that turns a joint by a world rotation, rotate unchanged. */
LILA_SCENE_API glm::dvec3 turnedJointOrient(Graph& g, NodeId joint, const glm::dmat3& worldRotation);
} // namespace rig

/** Joint, skinCluster and constraint node types. Idempotent; needs the built-in transform type first. */
LILA_SCENE_API void registerRigTypes();
/** joint, orientJoint, mirrorJoints, skinCluster, skinPercent, parentConstraint ... Idempotent. */
LILA_SCENE_API void registerRigCommands();

LILA_SCENE_API void registerSkinTypes();
LILA_SCENE_API void registerSkinCommands();
LILA_SCENE_API void registerConstraintTypes();
LILA_SCENE_API void registerConstraintCommands();
LILA_SCENE_API void registerIkTypes();
LILA_SCENE_API void registerIkCommands();
LILA_SCENE_API void registerEditSkeletonTypes();
/** editSkeleton, moveSkinnedJoints. */
LILA_SCENE_API void registerEditSkeletonCommands();
} // namespace lila::scene
