#pragma once

#ifdef LILA_TRACY
#include <tracy/Tracy.hpp>

// Put on the first line of any function that does real work.
#define LILA_ZONE() ZoneScoped
#define LILA_ZONE_N(name) ZoneScopedN(name)
#define LILA_FRAME_MARK() FrameMark
#define LILA_ZONE_TEXT(text, size) ZoneText(text, size)
#define LILA_THREAD_NAME(name) tracy::SetThreadName(name)
#else
#define LILA_ZONE()
#define LILA_ZONE_N(name)
#define LILA_FRAME_MARK()
#define LILA_ZONE_TEXT(text, size)
#define LILA_THREAD_NAME(name) ((void)(name))
#endif
