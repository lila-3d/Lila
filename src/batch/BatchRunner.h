#pragma once

#include "batch/BatchJob.h"
#include "core/Export.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace lila::batch
{
/** One worker process's assignment: process items[index] of the job file. */
struct WorkerTask
{
    fs::path exe;
    fs::path jobFile;
    size_t index = 0;
    fs::path resultFile;
    fs::path logFile;
};

class Worker
{
public:
    virtual ~Worker() = default;
    /** True once it has ended, with its exit code. Never blocks. */
    virtual bool exited(int& code) = 0;
    /** Ends it and whatever it started. */
    virtual void kill() = 0;
};

/** Starts workers. Injected so the queue can be tested without processes. Called on the run's thread. */
class Launcher
{
public:
    virtual ~Launcher() = default;
    /** Null with error set when it could not start. */
    virtual std::unique_ptr<Worker> start(const WorkerTask& task, std::string* error) = 0;
};

/** lilabatch processes with no console window, output captured to the task's log, ended with Lila. */
LILA_SCENE_API std::shared_ptr<Launcher> processLauncher();

/** lilabatch next to the running exe, unless setWorkerExecutable picked another. */
LILA_SCENE_API fs::path workerExecutable();
LILA_SCENE_API void setWorkerExecutable(const fs::path& exe);

/**
 * A batch run: its files go out to up to spec.workers workers at a time from a thread of its own, so nothing waits on
 * it. Each worker writes the file's FileResult next to the job; a worker that dies without one fails its file.
 */
class LILA_SCENE_API Run
{
public:
    Run(Spec spec, std::vector<Item> items, std::shared_ptr<Launcher> launcher = {});
    ~Run();
    Run(const Run&) = delete;
    Run& operator=(const Run&) = delete;

    int id() const { return mId; }
    const Spec& spec() const { return mSpec; }
    const std::vector<Item>& items() const { return mItems; }
    /** Where the job, the logs and the results are written. */
    const fs::path& dir() const { return mDir; }

    /** A dry run only reports the plan: every file comes back Planned. */
    void start();
    /** Ends running workers and leaves the files not started yet cancelled. */
    void cancel();
    /** True once every file has finished; timeout in seconds, negative to wait for good. */
    bool wait(double timeout = -1.0);
    bool isFinished() const;
    bool wasCancelled() const { return mCancel.load(); }

    FileResult result(size_t index) const;
    std::vector<FileResult> results() const;
    /** Indices of the files that failed or were cancelled. */
    std::vector<size_t> unfinishedOrFailed() const;
    /** Bumped on every change, for views that poll. */
    uint64_t version() const { return mVersion.load(); }

    /** {id, state, total, counts, seconds, dir, csv, files?: [{index, file, output, access, status, ...FileResult}]}. */
    json summary(bool withFiles = true) const;
    /** The totals (after one line per file with files), for consoles. */
    std::string report(bool files = true) const;

private:
    void loop();
    void finish(size_t index, FileResult r);
    void changed(int index);

    const int mId;
    Spec mSpec;
    std::vector<Item> mItems;
    std::shared_ptr<Launcher> mLauncher;
    fs::path mDir;

    mutable std::mutex mMutex;
    std::condition_variable mCv;
    std::vector<FileResult> mResults;
    std::atomic<bool> mCancel{false};
    std::atomic<bool> mFinished{false};
    std::atomic<uint64_t> mVersion{0};
    double mStarted = 0.0;
    double mSeconds = 0.0;
    std::thread mThread;
};

/** Plans spec, then starts the run and keeps it for findRun. Throws std::invalid_argument when there is no script. */
LILA_SCENE_API std::shared_ptr<Run> startRun(const Spec& spec, std::vector<std::string>* warnings = nullptr,
                                             std::shared_ptr<Launcher> launcher = {});
/** A new run over some of an earlier run's files (its failed and cancelled ones when indices is empty). */
LILA_SCENE_API std::shared_ptr<Run> retryRun(const Run& run, std::vector<size_t> indices = {}, std::shared_ptr<Launcher> launcher = {});
/** id 0 is the newest. Null when unknown. */
LILA_SCENE_API std::shared_ptr<Run> findRun(int id = 0);
/** Newest first; the last few dozen are kept. */
LILA_SCENE_API std::vector<std::shared_ptr<Run>> runs();

/** Cancels every run and waits for their workers to end. The host calls it on the way out. */
LILA_SCENE_API void shutdown();

/** fn(runId, index) on the main thread after a file changed state; index -1 when the run finished. */
LILA_SCENE_API int addListener(std::function<void(int runId, int index)> fn);
LILA_SCENE_API void removeListener(int id);

/** Whether a UI is up: batchRun then returns at once instead of waiting for the run. */
LILA_SCENE_API void setInteractive(bool on);
LILA_SCENE_API bool interactive();
} // namespace lila::batch
