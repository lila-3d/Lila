#include "core/Achievements.h"

#include "core/Log.h"
#include "core/Paths.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace lila::achievements
{
namespace
{
#ifdef _WIN32
/** The few flat-API entry points used, resolved from steam_api64.dll so no SDK is needed to build. */
struct SteamApi
{
    HMODULE dll = nullptr;
    int (*initFlat)(char* errMsg) = nullptr;
    void (*shutdown)() = nullptr;
    void (*runCallbacks)() = nullptr;
    void* (*userStats)() = nullptr;
    bool (*getAchievement)(void*, const char*, bool*) = nullptr;
    bool (*setAchievement)(void*, const char*) = nullptr;
    bool (*getStat)(void*, const char*, int32_t*) = nullptr;
    bool (*setStat)(void*, const char*, int32_t) = nullptr;
    bool (*storeStats)(void*) = nullptr;
    bool (*indicateProgress)(void*, const char*, uint32_t, uint32_t) = nullptr;
    void* stats = nullptr;

    template <class F>
    bool resolve(F& fn, const char* name)
    {
        fn = reinterpret_cast<F>(GetProcAddress(dll, name));
        return fn != nullptr;
    }

    bool load()
    {
        std::filesystem::path path = paths::executableDir() / "steam_api64.dll";
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec))
            return false;
        dll = LoadLibraryW(path.c_str());
        if (!dll)
            return false;
        bool ok = resolve(initFlat, "SteamAPI_InitFlat") && resolve(shutdown, "SteamAPI_Shutdown") &&
                  resolve(runCallbacks, "SteamAPI_RunCallbacks") && resolve(userStats, "SteamAPI_SteamUserStats_v013") &&
                  resolve(getAchievement, "SteamAPI_ISteamUserStats_GetAchievement") &&
                  resolve(setAchievement, "SteamAPI_ISteamUserStats_SetAchievement") &&
                  resolve(getStat, "SteamAPI_ISteamUserStats_GetStatInt32") &&
                  resolve(setStat, "SteamAPI_ISteamUserStats_SetStatInt32") &&
                  resolve(storeStats, "SteamAPI_ISteamUserStats_StoreStats") &&
                  resolve(indicateProgress, "SteamAPI_ISteamUserStats_IndicateAchievementProgress");
        if (!ok)
        {
            log::warn("steam: steam_api64.dll is missing an entry point; achievements are off");
            return false;
        }
        char err[1024] = {};
        if (int r = initFlat(err); r != 0)
        {
            log::info("steam: not connected ({}: {}); achievements are off", r, err);
            return false;
        }
        stats = userStats();
        return stats != nullptr;
    }
};
#else
struct SteamApi
{
    bool load() { return false; }
};
#endif

enum class Result
{
    Retry,   // Steam hasn't synced the stats yet
    Changed, // stats changed: stored with the next batch
    Unlocked,
};
using Op = std::function<Result(SteamApi&)>;

// Steam rate-limits StoreStats, so stat changes go out at most this often; an unlock goes out at once for its popup.
constexpr auto kStoreInterval = std::chrono::seconds(60);

struct Worker
{
    std::mutex mutex;
    std::condition_variable wake;
    std::vector<Op> queue;
    std::set<std::string, std::less<>> unlocked; // asked for this session, so repeats never reach the queue
    std::thread thread;
    std::atomic<bool> running = false;
    std::atomic<bool> connected = false;
};

Worker& worker()
{
    static Worker w;
    return w;
}

void push(Op op)
{
    Worker& w = worker();
    if (!w.running)
        return;
    {
        std::lock_guard lock(w.mutex);
        w.queue.push_back(std::move(op));
    }
    w.wake.notify_one();
}

void run()
{
    Worker& w = worker();
    SteamApi steam;
    if (!steam.load())
    {
        w.running = false;
        std::lock_guard lock(w.mutex);
        w.queue.clear();
        return;
    }
    w.connected = true;
    log::info("steam: connected; achievements are on");
#ifdef _WIN32
    std::vector<Op> pending;
    bool dirty = false;
    auto stored = std::chrono::steady_clock::now();
    // Past this, a stat that still won't read isn't defined in Steamworks; drop calls on it rather than pile them up.
    const auto giveUp = stored + std::chrono::seconds(30);
    while (w.running)
    {
        {
            std::unique_lock lock(w.mutex);
            w.wake.wait_for(lock, std::chrono::milliseconds(250), [&] { return !w.queue.empty() || !w.running; });
            for (Op& op : w.queue)
                pending.push_back(std::move(op));
            w.queue.clear();
        }
        steam.runCallbacks();
        bool now = false;
        std::erase_if(pending, [&](Op& op) {
            Result r = op(steam);
            dirty = dirty || r != Result::Retry;
            now = now || r == Result::Unlocked;
            return r != Result::Retry || std::chrono::steady_clock::now() > giveUp;
        });
        if (dirty && (now || std::chrono::steady_clock::now() - stored >= kStoreInterval))
        {
            steam.storeStats(steam.stats);
            stored = std::chrono::steady_clock::now();
            dirty = false;
        }
    }
    if (dirty)
        steam.storeStats(steam.stats);
    steam.shutdown();
#endif
    w.connected = false;
}
} // namespace

void start()
{
    Worker& w = worker();
    if (w.running.exchange(true))
        return;
    w.thread = std::thread(run);
}

void stop()
{
    Worker& w = worker();
    w.running = false;
    w.wake.notify_one();
    if (w.thread.joinable())
        w.thread.join();
}

bool active()
{
    return worker().connected;
}

void unlock(std::string_view api)
{
    Worker& w = worker();
    {
        std::lock_guard lock(w.mutex);
        if (!w.running || !w.unlocked.emplace(api).second)
            return;
    }
#ifdef _WIN32
    push([name = std::string(api)](SteamApi& s) {
        bool done = false;
        if (!s.getAchievement(s.stats, name.c_str(), &done))
            return Result::Retry;
        if (done)
            return Result::Changed;
        s.setAchievement(s.stats, name.c_str());
        return Result::Unlocked;
    });
#endif
}

void add(std::string_view stat, int delta, std::string_view api, int goal)
{
#ifdef _WIN32
    push([stat = std::string(stat), api = std::string(api), delta, goal](SteamApi& s) {
        int32_t value = 0;
        if (!s.getStat(s.stats, stat.c_str(), &value))
            return Result::Retry;
        int32_t next = value + delta;
        s.setStat(s.stats, stat.c_str(), next);
        if (value < goal && next >= goal)
        {
            s.setAchievement(s.stats, api.c_str());
            return Result::Unlocked;
        }
        // Steam's progress popup at each tenth of the way, not on every step.
        if (goal >= 10 && next < goal && next / (goal / 10) != value / (goal / 10))
            s.indicateProgress(s.stats, api.c_str(), uint32_t(next), uint32_t(goal));
        return Result::Changed;
    });
#else
    (void)stat, (void)delta, (void)api, (void)goal;
#endif
}

void addBits(std::string_view stat, uint32_t bits, std::string_view api, uint32_t all)
{
#ifdef _WIN32
    push([stat = std::string(stat), api = std::string(api), bits, all](SteamApi& s) {
        int32_t value = 0;
        if (!s.getStat(s.stats, stat.c_str(), &value))
            return Result::Retry;
        uint32_t next = uint32_t(value) | bits;
        if (next == uint32_t(value))
            return Result::Changed;
        s.setStat(s.stats, stat.c_str(), int32_t(next));
        if ((next & all) == all)
        {
            s.setAchievement(s.stats, api.c_str());
            return Result::Unlocked;
        }
        return Result::Changed;
    });
#else
    (void)stat, (void)bits, (void)api, (void)all;
#endif
}
} // namespace lila::achievements
