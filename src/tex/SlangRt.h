#pragma once

#include "core/Export.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/**
 * Slang at runtime: generated passes compile to host-callable CPU code (through slang-llvm, in process) or to SPIR-V.
 * Compiled kernels are cached by source hash for the life of the process.
 */
namespace lila::tex::slangrt
{
/** A pass compiled for the CPU. Call run() from any thread once compiled. */
struct CpuKernel
{
    using Fn = void (*)(void* varying, void* entryParams, void* globals);
    Fn fn = nullptr;
    /** Byte offsets of gPass, gU, gArena and gOut in the globals block, and its size. */
    size_t offPass = 0, offU = 0, offArena = 0, offOut = 0, globalsSize = 0;
    std::shared_ptr<void> keepAlive;
};

/** Whether the CPU target works here (slang-llvm found); why not otherwise. */
LILA_SCENE_API bool cpuAvailable(std::string* why = nullptr);

/** Compiles (or takes from the cache) a pass whose entry is texMain. Null with *err set on failure. */
LILA_SCENE_API std::shared_ptr<const CpuKernel> cpuKernel(const std::string& source, uint64_t hash, std::string* err);

/** SPIR-V 1.6 for Vulkan, entry texMain, bindings 0-3 in set 0 as Plan.h lists them. Empty with *err set on failure. */
LILA_SCENE_API std::vector<uint32_t> spirv(const std::string& source, std::string* err);

struct CacheStats
{
    size_t compiles = 0;
    size_t hits = 0;
    size_t failures = 0;
};
LILA_SCENE_API CacheStats cacheStats();
LILA_SCENE_API void clearCache();

/** The CPU side of a compiled pass's globals and entry varying: what gPass points at, the gU, gArena and gOut views, and the group range. */
struct PassHeader
{
    float res[4];
    float uv[4];
};

struct BufferView
{
    const void* data;
    size_t count;
};

struct Varying
{
    uint32_t start[3];
    uint32_t end[3];
};
} // namespace lila::tex::slangrt
