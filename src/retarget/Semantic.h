#pragma once

#include "core/Export.h"
#include "retarget/Retarget.h"

#include <array>
#include <vector>

/**
 * Retargeting between unrelated bodies, after Hecker et al. 2008 (Spore): a clip is stored independent of the body
 * and re-solved on each new body with its own IK.
 *
 * Body-independent means: the body moves in its own frame (origin under the pelvis, facing forward) in leg lengths;
 * each limb's goal is relative to its root in limb reaches; spines, necks, heads and tails keep the shape of their
 * bend. A body with a different number of legs takes the source legs' motion shifted in time so its legs keep a gait
 * (a biped's left and right become a dog's diagonal pairs, a beetle's tripods).
 */
namespace lila::retarget::semantic
{
struct LimbTrack
{
    Role role = Role::Leg;
    Side side = Side::Center;
    int index = 0;
    /**
     * The goal's offset from the limb's root in the body frame, less that offset in the retarget pose: legs in leg
     * lengths (so steps keep pace with the body), arms in their own reach. Bodies keep their own stance.
     */
    std::vector<glm::dvec3> goal;
    /** The goal's change of rotation from the retarget pose, in the body frame. */
    std::vector<glm::dquat> rotation;
    /** Planted on the ground. */
    std::vector<bool> contact;
};

struct ShapeTrack
{
    Role role = Role::Spine;
    Side side = Side::Center;
    int index = 0;
    /** The chain's change of rotation at its start, middle and end, in the body frame. */
    std::vector<std::array<glm::dquat, 3>> bend;
};

struct LILA_SCENE_API Clip
{
    double fps = 30.0;
    int frames = 0;
    /** The body frame's ground velocity in its own axes (forward, left, up), leg lengths per second. */
    std::vector<glm::dvec3> velocity;
    /** The body frame's turn since the first frame. */
    std::vector<glm::dquat> turn;
    /** Pelvis height over its retarget-pose height, leg lengths. */
    std::vector<double> height;
    /** The pelvis's change of rotation from the retarget pose, in the body frame. */
    std::vector<glm::dquat> tilt;
    std::vector<LimbTrack> limbs;
    std::vector<ShapeTrack> shapes;

    json toJson() const;
    /** Throws std::invalid_argument. */
    static Clip fromJson(const json& j);
};

/** Encodes a source clip. The rig's legs and arms become limb tracks; spine, neck, head and tail chains shape tracks. */
LILA_SCENE_API Clip encode(const Skeleton& s, const Rig& rig, const Pose& current, const Frames& frames, const PlantSettings& plant = {});

enum class Gait : uint8_t
{
    Auto,     // from the leg count: walk for two and four, tripod for six, tetrapod for eight, wave beyond
    Walk,     // lateral sequence: each foot a quarter cycle after the one before (left hind, left fore, right hind, right fore)
    Trot,     // diagonal pairs together
    Pace,     // same-side pairs together
    Canter,   // three beats
    Gallop,   // four beats, rotary
    Tripod,   // six legs: front and back of one side with the middle of the other
    Tetrapod, // eight legs: alternating groups of four
    Wave,     // a wave of steps from the back to the front, each side half a cycle apart
};
LILA_SCENE_API const char* gaitName(Gait g);
/** Throws std::invalid_argument. */
LILA_SCENE_API Gait gaitFromName(std::string_view name);

/** Where in the cycle (0..1) each leg's stance starts under a gait; legs as (side, index front to back). */
LILA_SCENE_API std::vector<double> gaitPhases(Gait gait, const std::vector<std::pair<Side, int>>& legs);

/** The clip's cycle length (seconds) and each limb's stance-start phase, from the contacts. Period 0: no steps. */
struct Cycle
{
    double period = 0.0;
    std::vector<double> phase; // per Clip::limbs entry; -1 for a limb that never touches down
};
LILA_SCENE_API Cycle measureCycle(const Clip& c);

struct ApplyOptions
{
    Gait gait = Gait::Auto;
    /** Hold planted feet still (from the clip's contacts). */
    bool plant = true;
};

/**
 * Solves the clip on a body: the body frame moves as the clip says scaled to the target's legs, shape chains bend,
 * each leg takes a source leg's track shifted to its gait phase, arms take the source's arms, and IK puts every goal.
 * Chains with no counterpart keep their retarget pose.
 */
LILA_SCENE_API Result apply(const Clip& clip, const Skeleton& target, const Rig& targetRig, const Pose& targetCurrent, const ApplyOptions& opts = {});
} // namespace lila::retarget::semantic
