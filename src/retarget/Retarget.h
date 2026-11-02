#pragma once

#include "core/Export.h"
#include "retarget/Rig.h"

#include <map>
#include <string>
#include <vector>

/**
 * Retargeting one skeleton's motion onto another (UE's IK Retargeter), as plain data. Settings mirror UE's names and
 * run in UE's order: root, FK chains, pole matching, IK goals (stride warping, contacts, planting), IK, pinned bones.
 *
 * FK copies each source chain's change of world rotation from its retarget pose onto the target chain, resampled by
 * length along the chain, so chains with different joint counts and joint orients map onto each other. IK then puts
 * hands and feet where the source puts them, scaled to the target's size.
 */
namespace lila::retarget
{
enum class FkRotation : uint8_t
{
    Interpolated,     // blend the source rotations at the same fraction of the chain's length
    OneToOne,         // joint i takes source joint i (the last source joint past the end)
    OneToOneReversed, // counted from the ends
    MatchChain,       // the source joint nearest the same fraction of length
    None,
};
enum class FkTranslation : uint8_t
{
    None,              // keep the target's own bone lengths
    GloballyScaled,    // the source joint's offset from its parent, scaled to the target's size
    Absolute,          // the source joint's offset unscaled
    StretchUniform,    // the target's bones scaled by the source chain's stretch
    StretchNonUniform, // each target bone by its source bone's stretch
};

struct FkSettings
{
    bool enabled = true;
    FkRotation rotation = FkRotation::Interpolated;
    double rotationAlpha = 1.0;
    FkTranslation translation = FkTranslation::None;
    double translationAlpha = 1.0;
    /** 0..1: turn the chain about its start-to-end axis so its bend faces the way the source's does. */
    double poleMatching = 0.0;
    /** Pole matching keeps the difference the two chains have in the retarget pose. */
    bool poleMaintainOffset = false;
    double poleOffset = 0.0; // degrees
};

struct IkSettings
{
    bool enabled = true;
    double blendToSource = 0.0; // 0..1 toward the source's unscaled goal position
    glm::dvec3 blendToSourceWeights{1.0};
    glm::dvec3 staticOffset{0.0};         // world, cm
    glm::dvec3 staticLocalOffset{0.0};    // in the goal's frame, cm
    glm::dvec3 staticRotationOffset{0.0}; // goal frame, degrees
    double scaleVertical = 1.0;           // goal height above the ground
    /** Distance of the goal from the chain's start, as a multiple: below 1 keeps the limb bent, above 1 reaches. */
    double extension = 1.0;
    bool affectedByWarping = true;
};

/** Holding a goal still while its source foot (or hand) is planted, so it never slides. */
struct PlantSettings
{
    bool enabled = true;
    /** Below this source speed (cm/s, scaled to the target) a low goal counts as planted. */
    double speedThreshold = 15.0;
    /** How far above the goal's lowest point it still counts as on the ground, cm on the source. */
    double heightTolerance = 4.0;
    /** Frames to ease into and out of each plant. */
    int blendFrames = 3;
    /** Shortest plant, frames. */
    int minFrames = 2;
};

struct ChainMap
{
    std::string target; // target chain name
    std::string source; // source chain name; empty leaves the target chain in its retarget pose
    FkSettings fk;
    IkSettings ik;
    PlantSettings plant;
};

struct RootSettings
{
    bool enabled = true;
    double rotationAlpha = 1.0;
    double translationAlpha = 1.0;
    double blendToSource = 0.0;
    glm::dvec3 blendToSourceWeights{1.0};
    double scaleHorizontal = 1.0;
    double scaleVertical = 1.0;
    glm::dvec3 translationOffset{0.0};
    glm::dvec3 rotationOffset{0.0}; // degrees, the pelvis's own frame
    /** IK goals move with the pelvis: horizontally (default) and vertically (off keeps feet on the ground). */
    bool affectIkHorizontal = true;
    bool affectIkVertical = false;
};

struct WarpSettings
{
    bool enabled = false;
    /** Velocity: the pelvis's direction of travel; Forward: the way the pelvis faces. */
    bool useVelocity = true;
    /** Stride length along the travel direction, as a multiple. */
    double warpForwards = 1.0;
    /** Feet out to their side (in) or apart (out), cm. */
    double sidewaysOffset = 0.0;
};

/** A bone copying another's world transform after everything else (UE's Pin Bone op: ik_foot_l on foot_l). */
struct PinBone
{
    std::string bone, to;
    bool translate = true, rotate = true;
    bool maintainOffset = true;
};

/** A hand held on the body where the source's is (hands on hips, clapping): goal chain against a point on a chain. */
struct Contact
{
    std::string effector;  // target chain with a goal
    std::string body;      // target chain the point is on
    double bodyParam = 1.0; // 0 its start, 1 its end
    double distance = 8.0;  // source cm: in contact at or below, released by 1.5 times
};

/** An animated attribute copied from the source's top joint to the target's, renamed. */
struct CurveMap
{
    std::string source, target;
};

struct LILA_SCENE_API Settings
{
    bool enableRoot = true, enableFk = true, enableIk = true;
    RootSettings root;
    std::vector<ChainMap> chains;
    WarpSettings warp;
    std::vector<PinBone> pins;
    std::vector<Contact> contacts;
    std::vector<CurveMap> curves;
    /** Copy animated attributes the source's top joint shares by name with the target's. */
    bool copyCurves = true;
    /** Target size over source size; 0 measures it from the legs (or the pelvis height). */
    double scale = 0.0;
    /**
     * Where a limb can't reach its goal, solve the whole body once more with every goal pinned, letting the pelvis and
     * spine give (UE's full-body IK op). Frames where every goal is met are left as they are.
     */
    bool fullBody = true;

    ChainMap* chain(std::string_view target);
    const ChainMap* chain(std::string_view target) const;
    json toJson() const;
    /** Unknown keys are ignored, missing ones keep their defaults. Throws std::invalid_argument on bad values. */
    static Settings fromJson(const json& j);
};

/**
 * Settings mapping every target chain to the source chain of the same name, else the same role, side and index, else
 * the same role and side. Root chains copy translation scaled; IK is on wherever both chains have a goal.
 */
LILA_SCENE_API Settings autoMap(const Rig& source, const Rig& target);

/**
 * Turns the target's retarget pose so each mapped chain points the way its source chain does in the source's
 * retarget pose (UE's auto-align), per bone by length fraction. Returns the new pose; the caller stores it.
 */
LILA_SCENE_API Pose alignRetargetPose(const Skeleton& source, const Rig& sourceRig, const Pose& sourceCurrent, const Skeleton& target,
                                      const Rig& targetRig, const Pose& targetCurrent, const Settings& settings);

/**
 * Turns the joints (a chain, parents first) so the last one reaches position and rotation, the rest of the skeleton
 * held; the current pose is the reference, so the bend keeps its side. Updates pose and world. Returns the miss, cm.
 */
LILA_SCENE_API double solveChainIk(const Skeleton& s, Pose& pose, std::vector<glm::dmat4>& world, const std::vector<int>& joints,
                                   const glm::dvec3& position, const glm::dquat& rotation);

struct Frames
{
    /** World matrix of every source joint, per frame. */
    std::vector<std::vector<glm::dmat4>> world;
    double fps = 30.0;
    /**
     * Animated attributes of the source's top joint, one value per frame, by name. plant<Chain> (plantLeftLeg) at or
     * above 0.5 says that chain's goal is planted, overriding detection: the animator's intent carries over.
     */
    std::map<std::string, std::vector<double>> channels;
};

struct Result
{
    std::vector<Pose> poses;
    /** Per goal chain (target name), whether it is planted at each frame. */
    std::vector<std::pair<std::string, std::vector<bool>>> planted;
    double scale = 1.0;
    std::vector<std::string> notes;
    /** Largest IK miss over the clip, cm, and per goal chain. */
    double ikError = 0.0;
    std::vector<std::pair<std::string, double>> chainError;
};

class LILA_SCENE_API Retargeter
{
public:
    /**
     * sourceCurrent and targetCurrent fill joints the rigs' retarget poses leave out. Throws std::invalid_argument when
     * either rig has no pelvis on its skeleton.
     */
    Retargeter(const Skeleton& source, const Rig& sourceRig, const Pose& sourceCurrent, const Skeleton& target, const Rig& targetRig,
               const Pose& targetCurrent, Settings settings);
    ~Retargeter();
    Retargeter(const Retargeter&) = delete;
    Retargeter& operator=(const Retargeter&) = delete;

    /** One frame: root, FK and IK, no planting or warping (those need the whole clip). */
    Pose frame(const std::vector<glm::dmat4>& sourceWorld) const;
    /** A whole clip. */
    Result clip(const Frames& frames) const;

    double scale() const;
    const std::vector<std::string>& notes() const;

private:
    struct Impl;
    Impl* mImpl;
};
} // namespace lila::retarget
