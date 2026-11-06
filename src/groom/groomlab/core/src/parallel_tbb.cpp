// SPDX-License-Identifier: Apache-2.0
// The core's parallel backend: TBB (the TBB the build resolved: Maya's in the Maya presets).
// A thread cap (parallel.h: GROOMLAB_THREADS, setParallelThreads) runs the loops inside a
// task_arena of that many threads, so the host's own TBB loops keep their parallelism.

#include "groomlab/parallel.h"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <tbb/task_arena.h>

namespace groomlab {
namespace {

std::size_t cores() { return std::max(1u, std::thread::hardware_concurrency()); }

// The cap GROOMLAB_THREADS asks for (0: none, or not a number).
std::size_t threadsFromEnvironment() {
    const char* text = std::getenv("GROOMLAB_THREADS");
    if (!text || !*text) return 0;
    char* end = nullptr;
    const unsigned long n = std::strtoul(text, &end, 10);
    return end && *end == '\0' ? static_cast<std::size_t>(n) : 0;
}

struct Cap {
    std::mutex lock;
    std::once_flag environment;      // read once, at the first parallel call or setter
    std::size_t limit = 0;           // 0: none
    std::shared_ptr<tbb::task_arena> arena;  // shared with the loops running in it while it is replaced
};

Cap& cap() {
    static Cap* const instance = new Cap();
    return *instance;
}

void apply(Cap& c, std::size_t threads) {
    std::lock_guard<std::mutex> guard(c.lock);
    c.limit = threads == 0 || threads >= cores() ? 0 : threads;
    c.arena.reset();
    if (c.limit) c.arena = std::make_shared<tbb::task_arena>(static_cast<int>(c.limit));
}

void applyEnvironment(Cap& c) {
    std::call_once(c.environment, [&] {
        if (const std::size_t threads = threadsFromEnvironment()) apply(c, threads);
    });
}

}  // namespace

namespace detail {

void parallelRange(std::size_t begin, std::size_t end, std::size_t grain, RangeBody body, const void* context) {
    Cap& c = cap();
    applyEnvironment(c);
    auto loop = [&] {
        tbb::parallel_for(tbb::blocked_range<std::size_t>(begin, end, grain),
                          [&](const tbb::blocked_range<std::size_t>& r) { body(context, r.begin(), r.end()); });
    };
    std::shared_ptr<tbb::task_arena> arena;
    {
        std::lock_guard<std::mutex> guard(c.lock);
        arena = c.arena;
    }
    if (arena) {
        arena->execute(loop);
    } else {
        loop();
    }
}

}  // namespace detail

void setParallelThreads(std::size_t threads) {
    Cap& c = cap();
    std::call_once(c.environment, [] {});  // an explicit cap stands: the environment's is not read over it
    apply(c, threads);
}

std::size_t parallelThreads() {
    Cap& c = cap();
    applyEnvironment(c);
    std::lock_guard<std::mutex> guard(c.lock);
    return c.limit ? c.limit : cores();
}

void runWithThreads(std::size_t threads, void (*fn)(void* context), void* context) {
    // (A cap below `threads` runs each loop in its own arena, inside this one: the cap stands.)
    if (threads == 0 || threads >= parallelThreads()) {
        fn(context);
        return;
    }
    tbb::task_arena arena(static_cast<int>(threads));
    arena.execute([&] { fn(context); });
}

}  // namespace groomlab
