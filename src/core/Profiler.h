#pragma once

#include "core/Export.h"

#include <cstdint>
#include <string_view>

/** The Tracy client every Lila process carries: what Help > Record Performance Trace records from. */
namespace lila::profiler
{
/** Built with Tracy. */
LILA_FOUNDATION_API bool available();

/** The 127.0.0.1 port this process's client listens on, 0 when it isn't listening (yet). */
LILA_FOUNDATION_API uint16_t port();

/** A capture is connected and recording. */
LILA_FOUNDATION_API bool connected();

/** Running as administrator: the only way Windows lets a process sample call stacks and see context switches. */
LILA_FOUNDATION_API bool elevated();

/** Call-stack sampling and context switches for a recording; false where the OS refuses (not elevated). */
LILA_FOUNDATION_API bool beginSystemTracing();
LILA_FOUNDATION_API void endSystemTracing();

/** Text a recording carries in its header: version, system, GPU. Sent with every new connection. */
LILA_FOUNDATION_API void setAppInfo(std::string_view text);
} // namespace lila::profiler
