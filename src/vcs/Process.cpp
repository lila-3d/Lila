#include "vcs/Process.h"

#include "core/Profile.h"

#include <cstdlib>
#include <map>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace lila::vcs
{
namespace
{
#ifdef _WIN32
std::wstring widen(std::string_view s)
{
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string narrow(std::wstring_view w)
{
    if (w.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

/** MSVC runtime argv quoting: backslashes only escape when they precede a quote. */
void appendQuoted(std::wstring& cmd, const std::wstring& arg)
{
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
    {
        cmd += arg;
        return;
    }
    cmd += L'"';
    for (size_t i = 0;; ++i)
    {
        size_t slashes = 0;
        while (i < arg.size() && arg[i] == L'\\')
        {
            ++i;
            ++slashes;
        }
        if (i == arg.size())
        {
            cmd.append(slashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"')
        {
            cmd.append(slashes * 2 + 1, L'\\');
            cmd += L'"';
        }
        else
        {
            cmd.append(slashes, L'\\');
            cmd += arg[i];
        }
    }
    cmd += L'"';
}

std::wstring environmentBlock(const std::vector<std::pair<std::string, std::string>>& overrides)
{
    struct Less
    {
        bool operator()(const std::wstring& a, const std::wstring& b) const { return _wcsicmp(a.c_str(), b.c_str()) < 0; }
    };
    std::map<std::wstring, std::wstring, Less> vars;
    if (wchar_t* env = GetEnvironmentStringsW())
    {
        for (const wchar_t* p = env; *p; p += wcslen(p) + 1)
        {
            std::wstring_view entry(p);
            // Entries like "=C:=C:\dir" start with '=' and are kept as they are.
            size_t eq = entry.find(L'=', 1);
            if (eq != std::wstring_view::npos)
                vars[std::wstring(entry.substr(0, eq))] = entry.substr(eq + 1);
        }
        FreeEnvironmentStringsW(env);
    }
    for (auto& [k, v] : overrides)
        vars[widen(k)] = widen(v);

    std::wstring block;
    for (auto& [k, v] : vars)
    {
        block += k;
        block += L'=';
        block += v;
        block += L'\0';
    }
    block += L'\0';
    return block;
}

void readAll(HANDLE h, std::string& out)
{
    char buf[16384];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0)
        out.append(buf, n);
}

class SystemRunner final : public ProcessRunner
{
public:
    ProcessResult run(const ProcessSpec& spec) override
    {
        LILA_ZONE();

        ProcessResult r;
        if (spec.argv.empty())
            return r;

        std::wstring cmd;
        for (size_t i = 0; i < spec.argv.size(); ++i)
        {
            if (i)
                cmd += L' ';
            appendQuoted(cmd, widen(spec.argv[i]));
        }

        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr, errRead = nullptr, errWrite = nullptr;
        if (!CreatePipe(&inRead, &inWrite, &sa, 0) || !CreatePipe(&outRead, &outWrite, &sa, 0) ||
            !CreatePipe(&errRead, &errWrite, &sa, 0))
        {
            r.err = "could not create pipes";
            return r;
        }
        SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);

        // Only these three handles reach the child, so pipes other threads are creating don't leak into it.
        HANDLE inherit[3] = {inRead, outWrite, errWrite};
        SIZE_T attrSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
        std::vector<char> attrBuf(attrSize);
        auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
        InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize);
        UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr);

        STARTUPINFOEXW si{};
        si.StartupInfo.cb = sizeof(si);
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = inRead;
        si.StartupInfo.hStdOutput = outWrite;
        si.StartupInfo.hStdError = errWrite;
        si.lpAttributeList = attrs;

        std::wstring env = environmentBlock(spec.env);
        std::wstring cwd = spec.cwd.empty() ? std::wstring() : spec.cwd.wstring();
        PROCESS_INFORMATION pi{};
        BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT, env.data(),
                                 cwd.empty() ? nullptr : cwd.c_str(), &si.StartupInfo, &pi);
        DWORD startError = GetLastError();
        DeleteProcThreadAttributeList(attrs);
        CloseHandle(inRead);
        CloseHandle(outWrite);
        CloseHandle(errWrite);

        if (!ok)
        {
            CloseHandle(inWrite);
            CloseHandle(outRead);
            CloseHandle(errRead);
            wchar_t* msg = nullptr;
            FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                           startError, 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
            r.err = "could not run " + spec.argv[0] + ": " + (msg ? narrow(msg) : "error " + std::to_string(startError));
            if (msg)
                LocalFree(msg);
            return r;
        }
        r.started = true;
        CloseHandle(pi.hThread);

        std::thread writer([&] {
            DWORD written = 0;
            size_t at = 0;
            while (at < spec.input.size() &&
                   WriteFile(inWrite, spec.input.data() + at, DWORD(std::min<size_t>(spec.input.size() - at, 1 << 20)), &written, nullptr))
                at += written;
            CloseHandle(inWrite);
        });
        std::thread errReader([&] { readAll(errRead, r.err); });
        std::thread outReader([&] { readAll(outRead, r.out); });

        DWORD wait = WaitForSingleObject(pi.hProcess, spec.timeoutMs > 0 ? DWORD(spec.timeoutMs) : INFINITE);
        if (wait == WAIT_TIMEOUT)
        {
            r.timedOut = true;
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, INFINITE);
        }
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        r.exitCode = int(code);
        CloseHandle(pi.hProcess);

        writer.join();
        outReader.join();
        errReader.join();
        CloseHandle(outRead);
        CloseHandle(errRead);
        return r;
    }
};
#else
class SystemRunner final : public ProcessRunner
{
public:
    ProcessResult run(const ProcessSpec& spec) override
    {
        LILA_ZONE();

        ProcessResult r;
        if (spec.argv.empty())
            return r;

        int in[2], out[2], err[2];
        if (pipe(in) || pipe(out) || pipe(err))
        {
            r.err = "could not create pipes";
            return r;
        }
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, in[0], 0);
        posix_spawn_file_actions_adddup2(&fa, out[1], 1);
        posix_spawn_file_actions_adddup2(&fa, err[1], 2);
        for (int fd : {in[0], in[1], out[0], out[1], err[0], err[1]})
            posix_spawn_file_actions_addclose(&fa, fd);
        if (!spec.cwd.empty())
            posix_spawn_file_actions_addchdir_np(&fa, spec.cwd.c_str());

        std::map<std::string, std::string> vars;
        for (char** e = environ; *e; ++e)
        {
            std::string_view entry(*e);
            size_t eq = entry.find('=');
            if (eq != std::string_view::npos)
                vars[std::string(entry.substr(0, eq))] = entry.substr(eq + 1);
        }
        for (auto& [k, v] : spec.env)
            vars[k] = v;
        std::vector<std::string> envStrings;
        for (auto& [k, v] : vars)
            envStrings.push_back(k + "=" + v);
        std::vector<char*> envp;
        for (auto& s : envStrings)
            envp.push_back(s.data());
        envp.push_back(nullptr);
        std::vector<char*> argv;
        for (const std::string& a : spec.argv)
            argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);

        pid_t pid = 0;
        int rc = posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), envp.data());
        posix_spawn_file_actions_destroy(&fa);
        close(in[0]);
        close(out[1]);
        close(err[1]);
        if (rc != 0)
        {
            close(in[1]);
            close(out[0]);
            close(err[0]);
            r.err = "could not run " + spec.argv[0] + ": " + std::strerror(rc);
            return r;
        }
        r.started = true;

        std::thread writer([&] {
            signal(SIGPIPE, SIG_IGN);
            size_t at = 0;
            while (at < spec.input.size())
            {
                ssize_t n = write(in[1], spec.input.data() + at, spec.input.size() - at);
                if (n <= 0)
                    break;
                at += size_t(n);
            }
            close(in[1]);
        });
        auto reader = [](int fd, std::string& dst) {
            char buf[16384];
            ssize_t n;
            while ((n = read(fd, buf, sizeof(buf))) > 0)
                dst.append(buf, size_t(n));
            close(fd);
        };
        std::thread errReader(reader, err[0], std::ref(r.err));
        std::thread outReader(reader, out[0], std::ref(r.out));

        int status = 0;
        if (spec.timeoutMs > 0)
        {
            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(spec.timeoutMs);
            while (waitpid(pid, &status, WNOHANG) == 0)
            {
                if (std::chrono::steady_clock::now() > deadline)
                {
                    r.timedOut = true;
                    kill(pid, SIGKILL);
                    waitpid(pid, &status, 0);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        else
        {
            waitpid(pid, &status, 0);
        }
        r.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        writer.join();
        outReader.join();
        errReader.join();
        return r;
    }
};
#endif
} // namespace

std::shared_ptr<ProcessRunner> systemRunner()
{
    static std::shared_ptr<ProcessRunner> runner = std::make_shared<SystemRunner>();
    return runner;
}

fs::path findExecutable(const std::string& name)
{
    LILA_ZONE();

    const char* pathEnv = std::getenv("PATH");
    if (!pathEnv)
        return {};
#ifdef _WIN32
    const char sep = ';';
    std::vector<std::string> exts = fs::path(name).has_extension() ? std::vector<std::string>{""}
                                                                    : std::vector<std::string>{".exe", ".cmd", ".bat"};
#else
    const char sep = ':';
    std::vector<std::string> exts{""};
#endif
    std::string_view all(pathEnv);
    while (!all.empty())
    {
        size_t end = all.find(sep);
        std::string_view dir = all.substr(0, end);
        all = end == std::string_view::npos ? std::string_view() : all.substr(end + 1);
        if (dir.empty())
            continue;
        for (const std::string& ext : exts)
        {
            std::error_code ec;
            fs::path p = fs::path(dir) / (name + ext);
            if (fs::is_regular_file(p, ec))
                return p;
        }
    }
    return {};
}
} // namespace lila::vcs
