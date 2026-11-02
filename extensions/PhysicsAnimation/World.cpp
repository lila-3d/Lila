// Jolt Physics behind World: shapes, the system, stepping and state. Single-threaded, so the order of every solver
// operation is fixed and the results are bitwise reproducible; Jolt itself is built cross-platform deterministic.

#include "World.h"

#include <lila/Simulation.h>

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/GroupFilterTable.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/PlaneShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/StateRecorderImpl.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace physanim
{
namespace
{
constexpr double kM = 0.01; // cm to m
constexpr double kPi = 3.14159265358979323846;

namespace layers
{
constexpr JPH::ObjectLayer kMoving = 0;
constexpr JPH::ObjectLayer kStatic = 1;
constexpr JPH::ObjectLayer kNone = 2;
} // namespace layers

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface
{
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer l) const override { return JPH::BroadPhaseLayer(l == layers::kStatic ? 1 : 0); }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer l) const override { return l == JPH::BroadPhaseLayer(1) ? "static" : "moving"; }
#endif
};

class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer l, JPH::BroadPhaseLayer bp) const override
    {
        if (l == layers::kNone)
            return false;
        return l == layers::kStatic ? bp == JPH::BroadPhaseLayer(0) : true;
    }
};

class LayerPairs final : public JPH::ObjectLayerPairFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override
    {
        if (a == layers::kNone || b == layers::kNone)
            return false;
        return a == layers::kMoving || b == layers::kMoving;
    }
};

/** Jolt's per-library state, torn down when the library unloads, after every world and shape is gone. */
struct Runtime
{
    std::unique_ptr<JPH::TempAllocatorImpl> temp;
    std::unique_ptr<JPH::JobSystemSingleThreaded> jobs;
    bool ready = false;
    ~Runtime() { shutdownJolt(); }
} gRuntime;

Runtime& runtime() { return gRuntime; }

JPH::Vec3 toJ(const glm::dvec3& v) { return JPH::Vec3(float(v.x), float(v.y), float(v.z)); }
JPH::Quat toJ(const glm::dquat& q) { return JPH::Quat(float(q.x), float(q.y), float(q.z), float(q.w)); }
JPH::Quat toJ(const glm::quat& q) { return JPH::Quat(q.x, q.y, q.z, q.w); }
glm::dvec3 fromJ(JPH::Vec3Arg v) { return {double(v.GetX()), double(v.GetY()), double(v.GetZ())}; }
glm::dquat fromJ(JPH::QuatArg q) { return glm::dquat(double(q.GetW()), double(q.GetX()), double(q.GetY()), double(q.GetZ())); }
glm::vec3 floatsOf(JPH::Vec3Arg v) { return {v.GetX(), v.GetY(), v.GetZ()}; }
glm::quat floatsOf(JPH::QuatArg q) { return glm::quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ()); }

glm::dvec3 rotationVector(const glm::dquat& q0)
{
    glm::dquat q = q0.w < 0.0 ? -q0 : q0;
    const double s = glm::length(glm::dvec3(q.x, q.y, q.z));
    if (s < 1e-12)
        return glm::dvec3(q.x, q.y, q.z) * 2.0;
    return glm::dvec3(q.x, q.y, q.z) * (2.0 * std::atan2(s, q.w) / s);
}

JPH::RefConst<JPH::Shape> create(const JPH::ShapeSettings& s)
{
    JPH::ShapeSettings::ShapeResult r = s.Create();
    if (r.HasError())
        throw std::invalid_argument(std::string("physics shape: ") + r.GetError().c_str());
    return r.Get();
}

JPH::RefConst<JPH::Shape> bodyShape(const BodyDef& b)
{
    const ShapeDesc& s = b.shape;
    JPH::Ref<JPH::ConvexShapeSettings> inner;
    glm::dquat extra(1, 0, 0, 0);
    const float r = float(std::max(s.radius, 0.05) * kM);
    switch (s.kind)
    {
    case ShapeKind::Capsule:
        inner = new JPH::CapsuleShapeSettings(float(std::max(s.length * 0.5, 1e-3) * kM), r);
        // Jolt's capsules lie along Y; a marker's along X.
        extra = glm::angleAxis(-kPi * 0.5, glm::dvec3(0, 0, 1));
        break;
    case ShapeKind::Box:
    {
        const glm::dvec3 half = glm::max(s.extents * 0.5, glm::dvec3(0.05)) * kM;
        const double smallest = std::min({half.x, half.y, half.z});
        inner = new JPH::BoxShapeSettings(toJ(half), float(std::min(0.01, smallest * 0.25)));
        break;
    }
    case ShapeKind::Sphere: inner = new JPH::SphereShapeSettings(r); break;
    case ShapeKind::ConvexHull:
    {
        JPH::Array<JPH::Vec3> points;
        for (const glm::dvec3& p : s.hull)
            points.push_back(toJ(p * kM));
        if (points.size() >= 4)
        {
            auto* hull = new JPH::ConvexHullShapeSettings(points, 0.002f);
            inner = hull;
            if (!hull->Create().HasError())
                break;
        }
        inner = new JPH::SphereShapeSettings(r);
        break;
    }
    }
    inner->SetDensity(float(std::max(b.density, 1e-3)));
    JPH::RotatedTranslatedShapeSettings placed(toJ(s.offset * kM), toJ(glm::normalize(s.rotation * extra)), inner);
    return create(placed);
}

JPH::RefConst<JPH::Shape> colliderShape(const ColliderDef& c)
{
    JPH::TriangleList tris;
    const std::vector<glm::dvec3>& t = *c.triangles;
    for (size_t i = 0; i + 2 < t.size(); i += 3)
    {
        const glm::dvec3 a = t[i] * kM, b = t[i + 1] * kM, d = t[i + 2] * kM;
        if (glm::length(glm::cross(b - a, d - a)) < 1e-12)
            continue;
        tris.push_back(JPH::Triangle(toJ(a), toJ(b), toJ(d)));
    }
    if (tris.empty())
        return create(JPH::SphereShapeSettings(0.001f));
    return create(JPH::MeshShapeSettings(tris));
}

/** Implicit spring-damper acceleration toward a target: stable at any stiffness for the step it is taken over. */
double springGain(double frequency, double damping, double dt, double& dampGain)
{
    const double w = 2.0 * kPi * std::max(frequency, 0.0);
    const double k = w * w, c = 2.0 * std::max(damping, 0.0) * w;
    const double d = 1.0 + c * dt + k * dt * dt;
    dampGain = c / d;
    return k / d;
}
} // namespace

void initJolt()
{
    if (gRuntime.ready)
        return;
    JPH::RegisterDefaultAllocator();
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
    runtime().temp = std::make_unique<JPH::TempAllocatorImpl>(8 * 1024 * 1024);
    runtime().jobs = std::make_unique<JPH::JobSystemSingleThreaded>(JPH::cMaxPhysicsJobs);
    gRuntime.ready = true;
}

void shutdownJolt()
{
    if (!gRuntime.ready)
        return;
    runtime().jobs.reset();
    runtime().temp.reset();
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
    gRuntime.ready = false;
}

struct Shapes::Impl
{
    std::vector<JPH::RefConst<JPH::Shape>> bodies;
    std::vector<JPH::RefConst<JPH::Shape>> colliders;
    JPH::RefConst<JPH::Shape> ground;
};

Shapes::Shapes(const RigDef& rig) : impl(std::make_unique<Impl>())
{
    initJolt();
    for (const BodyDef& b : rig.bodies)
        impl->bodies.push_back(bodyShape(b));
    for (const ColliderDef& c : rig.colliders)
        impl->colliders.push_back(colliderShape(c));
    impl->ground = create(JPH::PlaneShapeSettings(JPH::Plane(JPH::Vec3(0, 1, 0), float(-rig.groundHeight * kM)), nullptr, 1000.0f));
}

Shapes::~Shapes() = default;

struct World::Impl
{
    const RigDef& rig;
    const Shapes::Impl& shapes;
    WorldOptions options;
    // The filters outlive the system that points at them.
    BroadPhaseLayers bp;
    ObjectVsBroadPhase objectVsBp;
    LayerPairs pairs;
    JPH::Ref<JPH::GroupFilterTable> groups;
    JPH::PhysicsSystem system;
    std::vector<JPH::BodyID> bodies, colliders;
    JPH::BodyID ground;
    /** m/s: the velocity spring damping is measured against (a carrier's), so riding along costs no drag. */
    glm::dvec3 frameVelocity{0.0};
    std::vector<JPH::Ref<JPH::SwingTwistConstraint>> joints;
    int grabBody = -1;
    glm::dvec3 grabLocal{0.0}, grabTarget{0.0};

    Impl(const RigDef& r, const Shapes::Impl& s, const WorldOptions& o) : rig(r), shapes(s), options(o) {}

    JPH::BodyInterface& bi() { return system.GetBodyInterfaceNoLock(); }

    glm::dvec3 positionCm(size_t i) { return fromJ(bi().GetPosition(bodies[i])) / kM; }
    glm::dquat rotation(size_t i) { return fromJ(bi().GetRotation(bodies[i])); }

    /** Pushes a body toward a world pose with a spring, as force at the centre of mass and torque. */
    void spring(size_t i, const glm::dvec3* targetCm, const glm::dquat* targetRot, double frequency, double damping, double dt)
    {
        if (frequency <= 0.0)
            return;
        JPH::BodyLockRead lock(system.GetBodyLockInterfaceNoLock(), bodies[i]);
        if (!lock.Succeeded())
            return;
        const JPH::Body& body = lock.GetBody();
        if (!body.IsDynamic())
            return;
        const JPH::MotionProperties* mp = body.GetMotionProperties();
        double c = 0.0;
        const double k = springGain(frequency, damping, dt, c);
        if (targetCm)
        {
            const glm::dvec3 x = fromJ(body.GetPosition()), v = fromJ(body.GetLinearVelocity()) - frameVelocity;
            const glm::dvec3 a = k * (*targetCm * kM - x) - c * v;
            const double mass = 1.0 / std::max(double(mp->GetInverseMass()), 1e-12);
            bi().AddForce(bodies[i], toJ(a * mass), JPH::EActivation::Activate);
        }
        if (targetRot)
        {
            const glm::dquat q = fromJ(body.GetRotation());
            const glm::dvec3 e = rotationVector(*targetRot * glm::conjugate(q));
            const glm::dvec3 alpha = k * e - c * fromJ(body.GetAngularVelocity());
            const JPH::Mat44 inertia = mp->GetInverseInertiaForRotation(JPH::Mat44::sRotation(body.GetRotation())).Inversed3x3();
            bi().AddTorque(bodies[i], inertia.Multiply3x3(toJ(alpha)), JPH::EActivation::Activate);
        }
    }

    void accelerate(size_t i, const glm::dvec3& a)
    {
        JPH::BodyLockRead lock(system.GetBodyLockInterfaceNoLock(), bodies[i]);
        if (!lock.Succeeded() || !lock.GetBody().IsDynamic())
            return;
        const double mass = 1.0 / std::max(double(lock.GetBody().GetMotionProperties()->GetInverseMass()), 1e-12);
        bi().AddForce(bodies[i], toJ(a * mass), JPH::EActivation::Activate);
    }
};

World::World(const RigDef& rig, const Shapes& shapes, const std::vector<BodyState>& bodies, const std::vector<BodyState>& colliders,
             const std::vector<std::string>& constraintStates, const WorldOptions& options)
    : mImpl(std::make_unique<Impl>(rig, *shapes.impl, options))
{
    initJolt();
    Impl& w = *mImpl;
    const size_t n = rig.bodies.size();
    const JPH::uint maxBodies = JPH::uint(n + rig.colliders.size() + 4);
    w.system.Init(maxBodies, 0, 4096, 4096, w.bp, w.objectVsBp, w.pairs);
    JPH::PhysicsSettings ps;
    ps.mNumVelocitySteps = JPH::uint(std::clamp(rig.velocitySteps, 2, 64));
    ps.mNumPositionSteps = JPH::uint(std::clamp(rig.positionSteps, 1, 32));
    ps.mAllowSleeping = false;
    ps.mDeterministicSimulation = true;
    ps.mUseLargeIslandSplitter = false;
    w.system.SetPhysicsSettings(ps);
    w.system.SetGravity(options.live ? JPH::Vec3::sZero() : toJ(rig.gravity * kM));

    JPH::BodyInterface& bi = w.bi();
    if (rig.ground)
    {
        JPH::BodyCreationSettings g(w.shapes.ground, JPH::RVec3::sZero(), JPH::Quat::sIdentity(), JPH::EMotionType::Static, layers::kStatic);
        g.mFriction = float(rig.groundFriction);
        w.ground = bi.CreateAndAddBody(g, JPH::EActivation::DontActivate);
    }
    for (size_t c = 0; c < rig.colliders.size(); ++c)
    {
        const BodyState& s = colliders[c];
        JPH::BodyCreationSettings cs(w.shapes.colliders[c], JPH::RVec3(toJ(glm::dvec3(s.position))), toJ(s.rotation), JPH::EMotionType::Kinematic,
                                     layers::kStatic);
        cs.mFriction = float(rig.colliders[c].friction);
        cs.mRestitution = float(rig.colliders[c].restitution);
        cs.mAllowSleeping = false;
        w.colliders.push_back(bi.CreateAndAddBody(cs, JPH::EActivation::Activate));
    }

    w.groups = new JPH::GroupFilterTable(JPH::uint(std::max<size_t>(n, 1)));
    for (size_t i = 0; i < n; ++i)
        for (size_t j = 0; j < i; ++j)
        {
            const BodyDef &a = rig.bodies[i], &b = rig.bodies[j];
            const bool related = a.parent == int(j) || b.parent == int(i);
            const bool grouped = a.group > 0 && a.group == b.group;
            if (!rig.selfCollision || related || grouped)
                w.groups->DisableCollision(JPH::CollisionGroup::SubGroupID(i), JPH::CollisionGroup::SubGroupID(j));
        }

    for (size_t i = 0; i < n; ++i)
    {
        const BodyDef& b = rig.bodies[i];
        const BodyState& s = bodies[i];
        const bool kinematic = b.behaviour == Behaviour::Kinematic;
        JPH::BodyCreationSettings bs(w.shapes.bodies[i], JPH::RVec3(toJ(glm::dvec3(s.position))), toJ(s.rotation),
                                     kinematic ? JPH::EMotionType::Kinematic : JPH::EMotionType::Dynamic,
                                     b.group < 0 ? layers::kNone : layers::kMoving);
        bs.mLinearVelocity = toJ(glm::dvec3(s.velocity));
        bs.mAngularVelocity = toJ(glm::dvec3(s.angularVelocity));
        bs.mFriction = float(b.friction);
        bs.mRestitution = float(b.restitution);
        bs.mLinearDamping = float(b.linearDamping + (options.live ? options.liveDamping : 0.0));
        bs.mAngularDamping = float(b.angularDamping + (options.live ? options.liveDamping : 0.0));
        bs.mAllowSleeping = false;
        bs.mCollisionGroup = JPH::CollisionGroup(w.groups, 0, JPH::CollisionGroup::SubGroupID(i));
        if (b.mass > 0.0)
        {
            bs.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            bs.mMassPropertiesOverride.mMass = float(b.mass);
        }
        w.bodies.push_back(bi.CreateAndAddBody(bs, JPH::EActivation::Activate));
    }

    w.joints.resize(n);
    for (size_t i = 0; i < n; ++i)
    {
        const BodyDef& b = rig.bodies[i];
        if (b.parent < 0)
            continue;
        const BodyState &p = bodies[size_t(b.parent)], &c = bodies[i];
        const glm::dquat pq = glm::dquat(p.rotation), cq = glm::dquat(c.rotation);
        const glm::dquat f1 = pq * b.frameInParent, f2 = cq * b.frameInChild;
        JPH::SwingTwistConstraintSettings st;
        st.mSpace = JPH::EConstraintSpace::WorldSpace;
        st.mPosition1 = JPH::RVec3(toJ(glm::dvec3(p.position) + pq * (b.anchorInParent * kM)));
        st.mPosition2 = JPH::RVec3(toJ(glm::dvec3(c.position)));
        st.mTwistAxis1 = toJ(f1 * glm::dvec3(1, 0, 0)).Normalized();
        // Jolt's constraint space is twist X, normal Y, plane Z: the plane axis is the frames' Z.
        st.mPlaneAxis1 = toJ(f1 * glm::dvec3(0, 0, 1)).Normalized();
        st.mTwistAxis2 = toJ(f2 * glm::dvec3(1, 0, 0)).Normalized();
        st.mPlaneAxis2 = toJ(f2 * glm::dvec3(0, 0, 1)).Normalized();
        const double open = kPi;
        st.mPlaneHalfConeAngle = float(std::clamp(b.limited ? b.swing1 : open, 0.01, kPi));
        st.mNormalHalfConeAngle = float(std::clamp(b.limited ? b.swing2 : open, 0.01, kPi));
        st.mTwistMinAngle = float(-std::clamp(b.limited ? b.twist : open, 0.01, kPi));
        st.mTwistMaxAngle = float(std::clamp(b.limited ? b.twist : open, 0.01, kPi));
        auto* joint = static_cast<JPH::SwingTwistConstraint*>(bi.CreateConstraint(&st, w.bodies[size_t(b.parent)], w.bodies[i]));
        if (!joint)
            continue;
        if (i < constraintStates.size() && !constraintStates[i].empty())
        {
            JPH::StateRecorderImpl rec;
            rec.WriteBytes(constraintStates[i].data(), constraintStates[i].size());
            rec.Rewind();
            joint->RestoreState(rec);
        }
        w.system.AddConstraint(joint);
        w.joints[i] = joint;
    }

    // Restored impulses only warm-start when the system believes it stepped before, by the same dt.
    if (options.previousStep > 0.0 && std::any_of(constraintStates.begin(), constraintStates.end(), [](const std::string& s) { return !s.empty(); }))
    {
        JPH::StateRecorderImpl rec;
        rec.Write(JPH::EStateRecorderState::Global);
        rec.Write(float(options.previousStep));
        rec.Write(w.system.GetGravity());
        rec.Rewind();
        w.system.RestoreState(rec);
    }
}

World::~World()
{
    if (!mImpl)
        return;
    Impl& w = *mImpl;
    for (auto& j : w.joints)
        if (j)
            w.system.RemoveConstraint(j);
    JPH::BodyInterface& bi = w.bi();
    auto drop = [&](JPH::BodyID id) {
        if (id.IsInvalid())
            return;
        bi.RemoveBody(id);
        bi.DestroyBody(id);
    };
    for (JPH::BodyID id : w.bodies)
        drop(id);
    for (JPH::BodyID id : w.colliders)
        drop(id);
    drop(w.ground);
}

void World::step(const Drive& drive, double dt)
{
    Impl& w = *mImpl;
    JPH::BodyInterface& bi = w.bi();
    const float fdt = float(dt);
    w.frameVelocity = drive.frameVelocity * kM;
    for (size_t c = 0; c < w.colliders.size() && c < drive.colliders.size(); ++c)
    {
        const glm::dmat4 m = rigid(drive.colliders[c]);
        bi.MoveKinematic(w.colliders[c], JPH::RVec3(toJ(glm::dvec3(m[3]) * kM)), toJ(glm::normalize(glm::quat_cast(glm::dmat3(m)))), fdt);
    }

    const RigDef& rig = w.rig;
    for (size_t i = 0; i < rig.bodies.size() && i < drive.bodies.size(); ++i)
    {
        const BodyDef& b = rig.bodies[i];
        const glm::dmat4 anim = rigid(drive.bodies[i].animated);
        const glm::dvec3 targetPos(anim[3]);
        const glm::dquat targetRot = glm::normalize(glm::quat_cast(glm::dmat3(anim)));
        if (b.behaviour == Behaviour::Kinematic)
        {
            bi.MoveKinematic(w.bodies[i], JPH::RVec3(toJ(targetPos * kM)), toJ(targetRot), fdt);
            continue;
        }
        const bool poseMatch = b.behaviour == Behaviour::PoseMatch && !w.options.live;
        const double stiffness = drive.bodies[i].stiffness, damping = drive.bodies[i].damping;
        if (JPH::SwingTwistConstraint* joint = w.joints[i].GetPtr())
        {
            const bool motor = poseMatch && b.space != PoseSpace::World && stiffness > 0.0;
            const JPH::EMotorState state = motor ? JPH::EMotorState::Position : JPH::EMotorState::Off;
            joint->SetSwingMotorState(state);
            joint->SetTwistMotorState(state);
            if (motor)
            {
                const glm::dmat4 parentAnim = rigid(drive.bodies[size_t(b.parent)].animated);
                const glm::dquat parentRot = glm::normalize(glm::quat_cast(glm::dmat3(parentAnim)));
                const glm::dquat target = glm::conjugate(b.frameInParent) * glm::conjugate(parentRot) * targetRot * b.frameInChild;
                joint->SetTargetOrientationCS(toJ(glm::normalize(target)));
                const JPH::MotorSettings ms(float(stiffness), float(std::max(damping, 0.0)));
                joint->GetSwingMotorSettings() = ms;
                joint->GetTwistMotorSettings() = ms;
            }
        }
        // World space matching, and the local kind's stand-in on a body with no parent to be local to.
        if (poseMatch && (b.space != PoseSpace::Local || b.parent < 0))
        {
            // In a carrier's space the target is where it sits relative to the carrier at the step's start.
            const glm::dvec3 relative = targetPos - drive.frameShift;
            w.spring(i, &relative, &targetRot, stiffness, damping, dt);
        }
    }

    for (size_t p = 0; p < rig.pins.size() && p < drive.pins.size(); ++p)
    {
        const PinDef& pin = rig.pins[p];
        if (pin.body < 0)
            continue;
        const glm::dmat4 m = rigid(drive.pins[p].world);
        const glm::dvec3 at(m[3]);
        const glm::dquat rot = glm::normalize(glm::quat_cast(glm::dmat3(m)));
        w.spring(size_t(pin.body), &at, pin.orient ? &rot : nullptr, drive.pins[p].stiffness, pin.damping, dt);
    }

    if (!w.options.live)
        for (size_t f = 0; f < rig.fields.size() && f < drive.fieldMagnitudes.size(); ++f)
        {
            const FieldDef& fd = rig.fields[f];
            const double mag = drive.fieldMagnitudes[f];
            const double len = glm::length(fd.direction);
            const glm::dvec3 dir = len > 1e-12 ? fd.direction / len : glm::dvec3(1, 0, 0);
            double x = drive.seconds * std::max(fd.frequency, 1e-6);
            int64_t period = 0;
            if (drive.period > 0.0)
            {
                period = std::max<int64_t>(1, std::llround(drive.period * std::max(fd.frequency, 1e-6)));
                x = drive.seconds / drive.period * double(period);
            }
            for (size_t i = 0; i < w.bodies.size(); ++i)
            {
                const glm::dvec3 v = fromJ(bi.GetLinearVelocity(w.bodies[i]));
                glm::dvec3 a(0.0);
                switch (fd.kind)
                {
                case FieldKind::Gravity: a = dir * (mag * kM); break;
                case FieldKind::Wind: a = (dir * (mag * kM) - v); break;
                case FieldKind::Turbulence: a = lila::scene::sim::noise3(fd.seed * 1000003ull + uint64_t(i) + 1, x, period) * (mag * kM); break;
                case FieldKind::Drag: a = -v * mag; break;
                }
                w.accelerate(i, a);
            }
        }
    if (!w.options.live && drive.carried != glm::dvec3(0.0))
        for (size_t i = 0; i < w.bodies.size(); ++i)
            w.accelerate(i, drive.carried * kM);

    if (w.grabBody >= 0 && size_t(w.grabBody) < w.bodies.size())
    {
        JPH::BodyLockRead lock(w.system.GetBodyLockInterfaceNoLock(), w.bodies[size_t(w.grabBody)]);
        if (lock.Succeeded() && lock.GetBody().IsDynamic())
        {
            const JPH::Body& body = lock.GetBody();
            const glm::dvec3 point = fromJ(body.GetPosition()) + fromJ(body.GetRotation()) * (w.grabLocal * kM);
            const glm::dvec3 vp = fromJ(body.GetPointVelocity(JPH::RVec3(toJ(point))));
            double c = 0.0;
            const double k = springGain(4.0, 1.0, dt, c);
            const double mass = 1.0 / std::max(double(body.GetMotionProperties()->GetInverseMass()), 1e-12);
            const glm::dvec3 force = (k * (w.grabTarget * kM - point) - c * vp) * mass * 4.0;
            const JPH::BodyID id = body.GetID();
            lock.ReleaseLock();
            bi.AddForce(id, toJ(force), JPH::RVec3(toJ(point)), JPH::EActivation::Activate);
        }
    }

    w.system.Update(fdt, 1, runtime().temp.get(), runtime().jobs.get());
}

BodyState World::body(size_t i) const
{
    Impl& w = *mImpl;
    JPH::BodyInterface& bi = w.bi();
    const JPH::BodyID id = w.bodies[i];
    return {floatsOf(JPH::Vec3(bi.GetPosition(id))), floatsOf(bi.GetRotation(id)), floatsOf(bi.GetLinearVelocity(id)), floatsOf(bi.GetAngularVelocity(id))};
}

BodyState World::collider(size_t i) const
{
    Impl& w = *mImpl;
    JPH::BodyInterface& bi = w.bi();
    const JPH::BodyID id = w.colliders[i];
    return {floatsOf(JPH::Vec3(bi.GetPosition(id))), floatsOf(bi.GetRotation(id)), floatsOf(bi.GetLinearVelocity(id)), floatsOf(bi.GetAngularVelocity(id))};
}

std::vector<std::string> World::constraintStates() const
{
    std::vector<std::string> out(mImpl->joints.size());
    for (size_t i = 0; i < out.size(); ++i)
        if (const JPH::SwingTwistConstraint* j = mImpl->joints[i].GetPtr())
        {
            JPH::StateRecorderImpl rec;
            j->SaveState(rec);
            out[i] = rec.GetData();
        }
    return out;
}

void World::setGrab(int body, const glm::dvec3& localPoint, const glm::dvec3& target)
{
    mImpl->grabBody = body;
    mImpl->grabLocal = localPoint;
    mImpl->grabTarget = target;
}

size_t World::constraintStateSize()
{
    static const size_t size = [] {
        initJolt();
        JPH::SwingTwistConstraintSettings st;
        JPH::Ref<JPH::TwoBodyConstraint> c = st.Create(JPH::Body::sFixedToWorld, JPH::Body::sFixedToWorld);
        JPH::StateRecorderImpl rec;
        c->SaveState(rec);
        return rec.GetData().size();
    }();
    return size;
}

std::vector<glm::dvec3> hullTriangles(const std::vector<glm::dvec3>& points)
{
    std::vector<glm::dvec3> out;
    if (points.size() < 4)
        return out;
    initJolt();
    JPH::Array<JPH::Vec3> pts;
    for (const glm::dvec3& p : points)
        pts.push_back(toJ(p));
    JPH::ConvexHullShapeSettings settings(pts, 0.0f);
    JPH::ShapeSettings::ShapeResult r = settings.Create();
    if (r.HasError())
        return out;
    const auto* hull = static_cast<const JPH::ConvexHullShape*>(r.Get().GetPtr());
    std::vector<JPH::uint> face(64);
    for (JPH::uint f = 0; f < hull->GetNumFaces(); ++f)
    {
        face.resize(std::max<size_t>(face.size(), hull->GetNumVerticesInFace(f)));
        const JPH::uint count = hull->GetFaceVertices(f, JPH::uint(face.size()), face.data());
        for (JPH::uint k = 1; k + 1 < count; ++k)
            for (JPH::uint v : {face[0], face[k], face[k + 1]})
                out.push_back(fromJ(hull->GetPoint(v) + hull->GetCenterOfMass()));
    }
    return out;
}
} // namespace physanim
