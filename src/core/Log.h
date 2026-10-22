#pragma once

#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace lila
{
enum class LogLevel : uint8_t
{
    Debug,
    Info,
    Warning,
    Error,
    Output, // Python stdout
    Echo,   // command echo, as Python
};

struct LogEntry
{
    uint64_t seq = 0;
    LogLevel level = LogLevel::Info;
    std::string text;
};

/** Thread-safe. Keeps the last few thousand entries so late listeners (MCP, script editor) can catch up. */
namespace log
{
using Sink = std::function<void(const LogEntry&)>;

void write(LogLevel level, std::string_view text);

/** Sinks are called on the writing thread. Returns an id for removeSink. */
int addSink(Sink sink);
void removeSink(int id);

/** Entries with seq > since, oldest first. */
std::vector<LogEntry> since(uint64_t since, size_t maxCount = 1000);

const char* levelName(LogLevel level);

template <class... Args> void debug(std::format_string<Args...> f, Args&&... a) { write(LogLevel::Debug, std::format(f, std::forward<Args>(a)...)); }
template <class... Args> void info(std::format_string<Args...> f, Args&&... a) { write(LogLevel::Info, std::format(f, std::forward<Args>(a)...)); }
template <class... Args> void warn(std::format_string<Args...> f, Args&&... a) { write(LogLevel::Warning, std::format(f, std::forward<Args>(a)...)); }
template <class... Args> void error(std::format_string<Args...> f, Args&&... a) { write(LogLevel::Error, std::format(f, std::forward<Args>(a)...)); }
} // namespace log
} // namespace lila
