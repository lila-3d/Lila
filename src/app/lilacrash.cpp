// lilacrash.exe: Lila's crash watchdog. Started by Lila with --watch; never shows a window unless Lila crashed.
// --send <report folder> uploads one report to Sentry and exits.

#include "core/CrashUpload.h"
#include "core/CrashWatch.h"

#include <commctrl.h>
#include <shellapi.h>

#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace lila::crash;
namespace fs = std::filesystem;

namespace
{
enum Button
{
    kOpenReport = 100,
    kOpenFolder,
    kCopyReport,
    kRestart,
    kSend,
};

struct DialogState
{
    fs::path dir;
    std::wstring restart;
    fs::path consentFile;
    upload::Consent consent;
    std::atomic<HWND> hwnd = nullptr; // null once the dialog is gone
    std::thread sender;
    bool sendStarted = false;
};

/** Posted, never sent: the sender thread must not wait on a dialog that may be closing. Text must be a literal. */
void setFooter(DialogState& st, const wchar_t* text)
{
    if (HWND h = st.hwnd)
        PostMessageW(h, TDM_SET_ELEMENT_TEXT, TDE_FOOTER, reinterpret_cast<LPARAM>(text));
}

/** Sends on a thread so the dialog stays responsive; the footer says how it went. */
void startSend(DialogState& st)
{
    if (st.sendStarted)
        return;
    st.sendStarted = true;
    if (HWND h = st.hwnd)
        PostMessageW(h, TDM_ENABLE_BUTTON, kSend, FALSE);
    setFooter(st, L"Sending the crash report...");
    st.sender = std::thread([&st] {
        bool ok = upload::send(st.dir);
        setFooter(st, ok ? L"Crash report sent. Thank you, it helps get this fixed." : L"The crash report couldn't be sent. Copy it into a bug report instead.");
    });
}

void copyToClipboard(HWND owner, const std::wstring& text)
{
    if (!OpenClipboard(owner))
        return;
    EmptyClipboard();
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t)))
    {
        std::memcpy(GlobalLock(mem), text.c_str(), (text.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem))
            GlobalFree(mem);
    }
    CloseClipboard();
}

HRESULT CALLBACK dialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM, LONG_PTR ref)
{
    auto* st = reinterpret_cast<DialogState*>(ref);
    if (msg == TDN_DESTROYED)
    {
        st->hwnd = nullptr;
        return S_OK;
    }
    if (msg == TDN_VERIFICATION_CLICKED)
    {
        bool on = wParam != 0;
        upload::setAlways(st->consentFile, on);
        if (on)
            startSend(*st);
        return S_OK;
    }
    if (msg == TDN_CREATED)
    {
        st->hwnd = hwnd;
        if (st->consent.send && st->consent.always && !upload::alreadySent(st->dir))
            startSend(*st);
        // Lila was in front a moment ago; a background process may not take focus, so at least come to the top.
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetForegroundWindow(hwnd);
        return S_OK;
    }
    if (msg != TDN_BUTTON_CLICKED)
        return S_OK;
    switch (int(wParam))
    {
    case kOpenReport:
        ShellExecuteW(hwnd, L"open", (st->dir / L"report.txt").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return S_FALSE;
    case kOpenFolder:
        ShellExecuteW(hwnd, L"open", st->dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return S_FALSE;
    case kCopyReport:
        copyToClipboard(hwnd, widen(readText(st->dir / L"report.txt")));
        SendMessageW(hwnd, TDM_SET_ELEMENT_TEXT, TDE_FOOTER, reinterpret_cast<LPARAM>(L"The crash report is on the clipboard."));
        return S_FALSE;
    case kSend:
        startSend(*st);
        return S_FALSE;
    case kRestart:
        if (!st->restart.empty())
        {
            std::wstring cmd = L"\"" + st->restart + L"\"";
            STARTUPINFOW si{sizeof(si)};
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(st->restart.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
            {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
        }
        return S_OK;
    }
    return S_OK;
}

void showDialog(const watch::Result& res, const std::wstring& restart, const fs::path& consentFile, const upload::Consent& consent)
{
    DialogState st;
    st.dir = res.reportDir;
    st.restart = restart;
    st.consentFile = consentFile;
    st.consent = consent;
    const bool ask = consent.send && !consent.always;
    std::wstring summary = widen(res.summary);
    const std::wstring prefix = L"Lila crashed: ";
    if (summary.starts_with(prefix))
        summary = summary.substr(prefix.size());
    std::wstring footer = L"Saved to " + res.reportDir.wstring() + L"\nYour unsaved work, if any, is offered when Lila starts again.";

    std::vector<TASKDIALOG_BUTTON> buttons;
    if (ask)
        buttons.push_back({kSend, L"Send Crash Report"});
    buttons.push_back({kOpenReport, L"Open Crash Report"});
    buttons.push_back({kOpenFolder, L"Open Crash Folder"});
    buttons.push_back({kCopyReport, L"Copy Crash Report"});
    if (!restart.empty())
        buttons.push_back({kRestart, L"Restart Lila"});
    TASKDIALOGCONFIG cfg{};
    cfg.cbSize = sizeof(cfg);
    cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
    cfg.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    cfg.pszWindowTitle = L"Lila";
    cfg.pszMainIcon = TD_ERROR_ICON;
    cfg.pszMainInstruction = L"Lila crashed";
    cfg.pszContent = summary.c_str();
    cfg.pszFooter = footer.c_str();
    cfg.pszFooterIcon = TD_INFORMATION_ICON;
    cfg.pButtons = buttons.data();
    cfg.cButtons = UINT(buttons.size());
    cfg.nDefaultButton = ask ? kSend : kCopyReport;
    if (ask)
        cfg.pszVerificationText = L"Always send crash reports without asking";
    cfg.pfCallback = dialogProc;
    cfg.lpCallbackData = reinterpret_cast<LONG_PTR>(&st);
    TaskDialogIndirect(&cfg, nullptr, nullptr, nullptr);
    // A send still running finishes before the watchdog exits.
    if (st.sender.joinable())
        st.sender.join();
}

HANDLE handleArg(const wchar_t* v)
{
    return reinterpret_cast<HANDLE>(uintptr_t(_wcstoui64(v, nullptr, 10)));
}
} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    watch::Options o;
    std::wstring restart;
    fs::path consentFile;
    fs::path sendDir; // --send <report folder>: upload one report and exit
    bool watchMode = false, ui = true;
    for (int i = 1; i < argc; ++i)
    {
        std::wstring a = argv[i];
        bool hasValue = i + 1 < argc;
        if (a == L"--watch")
            watchMode = true;
        else if (a == L"--no-ui")
            ui = false;
        else if (a == L"--full")
            o.fullDump = true;
        else if (a == L"--process" && hasValue)
            o.process = handleArg(argv[++i]);
        else if (a == L"--shared" && hasValue)
            o.shared = handleArg(argv[++i]);
        else if (a == L"--crash" && hasValue)
            o.crashEvent = handleArg(argv[++i]);
        else if (a == L"--done" && hasValue)
            o.doneEvent = handleArg(argv[++i]);
        else if (a == L"--sessions" && hasValue)
            o.sessionsDir = argv[++i];
        else if (a == L"--crashes" && hasValue)
            o.crashesDir = argv[++i];
        else if (a == L"--keep" && hasValue)
            o.keep = size_t(_wtoi(argv[++i]));
        else if (a == L"--restart" && hasValue)
            restart = argv[++i];
        else if (a == L"--consent" && hasValue)
            consentFile = argv[++i];
        else if (a == L"--send" && hasValue)
            sendDir = argv[++i];
    }
    LocalFree(argv);
    if (!sendDir.empty())
        return upload::send(sendDir) ? 0 : 1;
    if (!watchMode || !o.process || o.sessionsDir.empty() || o.crashesDir.empty())
        return 2;
    if (const char* v = std::getenv("LILA_CRASH_NO_UI"); v && *v && *v != '0')
        ui = false;
    if (const char* v = std::getenv("LILA_CRASH_WER_WAIT_MS"); v && *v)
        o.werWaitMs = DWORD(std::strtoul(v, nullptr, 10));

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // The watchdog waits for hours; it should never compete with Lila for the CPU.
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    watch::Result res = watch::run(o);
    if (!res.crashed || res.reportDir.empty())
        return 0;
    upload::Consent consent = consentFile.empty() ? upload::Consent{} : upload::readConsent(consentFile);
    if (ui)
    {
        SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
        showDialog(res, restart, consentFile, consent);
        writeTextAtomic(res.reportDir / L".acknowledged", "");
    }
    else if (consent.send && consent.always)
    {
        upload::send(res.reportDir);
    }
    return 1;
}
