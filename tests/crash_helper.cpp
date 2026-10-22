// lila_crashtest.exe <mode>: installs Lila's crash handling and watchdog, logs a little, then dies the way mode says.
// Driven by test_crash.cpp; never run it by hand from a shell you care about.

#include "core/CrashHandler.h"
#include "core/Log.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace lila;

namespace
{
#pragma optimize("", off)
int recurse(volatile int* depth)
{
    volatile char pad[4096];
    pad[0] = char(*depth);
    if (++*depth < 0)
        return 0;
    return recurse(depth) + pad[0];
}

void corruptHeap()
{
    HANDLE heap = HeapCreate(0, 0, 0);
    HeapSetInformation(heap, HeapEnableTerminationOnCorruption, nullptr, 0);
    void* p = HeapAlloc(heap, 0, 64);
    HeapFree(heap, 0, p);
    HeapFree(heap, 0, p);
    // A second double free in case the first one was only noted.
    void* q = HeapAlloc(heap, 0, 64);
    std::memset(static_cast<char*>(q) - 16, 0xCD, 96);
    HeapFree(heap, 0, q);
}
#pragma optimize("", on)
} // namespace

int main(int argc, char** argv)
{
    std::string mode = argc > 1 ? argv[1] : "clean";
    crash::install();
    crash::setContext("context", "scene", "X:/crashtest/scene.lila");
    crash::setContext("context", "scene.modified", "true");
    crash::addSecret("crashtestSecretToken0123456789");
    log::info("crashtest: marker line for mode {}", mode);
    log::info("crashtest: the secret is crashtestSecretToken0123456789");
    log::write(LogLevel::Echo, "lila.cmds.polyCube(name='crashtestCube')\n");

    crash::WatchOptions opts;
    opts.ui = false;
    const char* reg = std::getenv("LILA_CRASH_REGISTER_WER");
    opts.registerWer = reg && *reg == '1';
    if (!crash::startWatchdog(opts))
        return 10;
    std::printf("watchdog %u\n", crash::watchdogPid());
    std::fflush(stdout);
    // Let the flusher write the session file once, as a running Lila would have.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    if (mode == "clean")
    {
        crash::shutdown();
        return 0;
    }
    if (mode == "fastfail")
        __fastfail(7); // FAST_FAIL_FATAL_APP_EXIT
    if (mode == "heap")
        corruptHeap();
    if (mode == "av")
        *static_cast<volatile int*>(nullptr) = 1;
    if (mode == "throw")
        throw std::runtime_error("crashtest boom");
    if (mode == "terminate")
        std::terminate();
    if (mode == "stackoverflow")
    {
        volatile int depth = 0;
        return recurse(&depth);
    }
    return 11;
}
