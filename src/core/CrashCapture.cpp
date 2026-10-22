#include "core/CrashCapture.h"

#ifdef _WIN32
#include <dbghelp.h>
#include <dxgi.h>
#include <intrin.h>
#include <psapi.h>
#include <shellscalingapi.h>
#include <tlhelp32.h>
#include <winevt.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>

#ifndef STACK_FRAME_TYPE_INLINE
#define STACK_FRAME_TYPE_INLINE 0x02
#endif

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "wevtapi.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "dxgi.lib")

namespace lila::crash
{
std::string narrow(const std::wstring& w)
{
    if (w.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring widen(const std::string& s)
{
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}

int64_t unixNow()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

namespace
{
int64_t fileTimeToUnix(const FILETIME& ft)
{
    ULARGE_INTEGER t{{ft.dwLowDateTime, ft.dwHighDateTime}};
    return int64_t(t.QuadPart / 10000000ULL) - 11644473600LL;
}

std::string mib(uint64_t bytes)
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.0f MiB", double(bytes) / (1024.0 * 1024.0));
    return buf;
}

std::wstring regString(HKEY root, const wchar_t* key, const wchar_t* value)
{
    wchar_t buf[512];
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return {};
    return buf;
}

DWORD regDword(HKEY root, const wchar_t* key, const wchar_t* value)
{
    DWORD v = 0, size = sizeof(v);
    RegGetValueW(root, key, value, RRF_RT_REG_DWORD, nullptr, &v, &size);
    return v;
}

std::string fileVersion(const std::wstring& path)
{
    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (!size)
        return {};
    std::vector<char> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()))
        return {};
    VS_FIXEDFILEINFO* info = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &len) || !info)
        return {};
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                  HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
    return buf;
}

void addOs(Section& s)
{
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW v{sizeof(v)};
    if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))
        fn(&v);
    const wchar_t* key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    std::string product = narrow(regString(HKEY_LOCAL_MACHINE, key, L"ProductName"));
    // Windows 11 still reports itself as Windows 10 here.
    if (v.dwBuildNumber >= 22000 && product.starts_with("Windows 10"))
        product.replace(0, 10, "Windows 11");
    std::string display = narrow(regString(HKEY_LOCAL_MACHINE, key, L"DisplayVersion"));
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s %s (%lu.%lu.%lu.%lu)", product.c_str(), display.c_str(), v.dwMajorVersion, v.dwMinorVersion,
                  v.dwBuildNumber, regDword(HKEY_LOCAL_MACHINE, key, L"UBR"));
    s.set("os", buf);
}

void addCpu(Section& s)
{
    int regs[4] = {};
    char brand[49] = {};
    __cpuid(regs, int(0x80000000));
    if (unsigned(regs[0]) >= 0x80000004u)
        for (int i = 0; i < 3; ++i)
        {
            __cpuid(regs, int(0x80000002 + i));
            std::memcpy(brand + i * 16, regs, 16);
        }
    std::string name = brand;
    name.erase(0, name.find_first_not_of(' '));
    s.set("cpu", name);
    s.set("cpu.logical", std::to_string(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)));

    MEMORYSTATUSEX mem{sizeof(mem)};
    if (GlobalMemoryStatusEx(&mem))
    {
        s.set("ram.total", mib(mem.ullTotalPhys));
        s.set("ram.available", mib(mem.ullAvailPhys));
        s.set("pagefile.available", mib(mem.ullAvailPageFile));
    }
}

void addGpus(Section& s)
{
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
        return;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 d{};
        adapter->GetDesc1(&d);
        LARGE_INTEGER umd{};
        std::string driver;
        if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
        {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", HIWORD(umd.HighPart), LOWORD(umd.HighPart), HIWORD(umd.LowPart),
                          LOWORD(umd.LowPart));
            driver = buf;
        }
        char line[512];
        std::snprintf(line, sizeof(line), "%s  vendor 0x%04X device 0x%04X  vram %s  driver %s%s", narrow(d.Description).c_str(),
                      d.VendorId, d.DeviceId, mib(d.DedicatedVideoMemory).c_str(), driver.c_str(),
                      (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? "  (software)" : "");
        s.line(line);
        adapter->Release();
    }
    factory->Release();
}

BOOL CALLBACK monitorProc(HMONITOR monitor, HDC, LPRECT, LPARAM param)
{
    auto* s = reinterpret_cast<Section*>(param);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info))
        return TRUE;
    UINT dpiX = 96, dpiY = 96;
    GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode);
    const RECT& r = info.rcMonitor;
    char line[256];
    std::snprintf(line, sizeof(line), "%s  %ldx%ld at (%ld,%ld)  %u dpi (%u%%)  %lu Hz%s", narrow(info.szDevice).c_str(), r.right - r.left,
                  r.bottom - r.top, r.left, r.top, dpiX, dpiX * 100 / 96, mode.dmDisplayFrequency,
                  (info.dwFlags & MONITORINFOF_PRIMARY) ? "  primary" : "");
    s->line(line);
    return TRUE;
}

/** What a thread is called, if it was named. */
std::string threadName(HANDLE thread)
{
    using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static auto fn = reinterpret_cast<GetThreadDescriptionFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
    if (!fn)
        return {};
    PWSTR desc = nullptr;
    if (FAILED(fn(thread, &desc)) || !desc)
        return {};
    std::string out = narrow(desc);
    LocalFree(desc);
    return out;
}

std::string moduleName(HANDLE process, DWORD64 addr, DWORD64* base = nullptr, SYM_TYPE* symType = nullptr)
{
    IMAGEHLP_MODULEW64 mod{};
    mod.SizeOfStruct = sizeof(mod);
    if (!SymGetModuleInfoW64(process, addr, &mod))
        return {};
    if (base)
        *base = mod.BaseOfImage;
    if (symType)
        *symType = mod.SymType;
    return narrow(fs::path(mod.ImageName).filename().wstring());
}

std::string frameLine(HANDLE process, int index, DWORD64 pc, DWORD inlineContext, bool returnAddress)
{
    // Return addresses point after the call; the line that made the call is one byte back.
    DWORD64 lookup = returnAddress && pc ? pc - 1 : pc;
    alignas(SYMBOL_INFOW) char buf[sizeof(SYMBOL_INFOW) + 512 * sizeof(wchar_t)] = {};
    auto* sym = reinterpret_cast<SYMBOL_INFOW*>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFOW);
    sym->MaxNameLen = 511;
    DWORD64 disp = 0;
    bool hasSym = SymFromInlineContextW(process, lookup, inlineContext, &disp, sym) != FALSE;
    IMAGEHLP_LINEW64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD lineDisp = 0;
    bool hasLine = SymGetLineFromInlineContextW(process, lookup, inlineContext, 0, &lineDisp, &line) != FALSE;
    DWORD64 base = 0;
    SYM_TYPE symType = SymNone;
    std::string mod = moduleName(process, pc, &base, &symType);

    std::string out = "#" + std::string(index < 10 ? "0" : "") + std::to_string(index) + " ";
    // Without a PDB the nearest export can be far away and unrelated, so the offset is what identifies the code.
    if (hasSym && symType == SymExport && disp > 0x1000)
        out += mod + "+" + hex(pc - base) + " (nearest export " + narrow(sym->Name) + ")";
    else if (hasSym)
        out += (mod.empty() ? "" : mod + "!") + narrow(sym->Name) + "+" + hex(disp);
    else if (!mod.empty())
        out += mod + "+" + hex(pc - base);
    else
        out += hex(pc, 16);
    if (hasLine && line.FileName)
        out += "  " + narrow(line.FileName) + ":" + std::to_string(line.LineNumber);
    if (inlineContext != INLINE_FRAME_CONTEXT_IGNORE && ((inlineContext >> 8) & 0xFF) == STACK_FRAME_TYPE_INLINE)
        out += "  [inlined]";
    return out;
}

std::vector<std::string> walk(HANDLE process, HANDLE thread, CONTEXT ctx)
{
    std::vector<std::string> out;
#if defined(_M_X64)
    STACKFRAME_EX frame{};
    frame.StackFrameSize = sizeof(frame);
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
    int physical = 0;
    for (int i = 0; i < 128; ++i)
    {
        if (!StackWalkEx(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64,
                         nullptr, SYM_STKWALK_DEFAULT) ||
            frame.AddrPC.Offset == 0)
            break;
        bool inlined = frame.InlineFrameContext != INLINE_FRAME_CONTEXT_IGNORE &&
                       ((frame.InlineFrameContext >> 8) & 0xFF) == STACK_FRAME_TYPE_INLINE;
        out.push_back(frameLine(process, i, frame.AddrPC.Offset, frame.InlineFrameContext, physical > 0));
        if (!inlined)
            ++physical;
    }
#else
    (void)process, (void)thread, (void)ctx;
    out.push_back("(stack walking is implemented for x64 only)");
#endif
    return out;
}

struct ModuleList
{
    HANDLE process;
    std::vector<std::string>* lines;
};

BOOL CALLBACK moduleProc(PCWSTR, DWORD64 base, PVOID param)
{
    auto* list = static_cast<ModuleList*>(param);
    IMAGEHLP_MODULEW64 mod{};
    mod.SizeOfStruct = sizeof(mod);
    if (!SymGetModuleInfoW64(list->process, base, &mod))
        return TRUE;
    const char* symbols = mod.SymType == SymPdb ? "pdb" : mod.SymType == SymExport ? "exports" : mod.SymType == SymDeferred ? "deferred" : "none";
    char buf[1024];
    std::snprintf(buf, sizeof(buf), "%s  %s  size %s  version %s  symbols %s", hex(base, 16).c_str(), narrow(mod.ImageName).c_str(),
                  hex(mod.ImageSize).c_str(), fileVersion(mod.ImageName).c_str(), symbols);
    list->lines->push_back(buf);
    return TRUE;
}

const char* fastFailName(uint64_t code)
{
    switch (code)
    {
    case 0: return "FAST_FAIL_LEGACY_GS_VIOLATION";
    case 1: return "FAST_FAIL_VTGUARD_CHECK_FAILURE";
    case 2: return "FAST_FAIL_STACK_COOKIE_CHECK_FAILURE";
    case 3: return "FAST_FAIL_CORRUPT_LIST_ENTRY";
    case 4: return "FAST_FAIL_INCORRECT_STACK";
    case 5: return "FAST_FAIL_INVALID_ARG";
    case 6: return "FAST_FAIL_GS_COOKIE_INIT";
    case 7: return "FAST_FAIL_FATAL_APP_EXIT";
    case 8: return "FAST_FAIL_RANGE_CHECK_FAILURE";
    case 10: return "FAST_FAIL_GUARD_ICALL_CHECK_FAILURE";
    case 11: return "FAST_FAIL_GUARD_WRITE_CHECK_FAILURE";
    case 13: return "FAST_FAIL_INVALID_SET_OF_CONTEXT";
    case 14: return "FAST_FAIL_INVALID_REFERENCE_COUNT";
    case 18: return "FAST_FAIL_INVALID_JUMP_BUFFER";
    case 21: return "FAST_FAIL_INVALID_EXCEPTION_CHAIN";
    case 37: return "FAST_FAIL_GUARD_SS_FAILURE";
    case 50: return "FAST_FAIL_HEAP_METADATA_CORRUPTION";
    }
    return "";
}
} // namespace

void addSystemInfo(Report& r)
{
    Section& s = r.section("system");
    addOs(s);
    addCpu(s);
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD n = DWORD(std::size(name));
    if (GetComputerNameW(name, &n))
        s.set("machine", narrow(name));
    addGpus(r.section("gpus"));
    EnumDisplayMonitors(nullptr, nullptr, monitorProc, reinterpret_cast<LPARAM>(&r.section("monitors")));
    r.section("monitors").set("count", std::to_string(GetSystemMetrics(SM_CMONITORS)));
}

void addProcessMemory(Section& s, HANDLE process)
{
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (!GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
        return;
    s.set("memory.workingSet", mib(pmc.WorkingSetSize));
    s.set("memory.peakWorkingSet", mib(pmc.PeakWorkingSetSize));
    s.set("memory.private", mib(pmc.PrivateUsage));
    s.set("memory.peakCommit", mib(pmc.PeakPagefileUsage));
    DWORD handles = 0;
    if (GetProcessHandleCount(process, &handles))
        s.set("handles", std::to_string(handles));
}

void addProcessTimes(Section& s, HANDLE process)
{
    s.set("pid", std::to_string(GetProcessId(process)));
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(process, &created, &exited, &kernel, &user))
    {
        int64_t start = fileTimeToUnix(created);
        DWORD code = 0;
        bool running = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
        int64_t end = running || (!exited.dwLowDateTime && !exited.dwHighDateTime) ? unixNow() : fileTimeToUnix(exited);
        s.set("started", isoTimeUtc(start));
        s.set("uptime", formatDuration(end - start));
    }
    wchar_t path[MAX_PATH * 4];
    DWORD size = DWORD(std::size(path));
    if (QueryFullProcessImageNameW(process, 0, path, &size))
        s.set("exe", narrow(std::wstring(path, size)));
}

void addModuleIdentity(Section& s, HMODULE module)
{
    auto* base = reinterpret_cast<const uint8_t*>(module);
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    s.set("build.linkTime", isoTimeUtc(int64_t(nt->FileHeader.TimeDateStamp)));
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    auto* entries = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(base + dir.VirtualAddress);
    for (size_t i = 0; dir.VirtualAddress && i < dir.Size / sizeof(IMAGE_DEBUG_DIRECTORY); ++i)
    {
        if (entries[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW || !entries[i].AddressOfRawData)
            continue;
        struct Rsds
        {
            DWORD signature;
            GUID guid;
            DWORD age;
            char path[1];
        };
        auto* cv = reinterpret_cast<const Rsds*>(base + entries[i].AddressOfRawData);
        if (cv->signature != 0x53445352) // 'RSDS'
            continue;
        const GUID& g = cv->guid;
        char id[64];
        std::snprintf(id, sizeof(id), "%08lX%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X%lX", g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1],
                      g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7], cv->age);
        s.set("build.id", id);
        s.set("build.pdb", cv->path);
        break;
    }
}

Capture captureProcess(HANDLE process, DWORD pid, DWORD crashedTid, const CONTEXT* crashedContext, uint64_t faultAddress,
                       const std::wstring& symbolPath, bool otherThreads)
{
    Capture c;
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
    // Symbols are only read from the given folders and the paths baked into each image, never a symbol server.
    if (!SymInitializeW(process, symbolPath.c_str(), TRUE))
    {
        c.threads.push_back("(SymInitialize failed: " + std::to_string(GetLastError()) + ")");
        return c;
    }

    if (faultAddress)
    {
        DWORD64 base = 0;
        c.module = moduleName(process, faultAddress, &base);
        c.moduleOffset = base ? faultAddress - base : 0;
    }

    std::vector<DWORD> tids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE)
    {
        THREADENTRY32 te{sizeof(te)};
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
            if (te.th32OwnerProcessID == pid)
                tids.push_back(te.th32ThreadID);
        CloseHandle(snap);
    }
    std::stable_partition(tids.begin(), tids.end(), [&](DWORD t) { return t == crashedTid; });
    if (crashedTid && std::find(tids.begin(), tids.end(), crashedTid) == tids.end())
        tids.insert(tids.begin(), crashedTid);

    bool self = pid == GetCurrentProcessId();
    for (DWORD tid : tids)
    {
        if ((self && tid == GetCurrentThreadId() && tid != crashedTid) || (!otherThreads && tid != crashedTid))
            continue;
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
        std::string name = t ? threadName(t) : std::string();
        std::string header = "thread " + std::to_string(tid) + (name.empty() ? "" : " \"" + name + "\"") + (tid == crashedTid ? "  (crashed)" : "");
        std::vector<std::string> frames;
        if (tid == crashedTid && crashedContext)
            frames = walk(process, t, *crashedContext);
        else if (t)
        {
            bool suspended = SuspendThread(t) != DWORD(-1);
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(t, &ctx))
                frames = walk(process, t, ctx);
            else
                frames.push_back("(context unavailable: " + std::to_string(GetLastError()) + ")");
            if (suspended)
                ResumeThread(t);
        }
        else
            frames.push_back("(cannot open thread: " + std::to_string(GetLastError()) + ")");
        if (t)
            CloseHandle(t);

        if (tid == crashedTid)
            c.crashedStack = frames;
        c.threads.push_back(header);
        for (const std::string& f : frames)
            c.threads.push_back("  " + f);
    }

    ModuleList list{process, &c.modules};
    SymEnumerateModulesW64(process, moduleProc, &list);
    std::sort(c.modules.begin(), c.modules.end());
    SymCleanup(process);
    return c;
}

void addPythonTracebacks(Report& r, const fs::path& file)
{
    std::string text = readText(file);
    if (text.find_first_not_of(" \r\n") == std::string::npos)
        return;
    Section& s = r.section("python");
    s.set("source", "faulthandler, every Python thread at each fatal fault (newest last)");
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? text.size() : nl + 1;
        while (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty())
            s.line(line);
    }
    // A run of caught faults could make this huge; the newest ones matter.
    if (s.lines.size() > 400)
        s.lines.erase(s.lines.begin(), s.lines.end() - 400);
}

void addCapture(Report& r, const Capture& capture)
{
    r.section("stack").lines = capture.crashedStack;
    r.section("threads").lines = capture.threads;
    r.section("modules").lines = capture.modules;
}

bool writeMinidump(HANDLE process, DWORD pid, DWORD tid, EXCEPTION_POINTERS* exception, bool clientPointers, const fs::path& file,
                   bool full, std::string* err)
{
    HANDLE f = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
    {
        if (err)
            *err = "cannot create " + narrow(file.wstring()) + " (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    auto type = MINIDUMP_TYPE(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs | MiniDumpWithHandleData |
                              MiniDumpWithUnloadedModules | MiniDumpWithProcessThreadData);
    if (full)
        type = MINIDUMP_TYPE(type | MiniDumpWithFullMemory | MiniDumpWithFullMemoryInfo);
    MINIDUMP_EXCEPTION_INFORMATION mei{tid, exception, clientPointers};
    BOOL ok = MiniDumpWriteDump(process, pid, f, type, exception ? &mei : nullptr, nullptr, nullptr);
    DWORD e = GetLastError();
    CloseHandle(f);
    if (!ok)
    {
        if (err)
            *err = "MiniDumpWriteDump failed (" + hex(e) + ")";
        DeleteFileW(file.c_str());
    }
    return ok != FALSE;
}

void addException(Report& r, const EXCEPTION_RECORD& record, DWORD tid, const Capture& capture)
{
    Section& s = r.section("crash");
    uint32_t code = record.ExceptionCode;
    s.set("exception.code", hex(code, 8));
    s.set("exception.name", exceptionName(code));
    s.set("exception.description", exceptionDescription(code));
    s.set("exception.address", hex(uint64_t(record.ExceptionAddress), 16));
    if (!capture.module.empty())
    {
        s.set("module", capture.module);
        s.set("module.offset", hex(capture.moduleOffset));
    }
    s.set("thread", std::to_string(tid));
    std::string params;
    for (DWORD i = 0; i < record.NumberParameters && i < EXCEPTION_MAXIMUM_PARAMETERS; ++i)
        params += (i ? " " : "") + hex(record.ExceptionInformation[i]);
    if (!params.empty())
        s.set("exception.parameters", params);
    if (code == 0xC0000005 && record.NumberParameters >= 2)
    {
        uint64_t kind = record.ExceptionInformation[0];
        s.set("access", std::string(kind == 0 ? "read" : kind == 1 ? "write" : kind == 8 ? "execute" : "access") + " of " +
                            hex(record.ExceptionInformation[1], 16));
    }
    if (code == 0xC0000409 && record.NumberParameters >= 1)
    {
        const char* ff = fastFailName(record.ExceptionInformation[0]);
        s.set("fastfail.code", std::to_string(record.ExceptionInformation[0]) + (*ff ? std::string(" ") + ff : ""));
    }
}

namespace
{
std::string utf16FileToUtf8(const fs::path& p)
{
    std::string raw = readText(p);
    if (raw.size() >= 2 && uint8_t(raw[0]) == 0xFF && uint8_t(raw[1]) == 0xFE)
    {
        std::wstring w(reinterpret_cast<const wchar_t*>(raw.data() + 2), (raw.size() - 2) / 2);
        return narrow(w);
    }
    return raw;
}

bool containsNoCase(std::wstring hay, std::wstring needle)
{
    std::transform(hay.begin(), hay.end(), hay.begin(), ::towlower);
    std::transform(needle.begin(), needle.end(), needle.begin(), ::towlower);
    return hay.find(needle) != std::wstring::npos;
}

std::wstring envDir(const wchar_t* name)
{
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(name, buf, DWORD(std::size(buf)));
    return n && n < std::size(buf) ? std::wstring(buf, n) : std::wstring();
}
} // namespace

std::vector<std::string> werReports(const std::wstring& exeName, int64_t sinceUnix)
{
    struct Found
    {
        int64_t time;
        fs::path file;
    };
    std::vector<Found> found;
    std::error_code ec;
    for (const wchar_t* root : {L"LOCALAPPDATA", L"ProgramData"})
    {
        std::wstring base = envDir(root);
        if (base.empty())
            continue;
        for (const wchar_t* kind : {L"ReportArchive", L"ReportQueue"})
        {
            fs::path dir = fs::path(base) / L"Microsoft" / L"Windows" / L"WER" / kind;
            for (const auto& e : fs::directory_iterator(dir, ec))
            {
                if (!e.is_directory(ec) || !containsNoCase(e.path().filename().wstring(), exeName))
                    continue;
                fs::path wer = e.path() / L"Report.wer";
                if (!fs::is_regular_file(wer, ec))
                    continue;
                auto t = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::clock_cast<std::chrono::system_clock>(fs::last_write_time(wer, ec)).time_since_epoch())
                             .count();
                if (t + 2 >= sinceUnix)
                    found.push_back({t, wer});
            }
        }
    }
    std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.time > b.time; });
    std::vector<std::string> out;
    for (const Found& f : found)
    {
        out.push_back("file: " + narrow(f.file.wstring()));
        std::string text = utf16FileToUtf8(f.file);
        size_t pos = 0;
        while (pos < text.size())
        {
            size_t nl = text.find('\n', pos);
            std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            pos = nl == std::string::npos ? text.size() : nl + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                line.pop_back();
            // The module list duplicates [modules]; the UI strings are noise.
            if (line.empty() || line.starts_with("LoadedModule[") || line.starts_with("UI[") || line.starts_with("State["))
                continue;
            out.push_back("  " + line);
        }
    }
    return out;
}

namespace
{
std::string renderEvent(EVT_HANDLE event)
{
    DWORD used = 0, props = 0;
    EvtRender(nullptr, event, EvtRenderEventXml, 0, nullptr, &used, &props);
    std::wstring xml(used / sizeof(wchar_t) + 1, L'\0');
    if (!EvtRender(nullptr, event, EvtRenderEventXml, DWORD(xml.size() * sizeof(wchar_t)), xml.data(), &used, &props))
        return {};
    xml.resize(wcslen(xml.c_str()));
    return narrow(xml);
}

/** "Provider id time  Name=value ..." from an event's XML. */
std::string summarizeEvent(const std::string& xml)
{
    auto attr = [&](const std::string& tag, const std::string& name) -> std::string {
        size_t t = xml.find("<" + tag);
        if (t == std::string::npos)
            return {};
        size_t a = xml.find(name + "='", t);
        if (a == std::string::npos)
            return {};
        a += name.size() + 2;
        return xml.substr(a, xml.find('\'', a) - a);
    };
    auto text = [&](const std::string& tag) -> std::string {
        size_t t = xml.find("<" + tag + ">");
        if (t == std::string::npos)
            return {};
        t += tag.size() + 2;
        return xml.substr(t, xml.find("</" + tag + ">", t) - t);
    };
    std::string out = attr("Provider", "Name") + " event " + text("EventID") + " at " + attr("TimeCreated", "SystemTime") + ":";
    size_t pos = 0;
    int unnamed = 0;
    while ((pos = xml.find("<Data", pos)) != std::string::npos)
    {
        size_t close = xml.find('>', pos);
        if (close == std::string::npos)
            break;
        std::string head = xml.substr(pos, close - pos);
        size_t end = xml.find("</Data>", close);
        std::string value = (xml[close - 1] == '/' || end == std::string::npos) ? std::string() : xml.substr(close + 1, end - close - 1);
        std::string name;
        if (size_t n = head.find("Name='"); n != std::string::npos)
            name = head.substr(n + 6, head.find('\'', n + 6) - n - 6);
        else
            name = std::to_string(unnamed++);
        if (!value.empty())
            out += " " + name + "=" + value;
        pos = close;
    }
    return out;
}
} // namespace

std::vector<std::string> eventLogEntries(DWORD pid, const std::wstring& exeName, int64_t sinceUnix)
{
    std::vector<std::string> out;
    int64_t ageMs = std::max<int64_t>(1, unixNow() - sinceUnix + 5) * 1000;
    std::wstring query = L"*[System[(Provider[@Name='Application Error'] or Provider[@Name='Windows Error Reporting']) and "
                         L"TimeCreated[timediff(@SystemTime) <= " +
                         std::to_wstring(ageMs) + L"]]]";
    EVT_HANDLE results = EvtQuery(nullptr, L"Application", query.c_str(), EvtQueryChannelPath | EvtQueryReverseDirection);
    if (!results)
        return out;
    char pidHex[32];
    std::snprintf(pidHex, sizeof(pidHex), "0x%lx", pid);
    std::string exe = narrow(exeName);
    for (char& ch : exe)
        ch = char(std::tolower(static_cast<unsigned char>(ch)));
    EVT_HANDLE events[16];
    DWORD returned = 0;
    while (EvtNext(results, DWORD(std::size(events)), events, 1000, 0, &returned))
    {
        for (DWORD i = 0; i < returned; ++i)
        {
            std::string xml = renderEvent(events[i]);
            EvtClose(events[i]);
            std::string lower = xml;
            for (char& ch : lower)
                ch = char(std::tolower(static_cast<unsigned char>(ch)));
            bool mine = lower.find(exe) != std::string::npos &&
                        (lower.find(pidHex) != std::string::npos || lower.find("windows error reporting") != std::string::npos);
            if (mine)
                out.push_back(summarizeEvent(xml));
        }
    }
    EvtClose(results);
    return out;
}
} // namespace lila::crash
#endif
