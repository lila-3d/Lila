#include "bake/CagePreview.h"

#include "bake/AutoDistance.h"
#include "bake/Backend.h"
#include "bake/BakeInputs.h"
#include "bake/BakeNodes.h"
#include "bake/BakePipeline.h"
#include "bake/CageEstimate.h"
#include "core/Events.h"
#include "core/Log.h"
#include "core/MainQueue.h"
#include "core/Profile.h"
#include "core/Progress.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>

namespace lila::bake
{
namespace
{
struct Job
{
    scene::NodeId group = scene::kNoNode;
    uint64_t generation = 0;
    ProjectSettings settings;
    BakeInputs inputs;
    std::string backend;
    bool greenUp = false;
};

struct Worker
{
    std::mutex mutex;
    std::thread thread;
    std::unique_ptr<Job> pending;
    bool running = false;
    std::string title;
    uint64_t generation = 0;
    std::map<scene::NodeId, std::shared_ptr<const CagePreview>> latest;
    std::map<int, std::function<void(const std::shared_ptr<const CagePreview>&)>> listeners;
    int nextListener = 1;
    bool hooked = false;

    ~Worker()
    {
        if (thread.joinable())
            thread.detach();
    }
};

Worker& worker()
{
    static Worker w;
    return w;
}

void cancelRunning(Worker& w)
{
    for (const progress::Snapshot& snap : progress::snapshot())
        if (snap.title == w.title && !snap.mainThread)
            progress::cancel(snap.id);
}

std::shared_ptr<CagePreview> runJob(Job& job)
{
    LILA_ZONE();

    const auto started = std::chrono::steady_clock::now();
    std::unique_ptr<Backend> backend;
    try
    {
        backend = createBackend(job.backend);
    }
    catch (const std::exception&)
    {
    }
    if (!backend)
        backend = createBackend("cpu");
    backend->prepare(job.inputs);
    resolveAutoDistances(job.inputs, *backend);

    auto out = std::make_shared<CagePreview>();
    out->group = job.group;
    out->generation = job.generation;
    out->backend = backend->mode();
    std::mutex texMutex;
    BakeOptions opt;
    opt.folder = std::filesystem::temp_directory_path() / "lila_cage_preview";
    opt.keepTexture = [&](const TextureBuffer& tex) {
        CagePreviewTexture t;
        t.mesh = tex.meshes.empty() ? std::string() : *tex.meshes.begin();
        t.map = tex.def ? tex.def->id : std::string();
        t.udim = tex.udim;
        t.width = tex.width;
        t.height = tex.height;
        t.value = tex.value;
        t.greenUp = job.greenUp;
        std::scoped_lock lock(texMutex);
        out->textures.push_back(std::move(t));
    };
    const BakeResult r = runBake(job.inputs, job.settings, opt, *backend);
    if (!r.groups.empty())
        out->stats = r.groups.front();
    out->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return out;
}

void loop()
{
    Worker& w = worker();
    for (;;)
    {
        std::unique_ptr<Job> job;
        {
            std::scoped_lock lock(w.mutex);
            if (!w.pending)
            {
                w.running = false;
                return;
            }
            job = std::move(w.pending);
            w.title = "Cage preview " + std::to_string(job->generation);
        }
        std::shared_ptr<CagePreview> result;
        try
        {
            progress::Scope scope(w.title, 1.0, progress::Silent | progress::Cancellable);
            result = runJob(*job);
        }
        catch (const progress::Cancelled&)
        {
        }
        catch (const std::exception& e)
        {
            log::warn("Cage preview failed: {}", e.what());
        }
        if (!result)
            continue;
        mainq::post([result] {
            Worker& w = worker();
            std::vector<std::function<void(const std::shared_ptr<const CagePreview>&)>> fns;
            {
                std::scoped_lock lock(w.mutex);
                auto it = w.latest.find(result->group);
                // A newer preview of the group may already have landed.
                if (it != w.latest.end() && it->second && it->second->generation > result->generation)
                    return;
                w.latest[result->group] = result;
                for (const auto& [id, fn] : w.listeners)
                    fns.push_back(fn);
            }
            for (const auto& fn : fns)
                fn(result);
        });
    }
}

/** The project's tangent normal map settings (its convention), else the default. */
MapSettings normalMapOf(const ProjectSettings& ps)
{
    for (const MapSettings& m : ps.maps)
        if (m.map == "normal" && m.enabled)
            return m;
    MapSettings m;
    m.map = "normal";
    return m;
}

scene::Node* nodeNamed(scene::Graph& g, const std::string& name)
{
    try
    {
        return g.find(name);
    }
    catch (const std::exception&)
    {
        return nullptr;
    }
}
} // namespace

const CagePreviewTexture* CagePreview::find(const std::string& mesh, const std::string& map) const
{
    const CagePreviewTexture* best = nullptr;
    for (const CagePreviewTexture& t : textures)
        if (t.mesh == mesh && t.map == map && (!best || t.udim < best->udim))
            best = &t;
    return best;
}

void requestCagePreview(scene::Graph& g, scene::NodeId group, const CagePreviewOptions& options)
{
    LILA_ZONE();

    scene::Node* grp = g.node(group);
    if (!grp || !isGroup(*grp))
        throw std::runtime_error("not a bake group");
    scene::Node* project = groupProject(g, *grp);
    if (!project)
        throw std::runtime_error(grp->name() + " is in no bake project");
    g.evaluate();
    auto job = std::make_unique<Job>();
    job->group = group;
    job->inputs = buildInputs(g, project->id(), {group}, true);
    if (job->inputs.groups.size() != 1 || job->inputs.groups[0].highs.empty())
        throw std::runtime_error(grp->name() + " has nothing to preview (no highs)");

    // A stroke in progress bakes with its working offsets.
    GroupInputs& gi = job->inputs.groups[0];
    for (const auto& [path, values] : options.offsets)
    {
        scene::Node* n = nodeNamed(g, path);
        if (!n || !n->type().isA("mesh"))
            continue;
        const auto it = std::ranges::find(gi.settings.lows, n->path());
        if (it == gi.settings.lows.end())
            continue;
        const size_t li = size_t(it - gi.settings.lows.begin());
        scene::MeshPtr m = scene::modifiers::exportMesh(g, n->id());
        if (li >= gi.lows.size() || !m || m->pointCapacity() != values.size())
            continue;
        const glm::dmat4 world = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        const std::vector<geo::Index> cp = soupCornerPoints(*m, world);
        if (cp.size() == gi.lows[li].cageScale.size())
            for (size_t c = 0; c < cp.size(); ++c)
                gi.lows[li].cageScale[c] = values[cp[c]];
    }

    ProjectSettings ps = projectSettings(*project);
    const MapSettings normal = normalMapOf(ps);
    job->greenUp = normal.options.value("greenUp", false);
    MapSettings error;
    error.map = "error";
    ps.maps = {normal, error};
    for (MapSettings& m : ps.maps)
    {
        m.enabled = true;
        m.suffix = m.map;
        m.format.clear();
        m.bits = 0;
    }
    ps.output.width = ps.output.height = std::clamp(options.resolution, 32, 4096);
    ps.output.supersampling = 1;
    ps.output.padding = 4;
    ps.output.format = "exr";
    ps.output.name = "{mesh}_{map}_{udim}";
    job->settings = std::move(ps);
    job->backend = options.backend.empty() ? preferredBackend() : options.backend;

    Worker& w = worker();
    if (!w.hooked)
    {
        w.hooked = true;
        events::subscribe(events::Event::Shutdown, [](const events::EventData&) { cancelCagePreview(true); });
    }
    std::scoped_lock lock(w.mutex);
    job->generation = ++w.generation;
    w.pending = std::move(job);
    if (w.running)
    {
        cancelRunning(w);
        return;
    }
    if (w.thread.joinable())
        w.thread.join();
    w.running = true;
    w.thread = std::thread(loop);
}

void cancelCagePreview(bool wait)
{
    Worker& w = worker();
    {
        std::scoped_lock lock(w.mutex);
        w.pending.reset();
        if (w.running)
            cancelRunning(w);
    }
    if (wait && w.thread.joinable())
        w.thread.join();
}

bool cagePreviewBusy()
{
    Worker& w = worker();
    std::scoped_lock lock(w.mutex);
    return w.running;
}

std::shared_ptr<const CagePreview> latestCagePreview(scene::NodeId group)
{
    Worker& w = worker();
    std::scoped_lock lock(w.mutex);
    auto it = w.latest.find(group);
    return it == w.latest.end() ? nullptr : it->second;
}

void clearCagePreview(scene::NodeId group)
{
    Worker& w = worker();
    std::scoped_lock lock(w.mutex);
    w.latest.erase(group);
}

int addCagePreviewListener(std::function<void(const std::shared_ptr<const CagePreview>&)> fn)
{
    Worker& w = worker();
    std::scoped_lock lock(w.mutex);
    const int id = w.nextListener++;
    w.listeners[id] = std::move(fn);
    return id;
}

void removeCagePreviewListener(int id)
{
    Worker& w = worker();
    std::scoped_lock lock(w.mutex);
    w.listeners.erase(id);
}
} // namespace lila::bake
