#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace lila::crash
{
/**
 * Installs the in-process crash handlers (unhandled exception filter, a vectored handler for stack overflow and heap
 * corruption, std::terminate, abort, pure virtual calls, invalid CRT parameters) and starts keeping the last log lines
 * and commands, flushed every second to <userConfigDir>/sessions/<pid>.crash.txt for the watchdog.
 */
void install();

struct WatchOptions
{
    /** Show the crash dialog from the watchdog when Lila dies. LILA_CRASH_NO_UI=1 overrides it off. */
    bool ui = true;
    size_t keepReports = 20;
    bool fullDump = false;
    /** Register lilawer.dll with Windows Error Reporting, so fail-fast crashes still get stacks and a minidump. */
    bool registerWer = true;
};

/**
 * Starts lilacrash.exe (next to the exe) to watch this process. It needs no window or console, opens no sockets, and
 * exits with Lila; on a crash it writes <userConfigDir>/crashes/<timestamp>_<pid>/ (report.txt + minidump.dmp).
 * False when the helper is missing; crashes are then reported in-process only.
 */
bool startWatchdog(const WatchOptions& options);
/** The watchdog's process id, 0 when none is running. */
uint32_t watchdogPid();

/** Stops the flushing and removes this session's crash files; call on a clean exit. */
void shutdown();

/** A value for the report, e.g. setContext("context", "scene", path). Cheap; unchanged values cost nothing. */
void setContext(std::string_view section, std::string_view key, std::string_view value);
/** Where Python's faulthandler writes tracebacks for the report; empty before install(). */
std::filesystem::path pythonTracebackFile();

/** A string never to appear in a report (the MCP token). */
void addSecret(std::string secret);

/**
 * Called once from the crash handler to save the user's work; writes the saved file's path into path (empty when
 * nothing was saved). It runs on its own thread, so a lock held by the crashing thread cannot hang the handler past a
 * short wait, and a second fault inside it only ends that thread.
 */
using EmergencySaveFn = void (*)(char* path, size_t size);
void setEmergencySave(EmergencySaveFn fn);

/** <userConfigDir>/crashes. */
std::filesystem::path reportsDir();
/** Crash folders, newest first. */
std::vector<std::filesystem::path> reports();
/** The report's one-line summary. */
std::string reportSummary(const std::filesystem::path& dir);
/** The pid of the process the report is about, 0 when unknown. */
int64_t reportPid(const std::filesystem::path& dir);
/** One field of the report, e.g. reportField(dir, "app", "mode"); empty when absent. */
std::string reportField(const std::filesystem::path& dir, std::string_view section, std::string_view key);
/** Whether the user has seen the report in Lila's startup dialog. */
bool acknowledged(const std::filesystem::path& dir);
void acknowledge(const std::filesystem::path& dir);
} // namespace lila::crash
