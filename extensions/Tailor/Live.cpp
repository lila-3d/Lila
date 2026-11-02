// Live drape: a garment simulating continuously on its own thread while its live attribute is on.

#include "ClothGpu.h"
#include "Tailor.h"

#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Tasks.h>
#include <lila/ViewportTool.h>

#include <chrono>
#include <map>

namespace tailor
{
namespace
{
/** Steps per second at most; the solver's dt stays 1/60 s, so this is real time. */
constexpr double kStepMs = 1000.0 / 60.0;

/**
 * Steams the cloth within radius of center: each point there moves halfway to the middle of its neighbours, a few times
 * over, fading out to the radius. Returns whether any point moved.
 */
bool relaxPositions(const ClothMesh& cloth, std::vector<Vec3>& x, const Vec3& center, float radius)
{
    if (x.size() != cloth.rest3d.size() || radius <= 0.f)
        return false;
    std::vector<float> weight(x.size(), 0.f);
    bool any = false;
    for (size_t i = 0; i < x.size(); ++i)
    {
        const float d = std::hypot(x[i][0] - center[0], x[i][1] - center[1], x[i][2] - center[2]);
        if (d < radius)
        {
            weight[i] = 0.5f * (1.f - d / radius);
            any = true;
        }
    }
    if (!any)
        return false;
    std::vector<std::vector<int>> ring(x.size());
    for (const auto& t : cloth.tris)
        for (int k = 0; k < 3; ++k)
            if (weight[size_t(t[size_t(k)])] > 0.f)
            {
                ring[size_t(t[size_t(k)])].push_back(t[size_t((k + 1) % 3)]);
                ring[size_t(t[size_t(k)])].push_back(t[size_t((k + 2) % 3)]);
            }
    for (int pass = 0; pass < 4; ++pass)
    {
        const std::vector<Vec3> from = x;
        for (size_t i = 0; i < x.size(); ++i)
        {
            if (weight[i] <= 0.f || ring[i].empty())
                continue;
            Vec3 mid{0.f, 0.f, 0.f};
            for (int nb : ring[i])
                for (int k = 0; k < 3; ++k)
                    mid[size_t(k)] += from[size_t(nb)][size_t(k)] / float(ring[i].size());
            for (int k = 0; k < 3; ++k)
                x[i][size_t(k)] += weight[i] * (mid[size_t(k)] - from[i][size_t(k)]);
        }
    }
    return true;
}

std::map<lila::scene::NodeId, std::weak_ptr<LiveDrape>>& registry()
{
    static std::map<lila::scene::NodeId, std::weak_ptr<LiveDrape>> live;
    return live;
}
} // namespace

LiveDrape::LiveDrape(lila::scene::NodeId garment, uint64_t key, std::shared_ptr<const ClothMesh> cloth, Garment g, Sdf collider,
                     bool useGpu, SolverSettings settings, std::vector<Vec3> start)
    : mKey(key), mCloth(std::move(cloth)), mSettings(settings), mLatest(std::move(start))
{
    mThread = std::thread([this, garment, g = std::move(g), collider = std::move(collider), useGpu] { run(garment, g, collider, useGpu); });
}

LiveDrape::~LiveDrape() { stop(); }

void LiveDrape::stop()
{
    mStop = true;
    if (mThread.joinable())
        mThread.join();
}

std::vector<Vec3> LiveDrape::positions() const
{
    std::scoped_lock lock(mMutex);
    return mLatest;
}

void LiveDrape::grab(int vertex, const Vec3& target)
{
    std::scoped_lock lock(mMutex);
    mGrab = Pin{vertex, target};
}

void LiveDrape::release()
{
    std::scoped_lock lock(mMutex);
    mGrab.reset();
}

void LiveDrape::relax(const Vec3& center, float radius)
{
    std::scoped_lock lock(mMutex);
    mRelax = std::pair{center, radius};
}

void LiveDrape::run(lila::scene::NodeId garment, const Garment& g, const Sdf& collider, bool useGpu)
{
    std::vector<const Sdf*> colliders;
    if (collider.nx > 0)
        colliders.push_back(&collider);
    std::vector<Vec3> start = positions();

    std::unique_ptr<ClothGpu> gpu;
    std::unique_ptr<ClothSolver> cpu;
    if (useGpu && ClothGpu::available())
    {
        gpu = std::make_unique<ClothGpu>();
        gpu->setColliders(colliders);
        std::string err;
        if (gpu->reset(*mCloth, g, mSettings, &err))
            gpu->setPositions(start);
        else
        {
            lila::log::warn("Tailor: live drape on the CPU: {}", err);
            gpu.reset();
        }
    }
    if (!gpu)
    {
        cpu = std::make_unique<ClothSolver>();
        cpu->reset(*mCloth, g, mSettings);
        cpu->setColliders(colliders);
        if (start.size() == cpu->positions().size())
            cpu->positionsMutable() = start;
    }

    const std::vector<Pin> held = resolvePins(*mCloth, g);
    using clock = std::chrono::steady_clock;
    while (!mStop)
    {
        const auto t0 = clock::now();
        std::vector<Pin> pins = held;
        std::optional<std::pair<Vec3, float>> relax;
        {
            std::scoped_lock lock(mMutex);
            if (mGrab)
            {
                std::erase_if(pins, [&](const Pin& p) { return p.vertex == mGrab->vertex; });
                pins.push_back(*mGrab);
            }
            relax.swap(mRelax);
        }
        if (relax)
        {
            std::vector<Vec3> x = gpu ? gpu->positions() : cpu->positions();
            if (relaxPositions(*mCloth, x, relax->first, relax->second))
            {
                if (gpu)
                    gpu->setPositions(x);
                else
                {
                    cpu->positionsMutable() = x;
                    for (Vec3& v : cpu->velocitiesMutable())
                        v = {0.f, 0.f, 0.f};
                }
            }
        }
        std::vector<Vec3> x;
        if (gpu)
        {
            gpu->setPins(pins);
            std::string err;
            if (!gpu->step(&err))
            {
                lila::log::warn("Tailor: live drape stopped: {}", err);
                break;
            }
            x = gpu->positions();
        }
        else
        {
            cpu->setPins(pins);
            cpu->step();
            x = cpu->positions();
        }
        {
            std::scoped_lock lock(mMutex);
            mLatest = std::move(x);
        }
        if (!mRefreshQueued.exchange(true))
            lila::tasks::postToMain([this, garment, alive = std::weak_ptr<bool>(mAlive)] {
                if (alive.expired())
                    return;
                mRefreshQueued = false;
                lila::scene::Graph& graph = lila::scene::Graph::current();
                if (!graph.node(garment))
                    return;
                graph.touch(garment);
                graph.evaluate();
                lila::ui::viewportTools::requestRedraw();
            });
        const double ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
        if (ms < kStepMs)
            std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(kStepMs - ms));
    }
}

void registerLive(lila::scene::NodeId garment, const std::shared_ptr<LiveDrape>& live)
{
    if (live)
        registry()[garment] = live;
    else
        registry().erase(garment);
}

std::shared_ptr<LiveDrape> liveDrape(lila::scene::NodeId garment)
{
    const auto it = registry().find(garment);
    return it == registry().end() ? nullptr : it->second.lock();
}

std::vector<lila::scene::NodeId> liveGarments()
{
    std::vector<lila::scene::NodeId> out;
    for (auto it = registry().begin(); it != registry().end();)
        if (it->second.expired())
            it = registry().erase(it);
        else
            out.push_back((it++)->first);
    return out;
}
} // namespace tailor
