#include "core/CrashWatch.h"

#ifdef _WIN32

namespace lila::crash::watch
{
fs::path sessionFile(const fs::path& sessionsDir, DWORD pid, const wchar_t* suffix)
{
    return sessionsDir / (std::to_wstring(pid) + suffix);
}

std::wstring symbolPathFor(const fs::path& exe)
{
    return exe.parent_path().wstring();
}

namespace
{
fs::path processImage(HANDLE process)
{
    wchar_t path[MAX_PATH * 4];
    DWORD size = DWORD(std::size(path));
    if (!QueryFullProcessImageNameW(process, 0, path, &size))
        return {};
    return fs::path(std::wstring(path, size));
}

void removeSessionFiles(const fs::path& sessionsDir, DWORD pid)
{
    std::error_code ec;
    for (const wchar_t* suffix : {L".crash.txt", L".crash.txt.tmp", L".wer.txt", L".wer.txt.tmp", L".wer.dmp", L".watch.dmp", L".python.txt"})
        fs::remove(sessionFile(sessionsDir, pid, suffix), ec);
}
} // namespace

Result run(const Options& o)
{
    Result res;
    auto* shared = o.shared ? static_cast<Shared*>(MapViewOfFile(o.shared, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared))) : nullptr;
    if (shared && (shared->magic != kSharedMagic || shared->size != sizeof(Shared)))
        shared = nullptr;
    DWORD pid = GetProcessId(o.process);
    fs::path exe = processImage(o.process);
    std::wstring symbols = symbolPathFor(exe);

    Report r;
    Capture capture;
    fs::path dump = sessionFile(o.sessionsDir, pid, L".watch.dmp");
    std::string dumpError;
    bool haveDump = false;
    bool signalled = false;
    Section live; // [process] taken from the living process

    HANDLE waits[2] = {o.process, o.crashEvent};
    DWORD w = WaitForMultipleObjects(o.crashEvent ? 2 : 1, waits, FALSE, INFINITE);
    if (w == WAIT_OBJECT_0 + 1 && shared && shared->crashed)
    {
        signalled = true;
        EXCEPTION_RECORD record = shared->record;
        CONTEXT context = shared->context;
        DWORD tid = shared->crashedTid;
        capture = captureProcess(o.process, pid, tid, &context, uint64_t(record.ExceptionAddress), symbols);
        addProcessMemory(live, o.process);
        haveDump = writeMinidump(o.process, pid, tid, reinterpret_cast<EXCEPTION_POINTERS*>(shared->exceptionPointers), TRUE, dump,
                                 o.fullDump, &dumpError);
        addException(r, record, tid, capture);
        r.section("crash").set("detected.by", "Lila's crash handler; captured by the lilacrash watchdog");
        SetEvent(o.doneEvent);
        // Lila still runs its emergency save, then lets the exception end it.
        if (WaitForSingleObject(o.process, o.fullDump ? 600000 : 180000) == WAIT_TIMEOUT)
        {
            TerminateProcess(o.process, record.ExceptionCode);
            WaitForSingleObject(o.process, 10000);
        }
    }
    DWORD exitCode = 0;
    GetExitCodeProcess(o.process, &exitCode);
    res.crashed = signalled || isCrashExitCode(exitCode);
    if (!res.crashed)
    {
        removeSessionFiles(o.sessionsDir, pid);
        return res;
    }
    int64_t crashTime = unixNow();

    // What Lila flushed last: app, context, gpu, commands, log.
    Report flushed = parseReport(readText(sessionFile(o.sessionsDir, pid, L".crash.txt")));
    flushed.summary.clear();
    Report base = flushed;
    base.merge(r);
    r = std::move(base);
    Section& crash = r.section("crash");
    crash.set("time", isoTimeUtc(crashTime));
    crash.set("exitCode", hex(exitCode, 8));
    if (!flushed.find("log"))
        crash.set("flushed.state", "none (Lila had not flushed its log yet, or the session file was missing)");

    if (!signalled)
    {
        fs::path werText = sessionFile(o.sessionsDir, pid, L".wer.txt");
        std::error_code ec;
        if (fs::is_regular_file(werText, ec))
        {
            r.merge(parseReport(readText(werText)));
            fs::path werDump = sessionFile(o.sessionsDir, pid, L".wer.dmp");
            if (fs::is_regular_file(werDump, ec))
            {
                fs::rename(werDump, dump, ec);
                haveDump = !ec;
            }
        }
        else
        {
            crash.set("exception.code", hex(exitCode, 8));
            crash.set("exception.name", exceptionName(exitCode));
            crash.set("exception.description", exceptionDescription(exitCode));
            crash.set("detected.by", "the process exit code (no handler ran and the WER helper did not report; no stack)");
        }
    }
    else
    {
        if (shared->note[0])
            crash.set("note", std::string(shared->note, strnlen(shared->note, sizeof(shared->note))));
        std::string saved(shared->emergencySave, strnlen(shared->emergencySave, sizeof(shared->emergencySave)));
        crash.set("emergency.save", saved.empty() ? "none (nothing unsaved, or it failed)" : saved);
        addCapture(r, capture);
        Section& proc = r.section("process");
        for (const auto& [k, v] : live.fields)
            proc.set(k, v);
        proc.set("memory.asOf", "the crash");
    }

    addPythonTracebacks(r, sessionFile(o.sessionsDir, pid, L".python.txt"));
    Section& app = r.section("app");
    addProcessTimes(app, o.process);
    addSystemInfo(r);

    // Windows logs the crash a moment after the process is gone.
    std::wstring exeName = exe.filename().wstring();
    std::vector<std::string> events;
    for (DWORD waited = 0;; waited += 500)
    {
        events = eventLogEntries(pid, exeName, crashTime - 60);
        if (!events.empty() || waited >= o.werWaitMs)
            break;
        Sleep(500);
    }
    r.section("windows.eventlog").lines = events;
    r.section("windows.wer").lines = werReports(exeName, crashTime - 60);

    fs::path dir;
    if (shared && shared->reportDir[0])
        dir = fs::path(std::wstring(shared->reportDir, wcsnlen(shared->reportDir, std::size(shared->reportDir))));
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec))
        dir = createReportDir(o.crashesDir, crashTime, pid);
    if (haveDump)
    {
        fs::rename(dump, dir / "minidump.dmp", ec);
        if (ec)
        {
            fs::copy_file(dump, dir / "minidump.dmp", fs::copy_options::overwrite_existing, ec);
            fs::remove(dump, ec);
        }
        r.section("crash").set("minidump", "minidump.dmp");
    }
    else
        r.section("crash").set("minidump", dumpError.empty() ? "none" : "none (" + dumpError + ")");

    sortSections(r);
    r.summary = summarize(r);
    writeTextAtomic(dir / "report.txt", renderReport(r));
    removeSessionFiles(o.sessionsDir, pid);
    pruneReports(o.crashesDir, o.keep);
    res.reportDir = dir;
    res.summary = r.summary;
    return res;
}

void writeWerCapture(HANDLE process, DWORD pid, HANDLE thread, const EXCEPTION_RECORD& record, const CONTEXT& context,
                     const fs::path& sessionsDir, bool fullDump)
{
    Report r;
    DWORD tid = GetThreadId(thread);
    std::wstring symbols = symbolPathFor(processImage(process));
    Capture c = captureProcess(process, pid, tid, &context, uint64_t(record.ExceptionAddress), symbols);
    addException(r, record, tid, c);
    r.section("crash").set("detected.by", "Windows Error Reporting (lilawer.dll in WerFault.exe)");
    addCapture(r, c);
    Section& proc = r.section("process");
    addProcessMemory(proc, process);
    proc.set("memory.asOf", "the crash");

    EXCEPTION_RECORD rec = record;
    CONTEXT ctx = context;
    EXCEPTION_POINTERS ep{&rec, &ctx};
    std::string err;
    if (!writeMinidump(process, pid, tid, &ep, FALSE, sessionFile(sessionsDir, pid, L".wer.dmp"), fullDump, &err))
        r.section("crash").set("minidump.error", err);
    // Last, so the watchdog never sees the text without the dump.
    writeTextAtomic(sessionFile(sessionsDir, pid, L".wer.txt"), renderReport(r));
}
} // namespace lila::crash::watch
#endif
