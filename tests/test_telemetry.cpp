#include <doctest/doctest.h>

#include "ui/Telemetry.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace lila;
namespace tel = lila::telemetry;
namespace fs = std::filesystem;

namespace
{
fs::path freshDir(const char* name)
{
    fs::path dir = fs::temp_directory_path() / "lila_test_telemetry" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

void setEnv(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    if (*value)
        setenv(name, value, 1);
    else
        unsetenv(name);
#endif
}
} // namespace

TEST_CASE("telemetry buckets")
{
    CHECK(tel::memoryBucket(4) == "<8");
    CHECK(tel::memoryBucket(16) == "16-31");
    CHECK(tel::memoryBucket(15.9) == "16-31");
    CHECK(tel::memoryBucket(64) == "64-127");
    CHECK(tel::memoryBucket(512) == "128+");
    CHECK(tel::daysBucket(0) == "0");
    CHECK(tel::daysBucket(6) == "1-6");
    CHECK(tel::daysBucket(7) == "7-29");
    CHECK(tel::daysBucket(400) == "90+");
}

TEST_CASE("telemetry install channel")
{
    fs::path root = freshDir("channel");
    fs::path steam = root / "SteamLibrary" / "steamapps" / "common" / "Lila";
    fs::create_directories(steam);
    CHECK(tel::installChannel(steam) == "steam");

    fs::path build = root / "src" / "build" / "bin" / "RelWithDebInfo";
    fs::create_directories(build);
    std::ofstream(root / "src" / "build" / "CMakeCache.txt") << "";
    CHECK(tel::installChannel(build) == "source");

    fs::path installed = root / "Programs" / "Lila";
    fs::create_directories(installed);
    CHECK(tel::installChannel(installed) == "installer");
}

TEST_CASE("telemetry environment opt-out")
{
    setEnv("DO_NOT_TRACK", "");
    setEnv("LILA_NO_TELEMETRY", "");
    CHECK_FALSE(tel::optedOutByEnvironment());
    setEnv("DO_NOT_TRACK", "0");
    CHECK_FALSE(tel::optedOutByEnvironment());
    setEnv("DO_NOT_TRACK", "1");
    CHECK(tel::optedOutByEnvironment());
    CHECK_FALSE(tel::enabled());
    setEnv("DO_NOT_TRACK", "");
    setEnv("LILA_NO_TELEMETRY", "1");
    CHECK(tel::optedOutByEnvironment());
    setEnv("LILA_NO_TELEMETRY", "");
}

TEST_CASE("telemetry queue caps, retries and drops")
{
    tel::Store store(freshDir("queue"));
    for (size_t i = 0; i < tel::kMaxQueued + 5; ++i)
        store.enqueue({{"event", "e"}, {"n", i}});
    json q = store.queue();
    REQUIRE(q.size() == tel::kMaxQueued);
    CHECK(q[0]["n"] == 5);

    store.dequeue(tel::kMaxQueued - 2);
    REQUIRE(store.queue().size() == 2);

    for (int i = 0; i < tel::kMaxAttempts - 1; ++i)
        store.failed(1);
    q = store.queue();
    REQUIRE(q.size() == 2);
    CHECK(q[0]["attempts"] == tel::kMaxAttempts - 1);
    CHECK_FALSE(q[1].contains("attempts"));
    store.failed(1);
    CHECK(store.queue().size() == 1);

    store.clear();
    CHECK(store.queue().empty());
}

TEST_CASE("telemetry sessions left by a gone process become crashed session ends")
{
    tel::Store store(freshDir("sessions"));
    tel::Session gone;
    gone.id = "a";
    gone.pid = 111;
    gone.startedMs = 1'000'000;
    gone.updatedMs = 1'000'000 + 90'000;
    gone.activeMinutes = 1;
    tel::countCommand(gone, "polyCube", true, "ui");
    tel::countCommand(gone, "studioSecretTool", false, "python");
    store.writeSession(gone);

    tel::Session running = gone;
    running.id = "b";
    running.pid = 222;
    store.writeSession(running);

    auto alive = [](int64_t pid, int64_t) { return pid == 222; };
    std::vector<tel::Session> stale = store.takeStaleSessions(333, alive);
    REQUIRE(stale.size() == 1);
    CHECK(stale[0].id == "a");
    CHECK(store.takeStaleSessions(333, alive).empty());
    CHECK(store.takeStaleSessions(222, [](int64_t, int64_t) { return true; }).size() == 1);

    json e = tel::sessionEndEvent(stale[0], "install", true);
    CHECK(e["event"] == "session_end");
    CHECK(e["distinct_id"] == "install");
    const json& p = e["properties"];
    CHECK(p["crashed"] == true);
    CHECK(p["duration_s"] == 90);
    CHECK(p["commands"]["polyCube"] == 1);
    CHECK_FALSE(p["commands"].contains("studioSecretTool"));
    CHECK(p["other_commands"] == 1);
    CHECK(p["command_sources"]["ui"] == 1);
    CHECK(p["command_sources"]["python"] == 1);
    CHECK(p["$geoip_disable"] == true);
}
