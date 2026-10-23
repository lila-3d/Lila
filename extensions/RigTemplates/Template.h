#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

/**
 * Rig templates: a skeleton described as markers, the rules that orient a joint at each marker, and what goes with a
 * character of that kind (limb groups, picker layout, poses, naming aliases). A .lilarig file is the JSON of one
 * Template; positions and orientations are world space in Lila's axes (Y up, right handed, cm).
 */
namespace rigt
{
using json = nlohmann::json;

/** Where a marker's position comes from. */
enum class Placement
{
    Free,    // a marker node the user places
    Parent,  // rigid in the parent's frame (helpers, correctives)
    Follow,  // rigid in another marker's frame (UE's ik bones follow the hands and feet)
    Segment, // along the parent's bone toward `to`, plus a residual in the parent's frame (twist joints)
};

/** How the joint at a marker is oriented. */
enum class Orient
{
    Aim,    // aim axis at aimAt, up axis toward the up reference, then the recorded offset
    Parent, // parent orientation times offset
    Follow, // follow's orientation times offset
    Fixed,  // offset is the world orientation
};

/** What the up axis leans toward when orienting by aim. */
enum class UpType
{
    World,  // upVector, world space
    Plane,  // the normal of parent, marker, aimAt (sign kept toward upVector)
    Parent, // upVector in the parent's frame
};

struct Marker
{
    std::string name;   // unique in the template; also the default joint name
    std::string parent; // marker name, empty for the top
    std::string joint;  // joint name the build makes
    std::string label;  // semantic label: pelvis, spine_02, hand_l ...
    std::string side = "center"; // center, left, right
    std::string mirror; // the opposite side's marker
    std::string limb;
    std::vector<std::string> flags; // ik, helper, twist, root
    glm::dvec3 position{0.0};
    glm::dquat orientation{1.0, 0.0, 0.0, 0.0}; // recorded world orientation of the joint
    double radius = 2.0;
    bool centerInMesh = false;

    Placement placement = Placement::Free;
    std::string follow;            // Follow: the marker followed
    std::string to;                // Segment: the bone's far end
    double t = 0.0;                // Segment: fraction from parent to `to`
    glm::dvec3 localOffset{0.0};   // Parent/Follow/Segment: offset in that marker's frame

    Orient orient = Orient::Aim;
    std::string aimAt;             // Aim: the free marker aimed at
    std::string aim = "z";         // local axis aimed, signed: "x", "-y" ...
    std::string up = "y";          // local axis leaned toward the up reference
    UpType upType = UpType::World;
    glm::dvec3 upVector{0.0, 1.0, 0.0};
    glm::dquat offset{1.0, 0.0, 0.0, 0.0}; // orientation relative to the rule's frame

    bool hasFlag(const std::string& f) const;
    bool isFree() const { return placement == Placement::Free; }
};

struct Limb
{
    std::string name;
    std::string side = "center";
    std::vector<std::string> markers;
    UpType upType = UpType::World;
};

struct Template
{
    std::string id;
    std::string name;
    std::string description;
    int version = 1;
    std::vector<Marker> markers; // parents, follows and segment ends before the markers that use them
    std::vector<Limb> limbs;
    json picker = json::object(); // a compact picker layout (scene::picker::generateFromLayout)
    json poses = json::object();  // name -> pose JSON (scene::poses)
    json aliases = json::object(); // marker or joint name -> [other names]
    /** Default spaces: semantic label or marker name -> ["world", or labels / marker names of other joints]. */
    json spaces = json::object();
    /**
     * Rig elements a build makes, by marker name: ikChains [{name, start, end, pole (world direction)}], reverseFeet
     * [{chain, ball, toe, pivots {heel, ball, toe, inner, outer} in the ankle marker's frame}], lookers [{joint, aimAxis,
     * upAxis, distance, weight}], twists [{joint, source, weight}] and nonDeform [marker names].
     */
    json rig = json::object();
    json source = json::object();  // where the template came from
    glm::dvec3 lateral{1.0, 0.0, 0.0}; // world direction from the character's right to its left
    json extra = json::object();   // unknown keys, kept on save

    const Marker* marker(const std::string& name) const;
    Marker* marker(const std::string& name);
    const Limb* limb(const std::string& name) const;
    /** The marker's default spaces, by its name first, then its label. */
    std::vector<std::string> spacesFor(const Marker& m) const;
    /** The marker a space entry names: a marker name, else a label. */
    const Marker* markerFor(const std::string& nameOrLabel) const;

    json toJson() const;
    /** Throws std::invalid_argument naming the first problem. */
    static Template fromJson(const json& j);
    /** Orders markers so each comes after everything it depends on. Throws on a cycle or a missing reference. */
    void sortMarkers();
};

Template readFile(const std::filesystem::path& file);
void writeFile(const Template& t, const std::filesystem::path& file);

/** Solved world placement for every marker. */
struct Solved
{
    std::map<std::string, glm::dvec3> position;
    std::map<std::string, glm::dquat> rotation;
};

/**
 * Positions and orientations for the whole template. `known` gives positions by marker name (free markers' nodes, or
 * an existing skeleton's joints); free markers missing from it sit at their template position (or are left out without
 * fillFree), derived ones follow their rule. An aim marker with nothing to aim at keeps its recorded angle to its parent.
 */
Solved solve(const Template& t, const std::map<std::string, glm::dvec3>& known, bool fillFree = true);

/** The rule frame of an Aim marker for the given positions; nullopt when aimAt is missing or coincident. */
std::optional<glm::dquat> aimFrame(const Template& t, const Marker& m, const std::map<std::string, glm::dvec3>& pos,
                                   const std::map<std::string, glm::dquat>& rot);

/** A joint of a skeleton being turned into a template. */
struct SourceJoint
{
    std::string name;
    std::string parent;
    glm::dvec3 position{0.0};
    glm::dquat rotation{1.0, 0.0, 0.0, 0.0};
};

struct FromSkeletonOptions
{
    std::string id;
    std::string name;
    /** Names to keep as free markers even when they look like helpers. */
    std::set<std::string> free;
    bool includeIk = true;
};

/** A template whose solve at its default positions reproduces the skeleton's world orientations. */
Template fromSkeleton(const std::vector<SourceJoint>& joints, const FromSkeletonOptions& opts);

/** A compact picker layout (front view) of the template's labelled free markers, for scene::picker::generateFromLayout. */
json pickerLayout(const Template& t);

/**
 * bind (every rotate zeroed), T (arms out along the lateral axis) and A (arms 45 degrees down) as pose JSON keyed by
 * name, for joints built from the template.
 */
json defaultPoses(const Template& t);

/** The semantic label for a joint name (UE names, common DCC names, mocap names), or empty. */
std::string labelFor(const std::string& jointName);
/** Every alias the built-in table knows for a label. */
std::vector<std::string> aliasesFor(const std::string& label);

glm::dvec3 axisOf(const std::string& axis);
std::string sideOf(const std::string& name);
} // namespace rigt
