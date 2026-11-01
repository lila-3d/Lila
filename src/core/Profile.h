#pragma once

#ifdef LILA_TRACY
#include <tracy/Tracy.hpp>

// Put on the first line of any function that does real work.
#define LILA_ZONE() ZoneScoped
#define LILA_ZONE_N(name) ZoneScopedN(name)
#define LILA_FRAME_MARK() FrameMark
#define LILA_ZONE_TEXT(text, size) ZoneText(text, size)
#define LILA_THREAD_NAME(name) tracy::SetThreadName(name)
/** name must be a string literal; the value is a number. */
#define LILA_PLOT(name, value) TracyPlot(name, value)
#define LILA_MESSAGE(text, size) TracyMessage(text, size)
/** A capture is recording: skip work that only feeds the trace. */
#define LILA_PROFILER_CONNECTED() TracyIsConnected
#else
#define LILA_ZONE()
#define LILA_ZONE_N(name)
#define LILA_FRAME_MARK()
#define LILA_ZONE_TEXT(text, size)
#define LILA_THREAD_NAME(name) ((void)(name))
#define LILA_PLOT(name, value) ((void)(value))
#define LILA_MESSAGE(text, size)
#define LILA_PROFILER_CONNECTED() false
#endif
