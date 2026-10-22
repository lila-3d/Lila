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
