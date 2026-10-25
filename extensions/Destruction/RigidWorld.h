#pragma once

#include "RigidState.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <memory>
#include <vector>

/**
 * The scene a rigid-body simulation runs, read from its node's inputs without Jolt, and the Jolt world that steps it.
 * Lila works in centimetres and Jolt in metres; the descriptions are in centimetres.
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
    Settings settings;
};

/** A Jolt world for one simulation. Not thread-safe; the framework calls it from the main thread. */
class RigidWorld
{
public:
    /** Builds the world from the scene at the start frame. */
    explicit RigidWorld(const SceneDesc& start);
    ~RigidWorld();
    RigidWorld(const RigidWorld&) = delete;
    RigidWorld& operator=(const RigidWorld&) = delete;

    bool matches(const SceneDesc& s) const;
    uint64_t serial() const;
    int64_t step() const;

    /** Puts the world in the saved state exactly. */
    void restore(const StateView& state);
    /**
     * One step of dt seconds, to `current` sampled at the step's end (kinematic targets, activation, moving colliders).
     * index is the step's number since the start, which places the blast.
     */
    void advance(const SceneDesc& current, int64_t index, double dt);
    /** The state bytes of where the world is now; without full, no Jolt snapshot (a substep inside a frame). */
    void save(std::vector<uint8_t>& out, bool full = true) const;

    struct Impl;

private:
    std::unique_ptr<Impl> mImpl;
};

/** Registers Jolt's types and its job system; idempotent. shutdownJolt runs at library unload. */
void initJolt();
void shutdownJolt();
} // namespace destruction
