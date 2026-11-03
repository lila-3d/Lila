// RigidWorld on lila_physics: pieces are the scene's first bodies (so their pair filter is indexed by piece), colliders
// follow, links are fixed or distance joints. Every call happens in a fixed order, which keeps the results the same
// bytes on any machine and thread count.

#include "RigidWorld.h"

#include <RigidScene.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <stdexcept>

namespace destruction
{
namespace
{
namespace ph = lila::physics;

std::atomic<uint64_t> gSerial{0};

/** Overlap Jolt leaves alone (its penetration slop is 2 cm), so pieces rejoined at it don't pop apart. */
constexpr double kRejoinOverlap = 1.5;

/** Scenes within this of the node's origin simulate about it; farther ones about their own centre, snapped to it. */
constexpr double kOriginSnap = 10000.0;

glm::dvec3 defaultOrigin(const SceneDesc& s)
{
    glm::dvec3 lo(1e300), hi(-1e300);
    for (const PieceDesc& p : s.pieces)
    {
        lo = glm::min(lo, p.position);
        hi = glm::max(hi, p.position);
    }
    if (s.pieces.empty())
        return glm::dvec3(0.0);
    return glm::round((lo + hi) * 0.5 / kOriginSnap) * kOriginSnap;
}
} // namespace

struct RigidWorld::Impl
{
    std::unique_ptr<ph::RigidScene> scene;
    glm::dvec3 origin{0.0};
    std::vector<LinkDesc> linkDescs;
    std::vector<int> joints; // per link, -1 when it holds nothing
    std::vector<LinkRecord> linkState;
    std::vector<uint8_t> active;
    std::vector<uint8_t> moving; // per collider
    Settings settings;
    size_t pieceCount = 0, colliderCount = 0;
    bool ground = false;
    uint64_t serial = 0;
    int64_t step = 0;

    int collider(size_t c) const { return int(pieceCount + c); }

    void setActive(size_t i, bool on)
    {
        active[i] = on ? 1 : 0;
        scene->setRole(int(i), on ? ph::Role::Dynamic : ph::Role::Kinematic);
    }

    bool validPair(int a, int b) const { return a >= 0 && b >= 0 && a != b && size_t(a) < pieceCount && size_t(b) < pieceCount; }

    /** Applies links' broken and colliding flags to the joints and the pair filter. */
    void applyLinkState()
    {
        for (size_t i = 0; i < linkDescs.size(); ++i)
        {
            const LinkRecord& r = linkState[i];
            if (joints[i] >= 0)
                scene->setJointEnabled(joints[i], r.brokenStep < 0);
            if (validPair(linkDescs[i].a, linkDescs[i].b))
                scene->setCollides(linkDescs[i].a, linkDescs[i].b, r.colliding != 0);
        }
    }

    /**
     * Blends a piece's velocities toward the ones that carry it to its target pose, by the guide weight: a critically
     * damped pull that stays stable at any step size.
     */
    void guide(size_t i, const PieceDesc& p, double dt)
    {
        const int body = int(i);
        const double omega = 2.0 * 3.14159265358979323846 * settings.guideStiffness * p.guide;
        const double blend = 1.0 - std::exp(-omega * dt);
        const ph::Pose now = scene->pose(body);
        const glm::dvec3 toTarget = (p.position - origin) - now.position;
        const glm::dvec3 vel = scene->velocity(body);
        const glm::dvec3 newVel = vel + (toTarget * omega - vel) * blend;

        glm::dquat delta = glm::normalize(p.rotation * glm::conjugate(now.rotation));
        if (delta.w < 0.0)
            delta = -delta;
        const double s = glm::length(glm::dvec3(delta.x, delta.y, delta.z));
        const glm::dvec3 rotVec = s < 1e-12 ? glm::dvec3(0.0) : glm::dvec3(delta.x, delta.y, delta.z) * (2.0 * std::atan2(s, delta.w) / s);
        const glm::dvec3 ang = scene->angularVelocity(body);
        scene->setVelocity(body, newVel, ang + (rotVec * omega - ang) * blend);
    }
};

RigidWorld::RigidWorld(const SceneDesc& s, std::optional<glm::dvec3> origin) : mImpl(std::make_unique<Impl>())
{
    Impl& w = *mImpl;
    w.settings = s.settings;
    w.serial = ++gSerial;
    w.origin = origin ? *origin : defaultOrigin(s);
    w.pieceCount = s.pieces.size();
    w.colliderCount = s.colliders.size();
    w.ground = s.settings.ground;
    const size_t n = s.pieces.size();

    ph::Settings ps;
    ps.gravity = s.settings.gravity;
    ps.ground = s.settings.ground;
    ps.groundHeight = s.settings.groundHeight - w.origin.y;
    ps.sleep = s.settings.sleep;
    ps.recordContacts = false;
    // Pieces are tens of centimetres to metres across: Jolt's own contact tuning, not the props' finer one.
    ps.penetrationSlop = 2.0;
    ps.speculativeDistance = 2.0;
    ps.sleepSpeed = 3.0;
    ps.maxBodies = uint32_t(n + s.colliders.size() + 8);
    ps.maxPairs = uint32_t(std::clamp<size_t>(n * 16, 4096, 1u << 20));
    ps.pairFilter = int(std::max<size_t>(n, 1));
    w.scene = std::make_unique<ph::RigidScene>(ps);

    w.active.assign(n, 0);
    for (size_t i = 0; i < n; ++i)
    {
        const PieceDesc& p = s.pieces[i];
        ph::BodyDesc b;
        b.shape.kind = ph::Shape::Kind::Hull;
        b.shape.points = p.hull;
        b.shape.rounding = 0.5;
        b.position = p.position - w.origin;
        b.rotation = p.rotation;
        b.role = p.active ? ph::Role::Dynamic : ph::Role::Kinematic;
        b.density = p.density;
        b.friction = p.friction;
        b.bounce = p.bounce;
        b.damping = s.settings.damping;
        b.velocity = p.velocity;
        b.angularVelocity = p.angularVelocity;
        b.user = int64_t(i);
        w.scene->add(b);
        w.active[i] = p.active ? 1 : 0;
    }
    for (const ColliderDesc& c : s.colliders)
    {
        ph::BodyDesc b;
        b.shape.kind = ph::Shape::Kind::Mesh;
        b.shape.points = c.triangles;
        b.position = c.position - w.origin;
        b.rotation = c.rotation;
        b.role = c.moving ? ph::Role::Kinematic : ph::Role::Static;
        b.friction = 0.6;
        b.canSleep = false;
        b.user = -1;
        w.scene->add(b);
        w.moving.push_back(c.moving ? 1 : 0);
    }

    w.linkDescs = s.links;
    w.joints.assign(s.links.size(), -1);
    w.linkState.assign(s.links.size(), LinkRecord{});
    for (size_t i = 0; i < s.links.size(); ++i)
    {
        const LinkDesc& l = s.links[i];
        // Pieces held together start apart in collision: their hulls touch or overlap along the cut.
        if (w.validPair(l.a, l.b))
            w.scene->setCollides(l.a, l.b, false);
        if (l.a < 0 || size_t(l.a) >= n || l.b >= int(n) || l.a == l.b)
            continue;
        ph::JointDesc j;
        j.a = l.a;
        j.b = l.b;
        j.pointA = j.pointB = l.anchor - w.origin;
        if (l.type == LinkType::Spring)
        {
            j.kind = ph::JointKind::Distance;
            j.frequency = std::max(s.settings.springStiffness, 0.01);
            j.damping = std::max(s.settings.springDamping, 0.0);
        }
        else
            j.kind = ph::JointKind::Fixed;
        w.joints[i] = w.scene->addJoint(j);
    }
}

RigidWorld::~RigidWorld() = default;

bool RigidWorld::matches(const SceneDesc& s) const
{
    const Impl& w = *mImpl;
    return s.pieces.size() == w.pieceCount && s.colliders.size() == w.colliderCount && s.links.size() == w.linkDescs.size() &&
           s.settings.ground == w.ground;
}

uint64_t RigidWorld::serial() const { return mImpl->serial; }

int64_t RigidWorld::step() const { return mImpl->step; }

glm::dvec3 RigidWorld::origin() const { return mImpl->origin; }

void RigidWorld::restore(const StateView& state)
{
    Impl& w = *mImpl;
    const StateHeader& h = state.header();
    if (h.bodies != w.pieceCount || h.links != w.linkDescs.size())
        throw std::runtime_error("the pieces changed during the simulation; it starts over from its first frame");
    if (h.partial)
        throw std::runtime_error("the simulation can't resume between the substeps of a frame");
    if (state.origin() != w.origin)
        throw std::runtime_error("the simulation moved; it starts over from its first frame");
    for (size_t i = 0; i < w.pieceCount; ++i)
    {
        const bool on = state.body(i).active != 0;
        if (on != (w.active[i] != 0))
            w.setActive(i, on);
    }
    for (size_t i = 0; i < w.linkDescs.size(); ++i)
        w.linkState[i] = state.link(i);
    w.applyLinkState();
    const std::span<const uint8_t> bytes = state.jolt();
    w.scene->restore(std::vector<uint8_t>(bytes.begin(), bytes.end()));
    w.step = h.step;
}

void RigidWorld::advance(const SceneDesc& current, int64_t index, double dt)
{
    Impl& w = *mImpl;
    ph::RigidScene& scene = *w.scene;
    const size_t n = std::min(w.pieceCount, current.pieces.size());

    for (size_t i = 0; i < n; ++i)
    {
        const PieceDesc& p = current.pieces[i];
        if (!w.active[i] && p.active)
            w.setActive(i, true);
        if (!w.active[i])
            scene.moveTo(int(i), {p.position - w.origin, p.rotation}, dt);
        else if (p.guide > 0.0 && w.settings.guideStiffness > 0.0)
            w.guide(i, p, dt);
    }
    for (size_t c = 0; c < w.colliderCount && c < current.colliders.size(); ++c)
        if (w.moving[c])
            scene.moveTo(w.collider(c), {current.colliders[c].position - w.origin, current.colliders[c].rotation}, dt);

    const Settings& st = w.settings;
    auto blast = [&](const BlastDesc& b) {
        const int64_t at = (b.frame - st.startFrame) * std::max(1, st.substeps) + 1;
        if (b.speed == 0.0 || index != std::max<int64_t>(1, at))
            return;
        const double radius = std::max(b.radius, 1e-6);
        for (size_t i = 0; i < w.pieceCount; ++i)
        {
            if (!w.active[i])
                continue;
            const glm::dvec3 d = scene.centerOfMass(int(i)) + w.origin - b.center;
            const double dist = glm::length(d);
            if (dist >= radius)
                continue;
            const double fall = 1.0 - dist / radius;
            const glm::dvec3 dir = dist > 1e-9 ? d / dist : glm::dvec3(0, 1, 0);
            scene.kick(int(i), dir * (b.speed * fall));
        }
    };
    blast({st.blastCenter, st.blastSpeed, st.blastRadius, st.blastFrame});
    for (const BlastDesc& b : current.blasts)
        blast(b);
    if (current.links.size() == w.linkDescs.size())
        for (size_t i = 0; i < w.linkDescs.size(); ++i)
            w.linkDescs[i].strength = current.links[i].strength;

    scene.step(dt);
    ++w.step;

    const double scale = std::max(st.strengthScale, 0.0);
    for (size_t i = 0; i < w.linkDescs.size(); ++i)
    {
        LinkRecord& r = w.linkState[i];
        const LinkDesc& l = w.linkDescs[i];
        if (w.joints[i] < 0 || r.brokenStep >= 0 || l.type == LinkType::Hard)
            continue;
        const double limit = l.strength * scale;
        const ph::JointLoad load = scene.jointLoad(w.joints[i]);
        // Torque counts as the force it takes at a 10 cm lever, so bending a slab breaks it as pulling would.
        const double impulse = l.type == LinkType::Spring ? load.linear : load.linear + load.angular / 0.1;
        // A link whose strength drops to 0 is cut, even with nothing pulling on it (pieces held still).
        if (limit <= 0.0 || impulse / dt > limit)
        {
            r.brokenStep = int32_t(w.step);
            r.impulse = float(impulse);
            scene.setJointEnabled(w.joints[i], false);
        }
    }
    // Broken pieces collide again once their hulls barely overlap, so a stack whose links broke rests on itself instead of
    // sinking through, while rough-cut pieces whose hulls cross wait until they have moved apart.
    for (size_t i = 0; i < w.linkDescs.size(); ++i)
    {
        LinkRecord& r = w.linkState[i];
        const LinkDesc& l = w.linkDescs[i];
        if (r.brokenStep < 0 || r.colliding || !w.validPair(l.a, l.b))
            continue;
        if (scene.overlap(l.a, l.b) <= kRejoinOverlap)
        {
            r.colliding = 1;
            scene.setCollides(l.a, l.b, true);
        }
    }
}

void RigidWorld::save(std::vector<uint8_t>& out, bool full) const
{
    const Impl& w = *mImpl;
    std::vector<BodyRecord> bodies(w.pieceCount);
    for (size_t i = 0; i < w.pieceCount; ++i)
    {
        BodyRecord& r = bodies[i];
        const ph::RawState s = w.scene->raw(int(i));
        r.position[0] = s.position.x;
        r.position[1] = s.position.y;
        r.position[2] = s.position.z;
        r.rotation[0] = s.rotation.x;
        r.rotation[1] = s.rotation.y;
        r.rotation[2] = s.rotation.z;
        r.rotation[3] = s.rotation.w;
        r.velocity[0] = s.velocity.x;
        r.velocity[1] = s.velocity.y;
        r.velocity[2] = s.velocity.z;
        r.angularVelocity[0] = s.angularVelocity.x;
        r.angularVelocity[1] = s.angularVelocity.y;
        r.angularVelocity[2] = s.angularVelocity.z;
        r.active = w.active[i];
    }
    const std::vector<uint8_t> jolt = full ? w.scene->save() : std::vector<uint8_t>();
    StateHeader h;
    h.partial = full ? 0 : 1;
    h.serial = w.serial;
    h.step = w.step;
    h.startFrame = w.settings.startFrame;
    h.substeps = w.settings.substeps;
    h.origin[0] = w.origin.x;
    h.origin[1] = w.origin.y;
    h.origin[2] = w.origin.z;
    writeState(out, h, bodies, w.linkState, jolt);
}
} // namespace destruction
