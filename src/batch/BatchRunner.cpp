#include "batch/BatchRunner.h"

#include "core/Achievements.h"
#include "core/Log.h"
#include "core/MainQueue.h"
#include "core/Paths.h"
#include "core/Profile.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace lila::batch
{
namespace
{
double now()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::atomic<int> gNextId{0};
std::atomic<bool> gInteractive{false};

std::mutex gRunsMutex;
std::deque<std::shared_ptr<Run>> gRuns;
constexpr size_t kKeepRuns = 32;

std::mutex gListenersMutex;
std::map<int, std::function<void(int, int)>> gListeners;
int gNextListener = 0;

std::mutex gExeMutex;
fs::path gWorkerExe;

void notify(int runId, int index)
{
    mainq::post([runId, index] {
        std::vector<std::function<void(int, int)>> fns;
        {
            std::lock_guard lock(gListenersMutex);
            for (auto& [id, fn] : gListeners)
                fns.push_back(fn);
        }
        for (auto& fn : fns)
            fn(runId, index);
    });
}

bool writeFile(const fs::path& p, const std::string& text)
{
    std::ofstream f(p, std::ios::binary);
    f << text;
    return bool(f);
}

std::string readFile(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

fs::path makeRunDir(int id)
{
    fs::path root = fs::temp_directory_path() / "lila-batch";
#ifdef _WIN32
    const unsigned long pid = GetCurrentProcessId();
#else
    const unsigned long pid = (unsigned long)getpid();
#endif
    const auto stamp = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    fs::path dir = root / (std::to_string(stamp) + "-" + std::to_string(pid) + "-" + std::to_string(id));
    std::error_code ec;
    fs::create_directories(dir, ec);
    // Runs from earlier days are only kept for their logs.
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
    {
        auto age = fs::file_time_type::clock::now() - it->last_write_time(ec);
        if (!ec && it->is_directory(ec) && age > std::chrono::hours(24 * 7))
            fs::remove_all(it->path(), ec);
    }
    return dir;
}

std::string exitText(int code)
{
    char buf[32];
    if (unsigned(code) >= 0xC0000000u)
        std::snprintf(buf, sizeof(buf), "0x%08X", unsigned(code));
    else
        std::snprintf(buf, sizeof(buf), "%d", code);
    return buf;
}

std::string accessMessage(const Item& item)
{
    switch (item.access)
    {
    case Access::Missing:
        return "file not found";
    case Access::Collision:
        return "another file in this run saves to " + paths::toUtf8(item.output);
    case Access::ReadOnly:
        return "read-only: checked out or made writable before saving";
    case Access::New:
        return "saves a new file";
    case Access::Replace:
        return "replaces the file";
    default:
        return {};
    }
}

#ifdef _WIN32
std::wstring quoteArgs(const std::vector<std::wstring>& args)
{
    std::wstring cmd;
    for (const std::wstring& a : args)
    {
        if (!cmd.empty())
            cmd += L' ';
        if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos)
        {
            cmd += a;
            continue;
        }
        cmd += L'"';
        size_t slashes = 0;
        for (wchar_t c : a)
        {
            if (c == L'\\')
            {
                ++slashes;
                continue;
            }
            cmd.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
            slashes = 0;
            cmd += c;
        }
        cmd.append(slashes * 2, L'\\');
        cmd += L'"';
    }
    return cmd;
}

class ProcessWorker final : public Worker
{
public:
    ProcessWorker(HANDLE process, HANDLE job) : mProcess(process), mJob(job) {}
    ~ProcessWorker() override
    {
        CloseHandle(mProcess);
        // KILL_ON_JOB_CLOSE: anything the script left running ends with the worker's job.
        CloseHandle(mJob);
    }

    bool exited(int& code) override
    {
        if (WaitForSingleObject(mProcess, 0) != WAIT_OBJECT_0)
            return false;
        DWORD c = 0;
        GetExitCodeProcess(mProcess, &c);
        code = int(c);
        return true;
    }

    void kill() override
    {
        TerminateJobObject(mJob, 1);
        WaitForSingleObject(mProcess, 2000);
    }

private:
    HANDLE mProcess;
    HANDLE mJob;
};

class ProcessLauncher final : public Launcher
{
public:
    std::unique_ptr<Worker> start(const WorkerTask& t, std::string* error) override
    {
        LILA_ZONE();

        auto fail = [&](const char* what) -> std::unique_ptr<Worker> {
            if (error)
                *error = std::string(what) + " (error " + std::to_string(GetLastError()) + ")";
            return nullptr;
        };
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE log = CreateFileW(t.logFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (log == INVALID_HANDLE_VALUE)
            return fail("cannot create the log");
        HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

        // Only these two are inherited, so pipes other threads are opening for their own tools never leak into a worker.
        HANDLE inherit[2] = {log, nul};
        SIZE_T attrSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
        std::vector<char> attrBuf(attrSize);
        auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
        InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize);
        UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr);

        STARTUPINFOEXW si{};
        si.StartupInfo.cb = sizeof(si);
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = nul;
        si.StartupInfo.hStdOutput = log;
        si.StartupInfo.hStdError = log;
        si.lpAttributeList = attrs;

        std::wstring cmd = quoteArgs({t.exe.wstring(), L"--batch-run-worker", t.jobFile.wstring(), std::to_wstring(t.index)});
        PROCESS_INFORMATION pi{};
        BOOL ok = CreateProcessW(t.exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, nullptr,
                                 nullptr, &si.StartupInfo, &pi);
        DeleteProcThreadAttributeList(attrs);
        CloseHandle(log);
        CloseHandle(nul);
        if (!ok)
            return fail("cannot start lilabatch");

        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        AssignProcessToJobObject(job, pi.hProcess);
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        return std::make_unique<ProcessWorker>(pi.hProcess, job);
    }
};
#else
class ProcessWorker final : public Worker
{
public:
    explicit ProcessWorker(pid_t pid) : mPid(pid) {}

    bool exited(int& code) override
    {
        int status = 0;
        if (waitpid(mPid, &status, WNOHANG) != mPid)
            return false;
        code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        return true;
    }

    void kill() override
    {
        ::kill(mPid, SIGKILL);
        int status = 0;
        waitpid(mPid, &status, 0);
    }

private:
    pid_t mPid;
};

class ProcessLauncher final : public Launcher
{
public:
    std::unique_ptr<Worker> start(const WorkerTask& t, std::string* error) override
    {
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addopen(&fa, 1, t.logFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        posix_spawn_file_actions_adddup2(&fa, 1, 2);
        std::string exe = t.exe.string(), job = t.jobFile.string(), index = std::to_string(t.index);
        std::string flag = "--batch-run-worker";
        char* argv[] = {exe.data(), flag.data(), job.data(), index.data(), nullptr};
        pid_t pid = 0;
        int rc = posix_spawn(&pid, exe.c_str(), &fa, nullptr, argv, environ);
        posix_spawn_file_actions_destroy(&fa);
        if (rc != 0)
        {
            if (error)
                *error = "cannot start lilabatch";
            return nullptr;
        }
        return std::make_unique<ProcessWorker>(pid);
    }
};
#endif
} // namespace

std::shared_ptr<Launcher> processLauncher() { return std::make_shared<ProcessLauncher>(); }

fs::path workerExecutable()
{
    std::lock_guard lock(gExeMutex);
    if (!gWorkerExe.empty())
        return gWorkerExe;
#ifdef _WIN32
    return paths::executableDir() / "lilabatch.exe";
#else
    return paths::executableDir() / "lilabatch";
#endif
}

void setWorkerExecutable(const fs::path& exe)
{
    std::lock_guard lock(gExeMutex);
    gWorkerExe = exe;
}

Run::Run(Spec spec, std::vector<Item> items, std::shared_ptr<Launcher> launcher)
    : mId(++gNextId), mSpec(std::move(spec)), mItems(std::move(items)), mLauncher(launcher ? std::move(launcher) : processLauncher()),
      mResults(mItems.size())
{
    mDir = makeRunDir(mId);
}

Run::~Run()
{
    cancel();
    if (mThread.joinable())
        mThread.join();
}

void Run::start()
{
    mStarted = now();
    if (mSpec.dryRun)
    {
        {
            std::lock_guard lock(mMutex);
            for (size_t i = 0; i < mItems.size(); ++i)
            {
                mResults[i].status = Status::Planned;
                mResults[i].message = accessMessage(mItems[i]);
            }
            mFinished = true;
        }
        mCv.notify_all();
        changed(-1);
        return;
    }
    json items = json::array();
    for (const Item& it : mItems)
        items.push_back({{"path", paths::toUtf8(it.path)}, {"root", paths::toUtf8(it.root)}, {"output", paths::toUtf8(it.output)}});
    json job = {{"runId", mId}, {"spec", toJson(mSpec)}, {"items", items}, {"dir", paths::toUtf8(mDir)}};
    if (!writeFile(mDir / "job.json", job.dump(1)))
    {
        std::lock_guard lock(mMutex);
        for (FileResult& r : mResults)
        {
            r.status = Status::Failed;
            r.message = "cannot write " + paths::toUtf8(mDir / "job.json");
        }
        mFinished = true;
        mCv.notify_all();
        changed(-1);
        return;
    }
    mThread = std::thread([this] { loop(); });
}

void Run::cancel()
{
    mCancel = true;
    mCv.notify_all();
}

bool Run::wait(double timeout)
{
    std::unique_lock lock(mMutex);
    auto done = [this] { return mFinished.load(); };
    if (timeout < 0)
        mCv.wait(lock, done);
    else
        mCv.wait_for(lock, std::chrono::duration<double>(timeout), done);
    return mFinished;
}

bool Run::isFinished() const { return mFinished; }

FileResult Run::result(size_t index) const
{
    std::lock_guard lock(mMutex);
    return index < mResults.size() ? mResults[index] : FileResult{};
}

std::vector<FileResult> Run::results() const
{
    std::lock_guard lock(mMutex);
    return mResults;
}

std::vector<size_t> Run::unfinishedOrFailed() const
{
    std::lock_guard lock(mMutex);
    std::vector<size_t> out;
    for (size_t i = 0; i < mResults.size(); ++i)
        if (mResults[i].status == Status::Failed || mResults[i].status == Status::Cancelled || mResults[i].status == Status::Pending)
            out.push_back(i);
    return out;
}

void Run::changed(int index)
{
    ++mVersion;
    notify(mId, index);
}

void Run::finish(size_t index, FileResult r)
{
    {
        std::lock_guard lock(mMutex);
        mResults[index] = std::move(r);
    }
    changed(int(index));
}

void Run::loop()
{
    LILA_ZONE();

    struct Active
    {
        size_t index;
        std::unique_ptr<Worker> worker;
        double started;
    };
    std::vector<Active> active;
    size_t next = 0;
    bool stopping = false;
    int workers = mSpec.workers;
    if (workers <= 0)
        workers = std::clamp(int(std::thread::hardware_concurrency()) / 2, 1, 8);
    workers = std::max(1, std::min(workers, int(mItems.size())));

    WorkerTask task{.exe = workerExecutable(), .jobFile = mDir / "job.json"};
    auto failed = [&](size_t i, std::string message) {
        FileResult r;
        r.status = Status::Failed;
        r.message = std::move(message);
        finish(i, std::move(r));
        stopping |= mSpec.stopOnError;
    };

    while (true)
    {
        while (!mCancel && !stopping && int(active.size()) < workers && next < mItems.size())
        {
            const size_t i = next++;
            if (mItems[i].access == Access::Missing || mItems[i].access == Access::Collision)
            {
                failed(i, accessMessage(mItems[i]));
                continue;
            }
            task.index = i;
            task.resultFile = mDir / (std::to_string(i) + ".result.json");
            task.logFile = mDir / (std::to_string(i) + ".log");
            std::error_code ec;
            fs::remove(task.resultFile, ec);
            std::string error;
            std::unique_ptr<Worker> w = mLauncher->start(task, &error);
            if (!w)
            {
                failed(i, "could not start a worker: " + error);
                continue;
            }
            {
                std::lock_guard lock(mMutex);
                mResults[i].status = Status::Running;
                mResults[i].log = paths::toUtf8(task.logFile);
            }
            changed(int(i));
            active.push_back({i, std::move(w), now()});
        }
        if (active.empty() && (next >= mItems.size() || mCancel || stopping))
            break;

        for (auto it = active.begin(); it != active.end();)
        {
            const size_t i = it->index;
            const fs::path logFile = mDir / (std::to_string(i) + ".log");
            int code = 0;
            if (it->worker->exited(code))
            {
                const fs::path resultFile = mDir / (std::to_string(i) + ".result.json");
                FileResult r;
                std::error_code ec;
                if (fs::exists(resultFile, ec))
                {
                    try
                    {
                        r = resultFromJson(json::parse(readFile(resultFile)));
                    }
                    catch (const json::exception& e)
                    {
                        r.status = Status::Failed;
                        r.message = std::string("unreadable result: ") + e.what();
                    }
                }
                else
                {
                    r.status = Status::Failed;
                    r.message = "the worker exited with code " + exitText(code) + " before finishing";
                }
                if (r.seconds <= 0.0)
                    r.seconds = now() - it->started;
                r.log = paths::toUtf8(logFile);
                const bool bad = r.status == Status::Failed;
                finish(i, std::move(r));
                stopping |= bad && mSpec.stopOnError;
                it = active.erase(it);
                continue;
            }
            const bool timedOut = mSpec.timeout > 0.0 && now() - it->started > mSpec.timeout;
            if (mCancel || timedOut)
            {
                it->worker->kill();
                FileResult r;
                r.status = mCancel ? Status::Cancelled : Status::Failed;
                char msg[64];
                std::snprintf(msg, sizeof(msg), "timed out after %.0f s", mSpec.timeout);
                r.message = mCancel ? std::string("cancelled") : std::string(msg);
                r.seconds = now() - it->started;
                r.log = paths::toUtf8(logFile);
                finish(i, std::move(r));
                stopping |= timedOut && mSpec.stopOnError;
                it = active.erase(it);
                continue;
            }
            ++it;
        }
        std::unique_lock lock(mMutex);
        mCv.wait_for(lock, std::chrono::milliseconds(20), [this] { return mCancel.load(); });
    }

    {
        std::lock_guard lock(mMutex);
        for (size_t i = next; i < mItems.size(); ++i)
        {
            mResults[i].status = mCancel ? Status::Cancelled : Status::Skipped;
            mResults[i].message = mCancel ? "cancelled" : "not run: the run stopped at an error";
        }
        mSeconds = now() - mStarted;
    }
    if (!mSpec.csv.empty())
    {
        fs::path csv = paths::fromUtf8(mSpec.csv);
        std::error_code ec;
        if (csv.has_parent_path())
            fs::create_directories(csv.parent_path(), ec);
        if (!writeFile(csv, toCsv(mItems, results())))
            log::warn("batchRun: cannot write {}", mSpec.csv);
    }
    {
        std::lock_guard lock(mMutex);
        mFinished = true;
    }
    if (mItems.size() >= 100)
        achievements::unlock("ASSEMBLY_LINE");
    mCv.notify_all();
    changed(-1);
}

json Run::summary(bool withFiles) const
{
    std::vector<FileResult> rs = results();
    json counts = json::object();
    for (Status s : {Status::Pending, Status::Running, Status::Done, Status::Failed, Status::Skipped, Status::Cancelled, Status::Planned})
        counts[statusName(s)] = 0;
    for (const FileResult& r : rs)
        counts[statusName(r.status)] = counts[statusName(r.status)].get<int>() + 1;
    const bool done = mFinished;
    json out = {{"id", mId},
                {"state", !done ? "running" : mCancel ? "cancelled" : "finished"},
                {"dryRun", mSpec.dryRun},
                {"total", mItems.size()},
                {"counts", counts},
                {"seconds", done ? mSeconds : now() - mStarted},
                {"dir", paths::toUtf8(mDir)},
                {"csv", mSpec.csv}};
    if (withFiles)
    {
        json files = json::array();
        for (size_t i = 0; i < mItems.size(); ++i)
        {
            json f = toJson(rs[i]);
            f["index"] = i;
            f["file"] = paths::toUtf8(mItems[i].path);
            f["output"] = paths::toUtf8(mItems[i].output);
            f["access"] = accessName(mItems[i].access);
            files.push_back(std::move(f));
        }
        out["files"] = std::move(files);
    }
    return out;
}

std::string Run::report(bool files) const
{
    std::vector<FileResult> rs = results();
    std::string out;
    char line[64];
    for (size_t i = 0; files && i < mItems.size(); ++i)
    {
        std::snprintf(line, sizeof(line), "%-9s %7.2fs  ", statusName(rs[i].status), rs[i].seconds);
        out += line + paths::toUtf8(mItems[i].path);
        if (rs[i].saved)
            out += " -> " + paths::toUtf8(mItems[i].output);
        if (!rs[i].message.empty())
            out += "  (" + rs[i].message + ")";
        if (!rs[i].errorFile.empty())
            out += "  " + rs[i].errorFile + ":" + std::to_string(rs[i].errorLine);
        out += "\n";
    }
    json s = summary(false);
    std::snprintf(line, sizeof(line), "%.1f", s["seconds"].get<double>());
    out += std::to_string(mItems.size()) + " files in " + line + " s: ";
    bool first = true;
    for (auto it = s["counts"].begin(); it != s["counts"].end(); ++it)
        if (it.value().get<int>() > 0)
        {
            out += (first ? "" : ", ") + std::to_string(it.value().get<int>()) + " " + it.key();
            first = false;
        }
    out += "\n";
    if (!mSpec.csv.empty())
        out += "results: " + mSpec.csv + "\n";
    out += "logs: " + paths::toUtf8(mDir) + "\n";
    return out;
}

namespace
{
std::shared_ptr<Run> keep(std::shared_ptr<Run> run)
{
    std::lock_guard lock(gRunsMutex);
    gRuns.push_front(run);
    // Only finished runs are dropped, so a run is never destroyed (and its workers killed) just for being old.
    for (size_t i = gRuns.size(); i-- > kKeepRuns;)
        if (gRuns[i]->isFinished())
            gRuns.erase(gRuns.begin() + std::ptrdiff_t(i));
    return run;
}
} // namespace

std::shared_ptr<Run> startRun(const Spec& given, std::vector<std::string>* warnings, std::shared_ptr<Launcher> launcher)
{
    Spec spec = given;
    if (spec.script.empty() && !spec.scriptFile.empty())
    {
        std::error_code ec;
        if (!fs::is_regular_file(paths::fromUtf8(spec.scriptFile), ec))
            throw std::invalid_argument("script file not found: " + spec.scriptFile);
        spec.script = readFile(paths::fromUtf8(spec.scriptFile));
    }
    if (spec.script.find_first_not_of(" \t\r\n") == std::string::npos)
        throw std::invalid_argument("no script to run");
    if (spec.scriptLabel.empty())
        spec.scriptLabel = spec.scriptFile.empty() ? "snippet" : paths::toUtf8(paths::fromUtf8(spec.scriptFile).filename());

    std::vector<std::string> local;
    std::vector<Item> items = gather(spec, warnings ? warnings : &local);
    if (items.empty())
        throw std::invalid_argument("no scene files match the inputs");
    plan(spec, items);
    auto run = keep(std::make_shared<Run>(std::move(spec), std::move(items), std::move(launcher)));
    run->start();
    return run;
}

std::shared_ptr<Run> retryRun(const Run& old, std::vector<size_t> indices, std::shared_ptr<Launcher> launcher)
{
    if (indices.empty())
        indices = old.unfinishedOrFailed();
    std::vector<Item> items;
    for (size_t i : indices)
        if (i < old.items().size())
            items.push_back(old.items()[i]);
    if (items.empty())
        throw std::invalid_argument("nothing to retry: every file succeeded or was skipped");
    Spec spec = old.spec();
    spec.dryRun = false;
    plan(spec, items);
    auto run = keep(std::make_shared<Run>(std::move(spec), std::move(items), std::move(launcher)));
    run->start();
    return run;
}

std::shared_ptr<Run> findRun(int id)
{
    std::lock_guard lock(gRunsMutex);
    if (gRuns.empty())
        return nullptr;
    if (id == 0)
        return gRuns.front();
    for (const auto& r : gRuns)
        if (r->id() == id)
            return r;
    return nullptr;
}

std::vector<std::shared_ptr<Run>> runs()
{
    std::lock_guard lock(gRunsMutex);
    return {gRuns.begin(), gRuns.end()};
}

void shutdown()
{
    std::deque<std::shared_ptr<Run>> all;
    {
        std::lock_guard lock(gRunsMutex);
        all.swap(gRuns);
    }
    for (auto& r : all)
        r->cancel();
    all.clear();
}

int addListener(std::function<void(int, int)> fn)
{
    std::lock_guard lock(gListenersMutex);
    gListeners[++gNextListener] = std::move(fn);
    return gNextListener;
}

void removeListener(int id)
{
    std::lock_guard lock(gListenersMutex);
    gListeners.erase(id);
}

void setInteractive(bool on) { gInteractive = on; }
bool interactive() { return gInteractive; }
} // namespace lila::batch
