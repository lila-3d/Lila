#include <doctest/doctest.h>

#include "core/MainQueue.h"
#include "core/PrefStore.h"

#include <chrono>
#include <fstream>
#include <thread>

using namespace lila;

TEST_CASE("prefs write through and pick up external edits")
{
    PrefStore& p = PrefStore::get();
    p.start();
    auto file = p.userDir() / "test_prefs.json";
    std::filesystem::remove(file);

    p.set("test_prefs", "/a/b", 42);
    p.flush();
    {
        std::ifstream in(file);
        json j = json::parse(in);
        CHECK(j["a"]["b"] == 42);
    }

    bool notified = false;
    int id = p.onChanged([&](const std::string& f) { notified |= f == "test_prefs"; });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::ofstream(file) << R"({"a": {"b": 7}})";

    for (int i = 0; i < 40 && p.value<int>("test_prefs", "/a/b", 0) != 7; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(p.value<int>("test_prefs", "/a/b", 0) == 7);
    mainq::drain();
    CHECK(notified);

    p.removeOnChanged(id);
    std::filesystem::remove(file);
}

TEST_CASE("studio prefs are defaults, locked keys win, and an offline share uses the local copy")
{
    namespace fs = std::filesystem;
    PrefStore& p = PrefStore::get();
    const fs::path studio = fs::temp_directory_path() / "lila_test_studio_prefs";
    const fs::path moved = fs::temp_directory_path() / "lila_test_studio_prefs_offline";
    fs::remove_all(studio);
    fs::remove_all(moved);
    fs::create_directories(studio / "shelves");
    std::ofstream(studio / "test_studio.json") << R"({"a": 1, "b": {"c": 2}, "$locked": ["/b"]})";
    std::ofstream(studio / "shelves" / "Studio Tools.json") << R"({"items": [], "$locked": ["/"]})";
    p.remove("test_studio", "/a");
    p.setStudioDirs({studio});

    CHECK(p.value<int>("test_studio", "/a", 0) == 1);
    p.set("test_studio", "/a", 5);
    CHECK(p.value<int>("test_studio", "/a", 0) == 5);

    CHECK(p.isLocked("test_studio", "/b/c"));
    CHECK_FALSE(p.isLocked("test_studio", "/a"));
    p.set("test_studio", "/b/c", 9);
    CHECK(p.value<int>("test_studio", "/b/c", 0) == 2);
    json merged = p.merged("test_studio");
    CHECK(merged["a"] == 5);
    CHECK(merged["b"]["c"] == 2);
    CHECK_FALSE(merged.contains("$locked"));

    CHECK(p.isLocked("shelves/Studio Tools", "/items"));
    CHECK(p.studioFiles("shelves/") == std::vector<std::string>{"shelves/Studio Tools"});

    // The share goes offline: the cached copy keeps the values and the lock.
    fs::rename(studio, moved);
    p.setStudioDirs({studio});
    CHECK(p.isLocked("test_studio", "/b/c"));
    CHECK(p.value<int>("test_studio", "/b/c", 0) == 2);
    CHECK(p.studioFiles("shelves/") == std::vector<std::string>{"shelves/Studio Tools"});

    p.setStudioDirs({});
    CHECK_FALSE(p.isLocked("test_studio", "/b/c"));
    p.remove("test_studio", "/a");
    fs::remove_all(moved);
}
