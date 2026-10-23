#pragma once

#include "core/Export.h"

#include "core/CrashCapture.h"

#ifdef _WIN32
/**
 * The protocol between Lila and its crash helpers.
 *
 * lilacrash.exe (the watchdog) is started at launch with inherited handles to Lila's process, a shared Shared block and
 * two events. A crash Lila's own handlers see is signalled through the block, and the watchdog dumps and walks the
 * still-living process. A crash no handler sees (fail-fast, heap corruption) is caught by lilawer.dll, which WER
 * loads into WerFault.exe while the dead process is still mapped; it leaves <sessions>/<pid>.wer.txt and .wer.dmp
 * for the watchdog. Either way the watchdog writes <crashes>/<timestamp>_<pid>/report.txt once Lila is gone.
 */
namespace lila::crash::watch
{
constexpr uint32_t kSharedMagic = 0x4C435731; // 'LCW1'
constexpr uint32_t kWerMagic = 0x4C435745;    // 'LCWE'

struct Shared
{
    uint32_t magic = kSharedMagic;
    uint32_t size = sizeof(Shared);
    volatile LONG crashed = 0;
    DWORD crashedTid = 0;
    uint64_t exceptionPointers = 0; // EXCEPTION_POINTERS* in Lila's address space
    EXCEPTION_RECORD record{};
    CONTEXT context{};
    char note[1024] = {};          // what a C++ exception said, for instance
    char emergencySave[1024] = {}; // set after the capture, before Lila exits
    wchar_t reportDir[1024] = {};  // the folder Lila told the user about; the watchdog writes there
};

/** Registered with WerRegisterRuntimeExceptionModule; lilawer.dll reads it out of the crashed process. */
struct WerBlock
{
    uint32_t magic = kWerMagic;
    uint32_t size = sizeof(WerBlock);
    volatile LONG handled = 0; // Lila's own handler took this crash, so the WER helper stays out of it
    uint32_t fullDump = 0;
    wchar_t sessionsDir[1024] = {};
};

LILA_CRASHKIT_API fs::path sessionFile(const fs::path& sessionsDir, DWORD pid, const wchar_t* suffix);

struct Options
{
    HANDLE process = nullptr;
    HANDLE shared = nullptr; // file mapping holding Shared
    HANDLE crashEvent = nullptr;
    HANDLE doneEvent = nullptr;
    fs::path sessionsDir;
    fs::path crashesDir;
    size_t keep = 20;
    bool fullDump = false;
    /** How long to wait for Windows to log the crash (event log, WER archive) before writing the report. */
    DWORD werWaitMs = 8000;
};

struct Result
{
    bool crashed = false;
    fs::path reportDir; // empty when nothing was written
    std::string summary;
};

/** Waits for the process to exit; on a crash writes the report folder. */
LILA_CRASHKIT_API Result run(const Options& o);

/** The lilawer.dll side: writes <sessions>/<pid>.wer.txt and .wer.dmp for a crash WER reports. */
LILA_CRASHKIT_API void writeWerCapture(HANDLE process, DWORD pid, HANDLE thread, const EXCEPTION_RECORD& record, const CONTEXT& context,
                     const fs::path& sessionsDir, bool fullDump);

/** The symbol search path for an exe: its own folder. */
LILA_CRASHKIT_API std::wstring symbolPathFor(const fs::path& exe);
} // namespace lila::crash::watch
#endif
