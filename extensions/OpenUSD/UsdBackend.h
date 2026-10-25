#pragma once

// The entry points OpenUSDCore exports and OpenUSD.dll looks up when the first USD command runs. Nothing here touches
// Pixar's headers, so the front loads without them. Errors come back as strings, never as exceptions across the boundary.

#include <lila/Command.h>

#include <nlohmann/json.hpp>

#include <string>

#if defined(_WIN32)
#define USD_BACKEND_EXPORT extern "C" __declspec(dllexport)
#else
#define USD_BACKEND_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace lila::usd
{
#if defined(_WIN32)
inline constexpr const char* kBackendFile = "OpenUSDCore.dll";
#elif defined(__APPLE__)
inline constexpr const char* kBackendFile = "OpenUSDCore.dylib";
#else
inline constexpr const char* kBackendFile = "OpenUSDCore.so";
#endif

/** Registers the package's plugInfo files. 0 on success. */
using BackendInit = int (*)(const char* packageRoot, std::string* error);
/** Runs importUsd or exportUsd. 0 on success, with the command's return value in result. */
using BackendRun = int (*)(const char* command, const Args* args, nlohmann::json* result, std::string* error);
} // namespace lila::usd
