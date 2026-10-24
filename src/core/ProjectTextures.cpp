#include "core/ProjectTextures.h"

#include "core/MainQueue.h"
#include "core/Paths.h"
#include "core/Profile.h"
#include "core/TextMatch.h"
#include "core/Workspace.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace lila::projectTextures
{
namespace
{
char lowerAscii(char c)
{
    return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
}

/** Glob over [p, pe) against [s, se); see globMatch. */
bool match(const char* p, const char* pe, const char* s, const char* se)
{
    while (p < pe)
    {
        if (*p == '*')
        {
            if (p + 1 < pe && p[1] == '*')
            {
                p += 2;
                // "**/" may also stand for no folders at all.
                if (p < pe && *p == '/' && match(p + 1, pe, s, se))
                    return true;
                for (const char* k = s; k <= se; ++k)
                    if (match(p, pe, k, se))
                        return true;
                return false;
            }
            ++p;
            for (const char* k = s;; ++k)
            {
                if (match(p, pe, k, se))
                    return true;
                if (k == se || *k == '/')
                    return false;
            }
        }
        if (s == se)
            return false;
        if (*p == '?')
        {
            if (*s == '/')
                return false;
        }
        else if (lowerAscii(*p) != lowerAscii(*s))
            return false;
        ++p;
        ++s;
    }
    return s == se;
}

bool matchWhole(std::string_view pattern, std::string_view s)
{
    return match(pattern.data(), pattern.data() + pattern.size(), s.data(), s.data() + s.size());
}

constexpr std::array kImageExtensions = {"png", "tga", "jpg", "jpeg", "exr", "tif", "tiff", "psd", "hdr", "bmp", "dds"};
} // namespace

bool isImageExtension(std::string_view ext)
{
    if (!ext.empty() && ext.front() == '.')
        ext.remove_prefix(1);
    std::string e = textMatch::lower(ext);
    return std::find(kImageExtensions.begin(), kImageExtensions.end(), e) != kImageExtensions.end();
}

bool skipsDirectory(std::string_view name)
{
    if (name.empty() || name.front() == '.')
        return true;
    static constexpr std::array kSkipped = {"node_modules", "__pycache__", "intermediate", "deriveddatacache", "binaries",
                                            "$recycle.bin", "system volume information"};
    std::string n = textMatch::lower(name);
    return std::find(kSkipped.begin(), kSkipped.end(), n) != kSkipped.end();
}

bool globMatch(std::string_view pattern, std::string_view relPath)
{
    while (!pattern.empty() && pattern.front() == '/')
        pattern.remove_prefix(1);
    while (!pattern.empty() && pattern.back() == '/')
        pattern.remove_suffix(1);
    if (pattern.empty())
        return false;

    // Without a '/', any one segment may match; with one, the whole path or any folder prefix of it.
    if (pattern.find('/') == std::string_view::npos)
    {
        size_t start = 0;
        while (start <= relPath.size())
        {
            size_t end = relPath.find('/', start);
            if (end == std::string_view::npos)
                end = relPath.size();
            if (matchWhole(pattern, relPath.substr(start, end - start)))
                return true;
            start = end + 1;
        }
        return false;
    }
    if (matchWhole(pattern, relPath))
        return true;
    for (size_t slash = relPath.find('/'); slash != std::string_view::npos; slash = relPath.find('/', slash + 1))
        if (matchWhole(pattern, relPath.substr(0, slash)))
            return true;
    return false;
}

bool Filter::accepts(std::string_view relPath) const
{
    auto hit = [&](const std::string& g) { return globMatch(g, relPath); };
    if (!include.empty() && std::ranges::none_of(include, hit))
        return false;
    return std::ranges::none_of(exclude, hit);
}

std::vector<std::string> Filter::apply(const std::vector<std::string>& relPaths) const
{
    std::vector<std::string> out;
    for (const std::string& p : relPaths)
        if (accepts(p))
            out.push_back(p);
    return out;
}

std::vector<std::string> scan(const fs::path& root, const ScanLimits& limits, const std::atomic<bool>* cancel, bool* truncated)
{
    LILA_ZONE();

    if (truncated)
        *truncated = false;
    std::vector<std::string> out;
    std::error_code ec;
    if (root.empty() || !fs::is_directory(root, ec))
        return out;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    size_t visited = 0;
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        if (cancel && cancel->load(std::memory_order_relaxed))
            break;
        if (++visited > limits.maxEntries)
        {
            if (truncated)
                *truncated = true;
            break;
        }
        const fs::directory_entry& e = *it;
        std::string name = paths::toUtf8(e.path().filename());
        std::error_code tec;
        if (e.is_directory(tec))
        {
            if (it.depth() + 1 >= limits.maxDepth || skipsDirectory(name))
                it.disable_recursion_pending();
            continue;
        }
        if (name.empty() || name.front() == '.' || !isImageExtension(paths::toUtf8(e.path().extension())))
            continue;
        out.push_back(paths::toUtf8(e.path().lexically_relative(root)));
    }
    std::ranges::sort(out, [](const std::string& a, const std::string& b) { return textMatch::lower(a) < textMatch::lower(b); });
    return out;
}

fs::path projectRoot()
{
    if (Workspace::get().isOpen())
        return Workspace::get().root();
    return paths::currentSceneDir();
}

namespace
{
/** Whether a changed path, relative to the root, can change the list: an image, or a folder (no extension). */
bool relevantChange(std::string_view rel)
{
    size_t start = 0;
    while (true)
    {
        size_t end = rel.find_first_of("/\\", start);
        if (end == std::string_view::npos)
            break;
        if (skipsDirectory(rel.substr(start, end - start)))
            return false;
        start = end + 1;
    }
    std::string_view leaf = rel.substr(start);
    if (leaf.empty() || leaf.front() == '.')
        return false;
    size_t dot = leaf.rfind('.');
    return dot == std::string_view::npos || isImageExtension(leaf.substr(dot));
}
} // namespace

struct Index::Impl
{
    mutable std::mutex mutex;
    mutable std::condition_variable cv;
    fs::path root;
    Snapshot last;
    bool scanned = false;
    uint64_t requested = 0;
    std::shared_ptr<std::atomic<bool>> cancel;
    std::map<int, std::function<void()>> listeners;
    int nextListener = 1;
#ifdef _WIN32
    /** Signalled to stop the current root's watcher thread; shared with it so neither closes it under the other. */
    std::shared_ptr<void> watchStop;
#endif

    void notify()
    {
        mainq::post([this] {
            std::vector<std::function<void()>> fns;
            for (auto& [id, fn] : listeners)
                fns.push_back(fn);
            for (auto& fn : fns)
                fn();
        });
    }

    /** Mutex held. */
    void startScan()
    {
        uint64_t gen = ++requested;
        if (cancel)
            cancel->store(true);
        cancel = std::make_shared<std::atomic<bool>>(false);
        last.scanning = true;
        std::thread([this, gen, r = root, c = cancel] {
            LILA_THREAD_NAME("Project textures");
            bool truncated = false;
            std::vector<std::string> files = scan(r, {}, c.get(), &truncated);
            {
                std::scoped_lock lock(mutex);
                if (gen != requested)
                    return;
                last = {r, std::move(files), false, truncated, gen};
                scanned = true;
            }
            cv.notify_all();
            notify();
        }).detach();
    }

#ifdef _WIN32
    /** Mutex held. Watches root for files and folders appearing, vanishing or being renamed. */
    void startWatch()
    {
        if (watchStop)
        {
            SetEvent(watchStop.get());
            watchStop.reset();
        }
        if (root.empty())
            return;
        HANDLE dir = CreateFileW(root.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (dir == INVALID_HANDLE_VALUE)
            return;
        std::shared_ptr<void> stopEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr), CloseHandle);
        watchStop = stopEvent;
        fs::path watched = root;
        std::thread([this, dir, stopEvent, watched] {
            HANDLE stop = stopEvent.get();
            LILA_THREAD_NAME("Project texture watch");
            OVERLAPPED ov{};
            ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            std::vector<DWORD> buf(16384);
            bool reading = false;
            bool pending = false;
            auto due = std::chrono::steady_clock::now();
            while (true)
            {
                if (!reading)
                {
                    ResetEvent(ov.hEvent);
                    if (!ReadDirectoryChangesW(dir, buf.data(), DWORD(buf.size() * sizeof(DWORD)), TRUE,
                                               FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME, nullptr, &ov, nullptr))
                        break;
                    reading = true;
                }
                DWORD timeout = INFINITE;
                if (pending)
                {
                    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(due - std::chrono::steady_clock::now()).count();
                    timeout = DWORD(std::max<long long>(left, 0));
                }
                HANDLE handles[2] = {ov.hEvent, stop};
                DWORD w = WaitForMultipleObjects(2, handles, FALSE, timeout);
                if (w == WAIT_OBJECT_0 + 1 || w == WAIT_FAILED)
                    break;
                if (w == WAIT_TIMEOUT)
                {
                    pending = false;
                    std::scoped_lock lock(mutex);
                    if (root == watched)
                        startScan();
                    continue;
                }
                DWORD bytes = 0;
                reading = false;
                if (!GetOverlappedResult(dir, &ov, &bytes, FALSE))
                    break;
                // Zero bytes means the buffer overflowed and the changes are unknown.
                bool relevant = bytes == 0;
                for (auto* p = reinterpret_cast<const BYTE*>(buf.data()); !relevant && bytes;)
                {
                    auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(p);
                    std::wstring name(info->FileName, info->FileNameLength / sizeof(WCHAR));
                    relevant = relevantChange(paths::toUtf8(fs::path(name)));
                    if (!info->NextEntryOffset)
                        break;
                    p += info->NextEntryOffset;
                }
                if (relevant && !pending)
                {
                    pending = true;
                    // Copies and bakes land as bursts of events; one rescan after they settle.
                    due = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
                }
            }
            if (reading)
            {
                CancelIoEx(dir, &ov);
                DWORD ignored = 0;
                GetOverlappedResult(dir, &ov, &ignored, TRUE);
            }
            CloseHandle(ov.hEvent);
            CloseHandle(dir);
        }).detach();
    }
#endif
};

Index::Index() : m(new Impl) {}

Index& Index::get()
{
    // Leaked: the worker threads are detached and may outlive static destruction.
    static Index* index = new Index;
    return *index;
}

void Index::setRoot(const fs::path& root)
{
    std::error_code ec;
    fs::path canon = root.empty() ? fs::path() : fs::weakly_canonical(root, ec);
    std::scoped_lock lock(m->mutex);
    if (canon == m->root && m->scanned)
        return;
    bool moved = canon != m->root;
    m->root = canon;
    if (canon.empty())
    {
        ++m->requested;
        m->last = {};
        m->scanned = true;
#ifdef _WIN32
        m->startWatch();
#endif
        m->cv.notify_all();
        m->notify();
        return;
    }
    if (moved)
    {
        m->last.files.clear();
        m->last.root = canon;
#ifdef _WIN32
        m->startWatch();
#endif
    }
    m->startScan();
}

void Index::follow()
{
    setRoot(projectRoot());
#ifndef _WIN32
    // No change notifications here, so every look rescans.
    refresh();
#endif
}

fs::path Index::root() const
{
    std::scoped_lock lock(m->mutex);
    return m->root;
}

void Index::refresh()
{
    std::scoped_lock lock(m->mutex);
    if (!m->root.empty())
        m->startScan();
}

Index::Snapshot Index::snapshot(bool wait) const
{
    std::unique_lock lock(m->mutex);
    if (wait)
        m->cv.wait(lock, [&] { return !m->last.scanning; });
    return m->last;
}

int Index::addListener(std::function<void()> fn)
{
    int id = m->nextListener++;
    m->listeners[id] = std::move(fn);
    return id;
}

void Index::removeListener(int id)
{
    m->listeners.erase(id);
}
} // namespace lila::projectTextures
