#pragma once

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
class Joint : public Transform
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
        kCount
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
    void compute(const EvalContext& ctx) override;

    static NodeType makeType(const NodeType& base);
};

namespace rig
{
bool isJoint(const Node& n);

/** "x", "-y", "z" ... to a unit vector. Throws std::invalid_argument. */
glm::dvec3 axisVector(std::string_view axis);

/**
 * Rotation whose `primary` local axis points along `aim` and whose `secondary` local axis leans toward `up`.
 * Axes are signed unit axis vectors such as (0, -1, 0); they must differ.
 */
glm::dmat3 aimFrame(const glm::dvec3& primary, const glm::dvec3& secondary, const glm::dvec3& aim, const glm::dvec3& up);

/**
 * Left/right name swap for mirroring: a suffix match first (arm_l), then a prefix (l_arm), then whole tokens
 * between underscores. Returns the name unchanged when nothing matches.
 */
std::string mirrorName(const std::string& name, const std::string& search, const std::string& replace);
} // namespace rig

/** Joint, skinCluster and constraint node types. Idempotent; needs the built-in transform type first. */
void registerRigTypes();
/** joint, orientJoint, mirrorJoints, skinCluster, skinPercent, parentConstraint ... Idempotent. */
void registerRigCommands();

void registerSkinTypes();
void registerSkinCommands();
void registerConstraintTypes();
void registerConstraintCommands();
} // namespace lila::scene
