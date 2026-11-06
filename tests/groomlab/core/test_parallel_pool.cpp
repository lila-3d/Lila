// SPDX-License-Identifier: Apache-2.0
// The C API library's parallel backend (engine/core/src/parallel_pool.cpp): the pool of the
// library's own threads, against groomlab_core_private (the TBB backend is groomlab_tests').
// Each test here runs in a process of its own under ctest (gtest_discover_tests), and the first
// one reads what the pool takes from the environment when it is made.
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "groomlab/parallel.h"

namespace groomlab {
namespace {

std::size_t cores() { return std::max(1u, std::thread::hardware_concurrency()); }

void setEnvironment(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

// Some work for one index, the same whichever thread does it.
double work(std::size_t i) {
    double s = 0.0;
    for (int k = 0; k < 64; ++k) s += std::sin(static_cast<double>(i * 64 + static_cast<std::size_t>(k)));
    return s;
}

// First in the file: the pool is made at the process's first parallel call, and GROOMLAB_THREADS
// caps its threads then (the cap is read once; setParallelThreads changes it after).
TEST(ParallelPool, EnvironmentCapsTheThreads) {
    setEnvironment("GROOMLAB_THREADS", "3");
    std::vector<double> out(64);
    parallelFor(0, out.size(), [&](std::size_t i) { out[i] = work(i); }, 1);
    for (std::size_t i = 0; i < out.size(); ++i) EXPECT_EQ(out[i], work(i));
    EXPECT_EQ(parallelThreads(), std::min<std::size_t>(3, cores()));
    setParallelThreads(0);
    EXPECT_EQ(parallelThreads(), cores());
}

TEST(ParallelPool, ManyTinyLoopsGiveTheSerialResult) {
    // Evaluations issue hundreds of small loops (a 2-chunk one wakes one worker, not every
    // sleeping one): each must complete with every chunk run exactly once. Timed back to back
    // (the workers still spinning from the last one) and with serial work between them longer
    // than the spin, so every loop finds the workers asleep and wakes them.
    constexpr std::size_t kLoops = 10000;
    const double want[2] = {work(0), work(1)};
    std::vector<double> out(2, 0.0);
    std::atomic<std::size_t> runs{0};
    auto series = [&](int gapMicros) {
        std::size_t wrong = 0;
        runs.store(0);
        double busy = 0.0;
        const auto t0 = std::chrono::steady_clock::now();
        for (std::size_t loop = 0; loop < kLoops; ++loop) {
            out[0] = out[1] = 0.0;
            parallelFor(0, 2, [&](std::size_t i) {
                out[i] = work(i);
                runs.fetch_add(1, std::memory_order_relaxed);
            }, 1);
            if (out[0] != want[0] || out[1] != want[1]) ++wrong;
            if (gapMicros > 0) {
                const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(gapMicros);
                while (std::chrono::steady_clock::now() < until) busy += work(loop);
            }
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        EXPECT_EQ(wrong, 0u) << "gap " << gapMicros;
        EXPECT_EQ(runs.load(), 2 * kLoops) << "gap " << gapMicros;
        return ms - static_cast<double>(kLoops * static_cast<std::size_t>(gapMicros)) / 1000.0;
    };
    const double backToBack = series(0), apart = series(400);
    std::printf("[ budget ] %zu parallel loops of 2 chunks on %zu threads: back to back %.1f ms (%.1f us a loop), 400 us apart %.1f ms "
                "beyond the gaps (%.1f us a loop)\n",
                kLoops, parallelThreads(), backToBack, 1000.0 * backToBack / static_cast<double>(kLoops), apart,
                1000.0 * apart / static_cast<double>(kLoops));
}

TEST(ParallelPool, ThreadCapLimitsTheWorkersAndKeepsTheResult) {
    constexpr std::size_t kCount = 200000;
    std::vector<double> full(kCount), capped(kCount);
    std::mutex lock;
    std::set<std::thread::id> threads;
    auto run = [&](std::vector<double>& out) {
        threads.clear();
        parallelFor(0, kCount, [&](std::size_t i) {
            out[i] = work(i);
            if ((i & 1023) == 0) {
                std::lock_guard<std::mutex> guard(lock);
                threads.insert(std::this_thread::get_id());
            }
        }, 1);
        return threads.size();
    };
    setParallelThreads(0);
    const std::size_t uncapped = run(full);
    setParallelThreads(2);
    EXPECT_EQ(parallelThreads(), std::min<std::size_t>(2, cores()));
    const std::size_t two = run(capped);
    EXPECT_LE(two, 2u);
    EXPECT_EQ(full, capped);
    // A cap past the cores is the cores; 0 lifts it.
    setParallelThreads(cores() + 7);
    EXPECT_EQ(parallelThreads(), cores());
    setParallelThreads(0);
    EXPECT_EQ(parallelThreads(), cores());
    if (cores() > 2) EXPECT_GT(uncapped, 2u);
}

TEST(ParallelPool, NestedLoopsCompleteUnderACap) {
    // A loop inside a parallel body (the caller helps with other calls' chunks while it waits):
    // with two threads as with every core, each inner loop runs every chunk once.
    for (const std::size_t cap : {std::size_t{2}, std::size_t{0}}) {
        setParallelThreads(cap);
        constexpr std::size_t kOuter = 64, kInner = 1000;
        std::vector<std::atomic<std::size_t>> counts(kOuter);
        for (auto& c : counts) c.store(0);
        parallelFor(0, kOuter, [&](std::size_t o) {
            parallelFor(0, kInner, [&](std::size_t i) {
                work(i);
                counts[o].fetch_add(1, std::memory_order_relaxed);
            }, 1);
        }, 1);
        for (std::size_t o = 0; o < kOuter; ++o) EXPECT_EQ(counts[o].load(), kInner) << "cap " << cap;
    }
    setParallelThreads(0);
}

}  // namespace
}  // namespace groomlab
