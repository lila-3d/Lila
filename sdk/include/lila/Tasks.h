#pragma once

/**
 * Threads for extensions: work on a worker with progress and Cancel, results back on the main thread.
 *
 * The scene graph, commands and UI are main-thread only. A scene::MeshPtr is an immutable snapshot, so any thread may
 * read it. TBB ships with the SDK and runs on Lila's own thread pool: tbb::parallel_for, parallel_reduce, task_group.
 *
 *     tasks::runInBackground("Scanning", n, [mesh](tasks::Task& t) {
 *         tbb::parallel_for(tbb::blocked_range<size_t>(0, n), [&](const auto& r) {
 *             t.checkCancel();
 *             ...
 *             t.progress.step(double(r.size()));
 *         });
 *     }, [](std::exception_ptr error) { ... }); // main thread
 */
#include "core/MainQueue.h"
#include "core/Progress.h"

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_reduce.h>
#include <tbb/task_group.h>

#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace lila::tasks
{
using progress::Cancelled;
using progress::Scope;

/** True on the thread that owns the scene and the UI. */
inline bool isMainThread()
{
    return mainq::isMainThread();
}

/** Runs fn on the main thread when it is next idle. Safe from any thread. */
inline void postToMain(std::function<void()> fn)
{
    mainq::post(std::move(fn));
}

/**
 * Runs fn on a worker and returns when it has, rethrowing what it threw. Meanwhile the main thread keeps the UI
 * painting, with progress and Cancel for the Scope open around the call. For a command that must finish before it
 * returns; fn may use the scene, since the main thread is only waiting.
 */
inline void runAndWait(const std::function<void()>& fn)
{
    progress::runOnWorker(fn);
}

namespace detail
{
struct JobState
{
    std::thread thread;
    std::once_flag joined;
    std::atomic<bool> done{false};
    std::atomic<bool> cancel{false};

    ~JobState()
    {
        if (!thread.joinable())
            return;
        if (thread.get_id() == std::this_thread::get_id())
            thread.detach();
        else
            thread.join();
    }
};
} // namespace detail

/** What the work of runInBackground gets: its progress, and whether to stop. */
class Task
{
public:
    Task(Scope& scope, const detail::JobState& state) : progress(scope), mState(state) {}

    /** Background progress: step() it from any thread, setStatus() for the line under the title. */
    Scope& progress;

    /** True once the user pressed Cancel or Job::cancel() was called. */
    bool cancelled() const { return mState.cancel || progress.cancelled(); }
    /** Throws Cancelled when cancelled; the job's onDone gets it as its error. */
    void checkCancel() const
    {
        if (cancelled())
            throw Cancelled();
    }

private:
    const detail::JobState& mState;
};

/** A runInBackground call. Copies share the job. */
class Job
{
public:
    Job() = default;
    explicit Job(std::shared_ptr<detail::JobState> state) : mState(std::move(state)) {}

    /** True until the work returned or threw. */
    bool running() const { return mState && !mState->done; }
    /** Asks the work to stop: Task::cancelled() turns true. */
    void cancel() const
    {
        if (mState)
            mState->cancel = true;
    }
    /**
     * Blocks until the work's thread ended. On the main thread it then runs queued main-thread work, so onDone has run
     * too. Call cancel() then wait() in lilaExtensionUninit for every job still running: no job may run the
     * extension's code after it unloads.
     */
    void wait() const
    {
        if (!mState)
            return;
        std::call_once(mState->joined, [this] {
            if (mState->thread.joinable() && mState->thread.get_id() != std::this_thread::get_id())
                mState->thread.join();
        });
        if (mainq::isMainThread())
            mainq::drain();
    }

private:
    std::shared_ptr<detail::JobState> mState;
};

/**
 * Runs work(Task&) on a new thread under a background progress indicator (title, total units, 0 for indeterminate,
 * with Cancel), then onDone(error) on the main thread: null on success, else what work threw (Cancelled when it was
 * cancelled). Returns at once. work must not touch the scene: give it snapshots and plain data, and apply its result in
 * onDone, checking that what it edits still exists.
 */
inline Job runInBackground(std::string title, double total, std::function<void(Task&)> work,
                           std::function<void(std::exception_ptr)> onDone = {})
{
    auto state = std::make_shared<detail::JobState>();
    state->thread = std::thread([state, title = std::move(title), total, work = std::move(work), onDone = std::move(onDone)]() mutable {
        std::exception_ptr error;
        try
        {
            Scope scope(title, total, progress::Silent | progress::Cancellable);
            Task task(scope, *state);
            work(task);
        }
        catch (...)
        {
            error = std::current_exception();
        }
        work = nullptr;
        if (onDone)
            mainq::post([onDone = std::move(onDone), error] { onDone(error); });
        state->done = true;
    });
    return Job(state);
}
} // namespace lila::tasks
