#pragma once

#include "ops/Command.h"
#include "core/MainQueue.h"
#include "scene/Node.h"

#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>

inline nlohmann::json run(const char* cmd, nlohmann::json call = nlohmann::json::object()) { return lila::ops::execute(cmd, call); }

/** Keeps the tests off the user's real config (no userSetup.py, no prefs) and registers the built-ins once. */
inline void isolate()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    std::random_device rd;
    std::filesystem::path dir = std::filesystem::temp_directory_path() / ("lila_extensions_test_" + std::to_string(rd()));
    std::filesystem::create_directories(dir);
    static std::filesystem::path cleanup = dir;
    std::atexit([] {
        std::error_code ec;
        std::filesystem::remove_all(cleanup, ec);
    });
#ifdef _WIN32
    _putenv_s("LILA_CONFIG_DIR", dir.string().c_str());
    _putenv_s("LILA_NO_STUB_WRITE", "1");
#else
    setenv("LILA_CONFIG_DIR", dir.c_str(), 1);
    setenv("LILA_NO_STUB_WRITE", "1", 1);
#endif
    lila::mainq::init();
    lila::ops::registerBuiltins();
    lila::scene::types::registerBuiltins();
}
