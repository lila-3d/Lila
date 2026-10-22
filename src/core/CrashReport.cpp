#include "core/CrashReport.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

namespace lila::crash
{
void Section::set(std::string_view key, std::string_view value)
{
    for (auto& [k, v] : fields)
        if (k == key)
        {
            v = value;
            return;
        }
    fields.emplace_back(std::string(key), std::string(value));
}

std::string Section::get(std::string_view key) const
{
    for (const auto& [k, v] : fields)
        if (k == key)
            return v;
    return {};
}

void Section::line(std::string_view text)
{
    lines.emplace_back(text);
}

Section& Report::section(std::string_view name)
{
    for (Section& s : sections)
        if (s.name == name)
            return s;
    sections.push_back({std::string(name), {}, {}});
    return sections.back();
}

const Section* Report::find(std::string_view name) const
{
    for (const Section& s : sections)
        if (s.name == name)
            return &s;
    return nullptr;
}

void Report::merge(const Report& other)
{
    if (!other.summary.empty())
        summary = other.summary;
    for (const Section& o : other.sections)
    {
        Section& s = section(o.name);
        for (const auto& [k, v] : o.fields)
            s.set(k, v);
        if (!o.lines.empty())
            s.lines = o.lines;
    }
}

namespace
{
/** Values and lines are single-line in the format; anything else would read back as a new key. */
std::string oneLine(std::string_view v)
{
    std::string out(v);
    for (char& c : out)
        if (c == '\n' || c == '\r')
            c = ' ';
    return out;
}

std::string_view trimRight(std::string_view s)
{
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n'))
        s.remove_suffix(1);
    return s;
}
} // namespace

std::string renderReport(const Report& r)
{
    std::string out = "Lila crash report\n";
    if (!r.summary.empty())
        out += "summary: " + oneLine(r.summary) + "\n";
    for (const Section& s : r.sections)
    {
        if (s.fields.empty() && s.lines.empty())
            continue;
        out += "\n[" + s.name + "]\n";
        for (const auto& [k, v] : s.fields)
            out += k + ": " + oneLine(v) + "\n";
        for (const std::string& l : s.lines)
        {
            // Multi-line entries (a log message with a traceback) become several indented lines.
            size_t start = 0;
            std::string_view text = trimRight(l);
            do
            {
                size_t nl = text.find('\n', start);
                std::string_view part = text.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
                out += "  ";
                out += trimRight(part);
                out += "\n";
                start = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
            } while (start <= text.size());
        }
    }
    return out;
}

void sortSections(Report& r)
{
    static constexpr std::string_view kOrder[] = {"crash", "stack", "python", "app", "context", "process", "system", "gpu", "gpus", "monitors",
                                                  "extensions", "commands", "log", "windows.eventlog", "windows.wer", "threads", "modules"};
    auto rank = [](const Section& s) {
        for (size_t i = 0; i < std::size(kOrder); ++i)
            if (kOrder[i] == s.name)
                return i;
        // Unknown sections go before the bulky tail.
        for (size_t i = 0; i < std::size(kOrder); ++i)
            if (kOrder[i] == "windows.eventlog")
                return i;
        return std::size(kOrder);
    };
    std::stable_sort(r.sections.begin(), r.sections.end(), [&](const Section& a, const Section& b) { return rank(a) < rank(b); });
}

namespace
{
bool systemModule(std::string_view frame)
{
    static constexpr std::string_view kSystem[] = {"ntdll.dll!", "kernelbase.dll!", "kernel32.dll!", "ucrtbase.dll!", "ucrtbased.dll!",
                                                   "vcruntime140.dll!", "vcruntime140_1.dll!", "msvcp140.dll!", "dbghelp.dll!"};
    std::string lower(frame);
    for (char& c : lower)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    for (std::string_view m : kSystem)
        if (lower.find(m) != std::string::npos)
            return true;
    return false;
}

/** "... X:/a.cpp:12" */
bool hasSource(std::string_view frame)
{
    size_t gap = frame.find("  ");
    if (gap == std::string_view::npos)
        return false;
    std::string_view where = frame.substr(gap + 2);
    size_t colon = where.rfind(':');
    return colon != std::string_view::npos && colon + 1 < where.size() && std::isdigit(static_cast<unsigned char>(where[colon + 1]));
}
} // namespace

std::string summarize(const Report& r)
{
    const Section* crash = r.find("crash");
    std::string code = crash ? crash->get("exception.code") : std::string();
    std::string out = "Lila crashed: ";
    if (!crash || code.empty())
        return out + "unknown cause";
    std::string desc = crash->get("exception.description");
    std::string name = crash->get("exception.name");
    out += (desc.empty() ? std::string("exception") : desc) + " (" + code + (name.empty() || name == desc ? "" : " " + name) + ")";
    std::string note = crash->get("note");
    if (!note.empty())
        out += ": " + note;
    std::string module = crash->get("module");
    if (!module.empty())
        out += " in " + module + "+" + crash->get("module.offset");
    // The first frame with source is Lila's own code; without one, the first frame outside the system DLLs.
    const Section* stack = r.find("stack");
    const std::string* pick = nullptr;
    for (int pass = 0; stack && pass < 2 && !pick; ++pass)
        for (const std::string& frame : stack->lines)
            if (!systemModule(frame) && frame.size() >= 4 && (pass == 1 || hasSource(frame)))
            {
                pick = &frame;
                break;
            }
    if (pick)
    {
        // "#03 lila.exe!lila::Foo::bar+0x12  X:/a.cpp:12" -> "lila.exe!lila::Foo::bar (X:/a.cpp:12)"
        std::string f = pick->substr(pick->find(' ') + 1);
        size_t plus = f.find("+0x");
        size_t gap = f.find("  ");
        std::string where = gap == std::string::npos ? std::string() : f.substr(gap + 2);
        if (size_t inl = where.find("  ["); inl != std::string::npos)
            where.resize(inl);
        out += ", at " + f.substr(0, std::min(plus, gap)) + (where.empty() || where.starts_with("[") ? "" : " (" + where + ")");
    }
    return out;
}

Report parseReport(std::string_view text)
{
    Report r;
    Section* cur = nullptr;
    size_t pos = 0;
    while (pos <= text.size())
    {
        size_t nl = text.find('\n', pos);
        std::string_view line = text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        pos = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
        line = trimRight(line);
        if (line.empty())
            continue;
        if (line.front() == '[' && line.back() == ']')
        {
            cur = &r.section(line.substr(1, line.size() - 2));
            continue;
        }
        if (line.starts_with("  "))
        {
            if (cur)
                cur->line(line.substr(2));
            continue;
        }
        size_t colon = line.find(": ");
        if (colon == std::string_view::npos)
            continue;
        std::string_view key = line.substr(0, colon), value = line.substr(colon + 2);
        if (!cur)
        {
            if (key == "summary")
                r.summary = value;
            continue;
        }
        cur->set(key, value);
    }
    return r;
}

void LineRing::push(std::string_view line)
{
    std::scoped_lock lock(mMutex);
    ++mPushed;
    if (mLines.size() < mCapacity)
    {
        mLines.emplace_back(line);
        return;
    }
    mLines[mHead].assign(line);
    mHead = (mHead + 1) % mCapacity;
}

std::vector<std::string> LineRing::lines() const
{
    std::scoped_lock lock(mMutex);
    std::vector<std::string> out;
    out.reserve(mLines.size());
    for (size_t i = 0; i < mLines.size(); ++i)
        out.push_back(mLines[(mHead + i) % mLines.size()]);
    return out;
}

bool LineRing::tryLines(std::vector<std::string>& out) const
{
    std::unique_lock lock(mMutex, std::try_to_lock);
    if (!lock.owns_lock())
        return false;
    out.clear();
    for (size_t i = 0; i < mLines.size(); ++i)
        out.push_back(mLines[(mHead + i) % mLines.size()]);
    return true;
}

size_t LineRing::size() const
{
    std::scoped_lock lock(mMutex);
    return mLines.size();
}

uint64_t LineRing::pushed() const
{
    std::scoped_lock lock(mMutex);
    return mPushed;
}

const char* exceptionName(uint32_t code)
{
    switch (code)
    {
    case 0xC0000005: return "EXCEPTION_ACCESS_VIOLATION";
    case 0xC0000006: return "EXCEPTION_IN_PAGE_ERROR";
    case 0xC000001D: return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case 0xC0000025: return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
    case 0xC000008C: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case 0xC000008E: return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    case 0xC0000094: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case 0xC0000095: return "EXCEPTION_INT_OVERFLOW";
    case 0xC0000096: return "EXCEPTION_PRIV_INSTRUCTION";
    case 0xC00000FD: return "STATUS_STACK_OVERFLOW";
    case 0xC0000135: return "STATUS_DLL_NOT_FOUND";
    case 0xC0000139: return "STATUS_ENTRYPOINT_NOT_FOUND";
    case 0xC0000142: return "STATUS_DLL_INIT_FAILED";
    case 0xC0000374: return "STATUS_HEAP_CORRUPTION";
    case 0xC0000409: return "STATUS_STACK_BUFFER_OVERRUN";
    case 0xC0000417: return "STATUS_INVALID_CRT_PARAMETER";
    case 0xC0000420: return "STATUS_ASSERTION_FAILURE";
    case 0xC0000602: return "STATUS_FAIL_FAST_EXCEPTION";
    case 0x80000003: return "EXCEPTION_BREAKPOINT";
    case kCodeCppException: return "CPP_EXCEPTION";
    case kCodeTerminate: return "LILA_STD_TERMINATE";
    case kCodeAbort: return "LILA_ABORT";
    case kCodePurecall: return "LILA_PURE_VIRTUAL_CALL";
    case kCodeInvalidParameter: return "LILA_INVALID_CRT_PARAMETER";
    }
    return "";
}

std::string exceptionDescription(uint32_t code)
{
    switch (code)
    {
    case 0xC0000005: return "access violation";
    case 0xC000001D: return "illegal instruction";
    case 0xC0000094: return "integer divide by zero";
    case 0xC00000FD: return "stack overflow";
    case 0xC0000374: return "heap corruption";
    case 0xC0000409: return "fail-fast (stack buffer overrun or __fastfail)";
    case 0xC0000417: return "invalid CRT parameter";
    case 0xC0000602: return "fail-fast exception";
    case 0x80000003: return "breakpoint";
    case kCodeCppException: return "unhandled C++ exception";
    case kCodeTerminate: return "std::terminate";
    case kCodeAbort: return "abort()";
    case kCodePurecall: return "pure virtual function call";
    case kCodeInvalidParameter: return "invalid parameter passed to a CRT function";
    }
    const char* name = exceptionName(code);
    return *name ? name : "exception";
}

bool isCrashExitCode(uint32_t code)
{
    // Ctrl+C or a closed console, not a fault.
    if (code == 0xC000013Au)
        return false;
    // System NTSTATUS errors (0xC000xxxx), plus 3 (abort) and our own codes. A debugger's or Task Manager's kill
    // (1, 0xFFFFFFFF) is not a crash.
    return (code & 0xFFFF0000u) == 0xC0000000u || code == 3 || code == 0x80000003u || (code & 0xFFFFFF00u) == 0xE0000000u ||
           code == kCodeCppException;
}

namespace
{
bool tokenChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '+' || c == '/' || c == '=' || c == '.';
}

bool startsWithNoCase(std::string_view s, size_t at, std::string_view word)
{
    if (at + word.size() > s.size())
        return false;
    for (size_t i = 0; i < word.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(s[at + i])) != word[i])
            return false;
    return true;
}
} // namespace

std::string redact(std::string_view text, const std::vector<std::string>& secrets)
{
    std::string out(text);
    for (const std::string& secret : secrets)
    {
        // Short strings would redact ordinary words.
        if (secret.size() < 6)
            continue;
        for (size_t at = out.find(secret); at != std::string::npos; at = out.find(secret, at))
            out.replace(at, secret.size(), "<redacted>");
    }
    static constexpr std::string_view kKeys[] = {"token", "bearer", "password", "passwd", "secret", "apikey", "api_key"};
    for (size_t i = 0; i < out.size(); ++i)
    {
        for (std::string_view key : kKeys)
        {
            if (!startsWithNoCase(out, i, key))
                continue;
            size_t j = i + key.size();
            while (j < out.size() && (out[j] == ' ' || out[j] == '"' || out[j] == '\'' || out[j] == ':' || out[j] == '='))
                ++j;
            size_t end = j;
            while (end < out.size() && tokenChar(out[end]))
                ++end;
            // A value this long after a secret-ish key is a credential, not prose.
            if (end - j >= 12 && out.compare(j, 10, "<redacted>") != 0)
            {
                out.replace(j, end - j, "<redacted>");
                end = j + 10;
            }
            i = end;
            break;
        }
    }
    return out;
}

std::string hex(uint64_t v, int digits)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%0*llX", digits, static_cast<unsigned long long>(v));
    return buf;
}

std::string folderTimestamp(int64_t unixSeconds)
{
    std::time_t t = std::time_t(unixSeconds);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d_%H-%M-%S", &tm);
    return buf;
}

std::string isoTimeUtc(int64_t unixSeconds)
{
    std::time_t t = std::time_t(unixSeconds);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string formatDuration(int64_t seconds)
{
    if (seconds < 0)
        seconds = 0;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%lldh %02lldm %02llds (%lld s)", static_cast<long long>(seconds / 3600),
                  static_cast<long long>(seconds / 60 % 60), static_cast<long long>(seconds % 60), static_cast<long long>(seconds));
    return buf;
}

fs::path createReportDir(const fs::path& root, int64_t unixSeconds, uint32_t pid)
{
    std::string base = folderTimestamp(unixSeconds) + "_" + std::to_string(pid);
    std::error_code ec;
    fs::create_directories(root, ec);
    for (int i = 0; i < 100; ++i)
    {
        fs::path dir = root / (i ? base + "_" + std::to_string(i) : base);
        if (fs::create_directory(dir, ec))
            return dir;
    }
    return root / base;
}

std::vector<fs::path> listReports(const fs::path& root)
{
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(root, ec))
        if (e.is_directory(ec) && fs::is_regular_file(e.path() / "report.txt", ec))
            out.push_back(e.path());
    // Folder names start with a sortable timestamp.
    std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b) { return a.filename() < b.filename(); });
    return out;
}

size_t pruneReports(const fs::path& root, size_t keep)
{
    std::vector<fs::path> all = listReports(root);
    size_t removed = 0;
    std::error_code ec;
    for (size_t i = 0; i + keep < all.size(); ++i)
        if (fs::remove_all(all[i], ec) > 0 && !ec)
            ++removed;
    return removed;
}

std::string readText(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool writeTextAtomic(const fs::path& p, std::string_view text)
{
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out.write(text.data(), std::streamsize(text.size()));
        if (!out)
            return false;
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    return !ec;
}
} // namespace lila::crash
