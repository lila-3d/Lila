#pragma once

#include "core/Export.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

/**
 * Steam achievements and the stats behind the counted ones. Everything is a no-op unless Lila was started by Steam:
 * steam_api64.dll ships only in the Steam depot, so other installs never load it. Calls are safe from any thread and
 * never block; a worker thread owns the Steam API.
 */
namespace lila::achievements
{
/** Loads steam_api64.dll from beside the executable and connects to Steam. Once, at startup. */
LILA_FOUNDATION_API void start();
LILA_FOUNDATION_API void stop();
/** Steam is connected. */
LILA_FOUNDATION_API bool active();

LILA_FOUNDATION_API void unlock(std::string_view api);
/** Adds delta to an int stat and unlocks api once it reaches goal, showing progress on the way. */
LILA_FOUNDATION_API void add(std::string_view stat, int delta, std::string_view api, int goal);
/** ORs bits into an int stat kept as a set, and unlocks api once it holds every bit of all. */
LILA_FOUNDATION_API void addBits(std::string_view stat, uint32_t bits, std::string_view api, uint32_t all);

/** An unlock (goal 0) or a step of progress toward one, with Steam's localized name and description. */
struct Shown
{
    std::string api;
    std::string name;
    std::string description;
    int progress = 0;
    int goal = 0;
};
/**
 * Called on the main thread for each unlock and progress step, so Lila can show it: the Steam overlay doesn't draw
 * over Lila's window.
 */
LILA_FOUNDATION_API void setOnShown(std::function<void(const Shown&)> fn);
} // namespace lila::achievements
