#pragma once

#include "core/Export.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>
#include <string_view>
#include <vector>

/**
 * Skeletons as plain data, so retargeting runs and tests without a scene. A joint's local matrix is
 * T(translate) * pre * R * after, where pre carries the joint orient and after the rotate axis and scale, as
 * Transform::splitLocal splits it. R and translate are the pose; the rest is fixed.
 */
namespace lila::retarget
{
struct LILA_SCENE_API Skeleton
{
    /** Bare joint names (no namespace or path), unique within the skeleton. */
    std::vector<std::string> names;
    /** Index of each joint's parent, -1 for a top joint. Parents come before their children. */
    std::vector<int> parent;
    std::vector<glm::dmat4> pre, after;
    /** World matrix above each top joint; only read where parent is -1. */
    std::vector<glm::dmat4> parentWorld;

    size_t size() const { return names.size(); }
    int find(std::string_view name) const;
    std::vector<int> children(int joint) const;
    /** Joints from start down to end, both included; empty when end is not start or below it. */
    std::vector<int> path(int start, int end) const;
    /** Whether joint is ancestor or below it (or is it). */
    bool isBelow(int joint, int ancestor) const;
    /** Appends a joint and returns its index. */
    int add(const std::string& name, int parentJoint, const glm::dmat4& pre = glm::dmat4(1.0), const glm::dmat4& after = glm::dmat4(1.0));
};

/** The animated part of a skeleton: each joint's R and translate. */
struct Pose
{
    std::vector<glm::dquat> rotation;
    std::vector<glm::dvec3> translate;
};

/** A pose with every joint at identity rotation and zero translate. */
LILA_SCENE_API Pose identityPose(const Skeleton& s);
LILA_SCENE_API glm::dmat4 localOf(const Skeleton& s, const Pose& p, int joint);
LILA_SCENE_API std::vector<glm::dmat4> worldOf(const Skeleton& s, const Pose& p);
/** World matrix above joint for the given worlds of the other joints. */
LILA_SCENE_API glm::dmat4 parentWorldOf(const Skeleton& s, const std::vector<glm::dmat4>& world, int joint);

/** The R that gives the joint the world rotation `want` under parentWorld. */
LILA_SCENE_API glm::dquat localRotationFor(const Skeleton& s, int joint, const glm::dmat4& parentWorld, const glm::dquat& want);
/** The translate that puts the joint's origin at worldPos under parentWorld. */
LILA_SCENE_API glm::dvec3 translateFor(const glm::dmat4& parentWorld, const glm::dvec3& worldPos);

/** Rotation of a matrix with scale and shear removed, as a unit quaternion. */
LILA_SCENE_API glm::dquat rotationOf(const glm::dmat4& m);
inline glm::dvec3 positionOf(const glm::dmat4& m) { return glm::dvec3(m[3]); }
/** The name without any namespace (a:b:hand_l is hand_l) or path. */
LILA_SCENE_API std::string bareName(std::string_view name);
} // namespace lila::retarget
