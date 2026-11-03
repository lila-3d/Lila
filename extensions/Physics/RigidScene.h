#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

/**
 * A general rigid-body scene on Jolt, without Jolt in the header: bodies with convex or primitive shapes, static triangle
 * mesh colliders, a ground plane, impulses, kinematic targets and the contacts the bodies made. For tools that settle,
 * push and drop things into place and for anything recording a simulation step by step (set dressing now, animation
 * later). Lila works in centimetres and Jolt in metres; everything here is centimetres, kilograms and seconds.
 *
 * Deterministic: the same calls in the same order give the same bytes at any thread count. Main thread only.
 */
namespace lila::physics
{
enum class Role : int
{
    Dynamic = 0,   // simulated
    Static = 1,    // never moves; others hit it
    Kinematic = 2, // moves only where it is told to (a held object), pushing dynamic bodies aside
};

struct Shape
{
    enum class Kind : int
    {
        Hull = 0,  // convex hull of points
        Box = 1,   // halfExtents about center
        Sphere = 2,
        Capsule = 3, // along the body's own Y
        Mesh = 4,  // triangle soup, three points each; static and kinematic bodies only
    };
    Kind kind = Kind::Hull;
    /** Hull points or mesh triangles in the body's own space, its scale applied. Shared between bodies with one reference. */
    std::shared_ptr<const std::vector<glm::dvec3>> points;
    glm::dvec3 center{0.0};
    glm::dvec3 halfExtents{1.0};
    double radius = 1.0;
    double halfHeight = 1.0;
    /** Rounding of hull and box edges, cm; also how close bodies rest. */
    double rounding = 0.2;
    /** Turns the shape in the body's own space, about its origin, before center places it. */
    glm::dquat rotation{1, 0, 0, 0};
};

struct BodyDesc
{
    Shape shape;
    glm::dvec3 position{0.0};
    glm::dquat rotation{1, 0, 0, 0};
    Role role = Role::Dynamic;
    double density = 600.0; // kg/m3
    double friction = 0.6;
    double bounce = 0.1;
    double damping = 0.05;
    glm::dvec3 velocity{0.0};        // cm/s
    glm::dvec3 angularVelocity{0.0}; // rad/s
    /** The caller's id for the body, reported back in contacts and hits. */
    int64_t user = -1;
    /** Damping of turning; negative uses damping. */
    double angularDamping = -1.0;
    /** kg; 0 takes it from the density. Inertia follows the shape either way. */
    double mass = 0.0;
    /** False touches nothing: a body only joints and forces move. */
    bool collides = true;
    bool canSleep = true;
    /** Swept collision, so small fast bodies don't pass through others; off is cheaper. */
    bool continuous = true;
};

struct Settings
{
    glm::dvec3 gravity{0.0, -981.0, 0.0}; // cm/s2
    bool ground = false;
    double groundHeight = 0.0;
    bool sleep = true;
    /** Collision steps per step(); more for fast or thin things. */
    int collisionSteps = 1;
    bool recordContacts = true;
    /**
     * Contact tuning, cm and cm/s: how deep resting bodies may sink, how far ahead contacts are found, and the speed below
     * which bodies fall asleep. The defaults suit props (Jolt's 2 cm of resting overlap is a book sunk into its shelf, and
     * 3 cm/s stops a book still sliding off it); metre-sized pieces want Jolt's own (2, 2, 3).
     */
    double penetrationSlop = 0.05;
    double speculativeDistance = 0.5;
    double sleepSpeed = 1.0;
    /** Solver iterations per step; 0 keeps Jolt's (10 velocity, 2 position). */
    int velocitySteps = 0;
    int positionSteps = 0;
    bool largeIslandSplitter = true;
    /** Bodies and touching pairs the scene makes room for; 0 for 65536. */
    uint32_t maxBodies = 0;
    uint32_t maxPairs = 0;
    /** Jolt on the calling thread only, whatever LILA_PHYSICS_THREADS says. */
    bool serial = false;
    /** The first this many bodies get a collision filter whose pairs setCollides switches; 0 for none. */
    int pairFilter = 0;
    double groundFriction = 0.7;
};

enum class JointKind : int
{
    Fixed = 0,      // holds the two bodies rigidly together
    Distance = 1,   // holds the distance between two points within a range, optionally springy
    SwingTwist = 2, // a ball joint with a swing cone and twist range, optionally motor driven (limbs)
};

/** A joint between two bodies, or a body and the world. Points and axes are world space, as the bodies are when it is added. */
struct JointDesc
{
    JointKind kind = JointKind::Fixed;
    int a = -1;
    int b = -1; // -1: the world
    glm::dvec3 pointA{0.0}; // cm
    glm::dvec3 pointB{0.0};
    /** Distance: the allowed range, cm, and the spring holding it (Hz and damping ratio; frequency 0 is rigid). */
    double minDistance = 0.0;
    double maxDistance = 0.0;
    double frequency = 0.0;
    double damping = 0.0;
    /** SwingTwist: each body's twist axis and the axis its swing cone's plane angle is measured about. */
    glm::dvec3 twistAxisA{1, 0, 0};
    glm::dvec3 planeAxisA{0, 0, 1};
    glm::dvec3 twistAxisB{1, 0, 0};
    glm::dvec3 planeAxisB{0, 0, 1};
    /** Radians. */
    double planeHalfCone = 3.14159265358979323846;
    double normalHalfCone = 3.14159265358979323846;
    double twistMin = -3.14159265358979323846;
    double twistMax = 3.14159265358979323846;
};

/** What a joint pushed with over the last step, as Jolt's impulses: linear N s, angular N m s. */
struct JointLoad
{
    double linear = 0.0;
    double angular = 0.0;
};

/** A body's state as the floats Jolt holds, metres and radians, so copying it elsewhere is exact. */
struct RawState
{
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 velocity{0.0f};
    glm::vec3 angularVelocity{0.0f};
};

struct Pose
{
    glm::dvec3 position{0.0};
    glm::dquat rotation{1, 0, 0, 0};
};

/** Two bodies touching, merged over every step since the last clearContacts. b is -1 for the ground. */
struct Contact
{
    int a = -1, b = -1;
    /** Where they last touched, and the normal from a toward b. */
    glm::dvec3 point{0.0};
    glm::dvec3 normal{0.0, 1.0, 0.0};
    /** Hardest hit, as the normal speed they met at times the pair's reduced mass: kg cm/s. */
    double peakImpulse = 0.0;
    /** Steps they spent touching, and how far they slid against each other while they did, cm. */
    int64_t steps = 0;
    double slide = 0.0;
    /** Still touching on the last step. */
    bool resting = false;
};

struct RayHit
{
    int body = -1; // -1 for the ground
    glm::dvec3 point{0.0};
    glm::dvec3 normal{0.0, 1.0, 0.0};
    double distance = 0.0;
};

class RigidScene
{
public:
    explicit RigidScene(const Settings& settings = {});
    ~RigidScene();
    RigidScene(const RigidScene&) = delete;
    RigidScene& operator=(const RigidScene&) = delete;

    /** Adds a body and returns its index; bodies are numbered in the order they were added. Throws std::invalid_argument. */
    int add(const BodyDesc& body);
    size_t size() const;
    const BodyDesc& desc(int body) const;

    /** Advances dt seconds. */
    void step(double dt);
    /** Steps of dt until every dynamic body sleeps or `seconds` have passed. Returns the time taken. */
    double settle(double seconds, double dt = 1.0 / 60.0);
    int64_t steps() const;

    Pose pose(int body) const;
    glm::dvec3 velocity(int body) const;
    bool asleep(int body) const;
    /** True when no dynamic body is awake. */
    bool atRest() const;

    void setRole(int body, Role role);
    /** Moves a kinematic body to the pose over the next step, giving it the velocity that does so. */
    void moveTo(int body, const Pose& target, double dt);
    /** Teleports a body, waking it. */
    void setPose(int body, const Pose& p);
    void setVelocity(int body, const glm::dvec3& linear, const glm::dvec3& angular);
    /** Adds a change of momentum (kg cm/s) at a world point, waking the body. */
    void impulse(int body, const glm::dvec3& impulse, const glm::dvec3& at);
    /** Adds a change of velocity (cm/s) at the centre of mass, whatever the mass, waking the body. */
    void kick(int body, const glm::dvec3& velocity);
    void wake(int body);
    /**
     * Pulls a body's point `local` (body space) toward a world target with a critically damped spring of the frequency
     * (Hz) for one step: how a hand drags something while it still collides.
     */
    void pull(int body, const glm::dvec3& local, const glm::dvec3& target, double frequency, double dt);

    double mass(int body) const;
    glm::dvec3 centerOfMass(int body) const;
    /** World bounds of a body's shape. */
    void bounds(int body, glm::dvec3& lo, glm::dvec3& hi) const;

    std::optional<RayHit> raycast(const glm::dvec3& origin, const glm::dvec3& direction, double maxDistance) const;
    /** Deepest overlap of the body with anything else, cm; 0 when clear. */
    double penetration(int body) const;

    /** Sorted by (a, b). */
    std::vector<Contact> contacts() const;
    void clearContacts();

    /** Jolt's full state, for exact restores and for proving two runs equal. */
    std::vector<uint8_t> save() const;
    void restore(const std::vector<uint8_t>& state);

    /** Adds a joint and returns its index; joints are numbered in the order they were added. Throws std::invalid_argument. */
    int addJoint(const JointDesc& joint);
    size_t joints() const;
    /** A disabled joint holds nothing; saved and restored with the scene. */
    void setJointEnabled(int joint, bool on);
    bool jointEnabled(int joint) const;
    JointLoad jointLoad(int joint) const;
    /**
     * SwingTwist: drives b's orientation in the joint's frames toward target with a spring of the frequency (Hz) and
     * damping ratio; frequency 0 lets go.
     */
    void setJointMotor(int joint, const glm::dquat& target, double frequency, double damping);
    /** A joint's solver state (its warm-start impulses), to carry into a scene rebuilt from saved bodies. */
    std::vector<uint8_t> saveJoint(int joint) const;
    void restoreJoint(int joint, const std::vector<uint8_t>& state);
    /** Bytes saveJoint gives for a kind, the same for every joint of it. */
    static size_t jointStateSize(JointKind kind);
    /** The next step warm-starts joints as if the step before it had been dt long: for a scene rebuilt from saved state. */
    void primeStep(double dt);

    /** Whether two bodies under Settings::pairFilter collide; all do at first. */
    void setCollides(int a, int b, bool on);
    /** How deep two bodies' shapes overlap, cm; 0 when apart. */
    double overlap(int a, int b) const;

    glm::dvec3 angularVelocity(int body) const;
    /** Velocity of a world point carried by the body, cm/s. */
    glm::dvec3 pointVelocity(int body, const glm::dvec3& at) const;
    RawState raw(int body) const;
    /** Accelerates a dynamic body over the next step, cm/s2 and rad/s2, whatever its mass and inertia. */
    void accelerate(int body, const glm::dvec3& linear, const glm::dvec3& angular = glm::dvec3(0.0));
    /** Adds a force (kg cm/s2) at a world point for the next step, waking the body. */
    void push(int body, const glm::dvec3& force, const glm::dvec3& at);

    struct Impl;

private:
    std::unique_ptr<Impl> mImpl;
};

/** Registers Jolt's types, allocator and job system; idempotent. Torn down when the library unloads. */
void initJolt();

/** Triangles (three points each) of the convex hull of points, in their units: for drawing and picking a hull. */
std::vector<glm::dvec3> hullTriangles(const std::vector<glm::dvec3>& points);
} // namespace lila::physics
