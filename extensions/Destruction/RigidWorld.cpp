// Jolt Physics behind RigidWorld. Jolt is built cross-platform deterministic; its results depend on the order of API
// calls, so every call here happens in a fixed order.

#include "RigidWorld.h"

#include <lila/Simulation.h>

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystem.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/GroupFilterTable.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/PlaneShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/StateRecorderImpl.h>
#include <Jolt/RegisterTypes.h>

#include <tbb/task_arena.h>
#include <tbb/task_group.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cmath>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace destruction
{
namespace
{
constexpr double kM = 0.01; // cm to m

namespace layers
{
constexpr JPH::ObjectLayer kMoving = 0;
constexpr JPH::ObjectLayer kStatic = 1;
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
        return l == layers::kStatic ? bp == JPH::BroadPhaseLayer(0) : true;
    }
};

class LayerPairs final : public JPH::ObjectLayerPairFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override { return a == layers::kMoving || b == layers::kMoving; }
};

/**
 * Jolt's jobs on Lila's TBB pool. Jolt's results don't depend on how its jobs are scheduled, only on the order of API
 * calls, so this stays deterministic. Jobs whose dependencies are met run as TBB tasks; a barrier counts the jobs added
 * to it and waits on the task group until they have all run.
 */
class TbbJobSystem final : public JPH::JobSystem
{
public:
    ~TbbJobSystem() noexcept override { mTasks->wait(); }

    int GetMaxConcurrency() const override { return tbb::this_task_arena::max_concurrency(); }

    JobHandle CreateJob(const char* name, JPH::ColorArg color, const JobFunction& fn, JPH::uint32 dependencies = 0) override
    {
        Job* job = new Job(name, color, this, fn, dependencies);
        JobHandle handle(job);
        if (dependencies == 0)
            QueueJob(job);
        return handle;
    }

    Barrier* CreateBarrier() override { return new BarrierImpl(); }
    void DestroyBarrier(Barrier* barrier) override { delete static_cast<BarrierImpl*>(barrier); }

    void WaitForJobs(Barrier* barrier) override
    {
        auto* b = static_cast<BarrierImpl*>(barrier);
        while (b->pending.load(std::memory_order_acquire) > 0)
        {
            mTasks->wait();
            if (b->pending.load(std::memory_order_acquire) > 0)
                std::this_thread::yield();
        }
        std::lock_guard lock(b->mutex);
        b->jobs.clear();
    }

protected:
    class BarrierImpl final : public Barrier
    {
    public:
        void AddJob(const JobHandle& handle) override
        {
            if (!handle.GetPtr()->SetBarrier(this))
                return;
            pending.fetch_add(1, std::memory_order_acq_rel);
            std::lock_guard lock(mutex);
            jobs.push_back(handle);
        }
        void AddJobs(const JobHandle* handles, JPH::uint count) override
        {
            for (JPH::uint k = 0; k < count; ++k)
                AddJob(handles[k]);
        }

        std::atomic<int> pending{0};
        std::mutex mutex;
        std::vector<JobHandle> jobs; // kept alive until the wait is over

    protected:
        void OnJobFinished(Job*) override { pending.fetch_sub(1, std::memory_order_acq_rel); }
    };

    void QueueJob(Job* job) override
    {
        job->AddRef();
        mTasks->run([job] {
            job->Execute();
            job->Release();
        });
    }

    void QueueJobs(Job** jobs, JPH::uint count) override
    {
        for (JPH::uint k = 0; k < count; ++k)
            QueueJob(jobs[k]);
    }

    void FreeJob(Job* job) override { delete job; }

private:
    std::unique_ptr<tbb::task_group> mTasks = std::make_unique<tbb::task_group>();
};

/** Jolt's per-library state, torn down when the library unloads. */
struct Runtime
{
    std::unique_ptr<JPH::TempAllocatorImpl> temp;
    std::unique_ptr<JPH::JobSystem> jobs, serialJobs;
    bool ready = false;
    ~Runtime() { shutdownJolt(); }
} gRuntime;

std::atomic<uint64_t> gSerial{0};

JPH::Vec3 toJ(const glm::dvec3& v) { return JPH::Vec3(float(v.x), float(v.y), float(v.z)); }
JPH::Quat toJ(const glm::dquat& q) { return JPH::Quat(float(q.x), float(q.y), float(q.z), float(q.w)).Normalized(); }

JPH::RefConst<JPH::Shape> create(const JPH::ShapeSettings& s, const char* what)
{
    JPH::ShapeSettings::ShapeResult r = s.Create();
    if (r.HasError())
        throw std::invalid_argument(std::string(what) + ": " + r.GetError().c_str());
    return r.Get();
}

/** A hull for the points, or a small sphere when they're degenerate (a flat or empty piece). */
JPH::RefConst<JPH::Shape> hullShape(const std::vector<glm::dvec3>& points, double density)
{
    JPH::Array<JPH::Vec3> pts;
    pts.reserve(points.size());
    glm::dvec3 lo(1e300), hi(-1e300);
    for (const glm::dvec3& p : points)
    {
        pts.push_back(toJ(p * kM));
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    const double size = points.empty() ? 0.0 : glm::length(hi - lo) * kM;
    JPH::RefConst<JPH::Shape> shape;
    if (pts.size() >= 4)
    {
        JPH::ConvexHullShapeSettings hs(pts, float(std::min(0.005, size * 0.05)));
        hs.SetDensity(float(std::max(density, 1e-3)));
        JPH::ShapeSettings::ShapeResult r = hs.Create();
        if (!r.HasError())
            shape = r.Get();
    }
    if (!shape)
    {
        JPH::SphereShapeSettings ss(float(std::max(size * 0.5, 0.005)));
        ss.SetDensity(float(std::max(density, 1e-3)));
        shape = create(ss, "piece shape");
    }
    return shape;
}

JPH::RefConst<JPH::Shape> meshShape(const std::vector<glm::dvec3>& t)
{
    JPH::TriangleList tris;
    for (size_t i = 0; i + 2 < t.size(); i += 3)
    {
        const glm::dvec3 a = t[i] * kM, b = t[i + 1] * kM, d = t[i + 2] * kM;
        if (glm::length(glm::cross(b - a, d - a)) < 1e-12)
            continue;
        tris.push_back(JPH::Triangle(toJ(a), toJ(b), toJ(d)));
    }
    if (tris.empty())
        return create(JPH::SphereShapeSettings(0.001f), "collider shape");
    return create(JPH::MeshShapeSettings(tris), "collider shape");
}
} // namespace

void initJolt()
{
    if (gRuntime.ready)
        return;
    JPH::RegisterDefaultAllocator();
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
    gRuntime.temp = std::make_unique<JPH::TempAllocatorImpl>(64 * 1024 * 1024);
    gRuntime.jobs = std::make_unique<TbbJobSystem>();
    gRuntime.serialJobs = std::make_unique<JPH::JobSystemSingleThreaded>(JPH::cMaxPhysicsJobs);
    gRuntime.ready = true;
}

void shutdownJolt()
{
    if (!gRuntime.ready)
        return;
    gRuntime.jobs.reset();
    gRuntime.serialJobs.reset();
    gRuntime.temp.reset();
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
    gRuntime.ready = false;
}

struct RigidWorld::Impl
{
    /** Single-threaded when LILA_PHYSICS_THREADS=1 at creation, which tests use to show threading changes nothing. */
    JPH::JobSystem* jobs = nullptr;
    // The filters outlive the system that points at them.
    BroadPhaseLayers bp;
    ObjectVsBroadPhase objectVsBp;
    LayerPairs pairs;
    JPH::Ref<JPH::GroupFilterTable> groups;
    JPH::PhysicsSystem system;
    std::vector<JPH::BodyID> pieces, colliders;
    JPH::BodyID ground;
    std::vector<JPH::Ref<JPH::TwoBodyConstraint>> links;
    std::vector<LinkDesc> linkDescs;
    std::vector<uint8_t> moving; // per collider
    std::vector<LinkRecord> linkState;
    std::vector<uint8_t> active;
    Settings settings;
    size_t pieceCount = 0, colliderCount = 0;
    uint64_t serial = 0;
    int64_t step = 0;

    JPH::BodyInterface& bi() { return system.GetBodyInterfaceNoLock(); }

    void setActive(size_t i, bool on)
    {
        active[i] = on ? 1 : 0;
        bi().SetMotionType(pieces[i], on ? JPH::EMotionType::Dynamic : JPH::EMotionType::Kinematic,
                           on ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
    }

    void enablePair(int a, int b, bool on)
    {
        if (a < 0 || b < 0 || a == b)
            return;
        if (on)
            groups->EnableCollision(JPH::CollisionGroup::SubGroupID(a), JPH::CollisionGroup::SubGroupID(b));
        else
            groups->DisableCollision(JPH::CollisionGroup::SubGroupID(a), JPH::CollisionGroup::SubGroupID(b));
    }

    /** Applies links' broken and colliding flags to the constraints and the collision table. */
    void applyLinkState()
    {
        for (size_t i = 0; i < links.size(); ++i)
        {
            const LinkRecord& r = linkState[i];
            if (links[i])
                links[i]->SetEnabled(r.brokenStep < 0);
            enablePair(linkDescs[i].a, linkDescs[i].b, r.colliding != 0);
        }
    }
};

RigidWorld::RigidWorld(const SceneDesc& s) : mImpl(std::make_unique<Impl>())
{
    initJolt();
    Impl& w = *mImpl;
    const char* threads = std::getenv("LILA_PHYSICS_THREADS");
    w.jobs = threads && std::atoi(threads) == 1 ? gRuntime.serialJobs.get() : gRuntime.jobs.get();
    w.settings = s.settings;
    w.serial = ++gSerial;
    w.pieceCount = s.pieces.size();
    w.colliderCount = s.colliders.size();
    const size_t n = s.pieces.size();
    const JPH::uint maxBodies = JPH::uint(n + s.colliders.size() + 8);
    const JPH::uint pairs = JPH::uint(std::clamp<size_t>(n * 16, 4096, 1u << 20));
    w.system.Init(maxBodies, 0, pairs, pairs, w.bp, w.objectVsBp, w.pairs);
    JPH::PhysicsSettings ps;
    ps.mDeterministicSimulation = true;
    ps.mAllowSleeping = s.settings.sleep;
    w.system.SetPhysicsSettings(ps);
    w.system.SetGravity(toJ(s.settings.gravity * kM));

    JPH::BodyInterface& bi = w.bi();
    w.groups = new JPH::GroupFilterTable(JPH::uint(std::max<size_t>(n, 1)));
    // Pieces held together start apart in collision: their hulls touch or overlap along the cut.
    for (const LinkDesc& l : s.links)
        w.enablePair(l.a, l.b, false);

    w.active.assign(n, 0);
    for (size_t i = 0; i < n; ++i)
    {
        const PieceDesc& p = s.pieces[i];
        static const std::vector<glm::dvec3> kEmpty;
        const std::vector<glm::dvec3>& hull = p.hull ? *p.hull : kEmpty;
        JPH::RefConst<JPH::Shape> shape = hullShape(hull, p.density);
        JPH::BodyCreationSettings bs(shape, JPH::RVec3(toJ(p.position * kM)), toJ(p.rotation), JPH::EMotionType::Dynamic, layers::kMoving);
        bs.mAllowDynamicOrKinematic = true;
        bs.mLinearVelocity = toJ(p.velocity * kM);
        bs.mAngularVelocity = toJ(p.angularVelocity);
        bs.mFriction = float(p.friction);
        bs.mRestitution = float(p.bounce);
        bs.mLinearDamping = float(s.settings.damping);
        bs.mAngularDamping = float(s.settings.damping);
        bs.mAllowSleeping = s.settings.sleep;
        bs.mMotionQuality = JPH::EMotionQuality::LinearCast;
        bs.mCollisionGroup = JPH::CollisionGroup(w.groups, 0, JPH::CollisionGroup::SubGroupID(i));
        w.pieces.push_back(bi.CreateAndAddBody(bs, JPH::EActivation::Activate));
        if (!p.active)
            w.setActive(i, false);
        else
            w.active[i] = 1;
    }

    for (const ColliderDesc& c : s.colliders)
    {
        static const std::vector<glm::dvec3> kEmpty;
        JPH::BodyCreationSettings cs(meshShape(c.triangles ? *c.triangles : kEmpty), JPH::RVec3(toJ(c.position * kM)), toJ(c.rotation),
                                     c.moving ? JPH::EMotionType::Kinematic : JPH::EMotionType::Static, layers::kStatic);
        cs.mFriction = 0.6f;
        cs.mAllowSleeping = false;
        w.colliders.push_back(bi.CreateAndAddBody(cs, c.moving ? JPH::EActivation::Activate : JPH::EActivation::DontActivate));
        w.moving.push_back(c.moving ? 1 : 0);
    }
    if (s.settings.ground)
    {
        JPH::BodyCreationSettings g(create(JPH::PlaneShapeSettings(JPH::Plane(JPH::Vec3(0, 1, 0), float(-s.settings.groundHeight * kM)), nullptr,
                                                                   1000.0f),
                                           "ground"),
                                    JPH::RVec3::sZero(), JPH::Quat::sIdentity(), JPH::EMotionType::Static, layers::kStatic);
        g.mFriction = 0.7f;
        w.ground = bi.CreateAndAddBody(g, JPH::EActivation::DontActivate);
    }

    w.linkDescs = s.links;
    w.links.resize(s.links.size());
    w.linkState.assign(s.links.size(), LinkRecord{});
    for (size_t i = 0; i < s.links.size(); ++i)
    {
        const LinkDesc& l = s.links[i];
        if (l.a < 0 || size_t(l.a) >= n || l.b >= int(n) || l.a == l.b)
            continue;
        JPH::Body* ba = nullptr;
        JPH::Body* bb = nullptr;
        {
            JPH::BodyLockWrite la(w.system.GetBodyLockInterfaceNoLock(), w.pieces[size_t(l.a)]);
            ba = la.Succeeded() ? &la.GetBody() : nullptr;
        }
        if (l.b >= 0)
        {
            JPH::BodyLockWrite lb(w.system.GetBodyLockInterfaceNoLock(), w.pieces[size_t(l.b)]);
            bb = lb.Succeeded() ? &lb.GetBody() : nullptr;
        }
        else
            bb = &JPH::Body::sFixedToWorld;
        if (!ba || !bb)
            continue;
        JPH::Ref<JPH::TwoBodyConstraint> c;
        const JPH::RVec3 anchor(toJ(l.anchor * kM));
        if (l.type == LinkType::Spring)
        {
            JPH::DistanceConstraintSettings ds;
            ds.mSpace = JPH::EConstraintSpace::WorldSpace;
            ds.mPoint1 = ds.mPoint2 = anchor;
            ds.mMinDistance = 0.0f;
            ds.mMaxDistance = 0.0f;
            ds.mLimitsSpringSettings.mFrequency = float(std::max(s.settings.springStiffness, 0.01));
            ds.mLimitsSpringSettings.mDamping = float(std::max(s.settings.springDamping, 0.0));
            c = static_cast<JPH::TwoBodyConstraint*>(ds.Create(*ba, *bb));
        }
        else
        {
            JPH::FixedConstraintSettings fs;
            fs.mSpace = JPH::EConstraintSpace::WorldSpace;
            fs.mAutoDetectPoint = false;
            fs.mPoint1 = fs.mPoint2 = anchor;
            c = static_cast<JPH::TwoBodyConstraint*>(fs.Create(*ba, *bb));
        }
        w.system.AddConstraint(c);
        w.links[i] = c;
    }
}

RigidWorld::~RigidWorld()
{
    if (!mImpl)
        return;
    Impl& w = *mImpl;
    for (auto& c : w.links)
        if (c)
            w.system.RemoveConstraint(c);
    JPH::BodyInterface& bi = w.bi();
    auto drop = [&](JPH::BodyID id) {
        if (id.IsInvalid())
            return;
        bi.RemoveBody(id);
        bi.DestroyBody(id);
    };
    for (JPH::BodyID id : w.pieces)
        drop(id);
    for (JPH::BodyID id : w.colliders)
        drop(id);
    drop(w.ground);
}

bool RigidWorld::matches(const SceneDesc& s) const
{
    const Impl& w = *mImpl;
    return s.pieces.size() == w.pieceCount && s.colliders.size() == w.colliderCount && s.links.size() == w.links.size() &&
           s.settings.ground == !w.ground.IsInvalid();
}

uint64_t RigidWorld::serial() const { return mImpl->serial; }

int64_t RigidWorld::step() const { return mImpl->step; }

void RigidWorld::restore(const StateView& state)
{
    Impl& w = *mImpl;
    const StateHeader& h = state.header();
    if (h.bodies != w.pieceCount || h.links != w.links.size())
        throw std::runtime_error("the pieces changed during the simulation; it starts over from its first frame");
    if (h.partial)
        throw std::runtime_error("the simulation can't resume between the substeps of a frame");
    for (size_t i = 0; i < w.pieceCount; ++i)
    {
        const bool on = state.body(i).active != 0;
        if (on != (w.active[i] != 0))
            w.setActive(i, on);
    }
    for (size_t i = 0; i < w.links.size(); ++i)
        w.linkState[i] = state.link(i);
    w.applyLinkState();
    std::span<const uint8_t> bytes = state.jolt();
    JPH::StateRecorderImpl rec;
    rec.WriteBytes(bytes.data(), bytes.size());
    rec.Rewind();
    if (!w.system.RestoreState(rec))
        throw std::runtime_error("the physics state couldn't be restored");
    w.step = h.step;
}

namespace
{
/**
 * Blends a piece's velocities toward the ones that carry it to its target pose, by the guide weight: a critically damped
 * pull that stays stable at any step size.
 */
void guide(RigidWorld::Impl& w, size_t i, const PieceDesc& p, double dt)
{
    JPH::BodyInterface& bi = w.bi();
    const JPH::BodyID id = w.pieces[i];
    const double omega = 2.0 * 3.14159265358979323846 * w.settings.guideStiffness * p.guide;
    const double blend = 1.0 - std::exp(-omega * dt);
    const JPH::RVec3 at = bi.GetPosition(id);
    const glm::dvec3 pos(at.GetX(), at.GetY(), at.GetZ());
    const glm::dvec3 toTarget = p.position * kM - pos;
    const JPH::Vec3 v = bi.GetLinearVelocity(id);
    const glm::dvec3 vel(v.GetX(), v.GetY(), v.GetZ());
    const glm::dvec3 newVel = vel + (toTarget * omega - vel) * blend;

    const JPH::Quat q = bi.GetRotation(id);
    glm::dquat delta = glm::normalize(p.rotation * glm::conjugate(glm::dquat(q.GetW(), q.GetX(), q.GetY(), q.GetZ())));
    if (delta.w < 0.0)
        delta = -delta;
    const double s = glm::length(glm::dvec3(delta.x, delta.y, delta.z));
    const glm::dvec3 rotVec = s < 1e-12 ? glm::dvec3(0.0) : glm::dvec3(delta.x, delta.y, delta.z) * (2.0 * std::atan2(s, delta.w) / s);
    const JPH::Vec3 a = bi.GetAngularVelocity(id);
    const glm::dvec3 ang(a.GetX(), a.GetY(), a.GetZ());
    const glm::dvec3 newAng = ang + (rotVec * omega - ang) * blend;
    bi.SetLinearAndAngularVelocity(id, toJ(newVel), toJ(newAng));
}
} // namespace

void RigidWorld::advance(const SceneDesc& current, int64_t index, double dt)
{
    Impl& w = *mImpl;
    JPH::BodyInterface& bi = w.bi();
    const float fdt = float(dt);
    const size_t n = std::min(w.pieceCount, current.pieces.size());

    for (size_t i = 0; i < n; ++i)
    {
        const PieceDesc& p = current.pieces[i];
        if (!w.active[i] && p.active)
            w.setActive(i, true);
        if (!w.active[i])
            bi.MoveKinematic(w.pieces[i], JPH::RVec3(toJ(p.position * kM)), toJ(p.rotation), fdt);
        else if (p.guide > 0.0 && w.settings.guideStiffness > 0.0)
            guide(w, i, p, dt);
    }
    for (size_t c = 0; c < w.colliders.size() && c < current.colliders.size(); ++c)
        if (w.moving[c])
            bi.MoveKinematic(w.colliders[c], JPH::RVec3(toJ(current.colliders[c].position * kM)), toJ(current.colliders[c].rotation), fdt);

    const Settings& st = w.settings;
    const int64_t blastStep = (st.blastFrame - st.startFrame) * std::max(1, st.substeps) + 1;
    if (st.blastSpeed != 0.0 && index == std::max<int64_t>(1, blastStep))
    {
        const double radius = std::max(st.blastRadius, 1e-6);
        for (size_t i = 0; i < w.pieceCount; ++i)
        {
            if (!w.active[i])
                continue;
            const glm::dvec3 at = glm::dvec3(bi.GetCenterOfMassPosition(w.pieces[i]).GetX(), bi.GetCenterOfMassPosition(w.pieces[i]).GetY(),
                                             bi.GetCenterOfMassPosition(w.pieces[i]).GetZ()) /
                                  kM;
            const glm::dvec3 d = at - st.blastCenter;
            const double dist = glm::length(d);
            if (dist >= radius)
                continue;
            const double fall = 1.0 - dist / radius;
            const glm::dvec3 dir = dist > 1e-9 ? d / dist : glm::dvec3(0, 1, 0);
            const glm::dvec3 dv = dir * (st.blastSpeed * fall * kM);
            bi.AddLinearVelocity(w.pieces[i], toJ(dv));
        }
    }

    w.system.Update(fdt, 1, gRuntime.temp.get(), w.jobs);
    ++w.step;

    const double scale = std::max(st.strengthScale, 0.0);
    for (size_t i = 0; i < w.links.size(); ++i)
    {
        LinkRecord& r = w.linkState[i];
        const LinkDesc& l = w.linkDescs[i];
        if (!w.links[i] || r.brokenStep >= 0 || l.type == LinkType::Hard)
            continue;
        double impulse = 0.0;
        if (l.type == LinkType::Spring)
            impulse = std::abs(double(static_cast<const JPH::DistanceConstraint*>(w.links[i].GetPtr())->GetTotalLambdaPosition()));
        else
        {
            const auto* f = static_cast<const JPH::FixedConstraint*>(w.links[i].GetPtr());
            // Torque counts as the force it takes at a 10 cm lever, so bending a slab breaks it as pulling would.
            impulse = double(f->GetTotalLambdaPosition().Length()) + double(f->GetTotalLambdaRotation().Length()) / 0.1;
        }
        if (impulse / dt > l.strength * scale)
        {
            r.brokenStep = int32_t(w.step);
            r.impulse = float(impulse);
            w.links[i]->SetEnabled(false);
        }
    }
    // Broken pieces collide again once their bounds part, so pieces cut from one another don't pop apart.
    for (size_t i = 0; i < w.links.size(); ++i)
    {
        LinkRecord& r = w.linkState[i];
        const LinkDesc& l = w.linkDescs[i];
        if (r.brokenStep < 0 || r.colliding || l.b < 0)
            continue;
        const JPH::AABox ba = bi.GetTransformedShape(w.pieces[size_t(l.a)]).GetWorldSpaceBounds();
        const JPH::AABox bb = bi.GetTransformedShape(w.pieces[size_t(l.b)]).GetWorldSpaceBounds();
        if (!ba.Overlaps(bb))
        {
            r.colliding = 1;
            w.enablePair(l.a, l.b, true);
        }
    }
}

void RigidWorld::save(std::vector<uint8_t>& out, bool full) const
{
    const Impl& w = *mImpl;
    JPH::BodyInterface& bi = const_cast<Impl&>(w).bi();
    std::vector<BodyRecord> bodies(w.pieceCount);
    for (size_t i = 0; i < w.pieceCount; ++i)
    {
        BodyRecord& r = bodies[i];
        const JPH::RVec3 p = bi.GetPosition(w.pieces[i]);
        const JPH::Quat q = bi.GetRotation(w.pieces[i]);
        const JPH::Vec3 v = bi.GetLinearVelocity(w.pieces[i]);
        const JPH::Vec3 a = bi.GetAngularVelocity(w.pieces[i]);
        r.position[0] = float(p.GetX());
        r.position[1] = float(p.GetY());
        r.position[2] = float(p.GetZ());
        r.rotation[0] = q.GetX();
        r.rotation[1] = q.GetY();
        r.rotation[2] = q.GetZ();
        r.rotation[3] = q.GetW();
        r.velocity[0] = v.GetX();
        r.velocity[1] = v.GetY();
        r.velocity[2] = v.GetZ();
        r.angularVelocity[0] = a.GetX();
        r.angularVelocity[1] = a.GetY();
        r.angularVelocity[2] = a.GetZ();
        r.active = w.active[i];
    }
    std::string data;
    if (full)
    {
        JPH::StateRecorderImpl rec;
        w.system.SaveState(rec);
        data = rec.GetData();
    }
    StateHeader h;
    h.partial = full ? 0 : 1;
    h.serial = w.serial;
    h.step = w.step;
    h.startFrame = w.settings.startFrame;
    h.substeps = w.settings.substeps;
    writeState(out, h, bodies, w.linkState, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(data.data()), data.size()));
}
} // namespace destruction
