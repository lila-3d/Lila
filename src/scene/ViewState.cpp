#include "scene/ViewState.h"

#include "ops/Command.h"

#include <atomic>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

namespace lila::scene::viewState
{
namespace
{
struct State
{
    std::atomic<int> finalDepth = 0;
    std::atomic<int> exportDepth = 0;
    std::mutex cameraMutex;
    ActiveCamera camera;
    std::vector<std::function<void()>> listeners;
    bool installed = false;
    std::optional<FinalScope> commandScope;
};

State& st()
{
    static State s;
    return s;
}

void notify()
{
    for (const auto& fn : st().listeners)
        fn();
}

bool evaluatesForOutput(std::string_view name)
{
    // Exports that write settings or rig data rather than evaluated geometry are left out.
    static constexpr std::string_view kNotGeometry[] = {"exportTheme", "exportPicker", "exportDeformerWeights", "exportDriverData"};
    for (std::string_view n : kNotGeometry)
        if (name == n)
            return false;
    static constexpr std::string_view kOthers[] = {"render", "renderSequence", "renderTurntable", "unrealSync", "unrealPlaceInstances", "link"};
    for (std::string_view n : kOthers)
        if (name == n)
            return true;
    return name.starts_with("export") || name.ends_with("Export");
}

bool isRender(std::string_view name) { return name == "render" || name == "renderSequence" || name == "renderTurntable"; }

void onExecute(const ExecuteEvent& e)
{
    if (!evaluatesForOutput(e.name))
        return;
    if (!e.done)
        st().commandScope.emplace(!isRender(e.name));
    else
        st().commandScope.reset();
}
} // namespace

bool isViewport() { return st().finalDepth == 0; }

bool isExport() { return st().exportDepth > 0; }

FinalScope::FinalScope(bool exporting) : mExporting(exporting)
{
    const bool exportStarts = exporting && st().exportDepth++ == 0;
    if (st().finalDepth++ == 0 || exportStarts)
        notify();
}

FinalScope::~FinalScope()
{
    const bool exportEnds = mExporting && --st().exportDepth == 0;
    if (--st().finalDepth == 0 || exportEnds)
        notify();
}

ActiveCamera activeCamera()
{
    std::scoped_lock lock(st().cameraMutex);
    return st().camera;
}

void setActiveCamera(const ActiveCamera& camera)
{
    {
        std::scoped_lock lock(st().cameraMutex);
        if (st().camera == camera)
            return;
        st().camera = camera;
    }
    notify();
}

void onChange(std::function<void()> fn) { st().listeners.push_back(std::move(fn)); }

void installCommandScopes()
{
    if (st().installed)
        return;
    st().installed = true;
    ops::addExecuteListener(onExecute);
}
} // namespace lila::scene::viewState
