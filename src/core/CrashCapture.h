#pragma once

#include "core/Export.h"

#include "core/CrashReport.h"

#ifdef _WIN32
#include <windows.h>

/**
 * Windows crash capture that works on any process this one can open: symbolized stacks of every thread, minidumps,
 * machine description, and what Windows Error Reporting recorded. Used in-process as a fallback, by the lilacrash
 * watchdog, and by the lilawer helper inside WerFault.exe.
 */
namespace lila::crash
{
/** Adds [system], [gpus] and [monitors] for this machine. */
LILA_CRASHKIT_API void addSystemInfo(Report& r);

/** Adds memory counters of process to s. */
LILA_CRASHKIT_API void addProcessMemory(Section& s, HANDLE process);

/** Adds pid, start time, uptime (to now, or to exit for an exited process) and the image path. */
LILA_CRASHKIT_API void addProcessTimes(Section& s, HANDLE process);

/** Link time and PDB id (GUID + age) of a module mapped in this process. */
LILA_CRASHKIT_API void addModuleIdentity(Section& s, HMODULE module);

struct Capture
{
    std::string module;      // faulting module, file name only
    uint64_t moduleOffset = 0;
    std::vector<std::string> crashedStack;
    std::vector<std::string> threads; // every thread, crashed first
    std::vector<std::string> modules;
};

/**
 * Walks every thread of process (suspending each while it is walked). crashedContext, when given, is the context of
 * crashedTid at the fault, used instead of the thread's live one. symbolPath lists the folders holding the PDBs.
 * otherThreads = false walks only the crashed thread: suspending a thread of the calling process can deadlock it on
 * a lock that thread holds.
 */
LILA_CRASHKIT_API Capture captureProcess(HANDLE process, DWORD pid, DWORD crashedTid, const CONTEXT* crashedContext, uint64_t faultAddress,
                       const std::wstring& symbolPath, bool otherThreads = true);

/** [python]: what faulthandler wrote to file, if anything. */
LILA_CRASHKIT_API void addPythonTracebacks(Report& r, const fs::path& file);

/** [stack] (the crashed thread), [threads] and [modules]. */
LILA_CRASHKIT_API void addCapture(Report& r, const Capture& capture);

/** ThreadInfo + indirectly referenced memory + data segments; full memory when full is set. */
LILA_CRASHKIT_API bool writeMinidump(HANDLE process, DWORD pid, DWORD tid, EXCEPTION_POINTERS* exception, bool clientPointers, const fs::path& file,
                   bool full, std::string* err = nullptr);

/** Fills [crash] from an exception record: code, name, address, parameters, faulting module. */
LILA_CRASHKIT_API void addException(Report& r, const EXCEPTION_RECORD& record, DWORD tid, const Capture& capture);

/** Report.wer contents (minus loaded modules) of reports for exeName written since sinceUnix, newest first. */
LILA_CRASHKIT_API std::vector<std::string> werReports(const std::wstring& exeName, int64_t sinceUnix);

/** Application event log entries (Application Error, Windows Error Reporting) mentioning pid or exeName since sinceUnix. */
LILA_CRASHKIT_API std::vector<std::string> eventLogEntries(DWORD pid, const std::wstring& exeName, int64_t sinceUnix);

LILA_CRASHKIT_API std::string narrow(const std::wstring& w);
LILA_CRASHKIT_API std::wstring widen(const std::string& s);
LILA_CRASHKIT_API int64_t unixNow();
} // namespace lila::crash
#endif
