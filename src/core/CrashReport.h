#pragma once

#include "core/Export.h"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
 * Crash report text, shared by Lila, the lilacrash watchdog and the lilawer WER helper. No dependencies beyond the
 * standard library so the helpers stay small.
 *
 * A report is a list of sections, each rendered as
 *   [name]
 *   key: value
 *     a verbatim line (indented two spaces)
 * which reads well and parses back with parseReport().
 */
namespace lila::crash
{
namespace fs = std::filesystem;

struct LILA_CRASHKIT_API Section
{
    std::string name;
    std::vector<std::pair<std::string, std::string>> fields;
    std::vector<std::string> lines;

    void set(std::string_view key, std::string_view value);
    /** Empty when absent. */
    std::string get(std::string_view key) const;
    void line(std::string_view text);
};

struct LILA_CRASHKIT_API Report
{
    /** One line at the very top: what crashed, where. */
    std::string summary;
    std::vector<Section> sections;

    Section& section(std::string_view name);
    const Section* find(std::string_view name) const;
    /** Adds every field and line of other, replacing fields that already exist and the lines of sections other has lines in. */
    void merge(const Report& other);
};

LILA_CRASHKIT_API std::string renderReport(const Report& r);
/** Puts the sections in reading order: what crashed first, the bulky ones (threads, modules) last. */
LILA_CRASHKIT_API void sortSections(Report& r);
/**
 * "Lila crashed: heap corruption (0xC0000374 STATUS_HEAP_CORRUPTION) in ntdll.dll+0x1037f1, at lila.exe!lila::Foo::bar
 * (X:/Lila/src/a.cpp:12)" from [crash] and the first non-system frame of [stack].
 */
LILA_CRASHKIT_API std::string summarize(const Report& r);
LILA_CRASHKIT_API Report parseReport(std::string_view text);

/** Fixed-capacity line buffer, oldest dropped first. Thread-safe. */
class LILA_CRASHKIT_API LineRing
{
public:
    explicit LineRing(size_t capacity) : mCapacity(capacity ? capacity : 1) {}

    void push(std::string_view line);
    std::vector<std::string> lines() const;
    /** As lines(), but gives up (returning false) rather than block, for use from a crash handler. */
    bool tryLines(std::vector<std::string>& out) const;
    size_t size() const;
    uint64_t pushed() const;

private:
    size_t mCapacity;
    mutable std::mutex mMutex;
    std::vector<std::string> mLines;
    size_t mHead = 0; // index of the oldest line once full
    uint64_t mPushed = 0;
};

/** STATUS_HEAP_CORRUPTION and friends; empty for an unknown code. */
LILA_CRASHKIT_API const char* exceptionName(uint32_t code);
/** "heap corruption"; falls back to the name, then to "exception". */
LILA_CRASHKIT_API std::string exceptionDescription(uint32_t code);
/** Exit codes that mean the process died of a fault rather than exiting or being killed. */
LILA_CRASHKIT_API bool isCrashExitCode(uint32_t code);

/** Codes Lila raises into its own report for crashes that are not SEH exceptions. */
constexpr uint32_t kCodeTerminate = 0xE0000001; // std::terminate
constexpr uint32_t kCodeAbort = 0xE0000002;     // abort()
constexpr uint32_t kCodePurecall = 0xE0000003;  // pure virtual call
constexpr uint32_t kCodeInvalidParameter = 0xE0000004;
constexpr uint32_t kCodeCppException = 0xE06D7363; // MSVC's code for a thrown C++ exception

/** Replaces every occurrence of each secret, and anything that looks like a token assignment, with <redacted>. */
LILA_CRASHKIT_API std::string redact(std::string_view text, const std::vector<std::string>& secrets);

LILA_CRASHKIT_API std::string hex(uint64_t v, int digits = 0);

/** "2026-10-22_14-03-11" in local time. */
LILA_CRASHKIT_API std::string folderTimestamp(int64_t unixSeconds);
/** "2026-10-22T14:03:11Z". */
LILA_CRASHKIT_API std::string isoTimeUtc(int64_t unixSeconds);
LILA_CRASHKIT_API std::string formatDuration(int64_t seconds);

/** A new <root>/<timestamp>_<pid> folder (suffixed further when it already exists). */
LILA_CRASHKIT_API fs::path createReportDir(const fs::path& root, int64_t unixSeconds, uint32_t pid);

/** Crash folders in root (those holding a report.txt), oldest first. */
LILA_CRASHKIT_API std::vector<fs::path> listReports(const fs::path& root);
/** Deletes the oldest crash folders so at most keep remain. Returns how many it removed. */
LILA_CRASHKIT_API size_t pruneReports(const fs::path& root, size_t keep);

LILA_CRASHKIT_API std::string readText(const fs::path& p);
/** Writes via a temporary file and a rename, so a reader never sees half a file. */
LILA_CRASHKIT_API bool writeTextAtomic(const fs::path& p, std::string_view text);
} // namespace lila::crash
