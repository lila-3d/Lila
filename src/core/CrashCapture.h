#pragma once

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
void addSystemInfo(Report& r);

/** Adds memory counters of process to s. */
void addProcessMemory(Section& s, HANDLE process);

/** Adds pid, start time, uptime (to now, or to exit for an exited process) and the image path. */
void addProcessTimes(Section& s, HANDLE process);

/** Link time and PDB id (GUID + age) of a module mapped in this process. */
void addModuleIdentity(Section& s, HMODULE module);

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
Capture captureProcess(HANDLE process, DWORD pid, DWORD crashedTid, const CONTEXT* crashedContext, uint64_t faultAddress,
                       const std::wstring& symbolPath, bool otherThreads = true);

/** [python]: what faulthandler wrote to file, if anything. */
void addPythonTracebacks(Report& r, const fs::path& file);

/** [stack] (the crashed thread), [threads] and [modules]. */
void addCapture(Report& r, const Capture& capture);

/** ThreadInfo + indirectly referenced memory + data segments; full memory when full is set. */
bool writeMinidump(HANDLE process, DWORD pid, DWORD tid, EXCEPTION_POINTERS* exception, bool clientPointers, const fs::path& file,
                   bool full, std::string* err = nullptr);

/** Fills [crash] from an exception record: code, name, address, parameters, faulting module. */
void addException(Report& r, const EXCEPTION_RECORD& record, DWORD tid, const Capture& capture);

/** Report.wer contents (minus loaded modules) of reports for exeName written since sinceUnix, newest first. */
std::vector<std::string> werReports(const std::wstring& exeName, int64_t sinceUnix);

/** Application event log entries (Application Error, Windows Error Reporting) mentioning pid or exeName since sinceUnix. */
std::vector<std::string> eventLogEntries(DWORD pid, const std::wstring& exeName, int64_t sinceUnix);

std::string narrow(const std::wstring& w);
std::wstring widen(const std::string& s);
int64_t unixNow();
} // namespace lila::crash
#endif
