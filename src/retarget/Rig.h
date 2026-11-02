#pragma once

#include "core/Export.h"
#include "retarget/Skeleton.h"

#include <nlohmann/json.hpp>

#include <map>
#include <string>
#include <vector>

/**
 * A retarget rig: what each part of a skeleton is (UE's IK Rig). The pelvis is the retarget root; chains are runs of
 * joints with a role, a side and an index (spider leg 3 on the left), and the joint an IK goal sits on. The retarget
 * pose is the pose both rigs are compared in.
 */
namespace lila::retarget
{
using json = nlohmann::json;

enum class Role : uint8_t
{
    Root, // the joint at the ground above the pelvis (UE's root)
    Spine,
    Neck,
    Head,
    Jaw,
    Clavicle,
    Arm,    // upper arm to hand
    Leg,    // thigh to foot; on many-legged bodies, index counts front to back
    Toe,    // the joints past the foot
    Finger, // index: 0 thumb, 1 index, 2 middle, 3 ring, 4 pinky
    Tail,
    Wing,
    Antenna,
    Other,
};
enum class Side : uint8_t
{
    Center,
    Left,
    Right,
};

LILA_SCENE_API const char* roleName(Role r);
/** Throws std::invalid_argument naming the bad role. */
LILA_SCENE_API Role roleFromName(std::string_view name);
LILA_SCENE_API const char* sideName(Side s);
LILA_SCENE_API Side sideFromName(std::string_view name);

struct Chain
{
    std::string name; // LeftArm, Spine, LeftLeg2 ...
    Role role = Role::Other;
    Side side = Side::Center;
    int index = 0;
    std::string start, end; // joint names, end at or below start
    /** Joint carrying the chain's IK goal (the hand or foot), empty for none. */
    std::string goal;
};

struct LILA_SCENE_API Rig
{
    std::string pelvis;
    std::vector<Chain> chains;
    /** Each joint's R and translate in the retarget pose, by joint name. Joints missing keep the skeleton's own. */
    std::map<std::string, glm::dquat> poseRotation;
    std::map<std::string, glm::dvec3> poseTranslate;
    /** The floor, as a height along up. */
    double ground = 0.0;
    glm::dvec3 up{0.0, 1.0, 0.0};
    /** Where the character faces in the retarget pose. */
    glm::dvec3 forward{0.0, 0.0, 1.0};
    /** What characterising found doubtful, one line each. */
    std::vector<std::string> notes;

    const Chain* chain(std::string_view name) const;
    Chain* chain(std::string_view name);
    /** The chain with this role, side and index, or null. */
    const Chain* chainFor(Role role, Side side, int index) const;
    glm::dvec3 left() const { return glm::normalize(glm::cross(up, forward)); }

    /** The retarget pose over `current`: joints the rig names take its values. */
    Pose retargetPose(const Skeleton& s, const Pose& current) const;
    /** Stores every joint of pose as the retarget pose. */
    void setRetargetPose(const Skeleton& s, const Pose& pose);

    json toJson() const;
    /** Throws std::invalid_argument naming the first problem. */
    static Rig fromJson(const json& j);
};

/** The chain's joints, start first; empty when the names are missing or end is not below start. */
LILA_SCENE_API std::vector<int> chainJoints(const Skeleton& s, const Chain& c);
/** Sum of the chain's bone lengths for the given worlds. */
LILA_SCENE_API double chainLength(const Skeleton& s, const std::vector<int>& joints, const std::vector<glm::dmat4>& world);
/** Name a chain gets from its role, side and index (LeftArm, Spine, RightLeg3, LeftIndex). */
LILA_SCENE_API std::string chainName(Role role, Side side, int index);

struct CharacterizeOptions
{
    glm::dvec3 up{0.0, 1.0, 0.0};
    /** Used when nothing on the body says which way it faces. */
    glm::dvec3 forward{0.0, 0.0, 1.0};
    /** Joints never put in a chain (non-deform helpers); same length as the skeleton, or empty. */
    std::vector<bool> skip;
    double ground = 0.0;
};

/**
 * Finds the pelvis and the chains of a skeleton posed as `pose`: by joint names where they say (UE, Mixamo, Maya,
 * Biped, BVH and similar conventions), else by shape (branches that reach the ground are legs, the biggest other
 * branch is the spine, the highest branch off the chest the neck and head, the rest arms). Works on any number of
 * legs. The pose becomes the retarget pose.
 */
LILA_SCENE_API Rig characterize(const Skeleton& s, const Pose& pose, const CharacterizeOptions& opts = {});
} // namespace lila::retarget
