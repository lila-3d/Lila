#pragma once

#include "RigidState.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <memory>
#include <optional>
#include <vector>

/**
 * The scene a rigid-body simulation runs, read from its node's inputs, and the world that steps it on lila_physics.
 * Everything here is centimetres.
 */
namespace destruction
{
struct PieceDesc
{
    /** Collision hull points in the piece's own space, its scale applied. Shared between pieces with one reference. */
    std::shared_ptr<const std::vector<glm::dvec3>> hull;
    glm::dvec3 position{0.0};
    glm::dquat rotation{1, 0, 0, 0};
    glm::dvec3 scale{1.0};
    double density = 2400.0; // kg/m3
    double friction = 0.6;
    double bounce = 0.1;
    bool active = true;
    /** 0 to 1: how strongly the simulated piece is pulled toward its own (animated) transform, for directed collapses. */
    double guide = 0.0;
    glm::dvec3 velocity{0.0};        // cm/s
    glm::dvec3 angularVelocity{0.0}; // rad/s
};

enum class LinkType : int
{
    Glue = 0,   // holds the pieces rigidly until the force through it passes its strength
    Spring = 1, // holds their distance softly, bending before it breaks
    Hard = 2,   // never breaks
};

struct LinkDesc
{
    int a = -1, b = -1; // piece indices; b = -1 ties a to the world
    LinkType type = LinkType::Glue;
    double strength = 1000.0; // N
    glm::dvec3 anchor{0.0};   // cm
    /** cm2 the pieces share; 0 when unknown. A wider joint takes more bending before it breaks. */
    double area = 0.0;
};

/** A push outward from a point on one frame, fading linearly to nothing at the radius. */
struct BlastDesc
{
    glm::dvec3 center{0.0}; // cm
    double speed = 0.0;     // cm/s at the centre
    double radius = 100.0;  // cm
    int64_t frame = 1;
};

struct ColliderDesc
{
    std::shared_ptr<const std::vector<glm::dvec3>> triangles; // own space, three points each, scale applied
    glm::dvec3 position{0.0};
    glm::dquat rotation{1, 0, 0, 0};
    bool moving = false; // follows its transform every step (an instance); otherwise fixed where it was at the start
};

struct Settings
{
    int64_t startFrame = 1;
    int substeps = 4;
    glm::dvec3 gravity{0.0, -981.0, 0.0}; // cm/s2
    bool ground = true;
    double groundHeight = 0.0;
    double damping = 0.05;
    bool sleep = true;
    double strengthScale = 1.0;
    glm::dvec3 blastCenter{0.0};
    double blastSpeed = 0.0; // cm/s given to pieces at the blast centre, fading to 0 at the radius
    double blastRadius = 100.0;
    int64_t blastFrame = 1;
    double springStiffness = 4.0; // Hz
    double springDamping = 0.5;
    double guideStiffness = 2.0; // Hz at guide 1
};

struct SceneDesc
{
    std::vector<PieceDesc> pieces;
    std::vector<LinkDesc> links;
    std::vector<ColliderDesc> colliders;
    /** Read every step, so they can be added or moved without restarting the simulation before their frame. */
    std::vector<BlastDesc> blasts;
    Settings settings;
};

/** One simulation's physics scene. Not thread-safe; the framework calls it from the main thread. */
class RigidWorld
{
public:
    /**
     * Builds the world from the scene at the start frame. origin is the point the physics scene is centred on, so pieces
     * kilometres from the node's origin keep their precision; by default the pieces' centre to the nearest 100 m.
     */
    explicit RigidWorld(const SceneDesc& start, std::optional<glm::dvec3> origin = std::nullopt);
    ~RigidWorld();
    RigidWorld(const RigidWorld&) = delete;
    RigidWorld& operator=(const RigidWorld&) = delete;

    bool matches(const SceneDesc& s) const;
    uint64_t serial() const;
    int64_t step() const;
    glm::dvec3 origin() const;

    /** Puts the world in the saved state exactly. */
    void restore(const StateView& state);
    /**
     * One step of dt seconds, to `current` sampled at the step's end (kinematic targets, activation, moving colliders,
     * link strengths). index is the step's number since the start, which places the blasts.
     */
    void advance(const SceneDesc& current, int64_t index, double dt);
    /** Every simulated piece asleep or slower than speed (cm/s) and spin (rad/s). */
    bool settled(double speed, double spin) const;
    /** The state bytes of where the world is now; without full, no Jolt snapshot (a substep inside a frame). */
    void save(std::vector<uint8_t>& out, bool full = true) const;

    struct Impl;

private:
    std::unique_ptr<Impl> mImpl;
};
} // namespace destruction
