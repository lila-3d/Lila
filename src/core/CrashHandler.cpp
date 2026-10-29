#include "core/CrashHandler.h"

#include "core/CrashReport.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Version.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <map>
#include <mutex>
#include <new>
#include <thread>
#include <typeinfo>

#ifdef _WIN32
#include "core/CrashWatch.h"

#include <csignal>
#include <werapi.h>
#pragma comment(lib, "wer.lib")
#else
#include <unistd.h>
#endif

namespace lila::crash
{
namespace fs = std::filesystem;

namespace
{
struct State
{
    std::mutex mutex;
    std::map<std::string, std::vector<std::pair<std::string, std::string>>, std::less<>> context;
    std::vector<std::string> secrets;
    uint64_t version = 0;
    LineRing log{500};
    LineRing commands{50};
    fs::path sessionsDir;
    fs::path flushFile;
    size_t keep = 20;
    bool fullDump = false;
    std::atomic<bool> running{false};
    std::mutex wakeMutex;
    std::condition_variable wake;
    std::thread* flusher = nullptr;
    int sink = 0;
};

/** Never destroyed: the flusher and a late crash may still use it during static destruction. */
State& st()
{
    static State* s = new State;
    return *s;
}

uint32_t pid()
{
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return uint32_t(getpid());
#endif
}

std::string clockTime()
{
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    int ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
    return buf;
}

void onLog(const LogEntry& e)
{
    std::string_view text = e.text;
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.remove_suffix(1);
    if (text.find_first_not_of(" \t\r\n") == std::string_view::npos)
        return;
    State& s = st();
    s.log.push(clockTime() + " " + log::levelName(e.level) + ": " + std::string(text));
    if (e.level == LogLevel::Echo)
        s.commands.push(clockTime() + " " + std::string(text));
}

/** What Lila knows about itself, flushed for the watchdog and used in-process as the report's base. */
bool buildLiveReport(Report& r, bool crashing)
{
    State& s = st();
    std::vector<std::string> secrets;
    {
        std::unique_lock lock(s.mutex, std::defer_lock);
        if (!crashing)
            lock.lock();
        else if (!lock.try_lock())
            return false;
        secrets = s.secrets;
        for (const auto& [section, fields] : s.context)
        {
            Section& sec = r.section(section);
            for (const auto& [k, v] : fields)
                sec.set(k, redact(v, s.secrets));
        }
    }
    std::vector<std::string> log, commands;
    if (crashing)
    {
        if (!s.log.tryLines(log) || !s.commands.tryLines(commands))
            return false;
    }
    else
    {
        log = s.log.lines();
        commands = s.commands.lines();
    }
    for (std::string& l : log)
        l = redact(l, secrets);
    for (std::string& l : commands)
        l = redact(l, secrets);
    r.section("commands").lines = std::move(commands);
    r.section("log").lines = std::move(log);
#ifdef _WIN32
    Section& proc = r.section("process");
    addProcessMemory(proc, GetCurrentProcess());
    proc.set("memory.asOf", isoTimeUtc(unixNow()));
#endif
    return true;
}

void flushNow(bool crashing)
{
    State& s = st();
    if (s.flushFile.empty())
        return;
    Report r;
    if (!buildLiveReport(r, crashing))
        return;
    writeTextAtomic(s.flushFile, renderReport(r));
}

void flusher()
{
    State& s = st();
    uint64_t last = ~0ull;
    int idle = 0;
    while (s.running)
    {
        uint64_t now;
        {
            std::scoped_lock lock(s.mutex);
            now = s.version + s.log.pushed() + s.commands.pushed();
        }
        // Memory counters drift without any log traffic, so refresh every 10 s regardless.
        if (now != last || ++idle >= 10)
        {
            flushNow(false);
            last = now;
            idle = 0;
        }
        std::unique_lock lock(s.wakeMutex);
        s.wake.wait_for(lock, std::chrono::seconds(1), [&] { return !s.running.load(); });
    }
}

#ifdef _WIN32
watch::Shared* gShared = nullptr;
HANDLE gCrashEvent = nullptr;
HANDLE gDoneEvent = nullptr;
HANDLE gWatchdog = nullptr;
watch::WerBlock gWer;

EmergencySaveFn gEmergencySave = nullptr;
std::atomic<DWORD> gCrashingThread{0};
std::atomic<DWORD> gHelperThread{0};
std::atomic<bool> gHandled{false};
char gSavedPath[1024];

/** The crash being handled, kept out of the crashing thread's stack (it may have overflowed). */
struct Pending
{
    EXCEPTION_RECORD record;
    CONTEXT context;
    EXCEPTION_POINTERS pointers;
    DWORD tid;
    char note[1024];
};
Pending gPending;

using HelperFn = void (*)();

DWORD WINAPI helperThread(void* param)
{
    gHelperThread = GetCurrentThreadId();
    reinterpret_cast<HelperFn>(param)();
    return 0;
}

/** Runs fn on a fresh thread and stack; a fault inside it ends only that thread (see enter()). */
void runOnThread(HelperFn fn, DWORD timeoutMs)
{
    HANDLE t = CreateThread(nullptr, 256 * 1024, helperThread, reinterpret_cast<void*>(fn), 0, nullptr);
    if (!t)
        return;
    // Long enough for the work; short enough that a deadlock against the crashed thread cannot hang forever.
    WaitForSingleObject(t, timeoutMs);
    CloseHandle(t);
}

void emergencySave()
{
    gSavedPath[0] = 0;
    if (gEmergencySave)
        gEmergencySave(gSavedPath, sizeof(gSavedPath));
}

/** False when this thread must not handle the crash: a fault inside a crash helper, or a crash already handled. */
bool enter()
{
    DWORD self = GetCurrentThreadId();
    if (self == gHelperThread.load())
        ExitThread(1);
    DWORD expected = 0;
    if (gCrashingThread.compare_exchange_strong(expected, self))
        return true;
    if (expected == self)
    {
        // Reported already (the vectored handler, or std::terminate on its way to abort): let it go on dying.
        if (gHandled)
            return false;
        TerminateProcess(GetCurrentProcess(), 3);
    }
    // Another thread is already handling a crash and will end the process.
    Sleep(INFINITE);
    return false;
}

bool watchdogAlive()
{
    return gShared && gWatchdog && WaitForSingleObject(gWatchdog, 0) == WAIT_TIMEOUT;
}

void createReportFolder()
{
    if (!gShared)
        return;
    fs::path dir = createReportDir(reportsDir(), unixNow(), pid());
    wcsncpy_s(gShared->reportDir, dir.c_str(), _TRUNCATE);
}

void flushCrashing()
{
    flushNow(true);
    createReportFolder();
}

/** The whole report from inside the process, for when no watchdog is running. */
void writeLocalReport()
{
    State& s = st();
    Report r;
    if (!buildLiveReport(r, true) && !s.flushFile.empty())
        r = parseReport(readText(s.flushFile));
    r.summary.clear();
    std::wstring symbols = paths::executableDir().wstring();
    Capture capture = captureProcess(GetCurrentProcess(), pid(), gPending.tid, &gPending.context,
                                     uint64_t(gPending.record.ExceptionAddress), symbols, false);
    addException(r, gPending.record, gPending.tid, capture);
    Section& crash = r.section("crash");
    crash.set("time", isoTimeUtc(unixNow()));
    crash.set("detected.by", "Lila's crash handler (no watchdog was running; other threads not walked)");
    if (gPending.note[0])
        crash.set("note", gPending.note);
    addCapture(r, capture);
    addPythonTracebacks(r, pythonTracebackFile());
    addSystemInfo(r);
    addProcessTimes(r.section("app"), GetCurrentProcess());

    fs::path dir = createReportDir(reportsDir(), unixNow(), pid());
    std::string err;
    if (writeMinidump(GetCurrentProcess(), pid(), gPending.tid, &gPending.pointers, FALSE, dir / "minidump.dmp", s.fullDump, &err))
        crash.set("minidump", "minidump.dmp");
    else
        crash.set("minidump", "none (" + err + ")");
    sortSections(r);
    r.summary = summarize(r);
    writeTextAtomic(dir / "report.txt", renderReport(r));
    pruneReports(reportsDir(), s.keep);
    std::fprintf(stderr, "%s\ncrash report: %s\n", r.summary.c_str(), narrow(dir.wstring()).c_str());
    for (const std::string& f : capture.crashedStack)
        std::fprintf(stderr, "  %s\n", f.c_str());
    std::fflush(stderr);
    if (gEmergencySave)
    {
        runOnThread(emergencySave, 30000);
        Report done = parseReport(readText(dir / "report.txt"));
        done.section("crash").set("emergency.save", gSavedPath[0] ? gSavedPath : "none (nothing unsaved, or it failed)");
        writeTextAtomic(dir / "report.txt", renderReport(done));
    }
}

void handleCrash(const EXCEPTION_RECORD& record, const CONTEXT& context)
{
    gWer.handled = 1;
    gPending.record = record;
    gPending.context = context;
    gPending.pointers = {&gPending.record, &gPending.context};
    gPending.tid = GetCurrentThreadId();

    if (!watchdogAlive())
    {
        runOnThread(writeLocalReport, st().fullDump ? 300000 : 90000);
        return;
    }
    gShared->crashedTid = gPending.tid;
    gShared->record = gPending.record;
    gShared->context = gPending.context;
    gShared->exceptionPointers = uint64_t(&gPending.pointers);
    std::memcpy(gShared->note, gPending.note, sizeof(gShared->note));
    InterlockedExchange(&gShared->crashed, 1);
    SetEvent(gCrashEvent);
    HANDLE waits[2] = {gDoneEvent, gWatchdog};
    WaitForMultipleObjects(2, waits, FALSE, st().fullDump ? 300000 : 120000);

    // The capture is safe now, so the riskier work (allocating, taking locks) can follow.
    runOnThread(flushCrashing, 5000);
    if (gEmergencySave)
    {
        runOnThread(emergencySave, 30000);
        std::memcpy(gShared->emergencySave, gSavedPath, sizeof(gShared->emergencySave));
    }
    if (gShared->reportDir[0])
    {
        std::fprintf(stderr, "Lila crashed (exception 0x%08lX); crash report: %ls\n", record.ExceptionCode, gShared->reportDir);
        std::fflush(stderr);
    }
}

/** The thrown type of an MSVC C++ exception, read from its throw info. */
void cppExceptionType(const EXCEPTION_RECORD& r, char* out, size_t size)
{
    __try
    {
        if (r.NumberParameters < 4)
            return;
        auto imageBase = uintptr_t(r.ExceptionInformation[3]);
        auto* throwInfo = reinterpret_cast<const int*>(r.ExceptionInformation[2]);
        // _ThrowInfo: attributes, pmfnUnwind, pForwardCompat, pCatchableTypeArray (RVAs on x64).
        auto* catchables = reinterpret_cast<const int*>(imageBase + uintptr_t(throwInfo[3]));
        if (catchables[0] < 1)
            return;
        // _CatchableType: properties, pType (RVA) ...; the TypeDescriptor's name follows two pointers.
        auto* catchable = reinterpret_cast<const int*>(imageBase + uintptr_t(catchables[1]));
        auto* type = reinterpret_cast<const char*>(imageBase + uintptr_t(catchable[1]));
        std::snprintf(out, size, "unhandled C++ exception of type %s", type + 2 * sizeof(void*));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

LONG WINAPI onCrash(EXCEPTION_POINTERS* info)
{
    if (!enter())
        return EXCEPTION_CONTINUE_SEARCH;
    gPending.note[0] = 0;
    if (info->ExceptionRecord->ExceptionCode == kCodeCppException)
        cppExceptionType(*info->ExceptionRecord, gPending.note, sizeof(gPending.note));
    handleCrash(*info->ExceptionRecord, *info->ContextRecord);
    gHandled = true;
    return EXCEPTION_CONTINUE_SEARCH;
}

/** First chance, so only for the faults nothing recovers from; the filter may never run for these. */
LONG WINAPI onVectored(EXCEPTION_POINTERS* info)
{
    DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code == 0xC00000FD || code == 0xC0000374 || code == 0xC0000409)
        onCrash(info);
    return EXCEPTION_CONTINUE_SEARCH;
}

/** Reports a crash that is not an SEH exception, with the caller's context. */
void syntheticCrash(uint32_t code)
{
    CONTEXT ctx{};
    RtlCaptureContext(&ctx);
    EXCEPTION_RECORD rec{};
    rec.ExceptionCode = code;
    rec.ExceptionAddress = reinterpret_cast<void*>(ctx.Rip);
    handleCrash(rec, ctx);
    gHandled = true;
}

[[noreturn]] void onTerminate()
{
    if (enter())
    {
        gPending.note[0] = 0;
        try
        {
            if (std::exception_ptr e = std::current_exception())
                std::rethrow_exception(e);
            std::snprintf(gPending.note, sizeof(gPending.note), "std::terminate called without an active exception");
        }
        catch (const std::exception& e)
        {
            std::snprintf(gPending.note, sizeof(gPending.note), "unhandled %s: %s", typeid(e).name(), e.what());
        }
        catch (...)
        {
            std::snprintf(gPending.note, sizeof(gPending.note), "unhandled exception of unknown type");
        }
        syntheticCrash(kCodeTerminate);
    }
    std::abort();
}

void onAbort(int)
{
    if (!enter())
        return;
    std::snprintf(gPending.note, sizeof(gPending.note), "abort() called");
    syntheticCrash(kCodeAbort);
}

void onPurecall()
{
    if (enter())
    {
        std::snprintf(gPending.note, sizeof(gPending.note), "pure virtual function call");
        syntheticCrash(kCodePurecall);
    }
    TerminateProcess(GetCurrentProcess(), kCodePurecall);
}

void onInvalidParameter(const wchar_t* expr, const wchar_t* func, const wchar_t* file, unsigned line, uintptr_t)
{
    if (enter())
    {
        // Release CRTs pass no details.
        if (func)
            std::snprintf(gPending.note, sizeof(gPending.note), "invalid parameter: %ls in %ls (%ls:%u)", expr ? expr : L"", func,
                          file ? file : L"", line);
        else
            std::snprintf(gPending.note, sizeof(gPending.note), "invalid parameter passed to a CRT function");
        syntheticCrash(kCodeInvalidParameter);
    }
    TerminateProcess(GetCurrentProcess(), kCodeInvalidParameter);
}

void installFilters()
{
    SetUnhandledExceptionFilter(onCrash);
    std::set_terminate(onTerminate);
}

std::wstring quoted(const std::wstring& s)
{
    return L"\"" + s + L"\"";
}

void registerWerHelper()
{
    fs::path dll = paths::executableDir() / "lilawer.dll";
    std::error_code ec;
    if (!fs::is_regular_file(dll, ec))
        return;
    // WER only loads helper modules listed here; per user, so no elevation.
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\Windows Error Reporting\\RuntimeExceptionHelperModules", 0,
                        nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS)
    {
        DWORD zero = 0;
        if (RegQueryValueExW(key, dll.c_str(), nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            RegSetValueExW(key, dll.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&zero), sizeof(zero));
        RegCloseKey(key);
    }
    wcsncpy_s(gWer.sessionsDir, st().sessionsDir.c_str(), _TRUNCATE);
    gWer.fullDump = st().fullDump ? 1 : 0;
    HRESULT hr = WerRegisterRuntimeExceptionModule(dll.c_str(), &gWer);
    if (FAILED(hr))
        log::warn("crash: WER helper not registered ({:#x})", uint32_t(hr));
}
#endif
} // namespace

void install()
{
    State& s = st();
    if (s.running)
        return;
    s.sessionsDir = paths::userConfigDir() / "sessions";
    std::error_code ec;
    fs::create_directories(s.sessionsDir, ec);
    s.flushFile = s.sessionsDir / (std::to_string(pid()) + ".crash.txt");

    setContext("app", "version", version::number());
    setContext("app", "build", version::buildId());
    setContext("app", "pid", std::to_string(pid()));
#ifdef _WIN32
    wchar_t exePath[MAX_PATH * 4];
    DWORD exeLen = GetModuleFileNameW(nullptr, exePath, DWORD(std::size(exePath)));
    setContext("app", "exe", narrow(std::wstring(exePath, exeLen)));
    Section ident;
    addModuleIdentity(ident, GetModuleHandleW(nullptr));
    for (const auto& [k, v] : ident.fields)
        setContext("app", k, v);
    setContext("app", "commandLine", narrow(GetCommandLineW()));
    setContext("app", "started", isoTimeUtc(unixNow()));
    setContext("app", "configDir", narrow(paths::userConfigDir().wstring()));

    // Room for the handler when a thread overflows its stack.
    ULONG guarantee = 64 * 1024;
    SetThreadStackGuarantee(&guarantee);
    installFilters();
    AddVectoredExceptionHandler(1, onVectored);
    signal(SIGABRT, onAbort);
    // No "abort() has been called" message box, and no second WER report on the way out.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_purecall_handler(onPurecall);
    _set_invalid_parameter_handler(onInvalidParameter);
    WerSetFlags(WER_FAULT_REPORTING_NO_UI);
    // Inherited from some launchers (shells, IDEs); it keeps WER, and with it the WER helper, from ever running.
    SetErrorMode(GetErrorMode() & ~SEM_NOGPFAULTERRORBOX);
#endif

    s.sink = log::addSink(onLog);
    s.running = true;
    s.flusher = new std::thread(flusher);
}

bool startWatchdog(const WatchOptions& options)
{
    State& s = st();
    s.keep = options.keepReports;
    s.fullDump = options.fullDump;
#ifdef _WIN32
    // Qt, Python and drivers have all had a chance to install their own by now.
    installFilters();
    if (options.registerWer)
        registerWerHelper();
    if (gWatchdog)
        return true;
    fs::path exe = paths::executableDir() / "lilacrash.exe";
    std::error_code ec;
    if (!fs::is_regular_file(exe, ec))
    {
        log::warn("crash: {} is missing; crashes are reported in-process only", exe.string());
        return false;
    }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(watch::Shared), nullptr);
    HANDLE crashEvent = CreateEventW(&sa, TRUE, FALSE, nullptr);
    HANDLE doneEvent = CreateEventW(&sa, TRUE, FALSE, nullptr);
    HANDLE self = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &self,
                    PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE | PROCESS_TERMINATE | SYNCHRONIZE, TRUE, 0);
    void* view = mapping ? MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(watch::Shared)) : nullptr;
    auto fail = [&](const char* what) {
        log::warn("crash: cannot start the watchdog ({}: {})", what, GetLastError());
        for (HANDLE h : {mapping, crashEvent, doneEvent, self})
            if (h)
                CloseHandle(h);
        if (view)
            UnmapViewOfFile(view);
        return false;
    };
    if (!view || !crashEvent || !doneEvent || !self)
        return fail("handles");
    auto* shared = new (view) watch::Shared();

    std::wstring cmd = quoted(exe.wstring()) + L" --watch";
    auto arg = [&](const wchar_t* name, const std::wstring& value) { cmd += L" " + std::wstring(name) + L" " + quoted(value); };
    arg(L"--process", std::to_wstring(uintptr_t(self)));
    arg(L"--shared", std::to_wstring(uintptr_t(mapping)));
    arg(L"--crash", std::to_wstring(uintptr_t(crashEvent)));
    arg(L"--done", std::to_wstring(uintptr_t(doneEvent)));
    arg(L"--sessions", s.sessionsDir.wstring());
    arg(L"--crashes", reportsDir().wstring());
    arg(L"--keep", std::to_wstring(options.keepReports));
    wchar_t exePath[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, exePath, DWORD(std::size(exePath)));
    arg(L"--restart", std::wstring(exePath, n));
    if (!options.consentFile.empty())
        arg(L"--consent", options.consentFile.wstring());
    if (options.fullDump)
        cmd += L" --full";
    if (!options.ui)
        cmd += L" --no-ui";

    HANDLE inherit[4] = {self, mapping, crashEvent, doneEvent};
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<char> attrBuf(attrSize);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) ||
        !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr))
        return fail("attributes");
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    // No "app starting" cursor while it launches.
    si.StartupInfo.dwFlags = STARTF_FORCEOFFFEEDBACK;
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                             nullptr, &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrs);
    if (!ok)
        return fail("CreateProcess");
    CloseHandle(pi.hThread);
    CloseHandle(self);
    gWatchdog = pi.hProcess;
    gShared = shared;
    gCrashEvent = crashEvent;
    gDoneEvent = doneEvent;
    setContext("app", "watchdog", "lilacrash.exe pid " + std::to_string(pi.dwProcessId));
    return true;
#else
    (void)options;
    return false;
#endif
}

uint32_t watchdogPid()
{
#ifdef _WIN32
    return gWatchdog ? GetProcessId(gWatchdog) : 0;
#else
    return 0;
#endif
}

void shutdown()
{
    State& s = st();
    if (!s.running.exchange(false))
        return;
    s.wake.notify_all();
    if (s.flusher)
    {
        s.flusher->join();
        delete s.flusher;
        s.flusher = nullptr;
    }
    if (s.sink)
        log::removeSink(s.sink);
    s.sink = 0;
    std::error_code ec;
    fs::remove(s.flushFile, ec);
    s.flushFile.clear();
}

void setContext(std::string_view section, std::string_view key, std::string_view value)
{
    State& s = st();
    std::scoped_lock lock(s.mutex);
    auto it = s.context.find(section);
    if (it == s.context.end())
        it = s.context.emplace(std::string(section), std::vector<std::pair<std::string, std::string>>{}).first;
    for (auto& [k, v] : it->second)
        if (k == key)
        {
            if (v != value)
            {
                v = value;
                ++s.version;
            }
            return;
        }
    it->second.emplace_back(std::string(key), std::string(value));
    ++s.version;
}

fs::path pythonTracebackFile()
{
    State& s = st();
    return s.sessionsDir.empty() ? fs::path() : s.sessionsDir / (std::to_string(pid()) + ".python.txt");
}

void addSecret(std::string secret)
{
    if (secret.empty())
        return;
    State& s = st();
    std::scoped_lock lock(s.mutex);
    s.secrets.push_back(std::move(secret));
    ++s.version;
}

void setEmergencySave(EmergencySaveFn fn)
{
#ifdef _WIN32
    gEmergencySave = fn;
#else
    (void)fn;
#endif
}

std::string liveReport()
{
    Report r;
    r.summary = "Lila session report (not a crash)";
    buildLiveReport(r, false);
#ifdef _WIN32
    addSystemInfo(r);
#endif
    sortSections(r);
    return renderReport(r);
}

fs::path reportsDir()
{
    return paths::userConfigDir() / "crashes";
}

std::vector<fs::path> reports()
{
    std::vector<fs::path> all = listReports(reportsDir());
    std::reverse(all.begin(), all.end());
    return all;
}

std::string reportSummary(const fs::path& dir)
{
    return parseReport(readText(dir / "report.txt")).summary;
}

int64_t reportPid(const fs::path& dir)
{
    std::string p = reportField(dir, "app", "pid");
    return p.empty() ? 0 : std::strtoll(p.c_str(), nullptr, 10);
}

std::string reportField(const fs::path& dir, std::string_view section, std::string_view key)
{
    Report r = parseReport(readText(dir / "report.txt"));
    const Section* s = r.find(section);
    return s ? s->get(key) : std::string();
}

bool acknowledged(const fs::path& dir)
{
    std::error_code ec;
    return fs::exists(dir / ".acknowledged", ec);
}

void acknowledge(const fs::path& dir)
{
    writeTextAtomic(dir / ".acknowledged", "");
}
} // namespace lila::crash
